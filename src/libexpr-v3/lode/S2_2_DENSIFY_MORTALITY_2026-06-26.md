# S2.2 densify — per-type mortality: the uniform density is a mixed-alloc ARTIFACT, BiBOP can fix it

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Investigation:** "densify-first" (user-chosen) — the S2.2/S2.3 PCT sweep showed the
moving compactor is RSS-negative at every PCT because firefox's blocks cluster uniformly
25-75% live (no sparse blocks). This asks WHY, and whether segregation can fix it.

## Instrument: per-CellType mortality

Added `v3 mortality% by type` to the mid-eval sweep (`deadCellTypeHist` mirrors the live
`cellTypeHist`; mortality = dead/(dead+live) for swept cells). firefox.drvPath, mid-eval
GC, NIX_VM_STATS. Peak sweep (overall reclaim 55.8%):

| Type | mortality | dead cells (peak) |
|---|---:|---:|
| Closure  | **81%** | high |
| Bindings | **75%** | 443,353 |
| List     | **73%** | — |
| Thunk    | 56% | **1,263,877** (dominant dead population) |
| Pair     | 45% | 409,886 |
| Chars    | 38% | 402,042 |
| Value    | 36% | — |
| Env      | **17%** | — (mostly LIVE) |

Mortality spans **17% → 81%** across types (and rises over the eval: Bindings 17→75%,
Pair 11→45% as the graph matures).

## Finding — the uniform block density is a MIXED-ALLOCATION ARTIFACT

Today every CellType is bump-allocated into SHARED blocks in allocation order, so each
block holds a mix of high-mortality (Closure/Bindings/List 73-81% dead) and low-mortality
(Env 17%, Value/Pair/Chars 36-45%) cells. The per-block average lands at ~50% dead →
UNIFORM 25-75% density → NO sparse blocks → compaction frees ~nothing (the S2.2/S2.3
result). The uniformity is NOT fundamental — it's the averaging of very different per-type
mortalities within each block.

**⇒ BiBOP type-segregation would manufacture the density variance compaction needs.**
Allocate each CellType (or at least the high-mortality movable ones — Closure, Bindings,
List, Thunk) into DEDICATED blocks: then Closure-blocks go ~81% dead, Bindings ~75%,
List ~73% — genuinely SPARSE → freeable by compaction (after packing the live remainder
into far fewer blocks). Env-blocks stay dense (17% dead) but Env is a small, mostly-live
population. The ~187MB peak dead (deadBytes at the 55.8%-reclaim sweep) becomes largely
reclaimable, vs ~0 today.

This OVERTURNS the earlier "modest ceiling / uniform density is fundamental" read: the
RSS opportunity is SUBSTANTIAL, gated not by lack of dead memory (there's plenty, 56% of
the arena at peak) but by its SCATTER across mixed-type blocks. Segregation declusters it.

## Path (re-scoped S2.2)

The RSS win needs BOTH, and they compose:
1. **BiBOP type-segregation** (foundational allocator change): per-type block pools, so
   high-mortality types cluster into sparse blocks. Creates the density variance.
2. **Immix dest-recycling** (the S2.2 evac fix): pack the live remainder of sparse blocks
   into existing partial same-type blocks (not fresh) → empty + free the sparse ones.
3. Correct relocation of the segregated movable types (Closure/Bindings/List/Thunk are all
   typed-movable per evac-declined=0; the blackhole-pin handles in-force Thunks; Chars/Env
   can stay in their own pinned pools — low mortality, small).

Caveats: BiBOP is a real allocator rewrite (per-type free pools + the metadata/GC
integration); mortality is measured at mid-eval sweeps (end-state may differ — worth an
L(t) check); and the LIVE remainder of high-mortality types must pack densely (compaction
quality). But the headline is clear: **the density problem is solvable, and the ceiling is
substantial, not modest** — the dead memory is there (56% at peak), just scattered.

Next: scope the BiBOP per-type pool allocator (sizes, metadata, GC walkers) as the S2.2
foundation, then the Immix recycling evac packs/frees the resulting sparse blocks.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
