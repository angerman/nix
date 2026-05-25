# Boehm Dependency Analysis — What stays, what goes, what memory we get back

**Date**: 2026-05-21.
**Question**: How much do we still need Boehm GC once v3 has its own generational
GC (Stage 3 nursery default-on)? Will that let us run in much lower memory,
especially on cardano-node-scale evaluations?
**Method**: Synthesis of `GC-REVIEW.md`, `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`,
`CHENEY_NURSERY_DESIGN.md`, the recent two-round GC audit
(`RCA_VALUEPAIR_EVALUATED_2026-05-21.md` + `GC_AUDIT_ROUND_2_2026-05-21.md`),
and a fresh inventory of Boehm-touching call sites in v3.
**Status**: design / hypothesis document. The committed roadmap goes only as far
as Layer 1 below. Layers 2 and 3 are pending measurement.

## TL;DR

**Boehm cannot be removed** as long as v3 is a library inside the nix CLI
process — cppnix's parser, store, EvalState, IFD realisation, and plugins
allocate `nix::Value` in Boehm-managed memory, and v3 holds `nix::Value*`
across every FFI boundary.

But **Boehm's contribution to peak RSS can shrink in three layers**, only the
first of which is committed:

| Layer | What | Hypothesis (unmeasured) — hello.drvPath / cardano-node peak RSS | Roadmap status |
|---|---|---|---|
| Today | Boehm + nursery off; arena allocate-and-leak | ~1+ GB / ~8-10 GB | current |
| **Layer 1** | Stage 3 nursery default-on — reclaim intermediate thunks | **~300-500 MB / ~2-4 GB** (50-70% reduction) | **committed** (Stage 3) |
| Layer 2 | De-root v3 storage from Boehm (copy-out at FFI exit) | similar peak, faster Boehm mark | partial (Stage 8 framing) |
| Layer 3 | Replace Boehm-tenured with Immix/Whippet (full reclamation) | ~100-200 MB / ~500 MB-1 GB | **not committed** |

The dominant single win is Layer 1. Layer 2 is mostly a perf win on Boehm's
mark-phase cost; the peak RSS gain is smaller because v3 arena memory still
leaks. Layer 3 is the only path to actually reclaiming long-lived v3
allocations — but it's a 6-12 month investment that effectively forks the
project's GC story.

The Phase 1.5 measurement spike (`ACTION_PLAN_2026-05-15.md` lines 65-90) plus
the profiler we just designed (`NIX_PROFILER_DESIGN_2026-05-21.md`) is what
converts the hypotheses above into measured numbers. Decide on Layer 2/3
commitment only after Layer 1 ships and Phase 1.5 returns.

## 1. What Boehm does for v3 today

Three categories of dependency:

### 1.1 Process-level (unavoidable)

cppnix's parser, store layer, `EvalState`, symbol table, IFD realisation,
libfetchers, libstore, libfetchers-c, plugins all link Boehm and allocate
`nix::Value`, `Path`, `StorePath`, etc. in Boehm-managed memory. v3 lives as
a library *inside* this process; the process owns Boehm whether or not v3
participates.

This contribution is **fixed** regardless of v3's GC design — typical baseline
is ~50-100 MB of long-lived TW infrastructure (symbol tables, store-path
caches, parser AST for the whole flake graph + transitive nixpkgs).

### 1.2 Cross-FFI value lifetime (unavoidable today)

Every time v3 calls into TW (store ops, IFD, file I/O, parser, derivation
construction) we get back a `nix::Value*` we must keep alive. The current
mechanism:

- v3 stores the pointer in `Thunk::bridgeSrc` (a `void *` cast of
  `nix::Value *`); per `closure.hh:182`.
- `Thunk` lives in the v3 arena.
- The v3 arena is registered with Boehm via `GC_add_roots(blk, blk + 16MB)`
  per block (`alloc.hh:357`).
- The v3 nursery's 32 MB buffer is also registered via `GC_add_roots`
  (`nursery.hh:275`).
- Bridge tables (`v3BridgeClosures` / `v3BridgeAttrs` / `v3BridgeLists` at
  `primops.cc:3525-3567`) use `traceable_allocator` so their backing storage
  is GC_MALLOC_UNCOLLECTABLE — Boehm sees them.

Result: Boehm's mark phase scans the entire v3 arena + nursery on every
collection, finding TW pointers wherever they live in v3 memory.

### 1.3 Traceable storage for hot v3 state

`VMState::valueStack` and `VMState::withStack` (vm.hh:102, 105) use
`std::vector<Value, traceable_allocator<Value>>`. Their data() backings are
GC_MALLOC_UNCOLLECTABLE. Reason per CRIT-2 fix: previously the
`std::allocator`-backed storage was invisible to Boehm; payloads (Closure*,
Thunk*, Bindings*) only stayed alive accidentally via the conservative C-stack
scan picking up the data() pointer.

This is structurally unnecessary IF v3 never stored `nix::Value*` in
valueStack/withStack — which it doesn't, directly. Indirectly it does, via
`Tag::Thunk{Bridge}` payloads whose bridgeSrc points to TW values. So the
traceable_allocator stays as long as Bridge thunks can appear on the stack.

## 2. What Stage 3 (nursery default-on) actually buys

Per `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` + `project_force_rate_decomposition_2026-05-18.md`, today's ~8-10 GB peak RSS on cardano-node
is dominated by **v3's own arena allocation**, not by TW values:

- A-normal-form IR lowers every binary op + binding into intermediate thunks.
- Tag::App memoization (`d3e41c13d`) adds a Value slot per applied function.
- Bytecode primops allocate Bindings and ListVecs liberally.
- Most of these objects die young (forced once, never re-used) — but the
  arena is allocate-and-leak, so they pin forever.

Stage 3 changes the lifecycle:
- Closure / Thunk / ListVec allocations route to the nursery (32 MB default).
- At scavenge time, only the live subset gets copied to the tenured arena.
- The vast majority — intermediate thunks force-d once — is reclaimed.

Quantitative expectation (structural argument, not yet measured):
- If ~80% of allocations are short-lived (the standard hypothesis behind
  generational GC), Stage 3 reduces the *tenured arena growth rate* by ~5×.
- Peak RSS scales roughly with tenured arena watermark, not total alloc volume.
- Hence the ~50-70% reduction estimate above. Falsified or confirmed by Phase 1.5.

Important: Stage 3 does **not** make Boehm's mark phase free. Boehm still
scans the (smaller) tenured arena. The win is in *total memory pinned*, not
in Boehm's mark cost per se.

## 3. Layer 2 — de-root v3 storage from Boehm (partial Stage 8)

If — and only if — v3 stops storing `nix::Value*` directly in long-lived v3
storage, we can remove the `GC_add_roots` calls on the arena + nursery.
Then Boehm's mark phase ignores v3 storage entirely and scales only with TW
heap size.

The technical path:
1. Bridge thunks copy out at FFI exit: every `nix::Value*` returned from TW
   gets walked + translated into a v3 Value tree at the bridge boundary. The
   TW pointer is then discarded. (Cost: O(returned-value graph) per FFI call.)
2. Bridge tables (`v3BridgeClosures/Attrs/Lists`) drop `traceable_allocator` —
   they hold v3-side wrappers only, no TW pointers.
3. valueStack/withStack drop `traceable_allocator` — they're walked by v3's
   own scavenger which doesn't need Boehm.
4. Delete the `GC_add_roots` calls in `Arena::refill()` and `Nursery::initLazy()`.

Cost per FFI call: walking the returned `nix::Value*` graph at copy-out time.
For shallow values (strings, integers, paths) this is cheap. For deep
attribute sets returned from `derivationStrict` or `getEnv` it could be
significant. **The FFI surface today is mostly shallow** per
`FFI_AUDIT_2026-05-20.md` — typical bridge return values are strings or
small attrsets — so copy-out is probably affordable, but worth measuring.

Benefit: Boehm's mark phase drops from O(v3 arena + TW heap) to O(TW heap).
On hello.drvPath that's the difference between scanning ~1 GB (mostly v3) and
~50-100 MB (TW only). The peak RSS doesn't shrink much (v3 arena still leaks),
but the per-collection latency drops and Boehm's behaviour under pressure
improves.

**Roadmap status**: Stage 8 (Thin FFI) reduces the FFI surface but is *not*
framed as a Boehm-detachment goal in `ROADMAP_TO_VISION`. The two are
complementary — narrower FFI makes Layer 2 cheaper because there are fewer
copy-out call sites to pay. Worth re-evaluating Stage 8's framing post-Layer-1
measurement.

## 4. Layer 3 — full tenured GC (NOT committed)

`GC-REVIEW.md` §4 lays out the only path that actually reclaims long-lived
v3 allocations: replace the Boehm-tenured fallback with a precise (or
hybrid conservative+precise) collector. Two candidates:

### 4.1 Whippet (Andy Wingo)

Designed as a Boehm replacement; embeddable C library; supports both
conservative and precise modes. The strongest off-the-shelf candidate per
GC-REVIEW.md §3 table. Adoption cost: ~6 months for the v3 tenured side,
longer if we want to migrate cppnix too.

### 4.2 GenImmix (custom, GHC/V8 territory)

Best end-state per GC-REVIEW.md but needs precise stack maps for primop
bodies. ~6-12 person-months. The literature target if shipping speed didn't
matter.

### 4.3 Cheney bigger nursery + Whippet tenured

Hybrid: keep v3's Cheney nursery, replace the tenured arena with Whippet.
v3 owns its memory lifecycle end-to-end. Boehm survives only for cppnix's
side of the process.

This is the architectural endpoint that closes the "Generational GC" scorecard
row meaningfully (not just "nursery default-on" but full reclamation). Today
the roadmap parks at Stage 3 = generational *write-once* into tenured;
"Generational GC ✅" in the end-state scorecard is honest only relative to
the nursery, not absolute generational semantics.

**Not committed**. Reasonable post-Stage-7 follow-on if Phase 1.5 + Stage 3
measurements indicate the tenured-leak ceiling is the dominant remaining
factor. Document this commitment criterion below.

## 5. Memory ceiling expectations (concrete hypotheses)

The numbers below are **structural hypotheses**, not measurements. They are
the falsifiable claims this document commits to.

| State | hello.drvPath peak RSS | cardano-node peak RSS | Boehm mark cost (per collection) |
|---|---|---|---|
| Today (Phase A nursery off) | ~1+ GB | ~8-10 GB | dominant in GC-scan factor of the 200× force-rate gap |
| **Stage 3 only (committed)** | **~300-500 MB** | **~2-4 GB** | drops ~half (smaller tenured arena to scan) |
| Stage 3 + Layer 2 (Stage 8-adjacent) | ~250-400 MB | ~1.5-3 GB | drops near-zero for v3 storage; only TW heap remains |
| Stage 3 + Layer 2 + Layer 3 (Whippet tenured) | ~100-200 MB | ~500 MB-1 GB | only TW heap left to scan |

Each cell is a falsifiable prediction. The Phase 1.5 measurement spike with
the `NIX_PROFILER_DESIGN_2026-05-21.md` data layer + `PERF_TRACE_TOOL_DESIGN_2026-05-20.md`
time-series tool produces the actual numbers.

## 6. Why the hypothesis is "most allocations are short-lived"

The generational hypothesis (Ungar 1984, refined by every modern GC since)
says that for typical workloads, the vast majority of allocations die before
their first survivor collection. For lazy functional languages with thunks
this is even more pronounced — every binary op, every let-binding, every
binary primop creates a thunk that may never be forced or, if forced, is
forced exactly once.

v3-specific evidence supporting this:
- Per `project_thunk_all_perf_2026-05-12`: 10.04 M thunks allocated /
  3.30 M forced (33.6% forced ratio) on hello.drvPath. 6.75 M thunks
  (= 526 MB) are pure waste — pinned in the arena, never forced.
- Per `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md`: factor ~2-5× of the
  200× per-force gap is "extra intermediate allocations" (vs TW which doesn't
  allocate those because it walks the AST).
- Per the Bindings size histogram in `SESSION_PLAN_2026-05-20`: 97K
  zero-sized Bindings allocated (already deduped to sentinel post-Phase A);
  hundreds of thousands of size-1 and size-2 bindings.

The signal is strong that most allocations are dead-on-arrival. Generational
reclamation will recover the bulk of that mass.

But this is still a hypothesis until measured. If Phase 1.5 shows the live
ratio is closer to 50% (rather than the expected ~20%), the Stage 3 win is
proportionally smaller, and Layer 3 becomes more urgent earlier.

## 7. Falsifiable claims (Rule 0)

This document commits to:

1. **Boehm process-level overhead is bounded by TW state** (parser AST, symbol
   table, store, EvalState, plugin allocations). Measurable as the peak RSS
   of a v3 process that does NOTHING but `runRootExpr` on the empty
   expression `null`. Hypothesis: ~50-100 MB.

2. **Stage 3 reduces hello.drvPath peak RSS by ≥ 40%**. Falsified if Phase 1.5
   measurement shows < 40% reduction. Mitigation if falsified: investigate
   which long-lived allocations dominate the tenured arena (probably Bindings
   of size > 100 per the histogram), apply targeted shrinking.

3. **Boehm mark-phase cost is proportional to v3 arena size today**. Falsified
   if removing the `GC_add_roots` calls produces no measurable difference in
   Boehm GC duration. Mitigation if falsified: Layer 2 isn't worth doing.

4. **Layer 3 (Whippet tenured) closes the remaining 50-70% of peak RSS**.
   Falsified only by actually implementing it. Pre-commitment criterion:
   only commit Layer 3 if Phase 1.5 + Stage 3 measurements show that the
   tenured-leak watermark exceeds a threshold (e.g., > 1 GB on cardano-node
   after Stage 3 ships).

5. **v3 NEVER deletes Boehm from its build**. Boehm is in the nix CLI process
   forever. The question is only how much of v3's memory is *under* Boehm's
   eye.

## 8. Measurement plan (Phase 1.5)

The `NIX_PROFILER_DESIGN_2026-05-21.md` data layer instruments per-position
allocation; the `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` time-series tool
instruments wall-clock RSS + Boehm heap size. Combined, they answer:

- **What is the live ratio?** Cumulative bytes allocated / peak tenured arena.
  Expected ~5:1 if the generational hypothesis holds; ~2:1 if it doesn't.
- **What is the Boehm mark-phase cost today?** Time spent in Boehm GC (via
  `GC_get_full_gc_total_time` / `GC_get_gc_no` deltas). Compares to v3
  scavenge time.
- **What grows the tenured arena?** Per-position allocation totals filtered
  by "byte-bucket survived scavenge" — i.e., the long-lived allocations.
  Lists positions to attack with shrinking or Stage 4 strictness.
- **What is the FFI-return-value size distribution?** Walks every
  `treeWalkerToV3` call and records value-tree size. Tells us how expensive
  Layer 2 (copy-out at FFI exit) would be.

Output: a measurement report that confirms / falsifies claims 1-4 above and
re-prioritises Stage 3 / Stage 8 / candidate Layer 3 accordingly.

## 9. Roadmap implications

### Stage 3 (committed)

Already-correct framing in `ROADMAP_TO_VISION` §"Stage 3" + the new Phase 1.7
in `ACTION_PLAN`. No change needed — this document just explains *why* Stage 3
delivers the dominant memory win even with Boehm staying.

### Stage 8 (re-evaluate framing post-Layer-1)

`ROADMAP_TO_VISION` §"Stage 8" frames Thin FFI as "ffi.cc documented surface;
bridge_yield gone." This is correct as far as it goes but doesn't capture
the Layer 2 opportunity (Boehm de-rooting). After Phase 1.5 measurement,
revisit whether Stage 8 should include "remove `GC_add_roots` from v3 arena
+ nursery" as an explicit deliverable — and whether the copy-out at FFI
boundaries is affordable enough to commit.

Don't change the ROADMAP until measured. The framing change is a Phase 1.5
verdict, not a 2026-05-21 decision.

### Layer 3 (commitment-gated post-Stage-7)

If Phase 1.5 + Stage 3 leaves cardano-node at > 1 GB peak RSS, OR if Boehm
mark-phase remains > 10% of total eval time after Layer 2, **then** Layer 3
becomes a serious candidate post-Stage-7. Candidate Stage 16 ("v3-tenured
GC: Whippet adoption") would be added to `## Candidate future stages` with
its own design doc.

Don't promote to committed without measurement.

## 10. Self-critique

**Critique 1**: the ~50-70% hypothesis for Stage 3 assumes the live ratio
on cardano-node matches hello.drvPath's. cardano-node has 100-1000× more
attribute sets, more cross-flake imports, more IFD. The live ratio could be
*higher* (more state retained for the bigger module-system evaluation) →
smaller Stage 3 win. Or *lower* (more redundant builder construction) →
bigger win. Genuinely unknown without measurement.

**Critique 2**: Layer 2 assumes copy-out at FFI exit is cheap. For
`derivationStrict` returning an attrset with hundreds of entries (cardano-node
has these), the copy is non-trivial. Worth profiling before committing.

**Critique 3**: this document mixes "what Boehm costs" (mark-phase time) with
"what Boehm pins" (memory ceiling). They're related but distinct — mark cost
is per-collection wall time; pinned memory is steady-state RSS. The three
layers shrink them differently. Specifically:
- Stage 3 shrinks pinned memory ~5×, mark cost ~2×.
- Layer 2 shrinks mark cost ~10× for v3 portion, pinned memory unchanged.
- Layer 3 shrinks pinned memory another ~3×, mark cost mostly unchanged
  (Whippet still scans tenured).

**Critique 4**: the document doesn't address concurrency. Boehm is not
GIL-safe; if v3 ever ships parallel eval (candidate Stage 13), Boehm becomes
a contended bottleneck. Whippet is concurrent-friendly. This is a separate
axis from RSS savings and is documented in
`PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`.

**Critique 5**: I haven't accounted for the cost of the v3 scavenger itself.
Stage 3 trades "Boehm scanning arena" for "v3 scavenger walking nursery
roots." Net win depends on workload. On hot loops with tight nursery
overflow, the scavenger fires every few ms; that's not free.

## 11. Cross-references

- `GC_BUILD_VS_BUY_2026-05-21.md` — companion doc; explains why we continue
  hand-rolling the v3 GC rather than adopting Whippet / MMTk / GHC's RTS.
  Same-day, complementary scope: Boehm-dependency vs build-vs-buy.
- `GC-REVIEW.md` — the original architectural critique of v3's
  bump-arena-plus-Boehm approach; §3 Strategy table + §4 phased plan are
  the parent of this document.
- `CHENEY_NURSERY_DESIGN.md` — Stage 3's source design.
- `NURSERY_PHASE_D_DESIGN_2026-05-18.md` — write barriers required before
  Stage 3 default-on.
- `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` — the 200× force-rate
  decomposition that motivates the GC-scan factor.
- `RCA_VALUEPAIR_EVALUATED_2026-05-21.md` + `GC_AUDIT_ROUND_2_2026-05-21.md`
  — recent GC correctness audits; the missed-root inventory affects Stage 3
  readiness (Phase 1.7).
- `ACTION_PLAN_2026-05-15.md` Phase 1.5 — workload measurement spike;
  produces the falsifiers for claims 1-4 above.
- `ACTION_PLAN_2026-05-15.md` Phase 1.7 — Stage 3 readiness GC correctness
  fixes; added 2026-05-21.
- `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` — time-series CPU/RSS/Boehm-heap
  sampler; the RSS + Boehm-heap track is the primary measurement
  instrument for claims 2-4.
- `NIX_PROFILER_DESIGN_2026-05-21.md` — per-position attribution; surfaces
  the long-lived allocations that need attacking.
- `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` — concurrency axis; orthogonal
  to RSS but informs the Whippet decision if Stage 13 ever commits.
- `ROADMAP_TO_VISION_2026-05-15.md` §"End-state target" — the scorecard
  row "Generational GC ✅ (nursery default-on; closure-pool retired)" is
  technically met by Stage 3; absolute generational reclamation requires
  Layer 3.

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
