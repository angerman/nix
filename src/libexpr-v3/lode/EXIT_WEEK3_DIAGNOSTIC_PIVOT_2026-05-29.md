# EXIT Week 3 — Pivot from Immix re-eval to Diagnostic Builds

**Date:** 2026-05-29 (evening, post-Day-2 re-measure)
**Status:** **STRATEGIC PIVOT** — Week 3 is no longer "Immix M5 SHIP-gate re-measurement" but **building distribution-shaped diagnostics first** per `DIAGNOSTIC_AUDIT_2026-05-29.md §9` + user directive to "properly count REAL allocations" rather than rely on OS RSS.
**Supersedes:** `EXIT_WEEK3_DECISION_2026-05-29.md` §5.2 (Immix re-measure plan)

---

## 1. What triggered the pivot

Two simultaneous findings, evening 2026-05-29:

### 1.1 Immix infrastructure has a latent correctness bug

Single-shot HNE eval under `NIX_V3_MAJOR_GC=1 V3_DBG_FREELIST_REUSE=1`:

```
v3 major-mark-sweep: closures=11574 thunks=496444 bindings=21581
   lists=3510 pairs=1866132 cells=474343 chars=955626 cons=146193
   consW=2940085 markedCells=3974996 blocks=44 hugeMarked=407
   markMs=11852.31 sweepMs=1405.34
v3 sweep: blocksScanned=44 liveCells=3458697 deadCells=816297
   liveBytes=476.4MB deadBytes=261.8MB reclaim%=35.5%
   freelist_entries=816297 blocksFreed=0 bytesFreed=0.0MB
v3 line-marks: blocks=45 totalLines=5898240 deadLines=2006870 deadPct=34.02%
v3-direct ABORT alloc: thunksForced=177692 insns=12337843
error: v3 OP_REC_BINDING_SLOT_REF: source is not a forced attrset
```

The major-mark-sweep ran successfully (35.5% reclaim) — then OP_REC_BINDING_SLOT_REF saw something it interprets as a non-forced attrset.  Mark-sweep is either freeing too aggressively, or interacting badly with the App3 rollback (which restored the legacy 2-pair encoding the sweep walks).

This isn't a fast fix.  Even if Immix were ready, we have NO measurement infrastructure to grade its M5 yield correctly — see §1.2.

### 1.2 User directive: stop relying on OS RSS

> "We should make sure we don't rely on OS's RSS values, but properly count _real_ allocations, incase the system's RSS values are off. (GHC pre-allocates 1TB of virtual memory, but never uses that much). We really need precise, and proper time and memory profiling for all phases, and stages, so we can correctly measure the impact. 'elsewhere' is a rather ominous bucket."

This validates the audit's central claim and the variance findings:
* Morning M5 σ=95 MB; afternoon σ=308 MB.  Same code.  OS-level page-eviction noise.
* HNE peak today varies 2152-2472 MB across measurements of the same binary.
* M5 "247 MB OVER watchdog" (Day 12 amendment) may be within noise envelope.
* "elsewhere" bucket (peak - arena - boehm) is the OS's "I don't know what this is" — exactly ominous.

**Conclusion:** every Week 3 lever decision sitting on peak_rss data is on sand.  Build the distribution-shaped, source-attributed, allocation-counter-based instruments FIRST, then revisit lever choice.

## 2. The new Week 3 plan (diagnostic builds, ranked by impact-per-effort)

Per `DIAGNOSTIC_AUDIT §6` + user directive on phase/stage allocation accounting:

| Day | Build | Effort | Outcome |
|---|---|---:|---|
| 16 | **DIAG-1**: Per-cycle GC CSV | ~0.5 d | Re-grade F2 / PHASE_4 verdicts; distribution not aggregate |
| 16-17 | **DIAG-3**: Per-Tag L(t) time-series | <1 d | Multi-dimensional L; pinpoint Tag growth |
| 17-18 | **DIAG-4**: Phase decomposition | 1-2 d | Attribute "elsewhere" + pre-eval/eval/post-eval bytes; addresses user's ominous-elsewhere call |
| 18-22 | **DIAG-2**: Per-PosIdx live-bytes rollup | 3-5 d | Source-level attribution; "84% Bindings" → "fixed-points.nix:95 = 85 MB" |

Total: ~5-8 days for the distribution-shaped fundamentals.

Skipping audit §6.5 (flat MS quadratic fix) until DIAG-1 grades whether flat MS is rescuable.  Skipping §6.6 + §6.7 (alloc-tick + retainer sampling) — both are 2-3+ days each and depend on the foundation laid by DIAG-2.

### 2.1 Pre-committed acceptance per build

Per `[[measure-twice-cut-once]]`, each diagnostic's acceptance criterion is set BEFORE building:

**DIAG-1 (Per-cycle GC CSV):**
* CSV writes one row per `runMajorMarkSweep`
* Columns: cycleIdx, trigger_reason, markMs, sweepMs, blocksScanned, blocksFreed, liveCells, deadCells, bytesFreed, freeListEntryCount, allocSincePrev, wallSincePrev
* Acceptance: on hello.drvPath single run, ≥1 cycle row emitted with non-zero markMs+sweepMs
* Decision unlock: if p99/p50 sweepMs > 5×, F2 verdict invalidated → flat MS may be rescuable

**DIAG-3 (Per-Tag L(t)):**
* CSV from `NIX_V3_LIVE_TRACE_PERIODIC=K` extended with per-Tag columns (live_closures_mb, live_thunks_mb, live_bindings_mb, live_lists_mb, live_pairs_mb)
* Acceptance: on HNE, columns sum within 5% of total live; per-Tag time-series visually informative

**DIAG-4 (Phase decomposition):**
* Counters: phaseBytesParse / phaseBytesLower / phaseBytesEvalPrimop / phaseBytesEvalMain / phaseBytesSerialize
* Phase-switch enter/exit at well-defined boundaries (lower, runMain, primop dispatch, serialize entry)
* Acceptance: phase sums to ~total bytes; "eval" phase dominates on hello/HNE; reveals cache-load vs eval-growth split (target of audit §6.4)
* Decision unlock: knows whether "elsewhere" is cache-state vs runtime state

**DIAG-2 (Per-PosIdx live-bytes rollup):**
* Extend LiveTracer::walkBindings + walkThunk with `unordered_map<posHandle, LivePosEntry>` aggregation
* Dump top-20 retainers by bytes at end-of-run + on `NIX_V3_LIVE_TRACE_PERIODIC`
* Acceptance: top-20 retainers cover ≥80% of Bindings+Thunk bytes; source positions visually meaningful

## 3. What's deferred

* **Immix M5 re-measurement** — deferred until DIAG-1 grades the actual sweep distribution.  If sweep distribution shows the F2 verdict was wrong, flat MS may rescue ahead of Immix.  If sweep distribution confirms F2, Immix re-measurement happens AFTER its OP_REC_BINDING_SLOT_REF bug is debugged.
* **Phase 4b LRU implementation** — deferred until DIAG-4 grades cache contribution per-phase.  The cache eviction projection (500-950 MB on HNE) may shrink or grow significantly under per-phase attribution.
* **mergeBindings pattern fix / Phase E v0.2 ship-readiness / Stage 4 v4+** — all deferred until DIAG-2 reveals source-attribution.  May or may not be the right levers depending on retention concentration.

## 4. What stays valid

* The Week 1 retrospective's bundle measurement (HNE -105 MB peak, -117 MB arena; M5 -219 MB peak wide-CI, -822 MB arena deterministic) stays the load-bearing claim because:
  - HNE Δpeak signal is 2σ — robust to noise within reason
  - Arena counter is deterministic — independent of OS RSS
  - The bundle's Δarena replicates across measurement sessions
* The App3 rollback stays — it killed a 3× allocation regression (deterministic counters confirmed; not OS-RSS-dependent).
* The fakeClo wire-back + capWiths intern stay — both deliver REAL allocation count reductions, independent of how we measure peak.

## 5. Methodology improvements codified by this pivot

* **OS RSS is a proxy, not a measurement.**  Treat `peak_rss` as informative but not authoritative.  Trust `v3_arena` (deterministic v3-counter) + per-Tag/per-PosIdx byte counters when they exist.
* **"Elsewhere" needs decomposition.**  Per DIAG-4, the elsewhere bucket should be attributed to specific phases (parser/lower/eval/serialize) + specific allocators (importCache, SQLite, std::vector growth).  Until DIAG-4 lands, treat any "elsewhere = X MB" claim as a residual, not a measurement.
* **Distribution > aggregate.**  Per audit §5 meta-pattern + measure-twice §5.7 (+ proposed §5.8): for any SHIP-gate decision, distribution-shape instruments are required.  Aggregates can confirm direction; distributions are needed to grade magnitude + spread.
* **OS-RSS variance is environmental.**  Same code, same N=10, σ can vary 3× across hours of the same day.  N=10 is insufficient for sub-σ-floor signals; rely on deterministic counters for tight thresholds.

## 6. Operational ordering

The diagnostic builds will be SEQUENTIAL not parallel (avoid building on shaky data from concurrent builds):

1. DIAG-1 first (smallest; rescues F2 retrospectively)
2. DIAG-3 next (small extension; multi-dimensional L)
3. DIAG-4 third (medium effort; attribution; addresses ominous-elsewhere)
4. DIAG-2 last (largest; foundation for source-level attribution)

After all four land, re-engage `EXIT_GC_SPIRAL_PLAN` §5 lever selection — but now with grounded instrumentation.

## 7. Cross-references

* [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) — the strategic audit driving this pivot
* [`NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md`](NIX_MEMORY_PROFILER_DESIGN_2026-05-27.md) §5.1 — DIAG-2 is its Phase 1
* [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) — DIAG-1 + DIAG-3 are sibling subsets
* [`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md) — bundle ships per arena counter (deterministic; survives RSS noise concerns)
* [`EXIT_WEEK3_DECISION_2026-05-29.md`](EXIT_WEEK3_DECISION_2026-05-29.md) — Immix re-measure plan that this supersedes
* [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 + §5.8 candidate — moment-vs-distribution
* Memory: [[bench-binary-fingerprint]] + [[arena-over-ram-peak-noise]] — both contributed to surfacing the OS-RSS-is-not-measurement realization

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
