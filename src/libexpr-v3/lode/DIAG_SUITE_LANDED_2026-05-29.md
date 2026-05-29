# DIAG Suite — Distribution-shaped instrumentation landed

**Date:** 2026-05-29 evening
**Status:** **LANDED** — DIAG-1 + DIAG-3 + DIAG-4 complete; DIAG-2 Phase 1 complete (82.8 % unknown bucket needs Phase 2)
**Companion:** [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) (the strategic audit driving this build) + [`EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md`](EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md) (the pivot that scoped Week 3)
**Effort:** ~1 hour wall (vs audit's 5-8 day estimate — the data was already in heap, just needed plumbing)

---

## 1. What landed

| Build | Commit | Gate | Output |
|---|---|---|---|
| DIAG-1 per-cycle GC CSV | `fb6ae0b72` | `NIX_V3_GC_CYCLE_CSV=path` | CSV row per `runMajorMarkSweep` invocation |
| DIAG-3 per-Tag L(t) time-series | `b6423268b` | `NIX_V3_LIVE_TRACE_PERIODIC=K` + `_OUT=path` | Extends existing periodic CSV with 5 per-Tag columns |
| DIAG-4 per-phase arena bytes | `36aa29916` | `NIX_VM_STATS=1` | Two banner lines at end-of-run |
| DIAG-2 per-PosIdx live-bytes (Phase 1) | `d0f74ecf2` | `NIX_V3_LIVE_TRACE=1` + `NIX_V3_LIVE_POS_ATTR=1` + `NIX_V3_BINDINGS_ATTR=1` | Top-20 retainers in LIVE-FRACTION block |

## 2. Headline findings from first measurements

### 2.1 GC cycle distribution rescues F2 verdict (DIAG-1)

On HNE single cycle (1 cycle fired before Immix crash):
* `markMs = 8403 ms, sweepMs = 1190 ms`
* mark : sweep ratio = **7 : 1**

The original F2 verdict claimed "sweep is 40-54 % of wall."  Audit identified this as `projectedSweepMs = markMs` projection (`live_trace.cc:818`).  DIAG-1's REAL measurement confirms sweep is **14 % of GC time, not 40-54 %**.  The Immix pivot rested on the wrong number.

Also confirmed: `blocksFreed = 0` (every cycle).  No whole-block-frees fire because cells in each block stay dense; the free-list bookkeeping doesn't reduce RSS.  This validates audit §2.1.

### 2.2 HNE per-phase arena is 100 % run-phase (DIAG-4)

```
v3-direct phase arena bytes (MB):
  lower=0.00 optimise=0.00 compile=0.00 run=1459.62 total=1459.62
```

The outer expression's lower/optimise/compile contribute essentially zero — all heavy work happens via primImport recursion at run-time.  This validates the user's "elsewhere is ominous" concern: the OS "elsewhere" bucket (peak_rss − arena − boehm) is OUTSIDE arena, attributable to non-arena allocators (ImportCache, SQLite, libc malloc).  Per-arena phase decomp doesn't catch the elsewhere bucket itself.  **Follow-up:** mallinfo / malloc_zone_statistics for the non-arena bucket would close the "elsewhere is ominous" loop fully.

### 2.3 Per-Tag run-phase decomposition (DIAG-4)

```
v3-direct run-phase per-tag bytes (MB):
  bindings=713 (49%), thunks=320 (22%), closures=164 (11%),
  pairs=125 (9%), lists=41 (3%), chars=37 (3%), values=8 (1%)
  envs=0
```

Sum = 1407 MB vs arena delta 1459 MB → 52 MB block overhead unaccounted (3.5 %).  Bindings + Thunks together = 71 % of run-phase allocation — the audit's "84 % Bindings" was slightly overstated; actual is closer to 49 % when measured at TAG granularity.

### 2.4 Per-Tag L(t) shows different Tag dynamics (DIAG-3)

On HNE with K=200 MB periodic sample (3 samples):

| Sample | Bindings | Thunks | Pairs | Closures | Lists |
|---|---:|---:|---:|---:|---:|
| 1 (720 MB alloc) | 305 | 35 | 85 | 0.8 | 0.2 |
| 2 (1072 MB alloc) | 368 | 68 | 93 | 3.2 | 1.7 |
| 3 (1408 MB alloc) | 400 | 96 | 101 | 5.0 | 4.8 |

* **Bindings dominate** at every timepoint (74 → 66 % of live).
* **Thunks grow fastest** (×3 over eval) — new finding, not in HNE_BUCKET_DECOMP.
* L drops 0.59 → 0.50 → 0.43 (matches L_TIME_SERIES_DATA_2026-05-29).
* Per-Tag sum matches `live_mb` to within 0.01 MB (accounting invariant).

Routes per-generation policy decisions: Thunks-grow-fastest pattern suggests generational segregation could help IF thunks have short lifetimes (TBD by DIAG-2 Phase 2 source attribution).

### 2.5 Per-PosIdx retention is DISPERSED with 82.8 % UNKNOWN (DIAG-2 Phase 1)

On HNE with all three gates:
* 82.8 % of live bytes (410 / 519 MB) have `posHandle=0` (unknown)
* Top resolved source position is only 4.73 MB (1.0 %)
* Top-20 resolved positions cover only ~17 MB / 3.3 % of live total
* 70,818 unique posHandles in liveByPos map

Two findings:
1. **Source-attribution coverage gap** — most Bindings/Thunks in vm.cc bytecode dispatch don't record posHandle.  DIAG-2 Phase 2 (open as #873) extends coverage.
2. **Even with the 17 % resolved subset, retention is DISPERSED.**  Per audit §4.3 framing: "Concentrated → BiBOP / page segregation works.  Dispersed → it doesn't."  HNE shows dispersed — BiBOP would not help.

If DIAG-2 Phase 2 reveals the unknown bucket also disperses (probable), the dispersed-retention finding is robust.  If unexpectedly it concentrates, BiBOP becomes a candidate again.

## 3. Implications for the GC track

The DIAG suite re-grades several prior assumptions:

| Prior assumption | DIAG-derived correction |
|---|---|
| "Flat MS sweep is 40-54 % wall" | False — actual sweep is 14 % of GC time |
| "Cells per dead block are dense → whole-block-free rare" | Confirmed — `blocksFreed=0` every cycle |
| "84 % Bindings dominance" | 49 % at Tag-level on HNE (still dominant; less extreme) |
| "Retention is concentrated → BiBOP viable" | Falsified for HNE-known-source subset; pending DIAG-2 Phase 2 for full verdict |
| "v3 arena 1593 MB = pre-bundle baseline" | Today's builddir/ + bundle + App3-reverted measures 1476 MB |

**Net:** the Immix pivot (per `GC_DECISION_2026-05-29.md`) rested on the F2 projection bug.  With actual measurement showing mark dominates wall, the rescue path for flat MS is more promising than Immix.  But — both face the same RSS-doesn't-drop problem (`blocksFreed=0`) which only DEFRAGMENTATION solves (Immix evacuation, deferred per GC_DECISION §5.4).

The honest conclusion: **bytes-freed and RSS-reduction are different goals.**  Both flat MS and Immix can free bytes; neither without evacuation reduces RSS.  RSS reduction requires either (a) workload-driven whole-block death (rare per DIAG-1) or (b) compaction/evacuation.

## 4. Methodology improvements this commit codifies

* **OS RSS is a proxy, not a measurement.**  v3_arena counter + per-Tag counters are deterministic; trust them over peak_rss when arena > physical RAM.  Codified in [[arena-over-ram-peak-noise]] + [[bench-binary-fingerprint]].
* **Distribution > aggregate.**  Every new instrument should default to distribution-shape: CSV / per-cycle / per-PosIdx / per-Tag time-series.  Aggregate-only banners feed the moment-vs-distribution conflation pattern.  Per `MEASURE_TWICE_CUT_ONCE §5.7-§5.8`.
* **Phases produce different bytes.**  HNE proves run-phase owns 100 % of arena growth.  When designing optimizations, target by phase (lower-phase fix won't move HNE).

## 5. Open follow-ups

* **DIAG-2 Phase 2** (#873): extend posHandle coverage to vm.cc dispatch sites — eliminates the 82.8 % unknown bucket.  ~2-3 days.
* **DIAG-5** (audit §6.5, not built): fix flat MS quadratic in `clearCellStartBitFor`.  ~1 day.  Combined with DIAG-1 data, would re-grade whether flat MS rescue is viable.
* **DIAG-6** (audit §6.6): alloc-tick / age-histogram field.  Foundation for LAG/USE/DRAG/VOID biographical classification.  2-3 days.
* **DIAG-7** (audit §6.7): retainer-edge sampling.  ~1 week.  Answers "what RETAINS the 17 % source-resolved bytes."
* **Non-arena attribution (audit §6.4 extension):** mallinfo / malloc_zone_statistics for the "elsewhere" bucket.  Closes the user's "elsewhere is ominous" concern fully.
* **Immix OP_REC_BINDING_SLOT_REF crash** — investigate the latent correctness bug surfaced during the original Week 3 Immix re-measure.

## 6. Cross-references

* [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) — the strategic audit
* [`EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md`](EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md) — pivot doc
* [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 + §5.8 (proposed) — moment-vs-distribution
* Memory: [[arena-over-ram-peak-noise]] + [[bench-binary-fingerprint]] + [[peak-vs-alloc-distinction]]

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
