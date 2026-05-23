# JIT confidence — rationale, upside, and why it isn't the next investment

**Date:** 2026-05-23
**Author:** session synthesis
**Status:** strategic — codifies the position behind "Stage 12 candidate, deferred" in `ROADMAP_TO_VISION_2026-05-15.md` and `OPTIMIZATION_STRATEGIES_2026-05-23.md` §6
**Triggering question:** "What rationale exists, and how confident are we that a JIT would *really* benefit? The biggest perf seems to be lost in the derivation* primops?"

Companion memos: [WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) (where wall actually goes on warm path), [GC_VS_TW_ANALYSIS_2026-05-23.md](GC_VS_TW_ANALYSIS_2026-05-23.md) (orthogonal GC dimension), [OPTIMIZATION_STRATEGIES_2026-05-23.md](OPTIMIZATION_STRATEGIES_2026-05-23.md) §6 (kill listing).

---

## 1. Position (TL;DR)

**JIT is NOT v3's next investment. Confidence: HIGH that it would underperform alternatives. The case is stronger than the original framing — OPCYCLES (#786/#787) and per-primop wall-clock (#788) data raise the confidence further.**

The rationale rests on four independently-measured facts:

1. **Dispatch is ~5 % of wall on hello.drvPath**, not ~21 % as initially derived from #780 wall-arithmetic. Per #786 OPCYCLES in-handler markers and #787 OP_RETURN-misattribution RCA: combined stack-motion ops cost ~36 ms = ~4.8 % wall; real OP_RETURN cost is ~36 ms = ~5 % (the 51 % "OP_RETURN" attribution was outer-C++ continuation work misattributed at dispatch-loop exit). The 48.92 % figure that #778 surfaced is opcode *count*, not wall *time*.
2. **Derivation primop bodies are 99 % of primop wall on hello.drvPath** (#788, commit `308002951`): __derivationFromPreprocessed 48 %, __derivCoerce 35 %, __derivationStrictRaw 16 %. Three primops account for 10 s of primop wall. JIT cannot accelerate primop bodies — they are C++ functions doing hashing, file I/O, store-path computation, force-chains over heterogeneous Value inputs.
3. **Mechanical primop-redundancy is already exhausted** (#791 + #792 audit, commit `9c9eea6e2` revert): __derivCoerce outPath fast path delivered −4.8 ± 7 ms (below 8 ms ship threshold, reverted); __derivationStrictRaw + __derivationFromPreprocessed audit found no clean redundancy. The only remaining architectural lever for these primops is #741 IFD content-addressed eval-result cache (Phase 1 landed 23bb231d2, falsifier passed with 56× margin).
4. **Equivalent or larger wall reduction is reachable via #741 IFD cache + AOT distribution + Tier 1 IC broadening + nursery default-on** — all already on the roadmap, collectively ~1/50th the cost of a JIT.

The user's intuition is correct, and more precisely correct than the original framing acknowledged: when ~95 % of wall lives outside the dispatch loop, JIT-ing the dispatch loop delivers a tiny fraction of v3's headroom.

---

## 2. Where the wall actually goes on hello.drvPath

### 2.1 Measured

| Source | Claim |
|---|---|
| #780 (`cbfeb53e2`) | Initial wall-arithmetic estimate: 6.3M local-stack-motion ops × ~25 ns ≈ ~158 ms ≈ ~21 % of wall. Register-VM ceiling ~10-15 %. *Subsequently corrected downward by #786/#787 to ~4-5 % real ceiling.* |
| #786 (`14c8da998+6ce43b486+3b0e567ed`) | OPCYCLES per-op cycle accumulator + in-handler markers. **Stack-motion ops measured at ~5 ns/op REAL** (vs #780's 25 ns estimate). Combined stack-motion = 36 ms = 4.8 % wall. Register-VM ceiling **~4-5 % wall, not 10-15 %**. |
| #787 (`36233450b`) | OP_RETURN appeared as 51 % wall in raw OPCYCLES — *misattribution bug*. In-case-body markers show real OP_RETURN is ~47 ns/return = 36 ms = ~5 % wall. The 360 ms attributed to OP_RETURN actually lives in primop continuations + callClosure cleanup + inter-dispatch-loop transitions. |
| #788 (`308002951`) | Per-primop wall-clock dumper. **99 % of primop wall (10 s) lives in 3 derivation primops on hello.drvPath**: __derivationFromPreprocessed 48 %, __derivCoerce 35 %, __derivationStrictRaw 16 %. Overturns prior "elem is hot" intuition — elem is 0.04 % of wall. |
| #791 (`37d07a1c2+9c9eea6e2`) | __derivCoerce outPath fast path implement-then-revert: −4.8 ± 7 ms (sub-threshold, reverted with measurement data). Closes the toStringCoerceCtx mechanical-redundancy lever. |
| #792 audit | __derivationStrictRaw + __derivationFromPreprocessed: no mechanical redundancy found. Both primop bodies are TIGHT; per-call cost dominated by inherent work (force chains, v3CoerceToString, libstore hashing). Only remaining architectural lever is #741 IFD cache. |
| #778 (`fe7c17498`) | Schema 10 OP_REC_BINDING_SLOT_REF IC: AttrSelect family = 2.24 % of dispatches (well below 10 % threshold for hidden classes). |
| #783 | Top-5 super-instruction class capped at ~18 ms / 2.3 % wall (per-fusion 5 ns; max 1.4 ms wall). |
| #741 Phase 1 (`23bb231d2`) | Round-trip Value-subset serializer LANDED. Phase 1 falsifier passed 56× margin: 785/785 success on hello.drvPath, 1.74 µs/result (target <100 µs). Phases 2-5 green-lit. |
| WARM_EVAL §4.3 | Estimated v3 execute ~882 ms vs TW execute ~525-575 ms = ~1.6× execute-only ratio. |

### 2.2 Decomposition (substantially measured post-#786/#787/#788)

```
Bytecode dispatch (all stack motion +
  RETURN + arithmetic combined)    ~5-10 %  <— JIT addressable; SMALLER than originally thought
Allocation paths (Bindings, Thunks,
  Closures, Phase D barriers)      ~15-25 % <— JIT addressable, partial
Force machinery (state machine,
  blackhole, cell update, inter-
  dispatch-loop transitions)       ~10-15 % <— JIT addressable, partial
Primop execution bodies            ~45-55 % <— NOT JIT addressable
  (3 derivation primops account
   for 99% of primop wall on
   hello.drvPath per #788; libstore
   hashing, drv-file writes, force
   chains over heterogeneous Values)
GC scan + barriers                 ~5-10 %  <— orthogonal subsystem
String / context propagation        ~5-10 % <— partial
```

The ~45-55 % primop slice is the user's observation, now empirically grounded by #788. On derivation-heavy real workloads (cardano-node M5, NixOS toplevel, anything producing store paths) this fraction grows further because the derivation work multiplies across the closure.

---

## 3. What JIT can and cannot accelerate

### 3.1 Can accelerate (the optimistic case)

- **Opcode dispatch:** eliminate switch / jump-table cost; direct branch to next op.
- **Argument-type checks:** specialize on observed types (Int vs Float vs String dispatch).
- **Multi-opcode fusion:** collapse "force; load; force; add" into native sequence.
- **Closure call indirection:** speculative inlining of known callees.
- **Allocation fast path:** inline bump-pointer + write-barrier emission natively.
- **Force-state-machine elision:** when control flow proves the thunk is already Evaluated.

### 3.2 Cannot accelerate (the hard ceiling)

- **Primop body execution.** `primDerivationStrictNative`, `primHashString`, `primToFile`, `primToString` on derivations — these are C++ functions. The JIT cannot reach inside them. At best, it eliminates the call overhead (already small).
- **Store-path computation.** SHA-256 hashing + derivation canonicalization is opaque.
- **File I/O.** Writing `.drv` files happens during evaluation in v3 (and TW); the JIT doesn't affect filesystem latency.
- **Cross-FFI context propagation.** `NixStringContext` aggregation traverses `std::set` / `std::vector`; not JIT-addressable without rewriting the data model.
- **GC scan time.** Different subsystem entirely — addressed by nursery default-on, not by JIT.
- **Boehm tenured allocation overhead.** Until Whippet or equivalent replaces Boehm, tenured allocs cost the same regardless of JIT.
- **The fundamental laziness semantics.** force/blackhole/evaluated transitions must execute the same state machine.

---

## 4. Realistic JIT upside arithmetic

Assume an idealised JIT that perfectly eliminates dispatch overhead + partially accelerates allocation, force, and string paths.

### 4.1 Best-case calculation (revised post-#786/#787/#788)

```
v3 today on hello.drvPath:           895 ms (v3:TW = 1.41×)

Hypothetical perfect JIT accelerating
~25 % of wall (dispatch + half of
  alloc/force/string fast paths):

  Dispatch saving (~5-10 % wall):     ~40-70 ms × (1 - 1/3 native penalty)
                                      ≈ ~30-50 ms saved

  Alloc/force fast-path saving:       ~40-60 ms

  String op saving:                   ~20 ms

  Total saving:                       ~90-130 ms

Post-JIT wall:                        ~765-805 ms
Post-JIT v3:TW ratio:                 ~1.14-1.20×
```

**A multi-year JIT investment plausibly delivers ~10-15 % wall reduction on hello.drvPath, bringing v3 from 1.41× TW to ~1.14-1.20× TW.** This is *worse* than the original 25 % framing in §1, because OPCYCLES (#786) revised the dispatch ceiling downward from ~21 % to ~5 %.

Compare to #741 IFD cache (Phase 1 landed; falsifier passed 56× margin): if the per-derivation eval-result cache hits even 50 % on hello.drvPath warm path, it eliminates ~5 s of primop wall directly. That's an order of magnitude larger than the JIT projection — at 1-2 weeks of remaining work for Phases 2-5, not multi-year.

### 4.2 On derivation-heavier workloads (cardano-node M5, NixOS toplevel)

The primop slice grows. If primops account for ~40 % of wall instead of ~25 %, the JIT-addressable fraction shrinks to ~30 % of wall. Upside drops proportionally to ~15-20 % wall reduction.

### 4.3 On dispatch-bound microbenchmarks (fib33, ackermann)

JIT helps more. fib33 (currently 1.64× TW) and ackermann are mostly dispatch + arithmetic. Plausible 40-60 % wall reduction. **But these aren't representative of real Nix workloads.**

### 4.4 Interpreter ceiling context

- Piumarta-Riccardi 1998: selective-inlining direct-threaded code reaches ~70 % of optimised C on numerical kernels (~1.4× native).
- LuaJIT-Remake baseline JIT: ~33 % slower than LuaJIT's optimising JIT — bytecode interpreter with full IC + tail-call dispatch sits ~2× off native.
- CPython 3.11 PEP 659 specialization: ~25 % geomean wall improvement; still ~30× off C.

v3 is at ~1.4× TW today. TW is a tree-walker, not native. Hypothetical native baseline is probably ~2-3× faster than TW. So v3 is ~2.8-4.2× native — within the interpreter ceiling band. **JIT could plausibly close this gap on dispatch-bound workloads. On derivation-heavy workloads it cannot.**

---

## 5. Cost vs alternatives

### 5.1 JIT cost (rough)

- Multi-year, multi-person commitment (1-2 senior engineers, 12-24 months)
- Architectural impact: code-cache management, deopt machinery, ABI design, IR design (likely BB-level or trace-based)
- Test surface explodes: must handle every primop / FFI boundary correctly
- Distribution complexity: JIT-emitted code is per-machine, can't easily ship in a binary cache
- Risk: deopt thrashing on shape changes; trace stability under Nix's dynamic semantics

### 5.2 Alternatives that target the actual bottleneck

| Investment | Estimated wall reduction | Cost | Targets |
|---|---|---|---|
| **AOT distribution + warm cache** (nixpkgs-bytecode-cache via cache.nixos.org) | ~5-10 % warm + asymmetric multi-tenant win | 1-2 weeks v3 + Nix-team buy-in | Compile residue (parse + lower + emit) |
| **Eval-result cache at IFD boundaries** (materialization-retirement program per IFD_DEEP_DIVE §11) | ~10-20 % on derivation-heavy workloads | 2-3 weeks | `primDerivationStrict*` repeat calls; haskell.nix-class workarounds become unnecessary |
| **Tier 1 IC broadening** (OPTIMIZATION_STRATEGIES §1) | ~10-15 % wall | 6-10 weeks | OP_CALL primop dispatch, attrset cardinality |
| **Top-of-stack caching** (OPTIMIZATION_STRATEGIES §2) | ~5-15 % wall | 2-4 weeks | Dispatch-adjacent — interpreter-side STG-register equivalent |
| **Nursery default-on** (GC_VS_TW_ANALYSIS) | 10-50× allocation throughput, 3-4× smaller working set, ~5-10 % wall as side effect | 1-2 weeks | Allocation cost, working set, pause time |
| **V3_RELEASE compile flag** (WARM_EVAL §6.1) | ~3-4 % wall + 25-40 MB memory | 1 day | Always-on instrumentation cost |
| **Native primop targeted optimization** (#673/#682/#674 thread continued) | varies, ~5-15 % on context-heavy paths | ongoing, cheap | The primop bodies that JIT *cannot* reach |

**Sum of plausible reductions from alternatives: ~40-70 % wall.** Even with significant overlap and over-optimistic stacking, the realistic combined effect exceeds what JIT could deliver on derivation-heavy workloads, at roughly 1/50th the cost.

### 5.3 Critical observation

The native primop optimization line is where JIT can NEVER help but where the team CAN. Every `forceStringNoCtx` audit, every `requireNoStringContext` fix, every `primToFile` context-correctness landing reduces real wall time inside primops. The team has been doing this organically (#673, #682, #674, #680, #685, #687). **This is the right form of work for the bottleneck the user identified.**

---

## 6. Confidence ratings (explicit)

| Claim | Confidence | Source |
|---|---|---|
| Dispatch is ~5 % of wall on hello.drvPath | HIGH | #786 OPCYCLES + #787 misattribution RCA; in-handler markers |
| Stack-motion ops cost ~5 ns/op (not 25 ns) | HIGH | #786 direct measurement; #780 estimate corrected |
| Three derivation primops account for 99 % of primop wall (~45-55 % of total wall) | HIGH | #788 per-primop wall-clock, hello.drvPath |
| JIT cannot accelerate primop bodies | HIGH | architectural — primops are C++ |
| Mechanical primop redundancy exhausted on the top-3 primops | HIGH | #791 implement-then-revert + #792 audit |
| Hypothetical perfect JIT delivers ~10-15 % wall reduction on hello.drvPath | MEDIUM-HIGH | revised from §4.1 post-OPCYCLES data |
| Multi-year JIT investment is NOT worth it given alternatives | HIGH | wall arithmetic + cost analysis above |
| #741 IFD cache + AOT distribution + Tier 1 ICs + nursery default-on deliver equivalent or larger reduction at ~1/50th cost | MEDIUM-HIGH | #741 Phase 1 landed (56× margin); WARM_EVAL framing; GC_VS_TW projections |
| User's observation about derivation* primops is correct | HIGH | empirically confirmed by #788 |

---

## 7. Codified position (proposed for inclusion in roadmap)

> **JIT is NOT on v3's near-term roadmap.** The decision is data-driven, not ideological.
>
> 1. Dispatch is ~5 % of wall on hello.drvPath (#786 OPCYCLES + #787 misattribution RCA). JIT addresses a tiny fraction of the time budget.
> 2. Three derivation primops account for 99 % of primop wall and ~45-55 % of total wall (#788). JIT cannot accelerate primop bodies — they are C++ functions doing hashing, file I/O, force chains, libstore work.
> 3. Mechanical primop redundancy is already exhausted (#791 implement-then-revert + #792 audit). The only remaining architectural lever for these primops is #741 IFD eval-result cache (Phase 1 landed, falsifier passed 56× margin).
> 4. Realistic JIT upside on derivation-heavy workloads: ~10-15 % wall reduction. Cost: multi-year, multi-person.
> 5. Equivalent or larger reduction is reachable via #741 IFD cache + AOT distribution + Tier 1 IC broadening + TOS caching + nursery default-on — all on the existing roadmap, at ~1/50th the cost.
> 6. Revisit only when (a) dispatch becomes the binding constraint (#786 says it is ~5 %, not binding) AND (b) all the easier wins listed above are exhausted AND (c) primop bodies are no longer the wall-leader on production workloads (#741 + native primop optimization should remove this).
>
> **Revival trigger (Rule 0 falsifier):** dispatch share of wall > 40 % on cardano-node M5 measured directly via OPCYCLES (not opcode count) AND remaining alternatives < 5 % wall to extract.

---

## 8. Cross-references to existing strategic docs

This memo refines the deferred-JIT position already implicit in:

- `ROADMAP_TO_VISION_2026-05-15.md` Stage 12 (candidate, pending measurement)
- `OPTIMIZATION_STRATEGIES_2026-05-23.md` §6 (family-wide deferrals)
- `PERF_STRATEGY_2026-05-17.md` Stage 12
- `WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md` §4 (execute-only ratio framing — primops dominate execute time)
- `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` (decomposed the 200× force-rate gap; primop slice was prominent)
- `IFD_DEEP_DIVE_2026-05-21.md` §11 (materialization-retirement = the right form of derivation-primop optimization)

It also strengthens Rule 0 application: the deferral is now backed by measured wall-arithmetic (not just intuition), and has an explicit revival falsifier.

---

## 9. Recommended next moves

In priority order:

1. **Re-read #765 CPU profile data** (if not already on hand). Confirm or refute the ~20-30 % primop estimate on hello.drvPath. If primops are MORE than 30 % on cardano-node M5, the JIT case weakens further — and the eval-result cache program becomes higher priority.
2. **Add the "JIT confidence" section to `OPTIMIZATION_STRATEGIES_2026-05-23.md` §6** referencing this memo. Current §6 lists JIT as deferred without the wall-arithmetic justification.
3. **Update `ROADMAP_TO_VISION_2026-05-15.md` Stage 12** to include the revival falsifier from §7 above.
4. **Prioritize the derivation-primop-targeted work that DOES help:** continued native primop optimization (#673/#682/#674 thread), eval-result cache spike (IFD_DEEP_DIVE §11), AOT distribution spec.

---

## 10. Honest limits

- §2.2 decomposition is estimated, not measured. #765 CPU profile should ground or correct it.
- §4.1 JIT upside arithmetic assumes a perfect JIT; real JITs (CPython 3.13/3.14 Tier 2, LuaJIT-Remake baseline) deliver less than the theoretical maximum due to deopt, code-cache misses, and missed specializations.
- §5.2 alternative estimates stack non-trivially; combined wall reduction is likely smaller than the column sum.
- The position assumes derivation-heavy workloads remain the user-facing target. If v3's primary workload shifts to dispatch-bound pure-Nix computation (lib.evalModules-only, module system stress tests), the JIT case strengthens.
- Confidence in JIT being outperformed by alternatives drops if (a) AOT distribution stalls indefinitely on Nix-team coordination, OR (b) eval-result cache turns out architecturally infeasible at IFD boundaries.

---

*Save trigger: explicit user request ("yes" confirmation 2026-05-23) after JIT-rationale question. Pairs with WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md and GC_VS_TW_ANALYSIS_2026-05-23.md to form the complete strategic doc set for the "wall is not where you think it is" thread.*
