# Falsifier #1 — Immix line-occupancy probe (Step 8)

**Date:** 2026-05-29
**Status:** EMPIRICAL MEASUREMENT — Immix PASSES the pre-committed threshold on BOTH hello.drvPath and HNE
**Task:** #844

**Pre-committed threshold (per `GC_DESIGN_POST_CHENEY_2026-05-28 §5.1`):**
≥30% lines fully dead → Immix path justified (~4 KLoC, 4-6 wk).
<30% → flat MS path dominates on LoC/risk.

---

## 1. TL;DR

**Immix PASSES with ≥30% line-dead fraction across all probed line-sizes (64/128/256/512 B) on both workloads.**

| Workload | 64 B | 128 B (Immix default) | 256 B | 512 B |
|---|---:|---:|---:|---:|
| hello.drvPath | 46.8% | **46.5%** | 46.3% | 46.1% |
| HNE | 53.1% | **50.5%** | 46.8% | 41.9% |

The probe — already implemented at `live_trace.cc:975-1182` and gated `NIX_V3_BLOCK_PROBE=1` + `NIX_V3_LIVE_TRACE=1` for active-VMState walk — partitions each 16 MB arena block into 128 B lines, marks any line containing ≥1 live byte, reports the fully-dead-line fraction.

**At Immix's canonical 128 B line size, 46.5-50.5% of lines are fully dead** — well above the 30% threshold.  Bump-realloc within reclaimed lines would deliver ~265 MB recoverable on hello.drvPath, ~796 MB on HNE.

---

## 2. Full measurement results

### 2.1 hello.drvPath

```
total arena: 570.4 MB across 34 blocks (kBlockSize=16 MB)
live bytes:  ~302 MB

Line size 64 B (8912896 lines):
  pinned (live):    4745050 lines (303.7 MB)
  fully dead:       4167846 lines (266.7 MB) = 46.8%
  blocks: all-dead=13, mostly-dead=3, mixed=0, mostly-live=9, all-live=9

Line size 128 B (4456448 lines):
  pinned (live):    2384443 lines (305.2 MB)
  fully dead:       2072005 lines (265.2 MB) = 46.5%
  blocks: all-dead=13, mostly-dead=3, mixed=0, mostly-live=5, all-live=13

Line size 256 B (2228224 lines):
  fully dead:       1030579 lines (263.8 MB) = 46.3%
  blocks: all-dead=13, mostly-dead=3, mixed=0, mostly-live=3, all-live=15

Line size 512 B (1114112 lines):
  fully dead:       513061 lines (262.7 MB) = 46.1%
  blocks: all-dead=12, mostly-dead=3, mixed=1, mostly-live=2, all-live=16
```

**hello shows 13 blocks (38% of arena) all-dead at 64 B granularity — these are recoverable to libc under Immix-style mark-region.**

### 2.2 HNE

```
total arena: 1577.1 MB across 94 blocks
live bytes:  ~781 MB

Line size 64 B (24641536 lines):
  pinned (live):    11545741 lines (738.9 MB)
  fully dead:       13095795 lines (838.1 MB) = 53.1%
  blocks: all-dead=0, mostly-dead=20, mixed=73, mostly-live=1, all-live=0

Line size 128 B (12320768 lines):
  pinned (live):    6100066 lines (780.8 MB)
  fully dead:       6220702 lines (796.2 MB) = 50.5%
  blocks: all-dead=0, mostly-dead=4, mixed=89, mostly-live=1, all-live=0

Line size 256 B (6160384 lines):
  fully dead:       2884072 lines (738.3 MB) = 46.8%
  blocks: all-dead=0, mostly-dead=1, mixed=90, mostly-live=3, all-live=0

Line size 512 B (3080192 lines):
  fully dead:       1291855 lines (661.4 MB) = 41.9%
  blocks: all-dead=0, mostly-dead=0, mixed=89, mostly-live=5, all-live=0
```

**HNE shows NO fully-dead blocks but 20 mostly-dead blocks at 64 B.** This is the key Immix advantage over flat MS: HNE's allocation pattern produces mixed blocks (most blocks have SOME live data, blocking page-return), but Immix's line-granularity allocator can still recover dead lines via bump-realloc.

---

## 3. Cross-validation against other falsifiers

### 3.1 Falsifier #2 (sweep cost) — FAIL per same data

The block probe also projects sweep wall cost:

| Workload | Mark wall | Per-cycle total (mark + sweep) | Eval wall | Per-cycle as % of eval |
|---|---|---|---|---|
| hello.drvPath | 266 ms | 533 ms | 1340 ms | **40%** |
| HNE | 2792 ms | 5583 ms | 10290 ms | **54%** |

**§5.2 threshold: <5% per cycle to ship.  Both workloads exceed 40% — FAIL.**  Flat MS sweep cost is unacceptable.

### 3.2 Falsifier #3 (post-GC peak) — FAIL per Step 3

Per `lode/PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`: gate-ON peak_rss does NOT drop sufficient to clear the SHIP gate (≥200 MB hello / ≥500 MB HNE).  Hello actually REGRESSES by 22 MB.

### 3.3 Falsifier #4 (BiBOP) — JUDGMENT CALL per Step 7

Per `lode/BIBOP_LITE_PROJECTION_2026-05-29.md`: realistic-clustering yield range 5-20% of dead Bindings = 30-158 MB recoverable.  Below 15% threshold on the projection's lower bound, marginal on the upper.

---

## 4. The decision flowchart's verdict

Per `GC_DESIGN_POST_CHENEY_2026-05-28 §5.5`:

```
Run Falsifier #1 (Immix line-occupancy)
├── ≥30% lines dead → Implement Immix path (~4 KLoC, 4-6 wk)
└── <30%      → Continue: Run F2/F3/F4
```

Falsifier #1 PASSES with 46.5-50.5% — the flowchart says **Implement Immix path**.

This is REINFORCED by F2 (sweep cost fail), F3 (peak fail), F4 (judgment): flat MS does NOT clear the SHIP gate, but Immix's mark-region reclamation could.

---

## 5. What the existing implementation provides

Per `live_trace.cc:975-1182` the BlockProbe + reportLinesAtSize is comprehensive measurement infrastructure.  It can serve as the post-Step-10 production-impl gating tool: any Immix landing must keep these numbers at-or-above 46.5%/50.5% to pass acceptance.

---

## 6. Honest limits

- **Single sample per workload.**  Run-to-run noise in arena layout is approximately 0 (arena bytes σ=0 per Step 2 noise-floor data).  Confident.
- **Active-VMState dependency:** the probe requires `NIX_V3_LIVE_TRACE=1 + NIX_V3_BLOCK_PROBE=1` together to fire in the active-VMState pass.  Without LIVE_TRACE, the probe runs only at end-of-VMState with all-empty results.  This is acceptable for measurement; production Immix would not have this constraint.
- **Line-size sweep covers Immix-canonical 64/128/256/512 B.**  Smaller lines (16/32 B) not measured — those approach cell-granularity (kCellAlign=16) and may show even higher fragmentation.
- **No measurement of "would the Immix allocator slow this workload down?"**  Per `GC_DESIGN §3.2`, Immix overhead is ~3% in literature.  Production impl must verify.
- **m5 cardano-node not measured here.**  Step 19 will measure post-Step-17.  If M5 shows <30% line-dead, Immix benefit may not generalize — but unlikely given 50%+ on both hello and HNE.

---

## 7. Implications for Step 9 synthesis + Step 10 decision

- F1 PASS → **Immix path is empirically justified.**
- F2 FAIL → **Flat MS shipped would have sweep-cost wall regression of 40-54% per cycle.**
- F3 FAIL → **Flat MS shipped does NOT clear peak-RSS SHIP gate.**
- F4 JUDGMENT → BiBOP is a Phase 13+ add-on, not gate-blocking.

**Step 9 verdict (anticipated): PIVOT-IMMIX.**

Step 10's binding decision will commit to:
1. Retain Phase 0-3.8 mark + free-list infrastructure as scaffolding (per `GC_DESIGN §4.6` "what survives").
2. Replace per-exact-size free-list bins with Immix mark-region allocator (per-block bump-allocator within reclaimed lines).
3. Defer BiBOP-lite to post-Immix Phase 14+ if measurement justifies.

---

## 8. Cross-references

- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §5.1 (F1) + §5.2 (F2) + §5.5 (flowchart)
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — sibling F3 verdict
- [`BIBOP_LITE_PROJECTION_2026-05-29.md`](BIBOP_LITE_PROJECTION_2026-05-29.md) — sibling F4 verdict
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — L(t) data; L ≥ 0.41 across all observed safepoints
- `live_trace.cc:975-1182` — the existing reportLinesAtSize implementation
- `bench/baselines/2026-05-29-immix-sim/` — raw probe outputs
- Memory: [[measure-twice-cut-once]] §3 (pre-commit thresholds), §3.7 (no carcass)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
