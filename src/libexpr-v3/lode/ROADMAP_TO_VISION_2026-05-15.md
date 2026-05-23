# Roadmap to Vision — 2026-05-15

A detailed, ordered, step-by-step path from v3's current state (≈30-35% of the architectural vision shipped) to the stated end-state: an **STG-inspired, V8-influenced bytecode VM** for Nix with a custom generational GC, cppnix parser reuse, thin FFI, and pure-bytecode evaluation.

Companion docs:
- `ACTION_PLAN_2026-05-15.md` — immediate 8-week corrective plan (Phases 0-4). Stage 1 below points to it.
- `ALIGNMENT_SCORECARD_2026-05-15.md` — vision-vs-reality scorecard updated quarterly.

The roadmap covers Stages 1 through 8 (~44 weeks, target end ≈ 2027-Q1).

---

## Optimization targets — memory is first-class alongside wall (codified 2026-05-23)

**Both wall-clock time AND peak memory consumption are first-class
optimization targets.** A wall-neutral change that reduces peak RSS
by ≥ 50 MB on hello.drvPath should ship. A wall-positive change
that costs > 100 MB peak RSS needs explicit justification. Even
if v3 stays slightly slower on wall than TW, lower memory is a
massive win because:

- **Concurrent capacity** — Hydra, CI, nix-eval-jobs all scale
  linearly with per-process RSS. A 50 % memory reduction is
  literally 2× the parallel job capacity at the same hardware.
- **Wall via second-order effect** — GC scan time, nursery
  scavenge cost, and cache-line pressure all scale with live
  set. Memory wins feed wall wins.
- **Empirical leverage** — In the 4 days 2026-05-19 to
  2026-05-23, peak RSS on hello.drvPath went 4.3 GB → ~1.08 GB
  (75 % reduction) while wall ratio moved from 30× → 1.41× TW
  over a longer window. **Memory has higher slope per
  engineering-day than wall.**
- **Below the interpreter ceiling** — Per
  `OPTIMIZATION_STRATEGIES_2026-05-23.md` §9, wall asymptotes at
  ~1.5-2× native. Memory has more headroom.

Operationalisation:

- Every optimisation claim must report both wall delta AND peak
  RSS delta. Claims missing either are unmeasured.
- Bench harness includes peak-RSS by default. Cross-eval matrix
  tables MUST have an RSS column.
- Per `measure-twice-cut-once`: memory claims need pre-committed
  thresholds the same as wall claims.
- Cardano-node M5 budget: v3 currently 919 MB peak RSS (1.02× TW)
  with 4 GB watchdog (`NIX_V3_MAX_HEAP=4G`). ~3 GB of headroom;
  future memory work compounds this margin.

Full rule in `feedback_memory_first_class.md` + the recently-landed
landings (#748 −14.6 MB, #750 −386 MB, #752 −849 MB peak) prove
the memory-first cadence works.

---

## Warm-eval is the primary user-facing target (codified 2026-05-23)

**The user-facing perf scenario is warm eval (cache load + execute)
vs TW, NOT cold eval (parse + lower + emit + execute).** AOT
precompilation of nixpkgs / haskell.nix / other libraries as
deployment artifacts is explicitly acceptable. Full analysis in
`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`.

Three reasons this framing matters:

1. **The disk cache (default-on per #777) already AOT-compiles on
   first eval; warm eval is the dominant scenario for any user
   who runs `nix` twice.** First-eval-after-install on a fresh box
   is the cold case; everything else is warm.

2. **TW has no equivalent caching layer.** `eval-cache-v5.sqlite`
   caches only top-level flake outputs; every import is re-parsed
   + re-AST-built on every eval. **In multi-tenant scenarios (CI
   farms, Hydra, nix-eval-jobs at 1000 evals/day on shared
   nixpkgs), TW pays parse cost 1000×; v3 pays ~1×.** This
   asymmetry is invisible in single-process benchmarks.

3. **The published v3:TW = 1.41× wall on hello.drvPath includes
   compile residue.** Estimated execute-only ratio is ~1.6×
   (deserialize 13 ms + execute ~882 ms vs TW parse ~100-150 ms +
   execute ~525-575 ms). Wall ratio masks the execute-path picture
   that warm-eval users actually experience.

Operationalisation:

- **Bench harness must publish both wall AND execute-only ratios.**
  The infrastructure exists (#769 V3_TIMING per-import phase
  timing) — needs a measurement spike to populate it.
- **A `V3_RELEASE` compile-flag** strips always-on
  instrumentation (per-category byte counters, attrset histogram,
  unused struct slots for `Thunk::forces`/`Thunk::shapeCell`,
  bigram array). Estimated recovery: ~3-4 % wall + ~25-40 MB
  permanent memory + ~5 KB i-cache. **Single-day work.**
- **Future AOT distribution work** (`nixpkgs-bytecode-cache` as
  a published binary-cache artifact) is Nix-infrastructure work,
  not v3-VM work. The v3 side is done; the broader Nix ecosystem
  step is what's missing.
- **Cross-process bytecode mmap** (Stage 8 candidate) becomes
  transformative for multi-tenant scenarios once the cache is
  distributed.
- **Eval-result-cache as sibling AOT artifact** (added 2026-05-23
  per `EVAL_CACHE_ARCHITECTURE_2026-05-23.md`): the same delivery
  story — Nix package built from offline batch evaluation, shipped
  via cache.nixos.org, mmap'd at v3 startup — applies to a
  `nixpkgs-eval-result-cache.mmap` artifact alongside the bytecode
  cache. #741 Phase 5 measured 95-97 % cross-workload hit rate; on
  a fresh CI box with both artifacts substituted, first-eval is
  warm-eval at parse-AND-primop level. **The two compose
  multiplicatively against TW**, which has neither layer and cannot
  easily ship either.

The strategic insight: **the team has been optimizing the
workload-as-measured (single-process, cold-include-compile), but
the user-facing scenario is warm-execute.** The Tier 1
optimization-strategies work (broaden ICs), nursery default-on,
and Tier B/C memory reductions all remain load-bearing — they
target the execute path. The reframing ELEVATES the release-build
cleanup + AOT distribution work that the team hasn't yet
prioritized.

Combines with the memory-first-class rule: a 1-day
release-build-mode flag delivers ~3-4 % wall + ~25-40 MB memory
recovery — exactly the kind of trade-off both rules endorse.

---

## Strategic ordering rationale (why this order)

Stage order is fixed by dependencies. Each stage *unlocks* the next; out-of-order execution wastes effort.

1. **Correctness before optimization.** V8-style PICs are useless if the VM cannot complete the workload. → Action plan first.
2. **Pure-eval before nursery default-on.** The nursery's value is proportional to allocation traffic *through v3*, not TW. If TW does the work, the nursery is dead weight. → Push TW out of the picture before measuring nursery wins.
3. **Uniform STG before shapes.** V8 hidden classes assume consistent allocation paths per source construct. Today's emit-time eager/lazy asymmetries mean the same source position produces different runtime shapes. → Fix lowering uniformity before tagging shapes.
4. **Shapes before PICs.** A PIC needs a shape key. No shapes → no inline cache.
5. **PICs before selector thunks.** Selector thunks share work; the shape system is what tells you that two uses share the same source.
6. **Thin FFI is parallel, ongoing.** Reducing the bridge surface happens alongside Stages 2-7; it has explicit checkpoints in Stages 2 and 8 but no standalone phase.

Diagram of dependencies (arrows = "must complete before"):

```
[Action Plan (Stage 1)]
        |
        v
[Stage 2: Pure-bytecode eval] ----+
        |                         |
        v                         v
[Stage 3: Nursery default-on]   [Stage 8: Thin FFI ongoing]
        |                         |
        v                         |
[Stage 4: Uniform STG-shape]      |
        |                         |
        v                         |
[Stage 5: Hidden classes]         |
        |                         |
        v                         |
[Stage 6: PICs]                   |
        |                         |
        v                         |
[Stage 7: Selector thunks] <------+
```

---

## Stage 1 — Action plan completion (Weeks 1-8, prerequisite)

Out of scope here. Refer to `ACTION_PLAN_2026-05-15.md`.

**Exit (≈2026-07-10)**:
- `hello.name` evaluates correctly in v3-direct.
- Bench within 1.3× of TW on `lib-evalModules-100`.
- `vm.cc` split into ≤6 files, each ≤2 000 LoC.
- Env-var count ≤30.
- C1-C8 silent semantic gaps closed; `.err.exp` diffing in test runner.

Until these are met, **do not start Stage 2.**

---

## Stage 2 — Achieve pure-bytecode evaluation (Weeks 9-14)

> **2026-05-20 FFI audit context**: per `FFI_AUDIT_2026-05-20.md`, Stage 2 retires the single biggest 🔻 drift item on the scorecard. This is the largest TW dependency by share-of-work — closing it flips scorecard component #11 from 🔻 to ✅.

### Goal

Retire `NIX_V3_SKIP_INSTALLABLE_PREEVAL` entirely. v3-direct evaluates all real nixpkgs workloads without TW pre-eval. Bridge layer reduced from ~370+ LoC in vm.cc to a clean FFI surface.

### Why now

The architectural promise is "most evaluation in the pure bytecode VM." Today TW pre-eval is default-on (commit 1ac5795b0). Until that's reversed, v3 is decorative — TW does the real work and v3 is along for the ride.

Doing this before Stage 3 matters because: the nursery's value scales with allocation traffic. If TW is doing the work, the nursery sees no traffic, and any perf measurement of "nursery on/off" is noise.

### Prerequisites

- Action plan Phase 2 closed (cycle-handling architectural decision committed: either CELL_EVERYWHERE default-on or fixed THUNK_ALL).
- Action plan Phase 3 closed (closure-pool reckoning).
- Bench harness running weekly.

### TODOs

- [ ] **Flip the gate**. Set `NIX_V3_SKIP_INSTALLABLE_PREEVAL=1` for one week of development. Run the v3-direct workload sweep. Catalogue every workload that breaks. (1 day to set up; 1 week observation.)
- [ ] **Triage breakages**. For each failure, classify: (a) v3 bug, (b) TW dependency we forgot existed, (c) bridge-layer escape hatch we can delete. (2 days.)
- [ ] **Fix category (a)** v3 bugs. Each fix follows the action plan's discipline: regression test in-commit, no new env-var without retirement criterion.
- [ ] **For category (b)**, identify the v3 equivalent: a primop bridge, IFD path, attribute traversal, etc. Implement the v3-native version. (2-3 weeks total across this and the next item.)
- [ ] **For category (c)**, delete the escape hatch and re-test.
- [ ] **Audit `bridge_yield.cc`** call sites. Each one is either (i) a legitimate FFI to cppnix store/derivation primitives — keep, but move to `ffi.cc` — or (ii) a v3-can't-handle escape hatch — delete after v3 handles. (4 days.)
- [ ] **Audit the ~370 LoC of bridge plumbing in vm.cc** (`v3CallBridge1`, `v3ForceAttr`, `clearBlackMarksOnException`, etc.). Move legitimate FFI to `ffi.cc`. Delete escape hatches. (1 week.)
- [ ] **Delete the `NIX_V3_SKIP_INSTALLABLE_PREEVAL` gate** entirely.
- [ ] **Document the new FFI surface**: one `.hh` file listing every cppnix entry point v3 calls. (1 day.)
- [ ] **Bench**. Re-run real-world workloads (hello.name, attrNames-on-nixpkgs, cardano-node, libsForQt5.kdevelop) and commit a `bench/baselines/stage-2-exit.json`.

### Exit criteria

- `NIX_V3_SKIP_INSTALLABLE_PREEVAL` is deleted (not just default-off).
- `bridge_yield.cc` is deleted or reduced to <100 LoC.
- vm.cc bridge plumbing is <100 LoC; ≤5 call sites total.
- Full nixpkgs `attrNames` completes in v3-direct.
- cardano-node evaluates in v3-direct.
- `ffi.cc` surface is documented.

### Verification

Weekly bench harness: must show v3-direct completing all real-world workloads. If any workload regresses to "cannot complete," Stage 2 is not done.

### Kill criterion

If after 6 weeks ≥3 workload categories still need TW pre-eval, the cycle-handling architecture chosen in Action Phase 2 was wrong. **Go back to that decision.** Do not paper over with new gates.

### What this stage unlocks

- Stage 3 can be measured meaningfully (allocation traffic now flows through v3).
- Stage 8 (thin FFI) has its first big checkpoint behind it.
- The project can honestly claim "pure-bytecode evaluation" for the first time.

---

## Stage 3 — Nursery default-on, closure-pool retired (Weeks 15-20)

### Goal

`NIX_V3_NURSERY` becomes opt-OUT (rename to `NIX_V3_NO_NURSERY`). Closure-pool is either deleted or simplified to a small fast-path without sentinel bits. The Cheney nursery is the primary allocator for v3 values.

### Why now

A generational GC is in the stated vision. Today the closure-pool fills the void — a hand-rolled recycling pool with `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, `kFakeCloMagic` sentinel infrastructure. A5/A6 bugs prove the recycle protocol is ill-defined. The Cheney design (`CHENEY_NURSERY_DESIGN.md`) is most of the way there — Phase A (allocator) and Phase C (scavenge) landed. What's missing is Phase D (write barriers) and Phase E (scavenge frequency policy).

Doing this before Stage 4 matters because: Stage 4 will push allocation rate up by 5-10× (every binding becomes a thunk). The nursery has to absorb that, or Stage 4 will look like a perf regression.

**Empirical motivation (added 2026-05-18 after Phase 1 closure)**: the `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` analysis of `hello.drvPath` (~30× slower than TW) found the Boehm heap growing past 1 GB during long evals, with GC scan time amortized into the per-force cost (estimated ~5-10× of the observed 200× force-rate gap). Boehm conservative GC inherited from cppnix has no generational separation, doesn't shrink the heap once grown, and conservatively retains pointer-shaped words. v3's bytecode emits substantially more intermediate allocations than TW's AST interpretation (A-normal form IR, Tag::App memoization entries, bytecode primops). Stage 3 — landing the Cheney nursery default-on — is the architectural fix for the GC-scan factor of the 200× gap. **This is now load-bearing for closing hello.drvPath/outPath, not just preparatory for Stage 4.**

### Prerequisites

- Stage 2 closed (real allocation traffic to measure against).
- `V3_DBG_GC_STRESS` (random forced GC) functional.
- **Phase 1.7 closed** (added 2026-05-21 after `GC_AUDIT_ROUND_2_2026-05-21.md`):
  Round 2's eight correctness sub-fixes landed (shapeCell walked, forceWriteTarget
  pointer forwarded, Blackhole tail walked, Auditor mirror parity, CFF_FORCE_WB*
  cleared on throw, transitive CU IC walk via `Scavenger::walkedCUs`, huge
  allocations included in `blockRanges()`, recycleFakeClo zeroes `capturedWiths`)
  + `V3_DBG_NURSERY_BRUTE=1` wired into CI. Stage 3 starts on a known-clean
  Phase C correctness baseline.

### TODOs

- [ ] **Re-read `CHENEY_NURSERY_DESIGN.md`**. Identify the three Phase D options (A/B/C in that doc).
- [ ] **Profile the workload**: how often does v3 actually write through old→new pointers in nixpkgs eval? Use `perf` or Instruments to measure. (3 days.)
- [ ] **Choose Phase D path** based on the profile:
  - If old→new writes are rare: card-table or per-page dirty-bits (cheapest barrier).
  - If old→new writes are frequent and concentrated: targeted barriers at known write sites only.
  - If old→new writes are pervasive: full Steele-style barrier on every write.
  Document the choice in a one-page `NURSERY_PHASE_D_DECISION.md`. Close `CHENEY_NURSERY_DESIGN.md` with a RESOLVED row.
- [ ] **Implement Phase D**. (1.5-2 weeks.)
- [ ] **Implement Phase E (scavenge frequency)**. Heuristic options: every N allocations, every M dispatch-loop entries, watermark-based on nursery occupancy. Pick one, with rationale. Bench-tune the threshold. (3-4 days.)
- [ ] **Stress-test**. `V3_DBG_GC_STRESS=1` forces a scavenge after every 10-100 allocations. Run lang tests + nixpkgs eval; verify no value corruption. (1 week of run-and-fix.)
  - Note (added 2026-05-21): the gate name appears in `CLAUDE.md` + `LESSONS_LEARNED §4.9` but is **not yet implemented** as of round-2 audit. Implementing the gate is the first sub-task of this item: thread-local opcode-counter decrement at the dispatch loop top-of-loop (sibling of the existing `nursery->maybeScavenge` site at `vm.cc:2167`), unconditional `scavengeNursery()` call when the counter wraps. Must preserve the `exitDepth == 0` gate per `feedback_v3_nursery_cstack_safety.md`.
- [ ] **Closure-pool decision**:
  - Option α: delete the pool entirely; everything allocates through the nursery. Simplest. Bench-measure.
  - Option β: keep the pool as a hot-path-only allocator for `cl_force` / `forceValue` Suspended thunks (the single highest-frequency closure shape). No recycle protocol; freshly nursery-allocated each call. No sentinel bits.
  Decide based on Stage 2 bench numbers + α-vs-β micro-bench. Default: choose α unless β shows ≥5% on canonical bench.
- [ ] **Flip default**: rename `NIX_V3_NURSERY` to `NIX_V3_NO_NURSERY`; default-OFF (i.e. nursery default-on).
- [ ] **Retire `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, `kFakeCloMagic`** and related sentinel infrastructure.
- [ ] **Add property-test**: under `V3_DBG_GC_STRESS`, run randomized expression evaluation and assert (a) no crash, (b) result matches non-stressed run. (3 days.) This is the property-test framework that the scorecard called out as an orphan; landing it here lets it cover all subsequent stages.
- [ ] **Add differential-under-stress test mode** (added 2026-05-21 from round-2 R4):
  for each lang/eval-okay-*.nix + repro-*.nix + property-test case, run baseline
  (`NIX_V3_DIRECT_EVAL=1` no nursery) and stress
  (`NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_GC_STRESS=10`)
  and assert byte-equal stdout. Cost: ~6× existing property-test wall time;
  nightly CI only. Catches any future regression of any missed-root class on
  existing 580+ property cases + 143 lang cases. (3 days, gated on stress-test
  implementation above.)
- [ ] **Add `mprotect(PROT_NONE)`-on-reset mode** (added 2026-05-21 from round-2 R2):
  new gate `V3_DBG_NURSERY_PROTECT=1`. After scavenge, `mprotect(base, sizeBytes,
  PROT_NONE)`; before next allocation, restore RW. Converts missed-root dereferences
  into immediate SIGSEGV at the deref site (with backtrace) instead of silent
  corruption reading memset-zero memory. Requires nursery base to be page-aligned
  (switch `calloc` to `mmap(MAP_ANON)`). (3 days; dev-time only, not on by default.)
- [ ] **Field-walker registry** (added 2026-05-21 from round-2 R5; optional —
  defer if stress-test + BRUTE coverage proves sufficient): codegen or X-macros
  that enumerate every pointer-bearing field of every walked struct (Closure,
  Thunk, Bindings, ListVec, ValuePair, CallFrame). Mechanically prevents the
  next ValuePair::evaluated-class regression at compile time. (5-7 days; defer
  unless stress-test reveals more class-1 failures.)

### Exit criteria

- Nursery is default-on.
- Closure-pool is either deleted or simplified (no sentinel bits).
- A5-class corruption is structurally impossible (adversarial stress test passes 1000+ runs).
- Allocation throughput on alloc-heavy workloads ≥ TW.
- Bench within 1.2× TW on canonical (no regression vs Stage 2 baseline).

### Kill criterion

If Phase D write-barrier work exceeds 4 weeks, or measurements show no allocation benefit over the closure-pool, defer Stage 3 and skip ahead to Stage 4 — uniform STG with closure-pool as the allocator. Re-attempt Stage 3 after Stage 6 (PICs), when reduced allocation pressure may make the choice clearer.

### What this stage unlocks

- Stage 4 can ship without an allocation-pressure cliff.
- A property-test framework exists for the remaining stages.

---

## Stage 4 — Uniform STG-shape (Weeks 21-28)

### Goal

Eliminate emit-time eager/lazy asymmetries. Every binding is lazy by default in `lower.cc`; a strictness-analysis pass un-thunkifies where provably safe. The S5 / eager-inherit-from bug class becomes structurally impossible.

### Why now

The recurring bug cascade `#496 → #497 → #498 → #516 → #546 → #548 → #577 → #583` is one root cause manifesting under N labels: lower.cc makes per-construct decisions about thunkifying, and those decisions don't match TW's blanket `maybeThunk`. The fix is architectural: move the laziness decision out of `lower.cc`'s heuristics and into a separate optimizer pass that runs on the IR.

Doing this before Stage 5 matters because: V8 hidden classes assume that the same source position produces the same runtime shape. Today's per-construct thunkification means a single source position can produce a thunk *or* a forced value depending on context. That's incompatible with shape-keying.

**Empirical motivation (added 2026-05-18)**: the `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` decomposition of the 200× per-force gap on hello.drvPath identifies ~2-5× as coming from extra intermediate allocations (each binary op allocates an intermediate thunk-binding through the A-normal-form IR; Tag::App and Bindings cells from bytecode primops). A strictness pass that un-thunkifies provably-strict positions would eliminate a fraction of these allocations at lower time, reducing the load on Stage 3's nursery. **Strictness analysis is now load-bearing for closing the per-op gap, not just for STG-shape uniformity.**

### Prerequisites

- Stage 3 (nursery can absorb the 5-10× allocation spike).
- Action plan Phase 1 closed (iterative forceValue, so deep thunk chains don't blow the C-stack). ✅ MET 2026-05-18.

### TODOs

- [ ] **Audit `lower.cc` thunkification sites**. Find every place lower.cc decides "thunkify this or not." Document in `LOWERING_THUNKIFY_AUDIT_2026-XX-XX.md`. Expect 8-15 sites. (3 days.)
- [ ] **Audit `opt_strictness.cc`** (185 LoC). What does it currently identify? Likely partial — perhaps just "binding immediately followed by a force." Document its coverage. (1 day.)
- [ ] **Design the uniform lowering**. Emit ALL bindings as thunks by default. Have a single explicit "force this argument" annotation for known-strict primops (e.g. `OP_ADD` integer args). (4 days; one-page design doc.)
- [ ] **Expand strictness analysis**. The pass needs to identify:
  - Definitely-forced bindings (single force on a path from definition to use, no conditional).
  - Inlinable-once bindings (one use, no force conditional).
  - Loop-invariant bindings (forced in every iteration; safe to pre-force outside).
  Reference: GHC's strictness analyzer + the optimizer plan doc. (2 weeks.)
- [ ] **Wire strictness pass into `optimise()`** after const-folding, before primop-fuse. (1 day.)
- [ ] **Re-implement lowering** with uniform-thunk-default. (1 week.)
- [ ] **Lang test green**. All 142 must pass at every commit during this stage. (Ongoing.)
- [ ] **Real-world bench**. Expect: allocation rate up 5-10×, perf neutral or +/-15% on canonical workloads thanks to the nursery + strictness pass.
- [ ] **Retire all per-construct laziness env-var gates**: `NIX_V3_INHERIT_FROM_THUNK_ALL`, `NIX_V3_NO_INHERIT_FROM_THUNK_ALL`, `NIX_V3_INHERIT_FROM_THUNK_FILTER`, etc. (1 day, after the above lands.)
- [ ] **Retire the `OP_ATTRS_REC_INIT` split** (#546): if all bindings are uniform thunks, the rec-attrs-vs-let-in-body distinction collapses to a single opcode. (3 days.)
- [ ] **Close `CALLPACKAGE_BUG_2026-05-09.md`, `EVAL_ORDER_DIVERGENCE_2026-05-08.md`**, `CELL_UPDATE_EVERYWHERE_2026-05-12.md` with RESOLVED rows.

### Exit criteria

- All per-construct laziness env-var gates deleted (-10+ from the inventory).
- `opt_strictness.cc` is a documented pass with full IR coverage.
- The S5 / eager-inherit-from / cycle bug class is structurally impossible to reintroduce.
- Bench within 1.2× of TW on canonical workloads.
- All lang tests pass.

### Kill criterion

If uniform-thunk allocation overhead exceeds 2× TW even with Stage 3's nursery on, strictness analysis isn't pulling its weight. Diagnose: is the strictness pass missing patterns it should catch, or is allocation itself the bottleneck? If the former, expand the pass; if the latter, Stages 5-7 might need to ship before Stage 4 is considered done.

### What this stage unlocks

- Stage 5: shape tagging now has a uniform allocation path to key off.
- All cycle/blackhole work from Action Plan Phase 2 becomes structurally retire-able.
- The cleanest part of the codebase (the optimizer) becomes the most load-bearing.

---

## Stage 5 — Hidden classes / attrset shapes (Weeks 29-34) — ✗ KILLED 2026-05-23

> **✗ KILLED 2026-05-23 by Rule 0 — see `STAGE_5_6_KILLED_2026-05-23.md`**
>
> Phase L0 dispatch-budget spike (#778, commit `fe7c17498`) measured
> AttrSelect family at 2.24 % of dispatch on hello.drvPath. Kill
> threshold was ≥ 10 %. Wall-clock ceiling argument: even if a PIC
> made every AttrSelect free, max wall savings ≈ 26 ms on
> hello.drvPath — not multi-week-justifying. Stage 6 (PICs) implicitly
> killed (built on Stage 5). ~12 weeks of original calendar
> reclaimed. Revival conditions documented in the kill memo.
>
> Section content below is preserved as historical reference for the
> original design intent and to support revival measurement if a
> trigger fires.

### Goal

Attrsets carry a shape descriptor. Same source position produces the same shape across runs. Same shape → cacheable lookup paths. This is V8's foundational optimization.

### Why now

Strictness analysis (Stage 4) made allocation paths uniform. Now they can be tagged with shape. Without uniformity, shapes would fragment.

Doing this before Stage 6 matters because: PICs are the optimization that uses shapes. No shapes → no PIC.

### Prerequisites

- Stage 4 (uniform allocation paths).
- **NEW (2026-05-17, post-Agent-1-review)**: Unison ABT refactor (de Bruijn `(depth, index)` identity per UNISON_IDEAS Item 2) MUST land before this stage. Shape interning and PIC keys are unstable across sessions without alpha-equivalent identity. This is also Stage 9 (linking) Phase L1; if Stage 9 runs ahead of Stage 5, L1 satisfies this prerequisite naturally. The 1-week refactor unlocks both shape stability AND disk-cache cross-file sharing.

### TODOs

- [ ] **Shape representation design doc** (`SHAPE_DESIGN_2026-XX-XX.md`). Options:
  - Interned name-set (sorted vector of `Symbol*`, hash-consed). Simplest. Probably right.
  - Transition tree (V8-style "hidden class chain"). More complex, supports incremental addition.
  - Bloom filter + name-set. Cheap "shape miss" detection.
  Pick one. (3 days.)
- [ ] **`Shape*` table + interning**. Global hash-consed table; each `Shape` has a canonical pointer. (3 days.)
- [ ] **Attrset cell carries a `Shape*`** (or shape ID, 32-bit interned index). Modify `Bindings` / attrset representation. (1 week.)
- [ ] **`OP_ATTRS_BUILD` emits shape-tagged attrsets**. At each emit site, compute the shape at compile time and bake it in. (3 days.)
- [ ] **`OP_ATTRS_SELECT` shape capture**. At first execution of each SELECT site, record the observed `(shape_id, slot_index)` in an inline cache slot tied to the bytecode position. Do NOT yet act on the cache — just observe. (3 days.)
- [ ] **Telemetry pass**. After 1 week of usage in development, query: what fraction of OP_ATTRS_SELECT sites observe ≤2 shapes? ≤4? Megamorphic (>10)? This validates whether shapes have predictive power. (1 day.)
- [ ] **Bench**. Should be near-neutral; if it's a regression, shape representation is too heavy. Tune. (2-3 days.)

### Exit criteria

- Every attrset has a shape descriptor.
- 95%+ of OP_ATTRS_SELECT sites observe ≤2 shapes on real workloads (validates predictive value).
- Bench within ±5% of Stage 4 baseline (no-op overhead until Stage 6 lights it up).

### Kill criterion

If attrsets at the same source position routinely produce 10+ distinct shapes, the shape system isn't capturing the V8-applicable pattern. Options:
- Redesign shape with name-subset matching (allow shape A to "match" shape B if A ⊂ B).
- Skip Stage 6 (PICs) and go directly to Stage 7 + 8.
- Abandon V8-style optimization for Nix; declare Stage 5+6+7 not-applicable; pivot to direct-threading dispatch instead.

### What this stage unlocks

- Stage 6: PICs have a cache key.
- Stage 7: selector thunks can determine sharing equivalence via shape.

---

## Stage 6 — Polymorphic Inline Caches (Weeks 35-40) — ✗ IMPLICITLY KILLED 2026-05-23

> **✗ IMPLICITLY KILLED 2026-05-23 — see `STAGE_5_6_KILLED_2026-05-23.md`**
>
> Stage 6 was built on Stage 5's shape system; PIC cache entries were
> to be keyed on shape identity. With Stage 5 killed (AttrSelect = 2.24 %
> of dispatch, below 10 % threshold), Stage 6 has no key substrate.
> Even a redesigned shape-free PIC would inherit the same wall-clock
> ceiling argument: 2.24 % is the ceiling for any OP_ATTRS_SELECT
> optimisation. ~6 weeks of original calendar reclaimed.
>
> Section content below is preserved as historical reference.

### Goal

`OP_ATTRS_SELECT` and `OP_CALL` hot paths cache `(shape → slot)` and `(closure → entry)` at the bytecode site. Cache invalidation on shape change.

### Why now

This is where the V8-style perf win materializes. Without it, Stage 5 is observation-only overhead.

### Prerequisites

- Stage 5.

### TODOs

- [ ] **`OP_ATTRS_SELECT` 1-PIC**: each select site has one cache slot `(Shape*, slot_index)`. Fast path: check shape pointer equality; on hit, direct slot access. On miss, fall back to dictionary lookup + update cache. (1 week.)
- [ ] **2-PIC**: each select site has two cache slots. Switch to 2-PIC when first-slot miss happens on a recurring second shape. (3 days.)
- [ ] **Megamorphic fallback**: after 5+ misses across distinct shapes, mark the site megamorphic; fall back to dictionary lookup permanently. (1 day.)
- [ ] **`OP_CALL` PIC**: cache `(closure_shape → fast_path)` at callsite. Closure shape is encoded by lambda position + capture set. (1 week.)
- [ ] **Cache invalidation**: when a shape is retired (rare; only on shape table garbage-collection), invalidate dependent PICs. Initially: don't GC shapes (they're cheap). Address only if shape table grows unbounded. (1 day to defer.)
- [ ] **Bench measurement**. This is THE perf checkpoint. Expected: 1.5-2× on attribute-heavy workloads (lib-evalModules, real nixpkgs lookups). (1 week of bench + tune.)

### Exit criteria

- Bench shows ≥30% improvement on `lib-evalModules-100` vs Stage 5 baseline.
- PIC hit rate ≥85% on real nixpkgs.
- vm.cc opcode dispatch for ATTRS_SELECT and CALL has a clean fast-path/slow-path split.

### Kill criterion

If PICs add <15% perf even at 85% hit rate, dispatch overhead in `vm.cc` is the dominant cost, not attribute-lookup overhead. Insert a **Stage 6.5** (direct threading / computed-goto opcode dispatch) before continuing.

### What this stage unlocks

- The V8-style perf win is realized.
- Stage 7's selector thunks can use shape sharing as evidence of common-subexpression-ness.

---

## Stage 7 — Selector thunks (Weeks 41-44)

### Goal

`inherit (a) b; inherit (a) c` shares the force of `a`. Repeated attribute-path access shares work. The pattern is structurally recognized by the optimizer — no nixpkgs-specific heuristics.

### Why now

Selector thunks are a known-good optimization for Nix workloads (cppnix has them; some Nix forks implement them). They depend on identifying sharing opportunities; the shape system (Stages 5-6) provides the mechanism.

This is the smallest stage — ~3 weeks. It's the natural cleanup after PICs.

### Prerequisites

- Stages 4, 5 (shape system identifies sharing equivalence).

### TODOs

- [ ] **Detection**. `lower.cc` identifies `inherit (X) a, b, c, ...` patterns. (2 days.)
- [ ] **`OP_SELECTOR_THUNK` opcode**. Wraps a target expression + a list of selector names. (3 days.)
- [ ] **Force protocol**. Forcing a selector thunk forces the target once; subsequent selections become attrset SELECT via the cached shape. (3 days.)
- [ ] **Lang tests + perf bench**. (1 week.)
- [ ] **Extend to attribute-path repetition**: `let p = a.b.c; in p + p` is the same opportunity. `opt_cse.cc` may already handle it; verify. (3 days.)

### Exit criteria

- Bench shows ≥10% on `inherit (X) a b c d`-heavy workloads (some lib/* modules; texlive).
- Pattern is recognized structurally; no `texlive`-specific or nixpkgs-specific heuristics.

### What this stage unlocks

- All stated optimization layers from the vision are in place.

---

## Stage 9 — Module linking (content-addressed cells) — ✗ KILLED 2026-05-22

> **✗ KILLED 2026-05-22 by Rule 0 — see `STAGE_9_KILLED_2026-05-22.md`**
>
> Phase L0 bytecode-dedup spike (#772, commit `37616ecc6`) measured
> 1.17 × function-level / 1.03-1.07 × byte-level lower-bound dedup on
> hello.drvPath + cardano-node M5. Kill threshold was < 2 ×.
> Decisively below. Per-thunk-body content-addressed cell store
> hypothesis falsified. ~5 weeks of L1-L4 work reclaimed. `LINKING_DESIGN_2026-05-17.md`
> superseded. ABT refactor (#773) demoted from Stage-5/Stage-9 prereq
> to dormant-pending-Unison-Item-3/4-or-lint-Phase-5. Revival
> conditions documented in the kill memo (coarser-granularity
> measurement is the most plausible path; ABT-level re-measurement is
> the secondary path).
>
> Section content below is preserved as historical reference for the
> original design intent and to support revival measurement if a
> trigger fires.

Added 2026-05-17 after Agent-2 design review.

### Goal

Replace v3's per-file SHA disk cache with a content-addressed cell store at thunk-body granularity. Each .nix file becomes a `ModuleManifest` listing constituent cell BLAKE3 hashes plus an entry-point reference. Modules sharing identical thunk bodies share cells. Symbolic primop resolution at module-load (ELF GOT/PLT style).

### Why

LESSONS_LEARNED §4.1 documents 5 000+ replicated callPackage closures in nixpkgs. Status quo per-file SHA caches miss this entirely. Unison-style content-addressed cells exploit it directly.

This stage also lands the ABT alpha-equivalent identity refactor (UNISON_IDEAS Item 2), which is the prerequisite for stable shape interning in Stage 5.

### Full design

See `LINKING_DESIGN_2026-05-17.md` for the complete design proposal:
- Unit of content-addressing = thunk-body (each `ir::MkThunk`/`MkClosure`)
- On-disk: `Modules` table (manifest) + `Cells` table (content-addressed, shared)
- Eval-time flow for `primImport`
- Builtins/primop symbolic resolution
- NIX_PATH / `<angle>` / scopedImport / IFD handling
- Position metadata across shared cells
- Open questions (hash function, hash IR pre-or-post-opt)

### TODOs (5 phases, ~5 weeks; each falsifies a hypothesis per Rule 0)

- [ ] **Phase L0** (1 wk, ~400 LoC): `structuralHash()` on IR nodes alongside `computeFreeVars()`. Falsifies "fragment hashing collides at acceptable rate." Verify via nixpkgs dedup ratio survey.
- [ ] **Phase L1** (1 wk, ~300 LoC): de Bruijn ABT identity in IR. Falsifies "alpha-equivalence enables cell sharing." This is also a Stage 5 prerequisite.
- [ ] **Phase L2** (2 wk, ~600 LoC): Schema bump to v9 — add `Cells` table, ModuleManifest in `Modules`. Falsifies "cells round-trip faithfully under concurrent insertion."
- [ ] **Phase L3** (1 wk, ~300 LoC): Migrate `primImport` to manifest+cell flow behind `NIX_V3_LINK=1`; default-on after parity confirmed.
- [ ] **Phase L4** (1 wk): `nix v3-inspect cell <hash>` CLI (UNISON_IDEAS Item 5).

### Exit criteria

- `disk_cache.cc` reads/writes the new schema.
- nixpkgs eval shows >5× cell dedup vs whole-file cache.
- ABT identity lands; Stage 5 unblocked.

### Kill criterion

If L0's nixpkgs dedup survey shows <2× collapse, the per-thunk-body granularity hypothesis is wrong; abandon Stage 9 and revisit at the whole-`ExprAttrs`-or-`ExprLet`-bindings level.

### Scheduling note

Stage 9 can run in parallel with Stages 2-4. L1 (ABT refactor) should land before Stage 5 begins regardless of ordering.

---

## Stage 8 — Thin FFI surface + primops classification (parallel, Weeks 9-44)

> **2026-05-20 audit landed**: see `FFI_AUDIT_2026-05-20.md` for the full inventory — 104 static `treeWalkerToV3`/`v3ToTreeWalker` call sites, 109 primop wrappers, 6 distinct TW dependency mechanisms. Identifies 4 tiers of migration: **Tier 0** (system-info primops as injected constants — ~1-2 days), **Tier 1** (Stage 2/3/9 architectural — sequenced here), **Tier 2** (bytecode-install more callback primops — ~1-2 weeks), **Tier 3** (opcode-ify pure arithmetic / string primops — ~1 week). Plus the **V3_DBG_TW_CROSS measurement spike** (~1 day) to convert static counts to dynamic per-eval counts — prerequisite for prioritizing Tier 2 vs Tier 3.

### Goal correction (important)

The goal is NOT "shrink primops.cc to <3 000 LoC." That was a misframing in the initial roadmap draft. **v3-native primops are architecturally correct** because:

1. **GC ownership.** v3 Values live in v3's nursery (or whatever v3 allocator); TW Values live in Boehm-managed memory. Marshalling between them requires copy or wrapping at every primop call boundary.
2. **Hot-path cost.** `primMap`, `primFilter`, `primAttrNames`, `primFoldlPrime`, `primConcatMap` etc. are called millions of times in real nixpkgs eval. Each call materializing v3 List/Bindings → TW List/Bindings → v3 List/Bindings would dominate eval time.
3. **GC safety.** Crossing the boundary mid-eval means a v3 GC scavenge cannot safely move v3 Values that are temporarily held by a TW primop, and a Boehm collection cannot safely move TW Values held by a v3 primop. Either marshall-by-copy (slow) or pin (correctness hazard).

Therefore the FFI surface is for **system boundaries**, not for pure data ops:

**FFI-bridged** (talk to cppnix):
- Store operations (deriving, addToStore, paths, IFD).
- File I/O (`readFile`, `readDir`, `findFile`).
- Process / network primitives (`fetchurl`, `fetchTarball`, etc., to the extent they survive in modern Nix).
- Path normalization and store-path validation.
- Eval-state operations that need cppnix's parser/state (`builtins.fromJSON`, `import` at runtime, etc.).
- Symbol/Name interning shared with cppnix where mutual visibility is required.

**v3-native primops** (stay v3-native):
- All pure list ops: `map`, `filter`, `foldl'`, `head`, `tail`, `length`, `elemAt`, `concatMap`, `genList`, `partition`, `groupBy`.
- All pure attrset ops: `attrNames`, `attrValues`, `hasAttr`, `getAttr`, `mapAttrs`, `listToAttrs`, `catAttrs`.
- All pure string ops: `split`, `replaceStrings`, `substring`, `stringLength`, `toString`, `concatStringsSep`.
- All arithmetic + comparison.
- `__toString` dispatch, `<-?`, `//`, `++`, `+` over strings/paths.
- Anything that takes only `Value*` (or v3 Cells/Bindings/Lists) and returns `Value*` of pure-data type.

### Why parallel

This stage is cleanup work distributed across Stages 2-7. It doesn't gate any stage but each stage has natural checkpoints to land FFI-clarification commits.

### TODOs (distributed)

- [ ] **(Stage 2)** Bridge audit + reduction (already in Stage 2 TODOs). The target here is removing `bridge_yield.cc` and the ~370 LoC bridge plumbing in vm.cc — these are v3-can't-do-it ESCAPE HATCHES, not the same thing as FFI primops.
- [ ] **(Stage 2)** Document the FFI surface in one `.hh` file (`ffi.hh` already exists; promote it to canonical). Lists every cppnix entry point v3 calls. (1 day.)
- [ ] **(Stage 3)** **Primops classification audit**. Classify every entry in `primops.cc` into (a) v3-native pure data op (stays), (b) FFI-bridged system primitive (stays, may move to a thin wrapper around `ffi.cc`), (c) duplicates work from cppnix that should be FFI-bridged. Document in `PRIMOPS_CLASSIFICATION_2026-XX-XX.md`. (4 days.) Expected: ~80% category (a), ~15% category (b), ~5% category (c).
- [ ] **(Stage 3-4)** For category (c) — actual duplication: replace with FFI thin wrapper. These are typically things that already require cppnix state (e.g. `builtins.fromJSON` uses a JSON parser cppnix has). Do NOT do this for pure ops just because cppnix has its own implementation.
- [ ] **(Stage 4)** Consolidate inline-opcode primop duplication. `primHead`, `primTail`, `primLength`, `primElemAt` are re-implemented inline in vm.cc OP_HEAD/TAIL/LENGTH/ELEM_AT. Factor to shared inline functions in a v3-internal header, OR accept duplication with explicit `// keep in sync with primops.cc:NNN` anchors. (2 days.) This is D4b in the scorecard.
- [ ] **(Stage 5-6)** Shape-system migrations: as shapes mature, primops that take attrsets (`attrNames`, `mapAttrs`, etc.) can use the shape descriptor for fast iteration instead of walking the cell. Migrate them. (3-5 days per primop, distributed.)
- [ ] **(Stage 7)** Final FFI surface review. `ffi.hh` should describe a stable, minimal surface. Documented contract. (2 days.)

### Exit criteria (at end of Stage 8, ≈Week 44)

- `ffi.hh` is the documented FFI surface (one canonical header listing every cppnix entry point v3 calls).
- `bridge_yield.cc` deleted.
- vm.cc bridge plumbing <100 LoC.
- Every entry in `primops.cc` is classified as native vs FFI; classification is in-source as a doc comment.
- Category (c) duplication eliminated (expected delta to primops.cc: ~500-1 000 LoC removed; final size likely ~7 000-7 800 LoC and that is fine).
- No v3 file has more than 100 lines of FFI plumbing (excluding `primops.cc` itself, where v3-native primops are correct and intentional).

---

## Stage 14 — Error-message UX (committed; post-perf + post-IFD)

Added 2026-05-20. **COMMITTED** stage (unlike candidates 10-13 below); detailed design + prior-art synthesis in `ERROR_UX_DESIGN_2026-05-20.md`.

### Goal

Make v3's error messages strictly better than cppnix's. Today v3 inherits cppnix's bad error UX (notoriously terse, stack-trace-dominated, no source spans on intermediates, no "did you mean" suggestions, opaque infinite-recursion errors). The current TW-parity sprint (#677-#681) correctly matches cppnix byte-for-byte where consumers depend on it; Stage 14 produces strictly-better output where they don't, with `--error-format=cppnix-compat` for legacy.

### Why this is committed, not a candidate

Unlike Stages 10-13 (salsa, HAMT, JIT, multi-core) which are measurement-gated, error UX is **observably bad today** with no measurement spike needed. The three before/after examples in `ERROR_UX_DESIGN_2026-05-20.md` §6 are concrete user-visible improvements. The case rests on existing UX evidence (GitHub issues #963 / #9636 / #6361 / #7552 / #7553 / #239351 + Discourse threads), not on hypothesis.

### Why this lands AFTER perf + IFD (the user's sequencing)

Two reasons:

1. **Engineering bandwidth**: this is ~5-9 weeks of focused work that competes with Stages 3-7 (perf closure) and Pillar 2 (cardano-node + IFD). Doing all three simultaneously dilutes velocity. Sequence: close perf → land IFD/cardano → tackle error UX.

2. **Stage 14's value is unlocked by Pillars 1 + 2 being done**: a beautiful error message on a slow eval is still slow; a beautiful error message on a v3 that can't evaluate cardano-node is incomplete. Stage 14 polishes a v3 that's already perf-competitive AND IFD-capable — that's when the "strictly better than cppnix" story closes.

### Prerequisites (must hold before Stage 14 starts)

- **Pillar 1 perf closure**: Stages 3-7 substantially complete; ROADMAP end-state target `≤1.3× TW on lib-evalModules-100` met OR demonstrably within reach.
- **Pillar 2 IFD support**: `CARDANO_NODE_FEASIBILITY_2026-05-18.md` Phase G.1-G.3 landed (getFlake bridge + IFD via realisePath + cardano-node M5 attempt complete with documented outcome).
- **Action plan Phase 4 (vm.cc decomp)** at least started — Stage 14's structured `Diagnostic` refactor touches ~50 throw sites; cleaner against a decomposed vm.cc than a 10K+ LoC monolith.

### Phased work (per `ERROR_UX_DESIGN_2026-05-20.md` §7)

| Phase | Effort (realistic) | Sub-tasks |
|---|---|---|
| **A — Foundation** | 4-5 weeks | Structured `Diagnostic` value (code, primary span, related spans, notes, suggestions); `--error-format=json` CLI flag; typed error hierarchy (`TypeError`, `AttrNotFoundError`, `ArgumentMismatchError`, `CoercionError`); Levenshtein typo correction + parent-attrset preview at attribute-miss; two-span lazy-eval errors (force-site + binding-site, à la Tvix `WithSpan`) |
| **B — Compounding wins** | 3-4 weeks | Error code corpus (`N0001`-`N0999` reserved ranges); `nix --explain Nxxxx` CLI subcommand + Markdown corpus; `addErrorContext` shown by default (per nix#7553); smart trace summarisation (collapse module-system frames); `nix eval` derivation short-circuit (per Discourse #14339) |
| **C — Deferred** | 3-4 weeks | PEP 657-style sub-expression spans; PEP 678 `__notes__`-style propagation; LSP integration via JSON output; snapshot-test corpus à la Elm's `error-message-catalog` |

### Exit criteria

- Three before/after examples from `ERROR_UX_DESIGN_2026-05-20.md` §6 produce the proposed v3 output (attribute-miss with Levenshtein; coercion with fix suggestions; infinite recursion with force chain).
- Top-10 most-cited bad-error GitHub issues from cppnix have v3 outputs that resolve the complaint.
- `--error-format=cppnix-compat` legacy mode passes string-match tests in nixpkgs CI unchanged.
- `--error-format=json` is stable enough for an LSP plugin to consume.
- 25-50 error codes (`N0xxx`) in the corpus with `nix --explain` text.

### Kill criterion

If Phase A delivers but no measurable shift in user reports / Discourse complaints over 3 months: the technical wins didn't translate to UX improvement. Pause Phase B; investigate where the gap is (probably: terminal rendering, color, hierarchy needing more design polish).

### Rule 0 framing

This stage kills the hypothesis "Nix error messages must be cppnix-shaped." Falsified by producing strictly better output for the same inputs while maintaining `--error-format=cppnix-compat` for legacy consumers.

### Architectural compatibility check

- Phase D singleton-closure intern + Stage 5/6 PIC key (the risk flagged 2026-05-18): **independent of Stage 14**.
- Cardano-node FFI bridge work: **complementary** — better error UX is most valuable on real workloads like cardano-node.
- ABT identity refactor (Stage 9 Phase L1): **independent** but the structured `Diagnostic` design should follow the same content-addressed-keying philosophy where possible.

### Self-critique

- Effort estimate (~7-9 weeks realistic) is conservative; if the team's observed velocity holds (~3-5× original plan estimates), this could be 3-4 weeks.
- The "do better than cppnix" framing risks scope creep into "compete with rustc" which is unrealistic. Phase A's bar should be "noticeably better than today" not "best-in-class."
- Migration of ~50 throw sites to typed errors should be incremental over PRs, not one big-bang refactor — each improvement ships independently with its own positive + negative + regression tests.

### Cross-references

- Full design: `ERROR_UX_DESIGN_2026-05-20.md` (10 sections, 3 worked before/after examples, prior-art tour, source bibliography).
- Tvix's `WithSpan` precedent: https://docs.tvix.dev/rust/tvix_eval/vm/trait.WithSpan.html
- Lix 2.92 release notes (parent-content preview + caret-on-failing-component): https://docs.lix.systems/manual/lix/nightly/release-notes/rl-2.92.html
- Prior-art bibliography: see `ERROR_UX_DESIGN_2026-05-20.md` §10.

---

## Stage 15 — Per-line profiler UX (committed; UX pillar)

Added 2026-05-21. **COMMITTED** stage; full design in `NIX_PROFILER_DESIGN_2026-05-21.md`.

### Goal

Ship an Xcode/Instruments-style per-line / per-token CPU + memory attribution
tool for Nix evaluation, so users can visually see where a complex flake
(e.g. `github:IntersectMBO/cardano-node`) spends its time and memory.
Visualisation via two off-the-shelf web viewers (`go tool pprof -http` for
annotated-source heatmap, `profiler.firefox.com` for flame graph + memory
track + share-by-URL + diff TW-vs-v3) — no custom rendering code in the MVP.

### Why this is committed, not a candidate

The data substrate already exists in v3 at ~70%: `posSnapshotPool`,
`LambdaDescriptor::posHandle` / `allocCount` / `forceCount` / `callCount`,
`attrPosTable`, `V3_DBG_ALLOC_DUMP`, `NIX_TRACE_EVAL`. The missing pieces
are small (per-IP source map, safe-point sampler, per-position alloc counters)
and well-scoped to ~5 days. The output formats (pprof + Firefox Profiler JSON)
are stable industry standards with mature free viewers. No measurement spike
needed: the user-visible value ("where is time spent?") is concrete, not
hypothetical.

### Why this lands where it does (UX pillar, after Stage 14)

Three reasons for the sequencing:

1. **Stage 14 (error UX) lands first because debugging a stuck eval needs
   good error messages before profiling matters.** A profile of an eval
   that errors out halfway is less useful than fixing the error message.

2. **The profiler ITSELF is enabling infrastructure for earlier stages**
   (it instruments Phase 1.5's "drvPath force-rate decomposition" TODO),
   so the *data-layer* part lands during Phase 1.5 / ACTION_PLAN
   regardless of whether the full UX ships then.

   In other words: **Phase 0+1 of Stage 15 (the data layer + pprof exporter,
   ~6 days) lands during ACTION_PLAN Phase 1.5 because it instruments the
   measurement spike.** Only the polished HTML report (Phase 4, optional)
   actually sits at the Stage 15 slot in the timeline.

3. **Stage 15 polishes a v3 that's already perf-competitive (post Stage 7) AND
   IFD-capable (post-cardano-node feasibility)** — that's when "easily see
   where I spend time on cardano-node" becomes a believable user story.

### Prerequisites (must hold before Stage 15's UX work starts)

- **Data layer landed** during ACTION_PLAN Phase 1.5 (per `NIX_PROFILER_DESIGN_2026-05-21.md` §5 Phase 0-2). The pprof + Firefox Profiler exporters
  must work end-to-end against `hello.name` and `lib-evalModules-100` before
  Stage 15's UX work begins.
- **Stage 7 substantially complete** OR demonstrably within reach. Profiling
  a 30× slower-than-TW eval is more confusing than illuminating; the
  workloads need to be at-parity for the profile to surface insights about
  the user's code rather than v3 overhead.
- **Cardano-node IFD support landed** (`CARDANO_NODE_FEASIBILITY_2026-05-18.md`
  Phase G.1-G.3). Profiling a fake-store fallback path attributes time to
  the wrong place.

### Phased work (per `NIX_PROFILER_DESIGN_2026-05-21.md` §5)

| Phase | When | Effort | Sub-tasks |
|---|---|---|---|
| **0 — JSONL contract** | landed during ACTION_PLAN Phase 1.5 | 0.5 day | Document JSONL intermediate format; stub `nix::v3::profile` API |
| **1 — Data layer** | landed during Phase 1.5 | 5 days | `ir::Binding::pos`; `cu.codePosMap`; safe-point sampler in `vm.cc:2307`; per-position alloc counters with per-poll aggregation; `profile.cc` modelled on `heap_trace.cc` |
| **2 — Exporters** | landed during Phase 1.5 | 2 days | Python `bench/prof.py`: `export-pprof` (~150 LoC text-format; protobuf later) + `export-firefox` (~200 LoC Firefox Profiler JSON) + free `export-speedscope` + `export-csv` |
| **3 — Docs** | landed during Phase 1.5 | 0.5 day | `USAGE.md` entry + walkthrough |
| **4 — Custom HTML report** | Stage 15 slot proper (post-Stage 14) | 2-3 weeks | Self-contained HTML with canvas flame graph + Instruments-style dual-gutter source view (CPU + alloc bars per line) + three lineage modes (Force-site / Alloc-site / Retention) + flake-input-aware framework blackboxing + bidirectional drill-down between panes |
| **5 — Differential view** | Stage 15.1 | 3 days | Compare two profiles (TW vs v3, or before vs after-PR); paired bars + ±Δ gutter |
| **6 — VSCode extension** | Stage 15.2 (deferred) | 1-2 weeks | CodeLens overlays per line consuming the same JSONL |

### Exit criteria

- `NIX_V3_PROFILE=/tmp/p.jsonl nix eval github:IntersectMBO/cardano-node#...` produces a JSONL trace.
- `bench/prof.py export-pprof /tmp/p.jsonl /tmp/p.pb && go tool pprof -http=:8080 /tmp/p.pb` opens a browser tab with per-line annotated source view of the hottest 10 Nix positions for cardano-node within ~10 seconds of post-processing.
- `bench/prof.py export-firefox /tmp/p.jsonl /tmp/p.json` produces a file loadable by `profiler.firefox.com/from-file` showing flame graph + memory track.
- ≥ 90% of cardano-node eval wall time is correctly attributed to specific Nix source positions (remaining ≤ 10% = bridge-to-TW time attributed to the v3 calling position).
- Profiler-OFF overhead ≤ 0.1% (one predicted-not-taken branch per safe-point poll + one per alloc).
- Profiler-ON overhead ≤ 2% on `hello.name`, ≤ 5% on cardano-node-scale.
- Phase 4 only: self-contained HTML report loads a 50-200 MB JSONL in ≤ 5 seconds and renders ≤ 60 fps on a modern laptop browser.

### Kill criterion

If Phase 4 (custom HTML report) effort exceeds 4 weeks, OR if the off-the-shelf
viewers (pprof + Firefox Profiler) prove sufficient for the team's actual
workflow after 1 month of MVP use, skip Phase 4 entirely. Phases 0-3 (data
layer + exporters) are unconditional; Phase 4 is the optional polish.

If Phase 0-1 (data layer) exceeds 2× budget (10+ days instead of 5.5),
the per-IP source map approach is wrong; pivot to a sparse per-Function map
+ ip-to-Function map (loses per-line precision but ships).

### Rule 0 framing

This stage kills two hypotheses:

1. **"Nix users need a custom Instruments package on macOS to get
   per-line attribution"** — falsified by shipping a Firefox Profiler JSON
   exporter that gives the same UX cross-platform.
2. **"Per-line attribution requires deterministic instrumentation of every
   force"** — falsified by safe-point sampling at the dispatch poll with
   per-poll allocation attribution, achieving ≤ 2% overhead.

If either claim fails (the user community demands native Instruments OR
overhead exceeds budget), the stage is wrong-shape and we revisit.

### Architectural compatibility check

- **Stage 3 (nursery default-on)**: profiler instruments allocation-by-position;
  the alloc-site lineage mode surfaces which positions allocate the most
  intermediate thunks — direct input to Stage 4's strictness pass.
- **Stage 4 (uniform STG-shape)**: profile output makes the "before vs after
  strictness" comparison legible at the source-position level.
- **Stage 5/6 (hidden classes / PICs)**: call-site profile data identifies the
  OP_CALL sites worth specialising.
- **Stage 7 (selector thunks)**: profile flags hot selector positions.
- **Stage 8 (thin FFI)**: bridge-to-TW time attribution lets us prioritise
  which TW fallback sites to move into v3.
- **Stage 14 (error UX)**: independent; both stages improve developer-facing
  UX along orthogonal axes.
- **Stage 17 (pattern lint UX)**: shares the position-attribution substrate
  and the LintRegistry that Phase 1 of Stage 17 builds. Both stages plug
  into the same registry — profiler emits "where time went", lint emits
  "where pattern fired". Phase 0-3 of this stage (data layer + pprof
  exporter) should align with Phase 1-2 of Stage 17 (LintRegistry + trace-
  driven seed) so the shared instrumentation lands once, not twice.

### Self-critique

- Effort estimate (8 days MVP + 2-3 weeks Phase 4) is realistic given the
  team's measured ~3-5× velocity ratio against original estimates. If the
  pattern holds, MVP could ship in 3-4 days.
- The "Phase 4 is optional" framing risks Phase 4 being deferred forever
  while the MVP delivers 80% of the value. That's actually the correct
  outcome — the off-the-shelf viewers are mature; building a custom HTML
  report only to match their feature set is wasted effort. Phase 4 should
  ship only if a Nix-specific feature (flake-input-aware blackboxing,
  alloc-site vs force-site toggle) becomes the decisive UX win.
- The pprof text-format exporter is faster to ship than protobuf but loses
  the binary efficiency benefit. Migrate to protobuf once the schema is stable.
- A custom Instruments package was deliberately dropped (`os_signpost`
  rate-limit + macOS-only + 2-week effort). If a user case ever requires it,
  add as Stage 15.3.

### Cross-references

- Full design: `NIX_PROFILER_DESIGN_2026-05-21.md` (11 sections; data layer, output formats, visualization UX, phased implementation, overhead budget, falsifiers).
- Complementary external sampler: `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` — gives time-series CPU%/RSS/heap (when time was spent). This profiler gives source attribution (where in source time was spent). Both should land before Phase 1.5 closes.
- ACTION_PLAN Phase 1.5 TODOs that this profiler enables: "drvPath/outPath force-rate decomposition profile" (factors b and c).
- Tree-walker side prior art: `src/libexpr/eval-profiler.cc:121-181` (`SampleStack` — folded-stack format). v3's pprof / Firefox emitters sit alongside; outputs can be loaded into the same viewer for TW-vs-v3 comparison.
- Firefox Profiler format spec: https://github.com/firefox-devtools/profiler/blob/main/src/types/profile.ts
- pprof format spec: https://github.com/google/pprof/blob/main/proto/profile.proto
- Speedscope format spec: https://github.com/jlfwong/speedscope/blob/main/src/lib/file-format-spec.ts

---

## Stage 17 — Pattern-lint UX (committed; UX pillar)

Added 2026-05-22. **COMMITTED** stage; full design in
`LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md`. Motivated by the #757
case (haskell.nix's `composeExtensions` consumer-side `.extend`
pattern legitimately producing 4096-deep `prev` slot chains —
the user knew none of this and the VM had no way to tell them).

### Goal

Ship `v3-lint`, a pathological-pattern detection tool with three
modes operating against a single 27-rule catalog:

- **Mode 1 — syntactic** (statix-territory; rnix/v3 parser).
  Catches v3-specific syntactic rules existing Nix tools don't:
  missing `forceStringNoCtx` audit, `std::runtime_error` arithmetic
  throws, IFD-smell shapes (`import (drv)`).
- **Mode 2 — IR-level** (post-lower; in `opt_lint.cc`). **Unique to
  v3 in the Nix ecosystem.** Catches patterns that emerge only
  after substitution / inlining / fix-point: M^N exponential
  lowering, deep `composeExtensions` chains, eager-on-lazy-binding.
- **Mode 3 — trace-driven** (existing diagnostic hooks emit findings
  via unified registry). **Unique to v3 in the Nix ecosystem.**
  Surfaces runtime pathologies (slot-chain depth, hot-loop re-forcing,
  attrset-allocation explosions, IFD output over-forcing) as they
  fire, with SARIF / JSON / text outputs.

Three user asks from the originating discussion:
- (a) "users know they happen" → Mode 3
- (b) "context to fix" → ShellCheck-style numbered codes +
  positions + explain links + suggested rewrites
- (c) "hlint or shellcheck for Nix" → Modes 1 + 2

### Why this is committed, not a candidate

The substrate already exists: **12+ existing diagnostic hooks** in
v3 today observe exactly the pathologies the rules need to detect
(slot-chain chase #757, hot-loop re-forcing, attrset histogram,
IFD profiling, resource-limit watchdogs). Position attribution is
mature (`resolvePosSnapshot` O(1) ~20 B/handle). The 27-rule
catalog is **derived from actual debugging** — every entry is a
real bug or pathology the team already root-caused once. No
measurement spike needed; the value proposition is concrete, not
hypothetical.

The gap analysis is decisive: of statix / deadnix / nil / nixd /
nixf-tidy / vulnix / nix-linter, **none is trace-driven, IR-level,
or IFD-aware.** Three rows of capability that no existing Nix
linter offers; v3-lint occupies a genuinely new niche.

### Why this lands where it does (UX pillar, after Stage 15)

Four reasons for the sequencing:

1. **Phase 1 (LintRegistry / hook unification) lands during the
   action plan's env-var hygiene cadence**, not at the Stage 17
   slot. The 70+ scattered `getenv()` gates already exist; folding
   them into one registry is plumbing that benefits Stages 2-15.
   In other words: **Phase 1 of Stage 17 (the registry, ~1 week)
   lands across ACTION_PLAN Phase 1.5+ as part of standing env-var
   cadence.** Only Phases 4-5 (the static-analysis modes) sit at
   the Stage 17 slot proper.

2. **Stage 14 (error UX) and Stage 15 (profiler UX) land first.**
   Lint is forward-looking guidance ("you could write this faster"),
   not retrospective diagnosis ("you wrote this wrong"). Users
   debugging stuck evals need clear errors (14) and time
   attribution (15) before pre-emptive hints add value.

3. **Stage 17 polishes a v3 that's already perf-competitive (post
   Stage 7) AND IFD-capable** — that's when users care about
   "could I write this faster?" rather than "why is this slow at
   all?".

4. **Mode 2 (IR-level) benefits from Stage 5 (shapes) and Stage 6
   (PICs)** because shape stability gives the IR pass better
   precision on which call sites are speculation-worthy. Shipping
   Mode 2 before shapes lands would mean re-tuning the rules
   afterward.

### Prerequisites (must hold before Stage 17's UX work starts)

- **Phase 1 (LintRegistry) landed** during ACTION_PLAN Phase 1.5+
  cadence (per `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` §8 Phase 1).
  This is the substrate that all later phases plug into.
- **Stage 14 (error UX) substantially complete.** Lint findings
  share the diagnostic format with errors; structured `Diagnostic`
  must exist before lint can emit through it.
- **Stage 15 Phase 0-3 landed** (data layer + pprof exporter).
  Profiler and lint share the position-attribution substrate; the
  per-IP source map must work before lint's runtime emission has
  precise position info.
- **Stage 8 (FFI) Tier-1 classification of primops** AND, if it
  ships, Unison Item 4 effect propagation (see
  `IFD_DEEP_DIVE_2026-05-21.md` §11). Effect-typed primops let
  Mode 2 IFD rules (V0014/V0015) graduate from heuristic to
  precise.
- **No `Stage 14`/`15` regression**: the diagnostic emission
  pipeline used by lint must not silently break errors or
  profiles. Add an integration test pairing all three sources.

### Phased work (per `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` §8)

| Phase | When | Effort | Sub-tasks |
|---|---|---|---|
| **1 — LintRegistry / hook unification** | landed during ACTION_PLAN Phase 1.5+ cadence | 1 wk | Single `LintRegistry` + `lintEmit(ruleId, pos, ctx)`; fold 70+ `getenv` gates into the registry; no new rules |
| **2 — Trace-driven seed** | landed during Phase 1.5 cadence (after Phase 1) | 1-2 wk | Wire 5 existing hooks (slot-chase, hot-force, IFD profile, primop-throw, alloc-explosion) to emit via registry; SARIF / JSON / text emitters |
| **3 — UX (numbered codes, severity, suppression, docs site)** | Stage 17 slot proper | 1 wk | `V<NNNN>` rule codes; 4-tier severity (error/warning/info/pedantic); 3-layer suppression (inline pragma + `.v3-lint.toml` + env-var); per-rule documentation pages |
| **4 — Syntactic mode** | Stage 17 slot proper | 1-2 wk | Starter rules V0003 (NoCtx audit), V0004 (uncatchable arith), V0012 (ungated counter), V0014 (IFD smell — heuristic) |
| **5 — IR-level mode** | Stage 17 slot proper | 2-3 wk | `opt_lint.cc` post-lower pass; rules V0001 (M^N lowering), V0002 (deep composeExtensions), V0005 (eager-lower-on-lazy-binding) |
| **6 — CI + LSP integration** | Stage 17.1 | 1 wk | GitHub Code Scanning workflow with SARIF upload; LSP server emitting findings to IDEs |

**Phase 1+2 alone deliver (a) and (b)** — runtime awareness +
context — without any of the static analysis work. Phases 4+5 are
the hlint-equivalent (c) part.

Total Stage 17 budget: ~7-10 weeks. Phase 1-2 (~2-3 weeks) is
amortized across earlier stages. Phase 3-5 (~4-7 weeks) sits at
the Stage 17 slot.

### Exit criteria

- `v3-lint check flake.nix` runs Modes 1+2 (no eval) and emits
  findings to stdout in `--format=text` (default), `--format=json`,
  or `--format=sarif`.
- `NIX_V3_LINT=warn nix eval .#x` runs Mode 3 inline during a
  normal eval, emits ≤ 1 finding per `(rule_id, file:line)` per
  session, and dumps a summary at exit.
- All 27 catalogued patterns from
  `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` §4 have either: a
  shipped rule, OR a `wontfix` rationale documenting why (e.g.
  resolved-already-regression-only).
- Re-running `v3-lint check` on `haskell-nix/bootstrap.nix` would
  have flagged the #756 M^N pattern (V0001) ahead of the #756 fix
  needing to land. Validated as a regression test.
- Lint findings on cardano-node M5 flake match what the team
  manually identified during the #754/#755/#757 investigation.
- LSP server exposes lint findings via standard
  `textDocument/publishDiagnostics` events; tested against
  VSCode + the Nix language extension.

### Kill criterion

If Phase 1 (LintRegistry) effort exceeds 2× budget (2+ weeks
instead of 1), the 70+ env-var gates are more entangled than
expected; either refactor the gates first OR ship lint as a
parallel system (its own registry) and absorb the duplication
debt.

If Phase 4 + Phase 5 combined produce fewer than 5 shipped rules
after 4 weeks of work, the catalog (§4) is over-promising; pivot
to a minimum viable scope (3-4 rules covering V0001 / V0002 /
V0003) and freeze further rules until field experience justifies
each.

If the Mode 3 inline overhead exceeds 2 % on `hello.name` with
`NIX_V3_LINT=warn`, the hook dedup or registry dispatch is
inefficient; throttle aggressively (1 finding per rule per
session) or move all rules to trace-replay post-mortem mode.

### Rule 0 framing

This stage kills three hypotheses:

1. **"Pathological-pattern detection for Nix requires a separate
   project / external tool."** Falsified by shipping `v3-lint` as
   an in-process VM mode that reuses 12+ existing diagnostic hooks
   — no separate parser, no separate runtime.
2. **"Existing Nix linters (statix etc.) already cover the
   important patterns."** Falsified by the catalog mapping: of 27
   documented patterns, ≥ 15 require trace-driven or IR-level
   analysis that no existing tool performs.
3. **"VM-side fixes like #757 slot-chain compression remove the
   need for user-facing lint."** Falsified by V0002's first-touch
   tax remaining O(N) even after compression — the lint nudges
   users toward shapes that compress *faster* and GC *cheaper*.

If any of these claims fails (the community demands a separate
tool, statix turns out to subsume the catalog, or compression
collapses first-touch to O(1)), the stage is wrong-shape and we
revisit.

### Architectural compatibility check

- **Stage 3 (nursery default-on)**: lint emits at the existing
  allocation hooks; nursery semantics don't change the firing
  surface. Phase 1 registry includes the nursery's already-existing
  alloc-stat counters.
- **Stage 4 (uniform STG-shape)**: strictness-analysis findings
  (which thunks could have been strict) flow naturally into lint
  rules ("over-thunkification opportunity"). Stage 4's IR pass
  output is what Mode 2 reads.
- **Stage 5/6 (hidden classes / PICs)**: shape stability gives
  Mode 2 precision on "same site, polymorphic shape — investigate"
  hints.
- **Stage 7 (selector thunks)**: lint rules can flag "this selector
  is hot but not specialised" once the optimization is default-on.
- **Stage 8 (thin FFI + primop classification)**: V0003 (NoCtx) and
  V0014/V0015 (IFD-aware) rules consume the Tier-1 classification
  metadata directly. If Stage 8's effect-tagging includes
  Unison-Item-4-style `RequiresStore`, V0014 graduates from
  heuristic to precise.
- **Stage 14 (error UX)**: lint findings share the structured
  `Diagnostic` format; the `--explain` mechanism is reused.
- **Stage 15 (profiler UX)**: lint and profiler share the
  position-attribution substrate AND the JSON/SARIF emitter. Both
  Stage 17 and Stage 15 plug into the same `LintRegistry` that
  Phase 1 builds.

### Self-critique

- **The 27-rule catalog is an upper bound, not a target.** Many
  entries are CRITICAL but already fixed (V0009 attrPosTable was
  fixed in #752); they're useful only as regression detectors.
  Realistic Stage 17 ship: 8-12 active rules. The exit criterion
  is honest about this distinction.
- **Lint cost is non-zero.** Each hook adds a few ns to the
  dispatch loop. On hot opcodes (OP_FORCE, OP_CALL) this matters.
  Per-hook gating + dedup keep it bounded; measurement spike
  needed before any default-on mode. The kill criterion bounds
  the overhead.
- **The severity ladder thresholds are guesses.** V0002's
  8/32/256 threshold for deep `composeExtensions` came from a
  single-workload observation (#757's 4096). Real depths on
  different cardano-class projects may vary by an order of
  magnitude. Expect re-tuning after first real CI use.
- **statix has community traction; v3-lint starts at zero.**
  Phase 4 (syntactic mode) could express rules in statix's format
  to ride that traction. Worth investigating during Phase 4 ramp.
- **The "ship Phase 1 during cadence" framing risks Phase 1
  perpetually slipping** because it lacks a stage-anchor deadline.
  Mitigation: tie Phase 1 to a specific env-var-count milestone
  (e.g. "≤ 50 gates by end of Stage 3"). Hold the team to it.
- **No prior Nix linter has shipped trace-driven analysis.**
  v3-lint would be first. That's both the value proposition and
  the risk: design space is unexplored; expect a long tail of
  UX edge cases in Phase 3-4.

### Cross-references

- Full design: `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` (13 sections;
  pattern catalog with 27 entries, three modes architecture,
  ShellCheck/Ruff/Clippy UX synthesis, ecosystem gap analysis,
  phased rollout, #757 case as canonical first rule).
- Pattern catalog substrate: derived from
  `DATA_STRUCTURE_AUDIT_2026-05-21.md`,
  `FORK_REVIEW_2026-05-21.md`, `IFD_DEEP_DIVE_2026-05-21.md` plus
  ~60 days of commit-level RCA memos in this folder.
- Diagnostic substrate: `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` +
  `NIX_PROFILER_DESIGN_2026-05-21.md` (both share the position
  attribution layer with this stage).
- IFD integration: `IFD_DEEP_DIVE_2026-05-21.md` §11 — Unison
  Item 4 (effect propagation) is the prerequisite for V0014/V0015
  graduating from heuristic to precise.
- Unison-ideas connection: `UNISON_IDEAS_2026-05-07.md` §1
  (content-addressed IR) gives Mode 2 a more stable rule-match
  substrate than AST hashing alone.
- Ecosystem prior art: ShellCheck wiki (numbered codes + 3-layer
  suppression), Ruff docs (safe/unsafe auto-fix), Clippy docs
  (8-category taxonomy), SARIF 2.1.0 spec.

---

## Candidate future stages (pending measurement)

Added 2026-05-17. The following are **candidates**, NOT committed stages. Their addition to the roadmap is conditional on the Phase 1.5 measurement spike in `ACTION_PLAN_2026-05-15.md`. Design analysis lives in `PERF_STRATEGY_2026-05-17.md`.

| Candidate | What | Commits if measurement shows | Falsified if measurement shows |
|---|---|---|---|
| **Stage 10** (salsa) — *partial subset implemented as #741 substrate 2026-05-23; full salsa may be unnecessary* | Incremental result cache keyed `(cellHash, envHash) → resultBytes`; persistent across invocations; 10-100× warm-eval potential | >40% warm fraction on real Nix workloads | <20% warm OR prototype <2× |
| **Stage 11** (HAMT) | Polymorphic attrset (flat ≤32-64; HAMT/CHAMP above); targets nixpkgs overlay `//` patterns | >20% time in `//` AND skew toward large attrsets | Median size <64 AND `//` <5% of eval |
| **Stage 12** (JIT decision) — *deferred-with-data 2026-05-23, see `JIT_CONFIDENCE_2026-05-23.md`* | Truffle (Java fork) / PyPy (RPython fork) / Cranelift (in-process JIT codegen); needed for >3× cold eval | Dispatch share > 40 % via OPCYCLES on cardano-node M5 AND remaining alternatives < 5 % wall to extract | OPCYCLES (#786) measured dispatch at ~5 % wall; #788 measured 3 derivation primops at 99 % of primop wall — JIT cannot reach primop bodies. Realistic upside ~10-15 % wall reduction for multi-year cost vs #741 IFD cache (Phase 1 landed) at 1-2 weeks |
| **Stage 13** (multi-core capabilities) | GHC-style parallel evaluator: capabilities, sparks, work-stealing, atomic thunk state, parallel GC, FFI serialization. Intra-invocation parallel eval | Critical path <30% of total work on real workloads AND process-level alternatives are insufficient | Critical path >60% OR process-level alternatives capture the same benefit |

**Important context**: a verification agent on 2026-05-17 flagged the load-bearing empirical premises as **unverified from public data**. No salsa-style fine-grained eval cache has shipped for Nix (Tvix/Snix defer it; Snix's "finer granularity" is store-layer, easy to misread). Rust-analyzer's salsa 3.0 migration is struggling with memory regressions on graphs smaller than nixpkgs would impose. Adapton (the cited theoretical foundation) has no production deployments and its cycle support is programmer-supplied, not provably sound. CHAMP's published gains are on iteration/equality, not insert/update; HAMT for small attrsets may be slower than flat-copy.

For Stage 13 (multi-core capabilities), self-correction recorded in `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` flagged several previous-turn overstatements: Nix's purity is not actually better than Haskell's; cost estimate revised from 6-12 months to 9-15 months; expected wins capped by Amdahl on the stdenv sequential chain; process-level parallelism (xargs -P, Hydra jobset-per-process) likely captures most of the benefit at zero v3 cost; I/O concurrency without full parallelism is the cheaper sub-option.

**Therefore**: do NOT plan resource against any of these candidates. Plan against the measurement spike (Phase 1.5, extended to include parallel-potential trace analysis per `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` §8). After measurement, revisit.

### Stage 10 partial-subset substrate landed 2026-05-23

The #741 series (Phases 1-5, commits `23bb231d2` → `bff1f670f`) implemented a **narrower form of Stage 10**: derivation-primop-keyed eval-result cache, not full whole-graph salsa. The substrate is **over-validated by four independent signals**:

| Signal | Result | Status |
|---|---|---|
| Correctness — byte-identical TW cross-process | 6 cold+warm runs, 0 mismatches | ✓ CONFIRMED |
| Determinism — canonical Value hash stable | Phase 2 cross-process | ✓ CONFIRMED |
| Cross-workload reuse — second workload hits first's cache | 95-97 % cold (gcc/python3 vs hello) | ✓ CONFIRMED |
| Intra-process redundancy — same eval has duplicates | 34.1 % hello, 58.3 % firefox | ✓ CONFIRMED |
| Wall savings at leaf primop scope (SQLite-backed L2) | −47 ms warm, −569 ms cold; STILL falsified at 58 % hit rate on firefox | ✗ FALSIFIED — structural |

**The Phase 5 falsifier is structural to the L2 implementation, not to the cache concept.** Per-call SQLite lookup (~60 µs warm) exceeds per-call saved libstore-tail work (~30-50 µs) regardless of hit rate. `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` proposes the obvious next step: replace SQLite-backed L2 with mmap'd flat file (~150 ns/lookup, demand-paged, cross-process via OS page cache, no syscalls on hot path). **The mmap variant flips the wall sign positive** (expected +3-5 % wall on hello, +5-10 % on firefox; pre-committed thresholds in §7.2 of that doc).

**Strategic implications for Stage 10 framing:**

1. **The cache substrate Stage 10 needed already exists.** The Phase 1-5 substrate is reusable as the foundation; only the L2 storage layer needs replacement.
2. **Full salsa machinery may be unnecessary.** The #741 substrate + mmap'd L2 + AOT distribution covers most of what Stage 10's "10-100× warm-eval potential" was reaching for, at a tiny fraction of full salsa's complexity. Rust-analyzer's 4× memory regression on smaller-than-nixpkgs graphs (the verification-agent concern from 2026-05-17) was always the load-bearing risk; the #741 narrower form sidesteps it by caching only at deterministic primop boundaries.
3. **Stage 10 may evolve into "ship the mmap'd L2 + AOT distribution," not "build salsa."** Effort estimate revises from multi-month salsa engineering to 3-5 day spike + 1-2 week distribution infra + cross-team Nix-infra coordination.
4. **Phase 4 (Class B IFD primops) remains the larger lever** even with mmap'd L2 at leaf scope. Per `IFD_DEEP_DIVE_2026-05-21.md` §11, IFD-boundary caching's per-hit savings are seconds (skipped nix-builds), not microseconds. The mmap'd cache substrate is the right Phase 4 substrate too — same format, larger Value payloads, same OS-page-cache sharing.

**Next moves codified in `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` §10:**
1. 3-5 day mmap-L2 spike with pre-committed ship/revert thresholds
2. Control measurement: re-run Phase 5 with L1-in-memory-only (no SQLite) on firefox
3. Parallel-track: measure cardano-node M5 with existing Phase 5 substrate
4. (Gated on success) AOT distribution spec — 1-2 weeks v3 work + cross-team coordination
5. (Independent) Phase 4 planning continues; multi-second-savings lever holds regardless

The Phase 1.5 measurement spike framing from 2026-05-17 is partially **subsumed by the #741 series data**: cache reach + redundancy rates are now measured, not hypothesised. What remains is the L2-implementation falsifier (mmap spike) and the deeper-graph workload measurement (cardano-node M5).

---

## Killed-stage revival triggers

Added 2026-05-23. Stages 5, 6, 9 were killed by Rule 0 during Week 1.
The kills are decisive — those stages are removed from the active
plan — but specific measurements could re-open them. This table
lists the explicit triggers, hooks, and decision rules so revival
isn't a question of "did anyone notice" but of "did the trigger
fire."

The discipline here matters: without pre-committed triggers, "we'll
revisit later" becomes either "never decide" (the lode/-proliferation
problem) or "whenever someone has a fresh idea" (which violates the
falsification rule). With triggers, the team knows EXACTLY when to
re-measure and EXACTLY what data changes the decision.

For full per-trigger detail (re-measurement procedure, decision
rules, probability assessments), see each kill memo. This is the
summary table.

| Killed stage | Trigger | Natural milestone hook | Effort to re-measure | Probability of revival |
|---|---|---|---|---|
| **Stage 5** (hidden classes) — Trigger A | AttrSelect family ≥ 5 % of dispatch on a representative workload after a denominator-shifting VM change | Post local-stack-motion fix (when GET/SET_LOCAL drops from 48.92 %) | 1 day (re-run #778 opcount banner) | Low |
| **Stage 5** — Trigger B | User report of slower-than-expected workload AND profile identifies AttrSelect as hot category | Workload-specific (user-driven) | Workload-specific | Low-moderate |
| **Stage 5** — Trigger C | ≥ 80 % of AttrSelect dispatch hits monomorphic call-sites AND wall-time savings would exceed 3 % | Speculative (would-be standalone investigation) | 2-3 days (instrument + analyse) | Speculative |
| **Stage 6** (PICs) | Stage 5 revives (any trigger), OR monomorphic-hot-attr workload emerges | Tied to Stage 5 revival | (tied to Stage 5) | Low |
| **Stage 9** (cell-level dedup) — Trigger A | Coarser-granularity re-measurement (whole ExprAttrs / ExprLet bindings) shows byte-dedup ≥ 2 × | Materialization-retirement Phase 2 commits to content-addressed eval cache (`IFD_DEEP_DIVE_2026-05-21.md` §11) | 2 days (modify dedup_survey.cc to ExprAttrs level) | Moderate |
| **Stage 9** — Trigger B | Post-ABT IR-level dedup ≥ 2 × | ABT refactor lands for any non-perf consumer (Unison Item 3, Item 4, or lint Phase 5) | 1 day (replace bytecode hash with IR hash in dedup_survey.cc) | Low |
| **ABT refactor** (Unison Item 2) | Any of: Unison Item 3 (hash-keyed eval cache) starts; Unison Item 4 (effect propagation) starts; lint Phase 5 (Mode 2 IR-level) starts; Stage 9 Trigger A fires | Tied to the consumer's prioritisation | (not a measurement; an implementation prereq) | High (as means, not end) |
| **Stage 12** (JIT) — Trigger A | Dispatch share > 40 % of wall via OPCYCLES (not opcount) on cardano-node M5 OR similar production workload | After #741 Phase 2-5 land + Tier 1 ICs + nursery default-on (the alternatives ship first) | 1 day (re-run #786 OPCYCLES on new baseline) | Low |
| **Stage 12** — Trigger B | All easier alternatives (#741 cache, AOT distribution, Tier 1 ICs, TOS caching, nursery default-on) have shipped AND v3 still > 2× TW on a representative production workload | Each alternative landing is a natural checkpoint | 0 (data is already on hand by then) | Low-moderate |
| **Stage 12** — Trigger C | Workload class shifts from derivation-heavy to dispatch-bound pure-Nix (lib.evalModules-only, module-system stress) AND that workload becomes user-facing primary | Workload-specific (user-driven; ecosystem shift) | Workload-specific | Speculative |

### Decision protocol

When a trigger fires:

1. **Run the re-measurement** at the effort cited above.
2. **Apply the decision rule** from the kill memo (specific
   numeric thresholds per stage).
3. **If revival is justified**, the stage re-enters as a NEW stage
   with the revival data as its rationale. It does NOT silently
   resume the original Stage 5/6/9 plan — the original plan was
   killed, and the revived form may be substantially different
   (e.g. Stage 9 coarser-granularity is architecturally distinct
   from Stage 9 thunk-body-granularity).
4. **If revival is NOT justified**, append the re-measurement
   data to the kill memo as confirmation, and move on. The
   revival path can fire again later if conditions change again.

### Non-trigger boundary

The following do NOT trigger revival:

- "Someone has a new idea." Without measurement, ideas are
  unfalsified. Refer to the falsification rule.
- "Velocity reclaim from other kills suggests we have time."
  Reclaimed calendar should go to the *highest-impact* next
  lever, not to re-attempting killed work.
- "The kill measurement felt close to the threshold." Specific
  numbers killed each stage; lobby for measurement against an
  explicit revival trigger instead.

---

## Cross-stage standing cadence (preserved throughout)

- **Action plan's weekly cadence continues**: Monday bench re-run; Friday env-var delta audit; net gate count must monotonically decrease.
- **Quarterly**: re-score `ALIGNMENT_SCORECARD_2026-05-15.md`. Trigger an alignment review (not just a stage review) if drift criteria fire.
- **Per-stage exit**: append a one-line RESOLVED row to the relevant scorecard component + this roadmap.
- **Per-commit**: every gate added has an inline retirement criterion; every "Phase/Stage follow-up" has a victory condition; no new RCA letter on an open one.
- **Perf-trace runs alongside Monday bench** (added 2026-05-20): `make perf-trace WORKLOAD=hello.{name,drvPath,outPath}` produces same-day SVG overlays in `bench/samples/<date>/`. Used to refute or confirm factor-attribution claims in the per-stage exit reports. Design: `PERF_TRACE_TOOL_DESIGN_2026-05-20.md`.

---

## Current state benchmark (2026-05-18 post-Phase-1)

| Workload | v3 | TW | Ratio |
|---|---|---|---|
| `pkgs.hello.name` | 0.58s | 0.47s | 1.4× slower (Phase 1 exit; ✅ met) |
| `pkgs.hello.pname / .version / .meta.description / .outputs / .system / .type` | ≈parity | ≈parity | parity |
| **`pkgs.hello.drvPath`** | **30-46s** | **1.3s** | **~30× slower** (active floor) |
| **`pkgs.hello.outPath`** | **30-46s** | **1.3s** | **~30× slower** (active floor) |

The drvPath/outPath gap decomposes per `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`:
- ~5-10× per-op bytecode dispatch overhead → IR Phases A-H (`IR_OPTIMIZATION_PLAN_2026-05-18.md`); Phase 4 (decomp + computed-goto); Stages 5-6 (PICs)
- ~5-10× Boehm-arena scan amortized into each force → Stage 3 (nursery default-on)
- ~2-5× extra intermediate allocations → IR Phase C (stream fusion); Stage 4 (strictness analysis)
- unknown× higher-level caching gap → Stage 10 candidate (salsa, pending Phase 1.5 measurement of TW caching)

Each factor maps to a roadmap stage. Closing any one alone doesn't close the gap; the multiplicative structure means all factors need attention.

**Instrumentation prerequisite** (2026-05-20): factors 2 and 3 above are currently *unverifiable*. `bench.py` is scalar (final-max-RSS, p50 wall); `V3_TIMING` and `NIX_VM_OPCOUNTS` are aggregates; no tool gives a time-series view of CPU% / RSS / Boehm-heap during a single eval. The Phase 1.5 force-rate decomposition cannot honestly proceed without that signal. Design committed: `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` (sidecar `psutil` sampler + in-process `GC_get_heap_size()` probe + SVG overlay). ~3 days to first measurement; runs parallel to the IR Phases A-H track.

## In-progress work track (2026-05-18 onwards): IR Phases A-H

The action plan's Phase 2(R) (chosen branch) is executing the 8-phase IR optimization plan in `IR_OPTIMIZATION_PLAN_2026-05-18.md`. This sits in the strategic structure as:

- **Below Stage 4** (uniform STG + strictness analysis) — IR Phases A-H are targeted optimizer passes that extend `opt_const_fold.cc`'s pipeline; Stage 4 is a bigger architectural shift (every binding lazy by default + full strictness pass). A-H can be precursors validating IR-level wins before committing to Stage 4's scope.
- **Complementary to Stage 3** (nursery default-on) — A-H reduces allocation rate at the source; Stage 3 handles the survivors generationally. Cumulative effect on the GC factor is multiplicative.
- **Orthogonal to Stages 5-6** (hidden classes, PICs) — A-H attacks IR-level dispatch; PICs attack runtime attribute-lookup dispatch.

**Stated target**: hello.drvPath 30× → ≤3× TW. Honest math: A-H alone closes per-op + intermediate-allocation factors → ~20-30× residual. Reaching ≤3× requires Stage 3 (nursery) for the GC factor in parallel. The team's stated cumulative target assumes Stage 3 lands shortly after A-H.

---

## End-state target (≈ 2027-Q1, after Stage 7 + Stage 14)

The scorecard at the end of Stages 7 + 14 should read:

| # | Component | Status |
|---|-----------|--------|
| 1 | Parser reuse | ✅ |
| 2 | Bytecode VM core | ✅ (vm.cc ≤2 500 LoC; clean dispatch) |
| 3 | Optimizer pipeline | ✅ (occur + strictness wired) |
| 4 | STG thunk states | ✅ |
| 5 | STG-shape uniformity | ✅ (uniform lowering + strictness analysis) |
| 6 | V8 hidden classes / shapes | ✅ |
| 7 | Polymorphic Inline Caches | ✅ |
| 8 | Selector thunks | ✅ |
| 9 | Generational GC | ✅ (nursery default-on; closure-pool retired) |
| 10| Thin FFI | ✅ (ffi.cc documented surface; bridge_yield gone) |
| 11| Pure bytecode evaluation | ✅ (NIX_V3_SKIP_INSTALLABLE_PREEVAL deleted) |
| 12| Bytecode disk cache | ✅ |
| 13| **Error UX** (Stage 14) | ✅ (structured `Diagnostic`; two-span lazy errors; Levenshtein at attribute-miss; error codes + `nix --explain`; `--error-format=json` for LSP; `--error-format=cppnix-compat` for legacy) |

**Bench**: v3 ≥1.0× TW on `lib-evalModules-100`, ≥1.0× on `fib33`, ≥0.7× on full nixpkgs `attrNames` (i.e. v3 is faster — the V8/STG win).

**Honest ceiling note (Agent-1 review, 2026-05-17)**: an interpreter-only PIC architecture caps around 2-3× on attrset-heavy work (Hölzle/Ungar 1991 + JSC pre-DFG numbers). The end-state target above is consistent with this ceiling. If we want >3× anywhere, the only paths the literature supports are Truffle-on-Graal (Java rewrite) or PyPy-style meta-tracing (RPython rewrite) — both fork the project. Read Bolz-Tereick et al., *Allocation Removal by Partial Evaluation in a Tracing JIT* (PEPM 2011) before deciding whether the C++/bytecode + PIC composition is sufficient for the project's perf ambitions.

**Code health**: vm.cc ≤2 500 LoC; `bridge_yield.cc` deleted; bridge plumbing in vm.cc ≤100 LoC; env-var count ≤20; lode/ has ≤5 active docs. primops.cc remains substantial (~7 000-7 800 LoC) — that's intentional and correct, because v3-native pure-data primops are part of the design.

**Process**: no RCA letter has been opened that wasn't closed within 7 days for the past 6 months.

---

## Meta-kill criterion (project-level decision point)

**At end of Stage 2 (≈ Week 14, ≈ 2026-08-21)**: if `NIX_V3_SKIP_INSTALLABLE_PREEVAL` cannot be deleted — i.e. v3-direct cannot evaluate real workloads without TW pre-eval — the architecture choice "v3-direct as primary" is wrong.

The honest options at that point are:
- **Pivot to "v3 as a JIT-style optimizer for hot paths, TW remains primary"**. Narrower, defensible, less ambitious. The action plan's wins (gate retirement, vm.cc decomp, correctness fixes) all still apply.
- **Abandon v3 and revisit later**. When one of (a) cppnix major version refactor, (b) better profiler tooling, (c) sponsored full-rewrite resourcing lands. The lode/ archive remains valuable as design study for a future attempt.

**Stages 3-7 only make sense if Stage 2 actually closes.** If Stage 2 misses, do NOT silently proceed to Stage 3.

---

## Risk register

| Risk                                                                 | Likelihood | Impact | Mitigation                                                                          |
|----------------------------------------------------------------------|------------|--------|-------------------------------------------------------------------------------------|
| Stage 2 reveals more TW-dependencies than expected                    | Medium     | High   | Kill criterion triggers project-level pivot; not a sunk-cost continuation           |
| Phase D nursery write barriers blow out timeline                      | Medium     | Medium | Stage 3 kill criterion defers nursery; uniform STG can still ship on closure-pool   |
| Strictness analysis can't catch enough patterns                       | Low        | High   | Reference GHC + the optimizer doc; this is well-studied territory                   |
| Shapes fragment beyond predictive value on real nixpkgs               | Medium     | High   | Stage 5 has a measure-before-acting checkpoint; kill criterion redirects to Stage 7 |
| PICs add little perf due to dispatch overhead                         | Low        | Medium | Insert Stage 6.5 (direct threading) if measured                                     |
| Stage 4 allocation spike exceeds nursery absorption                   | Medium     | Medium | Stage 3 must close before Stage 4; if not, Stage 4 paces back                       |
| New env-vars accumulate during stages (action plan rules violated)    | High       | Medium | Weekly env-var audit; PRs that violate are reverted, not amended                    |
| Researcher-archaeology pattern returns (lode/ doc proliferation)      | Medium     | Medium | "No new design doc until previous closes" rule; quarterly re-evaluation             |
| Stage 17 Phase 1 (LintRegistry) slips because it has no anchor deadline | Medium     | Low    | Tie to specific env-var-count milestone (e.g. "≤ 50 gates by end of Stage 3"); track in standing weekly env-var audit |
| Lint inline-mode overhead exceeds 2 % budget on hot workloads          | Low-Medium | Medium | Kill criterion in Stage 17 throttles aggressively or moves all rules to post-mortem replay |
| 27-rule catalog turns out over-promised once shipped                  | Medium     | Low    | Stage 17 exit criterion is "shipped rule OR documented wontfix"; not "all 27 active" |

---

## Estimated effort summary

| Stage                                  | Weeks | Cumulative |
|----------------------------------------|-------|------------|
| 1 — Action plan completion             | 8     | 8          |
| 2 — Pure-bytecode eval                 | 6     | 14         |
| 3 — Nursery default-on                 | 6     | 20         |
| 4 — Uniform STG-shape                  | 8     | 28         |
| 5 — Hidden classes / shapes            | 6     | 34         |
| 6 — Polymorphic Inline Caches          | 6     | 40         |
| 7 — Selector thunks                    | 4     | 32         |
| 8 — Thin FFI (parallel)                | 0     | 32         |
| 14 — Error UX (committed UX pillar)    | 5-9   | 37-41      |
| 15 — Per-line profiler UX              | 1*    | 38-42      |
| 17 — Pattern lint UX                   | 4-7*  | 42-49      |

\* Stage 15 effort is 2-3 weeks at the Stage 15 slot; Phase 0-3 (~6
days) land earlier during ACTION_PLAN Phase 1.5 cadence and don't add
to the perf-stage timeline. Stage 17 similarly: Phase 1-2 (~2-3 weeks)
land amortized across earlier stages as part of env-var hygiene;
Phase 3-5 (~4-7 weeks) sit at the Stage 17 slot proper.

**Revised after 2026-05-22/23 kills:**
- Stage 9 (~5 wk) cancelled by Rule 0 (`STAGE_9_KILLED_2026-05-22.md`)
- Stage 5 (~6 wk) cancelled by Rule 0 (`STAGE_5_6_KILLED_2026-05-23.md`)
- Stage 6 (~6 wk) implicitly cancelled (Stage 5 prereq)
- **Cumulative reclaim: ~17 weeks** vs the original 44-week perf-core plan
- New work entered the plan: yet-to-be-designed "local-stack-motion fix"
  targeting register-VM / super-instructions / threaded code (the
  48.92 % dispatch share #778 revealed). Sizing unknown until design
  spike completes (target effort 4-8 wk if it lands).

**Total**: ≈32 weeks of perf stages from 2026-05-15 (down from 44)
PLUS the local-stack-motion fix (4-8 wk if it commits). UX pillars
(14/15/17) extend to ≈42-49 weeks if shipped sequentially after
perf. Target completion of perf core: ≈ **2026-Q4** (down from
2027-Q1). UX-complete target: ≈ **2027-Q1** (down from 2027-Q3).

This is **aggressive** for one engineer; comfortable for two. The single-engineer path implies fewer parallel Stage 8 commits and slower bench-tuning iteration. Pad timeline by ~25% (≈55 weeks, ≈ 2027-Q2) for a realistic single-engineer estimate.

---

## Appendix — Stage completion log

Append one row per stage as exits land. Format: `Stage N — RESOLVED YYYY-MM-DD — [met / met-with-caveats / missed]: <one-line summary>`.

- Stage 1 — [in-progress ~85-95 % Week 1 day 8; original target 2026-07-10]
- Stage 2 — **RESOLVED 2026-05-22 — met**: `NIX_V3_SKIP_INSTALLABLE_PREEVAL` retired (commit `3af813638`, Week 1 day 7). v3-direct owns evaluation; meta-kill criterion closed positive. 13 weeks ahead of plan.
- Stage 3 — [~95 % Week 1 day 8; original target 2026-10-02; Phase D milestone hit + default-on; Phase E v0.2 landed]
- Stage 4 — [in-progress; Force(MkThunk) variant falsified (#775); v4.3 cross-fn landed; v4.4 cross-fn through higher-order callees pending]
- Stage 5 — **CANCELLED 2026-05-23 — killed-by-Rule-0**: see `STAGE_5_6_KILLED_2026-05-23.md`
- Stage 6 — **CANCELLED 2026-05-23 — implicitly killed** (Stage 5 prereq)
- Stage 7 — [pending; was W41-W44, shifts left after Stage 5/6 reclaim]
- Stage 8 — [in-progress, runs parallel; disk-cache productionised, NIX_V3_DISK_CACHE default-on, deserialize cost dropped from 1.78ms/file]
- Stage 9 — **CANCELLED 2026-05-22 — killed-by-Rule-0**: see `STAGE_9_KILLED_2026-05-22.md`
- Stage 14 — [pending, post-perf]
- Stage 15 — [pending; Phase 0-3 lands during ACTION_PLAN Phase 1.5; Phase 4 at Stage 15 slot]
- Stage 17 — [pending; Phase 1-2 land during env-var hygiene cadence; Phase 3-5 at Stage 17 slot]
- **NEW** (post-2026-05-23, not yet numbered): "Local-stack-motion fix" — 48.92 % of dispatch is GET_LOCAL + SET_LOCAL + GET_UPVALUE per #778. The new biggest single perf lever after Stage 5/6 kills. Candidate VM-shape changes: register-based VM, super-instructions, threaded code, ABT/closure-form. 1-day design spike before committing; 4-8 weeks if it proceeds.

---

## Cross-references

- Action plan: `ACTION_PLAN_2026-05-15.md`
- Scorecard: `ALIGNMENT_SCORECARD_2026-05-15.md`
- What worked / what didn't: `LESSONS_LEARNED_2026-05-15.md`
- Linking design (Stage 9): `LINKING_DESIGN_2026-05-17.md`
- Perf strategy / candidate Stages 10-12 (pending measurement): `PERF_STRATEGY_2026-05-17.md`
- Multi-core capabilities / candidate Stage 13 (pending measurement): `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`
- **FFI audit / Stage 8 inventory (2026-05-20)**: `FFI_AUDIT_2026-05-20.md` — TW fallback inventory, 4-tier migration plan, system-info-as-constants correction
- **Perf-trace tool design (2026-05-20)**: `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` — time-series CPU% / RSS / Boehm-heap sampler with SVG overlay; closes LESSONS §4.9 Item 5 ("documented CPU-profile workflow") and supplies the missing instrument for Phase 1.5's force-rate decomposition
- Error UX (Stage 14): `ERROR_UX_DESIGN_2026-05-20.md`
- **Per-line profiler UX (Stage 15) (2026-05-21)**: `NIX_PROFILER_DESIGN_2026-05-21.md` — Xcode/Instruments-style per-line CPU + memory attribution via pprof + Firefox Profiler exporters; complements perf-trace (when-time-spent vs where-in-source-time-spent); ~8 days MVP, Phase 0-3 land during ACTION_PLAN Phase 1.5
- **Boehm-GC dependency analysis (2026-05-21)**: `BOEHM_DEPENDENCY_2026-05-21.md` — Why Boehm stays even after Stage 3; three-layer memory-savings model (Stage 3 committed → Layer 2 partial Stage 8 → Layer 3 Whippet-tenured uncommitted); commitment criteria for candidate Stage 16
- **GC build-vs-buy analysis (2026-05-21)**: `GC_BUILD_VS_BUY_2026-05-21.md` — Why we continue hand-rolling rather than adopting GHC's GC (not extractable) / Whippet (defer until Layer 3 trigger) / MMTk (Rust toolchain, premature); decision summary table + Rule 0 falsifiers
- **Pattern-lint UX (Stage 17) (2026-05-22)**: `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` — `v3-lint` with three modes (syntactic / IR-level / trace-driven) against a 27-rule catalog derived from team RCA history; ShellCheck-numbered codes + 4-tier severity + 3-layer suppression + Ruff safe/unsafe auto-fix + SARIF 2.1.0; #757 deep-`composeExtensions`-chain case as canonical first rule (V0002); Phase 1-2 land amortized across env-var hygiene cadence, Phase 3-5 at Stage 17 slot proper
- **Formal verification analysis (2026-05-22)**: `FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md` — TLA+ / Coq / Lean cost-benefit. Three TLA+-shaped targets pay (cell-update protocol, write-barrier post-Phase-D, fiber/bridge_yield); full evaluator verification doesn't. Higher-ROI techniques to ship first (sanitizer CI, differential fuzzing, property tests, issue→fixture manifest)
- **Stage 9 KILLED (2026-05-22)**: `STAGE_9_KILLED_2026-05-22.md` — Phase L0 dedup-survey spike (commit `37616ecc6`); bytecode dedup 1.17 × function / 1.03-1.07 × byte vs 2 × kill threshold. Per-thunk-body content-addressed cell store hypothesis falsified. Revival conditions in §7.5 (coarser-granularity / post-ABT IR-level / cross-process)
- **Stages 5 + 6 KILLED (2026-05-23)**: `STAGE_5_6_KILLED_2026-05-23.md` — Phase L0 dispatch-budget spike (commit `fe7c17498`); AttrSelect family 2.24 % of dispatch vs 10 % kill threshold; wall-clock ceiling argument (~26 ms even if PIC free). Stage 6 implicitly killed (Stage 5 prereq). Revival conditions in §7
- **Roadmap progress snapshots**: `ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md` (superseded), `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` (current) — point-in-time velocity + stage-progress capture for retrospective review
- Nursery design: `CHENEY_NURSERY_DESIGN.md`
- Optimization plan (input for Stage 4-6): `OPTIMIZATION_PLAN.md`
- Occurrence analysis plan: `OPT_OCCUR_PLAN_2026-05-08.md`
- Reduction audit: `REDUCTION_AUDIT_2026-05-09.md`
- Cleanup audit: `CLEANUP_AUDIT_2026-05-09.md`
- V3-NATIVE constraint origin: commit `cf12c1880`
