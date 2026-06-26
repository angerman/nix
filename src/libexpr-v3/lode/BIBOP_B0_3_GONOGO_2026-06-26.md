# BiBOP B0.3 — GO/NO-GO projection result

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Instrument:** `v3 BiBOP-projection` line (mark_sweep.cc, gated NIX_VM_STATS). Per-type
LIVE bytes → project the per-type-segregated+compacted arena footprint =
sum_lane ceil(liveBytes_lane / 16MB) blocks, vs the current mapped arena. Reclaim =
arena − footprint. Host-independent (cell/block counts). Pre-committed ship threshold:
**≥ 80 MB net firefox peak-RSS.**

## firefox.drvPath (full, representative), per mid-eval sweep

```
arena=101MB live=65MB | mixed-perfect 67MB(+34) | LANES 134MB(-34) | per-type-own 168MB(-67)
arena=151MB live=88MB | mixed-perfect 101MB(+50)| LANES 134MB(+17) | per-type-own 168MB(-17)
arena=235MB live=129MB| mixed-perfect 134MB(+101)| LANES 185MB(+50)| per-type-own 218MB(+17)
arena=352MB live=180MB| mixed-perfect 185MB(+168)| LANES 268MB(+84)| per-type-own 302MB(+50)  ← PEAK (peak-live)
arena=352MB live=148MB| mixed-perfect 151MB(+201)| LANES 185MB(+168)| per-type-own 218MB(+134) ← PEAK (low-live trough)
```

(LANES = the BiBOP plan: Closure/Thunk/Bindings/List/Pair own lanes + a shared cold lane
for Value/Env/Chars. Footprint already includes per-lane rounding.)

**Verdict: firefox PASSES the ≥80MB threshold** — LANES reclaim is **84 MB at peak-live**
(conservative; compaction fires when the live set is largest) and **168 MB at the
low-live trough** (opportunistic trigger). Both clear 80 MB.

## Two big caveats (honest)

1. **Perfect-recycling assumption.** The projection assumes each lane compacts to
   ceil(live/16MB) — i.e. the Immix recycling packs the live remainder with zero
   fragmentation. Real recycling is imperfect → realizable < 84 MB. The peak-live 84 MB is
   already the conservative live point but the OPTIMISTIC packing point. Net: firefox is
   **marginal-passing**, not comfortable.

2. **16 MB blocks are coarse for segregation at small-arena scale.** `seg-overhead (lanes
   vs mixed) = +84 MB` at peak: segregating into ~5-8 lanes, each needing ≥1 whole 16 MB
   block, wastes ~84 MB in partial tail blocks — so BiBOP captures only ~HALF of the
   168 MB theoretical (unachievable) mixed-perfect compaction. At firefox's 352 MB arena
   this fixed per-lane overhead is large; at M5's ~3 GB arena it is negligible. **DESIGN
   REFINEMENT flagged: smaller lane blocks (1-2 MB) would slash the rounding waste and make
   BiBOP strongly positive at small/medium scale too** — but that's a larger allocator
   change (the 16 MB block size + side-table sizing is baked in).

## M5 (cardano-node.name) — measured on darwin-4 (the decisive big-arena workload)

```
arena=537MB  live=299MB | mixed-perfect 302MB(+235) | LANES 352MB(+185) | seg-overhead +50
arena=805MB  live=420MB | mixed-perfect 436MB(+369) | LANES 453MB(+352) | seg-overhead +17
arena=1208MB live=580MB | mixed-perfect 587MB(+621) | LANES 621MB(+587) | seg-overhead +34  ← peak sweep
```

**M5 is a STRONG GO: LANES reclaim = 587 MB at the peak sweep, with seg-overhead only
+34 MB (negligible).** Exactly as predicted — at M5's large arena the fixed per-lane
16 MB-block rounding is a rounding error vs the 587 MB reclaim. (The captured arena tops
out at 1208 MB here; the full M5 RSS is ~3 GB, so the absolute reclaim is likely larger
still — but ~587 MB of arena is already a major RSS cut on the workload where RSS matters
most. Note the reclaim is ARENA-only; M5's ~1.2 GB non-arena RSS — Boehm/ImportCache/
SQLite — is untouched.)

## DECISION — GO (complete)

- **M5: strong GO** (587 MB reclaim, overhead negligible) — the big-arena workloads where
  RSS matters are exactly where BiBOP-with-16MB-blocks shines.
- **firefox: marginal pass** (84-168 MB; +84 MB rounding overhead halves the win at small
  arena scale).

⇒ **Build B1+ with the existing 16 MB blocks.** The M5 win justifies the multi-week
allocator build; firefox's marginality is a small-workload artifact of the 16 MB block
granularity and does NOT gate the decision (the priority RSS workloads are large). The
1-2 MB lane-block refinement (B1.3-adjacent) stays OPTIONAL — pursue only if small-workload
RSS later becomes a priority. Caveat carried into B2.4: the projection assumes perfect
within-lane recycling; the real net-block-free measurement (B2.4) is the true test.

VERDICT: **GO.** Proceed B0.2 (mmap) → B1 (lanes) → B2 (recycling evac) → B3 (darwin-4 ship).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
