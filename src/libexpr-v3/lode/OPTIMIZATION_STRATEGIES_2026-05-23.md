# v3 Optimization Strategies — Post-Kill Synthesis — 2026-05-23

Five-agent literature + codebase survey of optimization strategies
applicable to v3 in the wake of Stage 5/6/9 kills and the #780
register-VM falsifier. Companion to `ROADMAP_TO_VISION_2026-05-15.md`
+ `PERF_AUDIT_2026-05-23.md`.

**The order-of-magnitude truth:** No single optimization changes v3's
order of magnitude on hello.drvPath. The ~30× gap closes from
multiplicative single-digit wins across dispatch + GC + allocs +
caching. The non-JIT interpreter ceiling is **~1.5-2× native**
(LuaJIT-Remake, Piumarta-Riccardi, JSC LLInt evidence converges).
Closing the remaining gap below that ceiling requires JIT — which
the team has explicitly falsified at this scope (#780, ~10-15% wall
for multi-month investment).

The strategy is therefore: **stack five well-measured single-digit
wins, not one architectural breakthrough.** This is what GHC's
optimization stack has done since the 1990s; the literature confirms
no breakthrough is hiding.

---

## §1 — The headline strategy: broaden inline caches

The single highest-ROI direction post-bigram-fusion is **broadening
PEP-659-style inline caches from `OP_REC_BINDING_SLOT_REF` to
`OP_ATTRS_SELECT` / `OP_ATTRS_SELECT_DYN` / `OP_CALL` / primop
bridges**. Cross-agent convergence on this is unusually strong:

- **Dispatch agent**: CPython 3.11 LOAD_ATTR specialization gave
  36-44 % improvement; PEP 659 geomean is 25 % with range 10-60 %
  on pyperformance.
- **Codebase agent**: v3 has Schema 10 IC infrastructure shipped for
  OP_REC_BINDING_SLOT_REF (#779), 4-way polymorphic fast path. The
  mirror infrastructure for OP_ATTRS_SELECT already exists (per
  the #779 commit body); validate and broaden coverage.
- **GHC agent**: GHC achieves equivalents via static specialisation
  (SpecConstr / dictionary specialisation) — those don't transfer
  because they require static types. The dynamic-typed equivalent
  is exactly per-call-site PIC.

| Opcode | Current IC status | Expected per-opcode win | Effort |
|---|---|---|---|
| OP_REC_BINDING_SLOT_REF | Shipped (#779 Schema 10) | -24 % already realised on sweep | done |
| OP_ATTRS_SELECT | Infrastructure exists (#779 commit) | 10-20 % wall (CPython evidence) | 3-5 days |
| OP_ATTRS_SELECT_DYN | None | 5-10 % wall (smaller scope) | 3-5 days |
| OP_CALL specialisation per call-site | None | 5-15 % wall (CPython LOAD_GLOBAL/LOAD_METHOD analog) | 1-2 weeks |
| Primop bridge ICs | None | 2-5 % wall | 1 week |

**Falsifier per measure-twice-cut-once**: each IC site emits a
hit/miss counter. If the IC hit rate on cardano-node M5 is < 80 %,
the cache is megamorphic and revert (per `MEASURE_TWICE_CUT_ONCE`
§3.8). If hit rate ≥ 80 % AND wall delta ≥ 3 % per opcode, keep.

**Aggregate ceiling**: this family of work could plausibly compound
to **15-30 % on real Nix workloads** if all four follow-on opcodes
hit ≥ 80 % cache. That's the *biggest* lever the literature
identifies for v3's current shape.

---

## §2 — The second-headline: stack caching (TOS-in-register)

Per #778: 48.92 % of dispatch is local-stack motion (GET_LOCAL +
SET_LOCAL + GET_UPVALUE). Per #780 falsifier: register-VM rewrite
costs multi-month for ~10-15 % wall — **not** justified. But **stack
caching** (Ertl 1995, Gforth) attacks the same workload differently:
keep the top-of-stack in a register across consecutive opcodes,
avoiding the load/store on the obvious pattern of "GET_LOCAL N;
*use TOS*; SET_LOCAL M."

- **Dispatch agent**: 15.75 % geomean / 47.2 % best on P4; up to 58 %
  over 1-slot register baseline. Modern hardware bounds the win
  tighter but the technique is well-attested.
- **Cost**: state machine over opcodes (each opcode declares whether
  it consumes TOS / produces TOS / both / neither). Compiles to
  fewer load/store pairs in the dispatch loop. 2-4 weeks of careful
  work; not architectural rewrite.

**Falsifier**: instrument current stack motion vs theoretical
TOS-cached motion (count avoidable load/store pairs on hello.drvPath).
If < 30 % of stack motion is avoidable via 1-deep TOS cache, the
ceiling is too low — kill it like Stage 5. If ≥ 50 %, commit.

**This is the post-bigram-fusion next layer.** Bigram fusion
(T2.1-T2.4 in `PERF_AUDIT_2026-05-23.md`) attacks the same
bottleneck via different mechanism; stack caching attacks the
same bottleneck *between* bigram-fused superinstructions. They
compose multiplicatively.

---

## §3 — GHC-shaped wins: cardinality + update-frame elision

The GHC research agent identified **one coupled pair worth pursuing**:

**A1 — Cardinality analysis** (Sergey/Vytiniotis/Peyton Jones POPL 2014).
Extend `opt_occur.cc` from "0/1/many uses" to "0/1/many forces under
context demand C." Detects single-entry thunks — thunks that are
forced exactly once.

**A2 — Update-frame elision for one-shot thunks**. If A1 marks a
thunk as single-entry, OP_FORCE can skip the
indirection-write/update-frame push. The thunk's code compiles to
a tail call into its body.

- **Effort**: A1 ~2-3 weeks; A2 ~1 week given A1.
- **GHC win**: "modest improvements (1-3 % geomean on nofib);
  biggest wins in monad-heavy code."
- **Why it composes for v3**: v3's force-rate is currently ~5K/s vs
  TW ~1M/s (200× per-op gap per
  `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`). Removing a
  write + frame-push per evaluated thunk is multiplicatively
  useful with the nursery (less Phase D barrier churn) AND the
  bigram fusions (less dispatch per force-then-use sequence).

**Falsifier**: count thunks evaluated exactly once on hello.drvPath
+ cardano-node M5. If < 30 % of force events hit single-entry
thunks, A1+A2's wall win can't exceed ~5 % — defer. If ≥ 50 %,
commit.

---

## §4 — Competitive convergence: lessons from Lix / Tvix / Determinate

The Nix landscape agent identified four convergent directions across
shipping competitors. v3 should weigh each against its own design.

### §4.1 Atomic 3-state thunk (Pending / Awaited / Failed)

**Source**: Determinate Nix 3.11+, builds on cppnix PR #10938 (closed).

**What**: Thunk's `state` field becomes atomic with three states:
`Pending` (work not started), `Awaited` (one thread waiting; others
should block), `Failed` (exception, all waiters unblocked).

**Relevance to v3**: even though v3 is single-threaded today, the
atomic 3-state machine is independently useful: it converts the
current `clearBlackMarksOnException` exception-recovery shape into
state-machine-with-failed-edge, simpler invariants. Stage 13
(multi-core capabilities) candidate would inherit it cleanly.

**Effort**: 1-2 weeks to refactor existing Thunk state machine.
**Win**: not perf alone — correctness simplification + future-proofing.

### §4.2 VFS-style lazy trees

**Source**: Determinate Nix 3.5+ (100 % rollout). Claimed 3×+ wall
and 20×+ disk reduction on nixpkgs.

**What**: Source tree access is virtualised; files are read lazily
on demand instead of full git/tarball checkout upfront.

**Relevance to v3**: orthogonal to evaluator. Pairs with v3 as a
substrate. **Architectural question for the team**: should v3 ship
its own lazy-tree shim, or wait for upstream / Determinate
contribution to land?

**Effort**: this isn't v3's lane. Recommend cross-team coordination,
not v3-side implementation.

### §4.3 Lock-free symbol-table arena

**Source**: Determinate Nix parallel-eval architecture.

**What**: Symbol table backed by a lock-free contiguous arena.
Improves cache behaviour even single-threaded.

**Relevance to v3**: v3's #781b Schema 9 sparse-symbolTable fix
addressed the *serialisation* cost (297 → 5 ms). A lock-free
in-memory arena attacks the *interning* cost during eval.

**Effort**: 1-2 weeks. **Win**: small (< 5 % wall) but composes with
future parallel eval. **Recommend after broader ICs (§1).**

### §4.4 Tvix's shape-specialised attrset reps (Empty / KV / Map)

**Source**: Tvix attrsets per
`https://tvl.fyi/blog/tvix-status-september-22`.

**What**: One attrset value can be Empty, a single (k,v) pair, or
a full hash-map — chosen dynamically.

**Relevance to v3**: v3's `Alloc::emptyBindingsSentinel()` (per
`DATA_STRUCTURE_AUDIT_2026-05-21.md`) already implements Empty.
Single-KV path is unshipped.

**Effort**: 3-5 days. **Win**: small per attrset (saves Bindings
allocation for singleton attrsets, which the histogram says are
~12 % of all attrsets — already counted in
`attrsetSizeBuckets[1]`). Could save ~5-10 MB on hello.drvPath.

---

## §5 — Memoization / eval-result caching

The memoization research agent's central finding: **no production
system caches intermediate hash-keyed WHNF Values in a lazy
functional language at nixpkgs scale**. v3's hash-keyed eval cache
(per `IFD_DEEP_DIVE_2026-05-21.md` §11) would be genuinely novel
work.

Three flavour choices:

### §5.1 IFD-boundary-only cache (recommended)

**Scope**: cache only at the IFD boundary (`drvPath` after
`coerceToString-with-context`). Key = derivation hash. Value =
post-force WHNF Value.

**Storage**: extend existing per-file disk cache (#777), do NOT
introduce a new SQLite DB.

**Win threshold**: 10× speedup on haskell.nix `materialization`
workload. If < 2× on a 1-hour bench, kill it like Stage 9.

**Effort**: 2-3 weeks for the materialization-retirement program
per IFD §11.

### §5.2 Whole-graph salsa-style incrementality (DON'T)

**Source**: rust-analyzer's salsa-2 → salsa-3 migration caused a 4×
memory regression (5-6 GB → 22-30 GB at startup) that took 6+
months to claw back. nixpkgs's eval graph is deeper than
rust-analyzer's type-resolution graph.

**Verdict**: do not commit. The literature evidence is decisive
against this at nixpkgs scale.

### §5.3 Persistent attrsets (Stage 11 candidate)

**Source**: PERF_STRATEGY Stage 11 candidate.

**Evidence**: CHAMP's published wins are 1.3-6.7× iteration / 3-25.4×
equality but **inserts up to 28 % slower at small sizes**. v3's
hot path per #778 is `GET_LOCAL`/`SET_LOCAL`/`GET_UPVALUE`,
not attrset iteration.

**Verdict**: measure attrset iteration cost on cardano-node before
committing. If iteration is < 10 % of dispatch, kill.

---

## §6 — Negative findings (validated; do not pursue)

The literature confirms multiple directions the team has either
already killed or shouldn't attempt:

| Direction | Status | Source |
|---|---|---|
| Stage 5/6 (hidden classes / PICs for attribute lookup) | KILLED #778 | Codebase agent confirms 2.24 % dispatch share |
| Stage 9 (per-thunk-body content-addressed dedup) | KILLED #772 | Codebase agent confirms 1.17×/1.03× dedup |
| Register-VM rewrite | FALSIFIED #780 (further reinforced by #786) | Dispatch agent confirms: Shi/Gregg 2008 = 15 % win for multi-month effort; #786 OPCYCLES showed real ceiling ~4-5 % wall |
| **Stage 12 (JIT)** — *deferred-with-data 2026-05-23* | DEFERRED w/ revival trigger | `JIT_CONFIDENCE_2026-05-23.md`: dispatch is ~5 % wall (#786 OPCYCLES + #787 RCA, corrected from #780's ~21 % estimate); 3 derivation primops are 99 % of primop wall ≈ 45-55 % of total wall (#788) and JIT cannot reach primop bodies; realistic upside ~10-15 % wall for multi-year cost vs #741 + Tier 1 ICs + nursery default-on at ~1/50th cost. Revival trigger: dispatch > 40 % via OPCYCLES on M5 AND alternatives < 5 % wall to extract |
| Computed-goto on Clang ≥ 18 | Gap closed | Dispatch agent: Nelhage 2025 shows Clang auto-tail-duplicates switch dispatch |
| `musttail` tail-call interpreter | 1-5 % only | Dispatch agent: CPython 3.14 corrected number from 10 % to 1-5 % |
| Hand-asm interpreter (LuaJIT-style) | Beaten by tail-call C++ | Dispatch agent: LuaJIT-Remake beats Mike Pall's asm by 28 % |
| CPR / SpecConstr / Worker-Wrapper-with-unboxing | Same family as killed Stage 5/6 | GHC agent: requires static types, dynamic-typed equivalent is PIC (killed) |
| Whole-graph salsa-style incrementality | Memory regression at scale | Memoization agent: rust-analyzer 4× regression evidence |
| Skip language patterns | Facebook abandoned | Memoization agent: even FB couldn't justify memoization-first language |
| DDlog / Differential Dataflow | 20+ GB memory overhead | Memoization agent: memory blowup at smaller scale than nixpkgs |
| Adapton as library | No production users in 12 years | Memoization agent: research-grade only |
| Optimistic evaluation (Ennals/Peyton Jones 2003) | Removed from GHC | GHC agent: removed due to complexity + instability |
| Context threading (Berndl 2005) | Modern ITTAGE handles it | Dispatch agent: 95 % mispredict win was P4-era; modern Skylake+ BTB neutralises |
| Variable-length opcodes for dispatch perf | No measured win | Dispatch agent: mild i-cache only |

**Do not revisit any of the above under a different name.** Several
are the same family (Stage 5/6 = PIC = CPR/SpecConstr = hidden
classes; Stage 9 = cell dedup = whole-graph salsa = Adapton-style
memoization). The kills are family-wide, not instance-specific.

### §6.1 — Stage 12 (JIT) deferral rationale

The JIT deferral added to the table above is documented in detail in
[`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md). Short
version: the original "JIT addresses dispatch" framing assumed
dispatch was a substantial wall fraction. OPCYCLES (#786) + the
OP_RETURN-misattribution RCA (#787) measured it at ~5 % wall, not
~21 %; per-primop wall-clock (#788) measured 99 % of primop wall in
3 derivation primops accounting for ~45-55 % of total wall. JIT
cannot accelerate primop bodies, so the JIT-addressable fraction is
small. Equivalent or larger reductions are reachable via #741 IFD
cache (Phase 1 landed `23bb231d2`, 56× falsifier margin), AOT
distribution, Tier 1 IC broadening, TOS caching, and nursery
default-on — all on the existing roadmap at ~1/50th the cost.

The deferral is data-driven and reversible: see the revival triggers
in `ROADMAP_TO_VISION_2026-05-15.md` §"Killed-stage revival triggers"
(Stage 12 rows added 2026-05-23).

---

## §7 — Ranked recommendation

Sorted by ROI / effort ratio, applying the measure-twice rule:

### Tier 1 — Highest ROI, well-bounded (~6-10 weeks total)

1. **Broaden inline caches** (§1) — OP_ATTRS_SELECT validation +
   OP_ATTRS_SELECT_DYN + OP_CALL specialisation + primop bridges.
   ~3 weeks; expected 15-30 % wall on real workloads.
2. **A1+A2 cardinality + update-frame elision** (§3) — ~3-4 weeks;
   expected 3-8 % wall but composes with nursery + bigram fusions.
3. **Stack caching / TOS-in-register** (§2) — ~2-4 weeks; expected
   5-15 % wall on local-stack-motion workloads.

**Total expected if all three land**: ~20-50 % wall reduction on
hello.drvPath, compounding with the PERF_AUDIT_2026-05-23 Tier 2
work (~5 %). Closes the gap from ~30× to plausibly ~10-15× without
JIT investment.

### Tier 2 — Composable wins (~3-5 weeks total)

4. **IFD-boundary hash-keyed eval cache** (§5.1) — ~2-3 weeks; the
   materialization-retirement program. Per-eval win unclear but
   transformative for haskell.nix users.
5. **Atomic 3-state thunk machine** (§4.1) — ~1-2 weeks; correctness
   simplification + future-proofing for parallel eval.
6. **Codebase mapping cheap gaps** (`opt_cross_block_cse.cc`,
   deepening `opt_primop_fold`, etc.) — ~1-2 weeks; small wins each
   but cheap to ship.

### Tier 3 — Measurement-gated (~1-2 weeks per spike)

7. **Lock-free symbol-table arena spike** (§4.3) — measure cache
   behaviour delta first.
8. **Tvix single-KV attrset rep** (§4.4) — measure singleton-attrset
   frequency first.
9. **Persistent attrsets / HAMT spike** (§5.3) — measure attrset
   iteration cost on cardano-node before committing.

### Tier 4 — Architectural items deferred

10. **Auto-parallelisation / multi-core capabilities** (Stage 13
    candidate) — defer to `PARALLEL_EVAL_CAPABILITIES_2026-05-18`
    decision point. Process-level fan-out via `nix-eval-jobs` is the
    cheaper alternative that should ship first.

### Tier 5 — Don't pursue

See §6 negative-findings table.

---

## §8 — Coordination with existing roadmap

This document doesn't change `ROADMAP_TO_VISION_2026-05-15.md`'s
stage structure. It supplies the *content* for what fills the
calendar after Stage 5/6/9 kills released ~17 weeks of original
budget.

Mapping:

- **Stage 4 (Uniform STG-shape + strictness)** — A1+A2 cardinality
  + update-frame elision are extensions of this stage. The team's
  v4.3 cross-fn strictness landed; A1+A2 are the next refinement.
- **Stage 7 (Selector thunks)** — partially landed via `selectorSym`
  fast path; can absorb the broader-IC work in §1.
- **Stage 8 (Thin FFI)** — primop bridge ICs in §1 are Stage 8
  work.
- **Stage 13 candidate (Parallel eval)** — §4.1 atomic 3-state thunk
  is a prereq even though Stage 13 itself is pending measurement.
- **Stage 17 (Pattern lint UX)** — unaffected; design landed
  2026-05-22.
- **New work surfaced by this doc**: stack caching (§2), IFD-boundary
  hash-keyed eval cache (§5.1), `opt_cross_block_cse.cc` etc. None
  of these warrant a new stage number; they fit inside existing
  stage scopes.

The reclaimed ~17 weeks from Stage 5/6/9 kills is more than enough
to land Tier 1 + Tier 2 here. **The roadmap calendar is not the
bottleneck post-kills; engineering bandwidth is.**

---

## §9 — Honest limits

1. **The interpreter ceiling is ~1.5-2× native.** Even if all of
   Tier 1 lands perfectly, hello.drvPath will plateau somewhere
   around 5-10× TW (TW itself is a tree-walker with substantial
   overhead; v3 should plausibly beat TW on some workloads).
   Closing further requires JIT (multi-year commitment) which the
   team has not chosen.
2. **The 200× force-rate gap is multi-factor.** Per the
   `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` decomposition,
   it splits into ~5-10× dispatch + ~5-10× GC + ~2-5× allocs +
   unknown caching gap. Each factor closes independently. No
   single optimisation closes all four.
3. **Competitive landscape is converging.** Everyone is doing
   pointer-tagged 1-word values, parallel eval via atomic thunk
   state, shape-specialised attrsets. v3's distinguishing bets are
   (a) bytecode VM inside cppnix, (b) STG-shape closures, (c)
   measurement discipline (hyperfine ≥ 10 runs, kill criteria).
   Don't lose the discipline in pursuit of feature parity.
4. **Eval-result caching is research territory.** No production
   system does what v3's IFD-boundary cache proposes. The Stage 9
   kill should be a reminder that "novel" can mean "doesn't pan
   out at scale." Cheap proxy first.

---

## §10 — Cross-references

- `PERF_AUDIT_2026-05-23.md` — Tier 1+2 cheap wins (bigram fusion,
  dead code, alloc reductions). Compounds with this document.
- `ROADMAP_TO_VISION_2026-05-15.md` — stage structure; this doc
  supplies the content post-Stage-5/6/9 kills.
- `STAGE_9_KILLED_2026-05-22.md` + `STAGE_5_6_KILLED_2026-05-23.md`
  — falsifier evidence the §6 negative findings rest on.
- `MEASURE_TWICE_CUT_ONCE_2026-05-23.md` — every Tier 1-3 item has
  a falsifier; spikes precede commitments.
- `IFD_DEEP_DIVE_2026-05-21.md` §11 — materialization retirement
  program; §5.1 IFD-boundary cache is part of this.
- `DATA_STRUCTURE_AUDIT_2026-05-21.md` — Bindings / Closure / Thunk
  sizing; §4.4 Tvix single-KV attrset proposal extends this.
- `FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md` — TLA+ cell-update
  protocol target becomes more attractive once §4.1 atomic 3-state
  thunk lands.
- `JIT_CONFIDENCE_2026-05-23.md` — Stage 12 (JIT) deferral with
  measured wall-arithmetic and explicit revival trigger. See §6.1
  above; doc backs the JIT row in §6's negative-findings table.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0.
