# M3 (#136) — arena page-release KILLED by direct measurement — 2026-06-23

Task #136: "make the mid-eval arena reclaim visible to the OS (mmap blocks +
whole-block-free), OR kill it."  Measure-first via the existing per-block density
histogram + evac-opportunity report (`NIX_VM_STATS`, R2.1 2026-06-02), driven by
this session's mid-eval non-moving sweep on firefox.drvPath.

## The two ways to return arena pages to the OS — both measured ~0

Peak RSS only drops if WHOLE 16 MB blocks are unmapped.  Two mechanisms:

### (1) whole-block-free (no moving) — `blocksFreed=0` on EVERY sweep

Every mid-eval sweep (6 sweeps over the firefox eval) reported `blocksFreed=0
bytesFreed=0.0MB`, despite reclaim% climbing 18→56%.  Live cells are scattered
across every block by allocation order — not one block goes fully dead.  munmap
can never fire with a non-moving sweep.

### (2) evacuation (moving, the Immix path) — `sparseBlocks=0`, evacuable_RSS≈0

The density histogram (live-byte fraction per block) over the eval:

| sweep | [0-10] | [10-25] | [25-50] | [50-75] | [75-100] | sparseBlocks | evacuable |
|---|---|---|---|---|---|---|---|
| 1 | 0 | 0 | 1 | 0 | 4 | 0 | 0.0 MB |
| 3 | 0 | 0 | 6 | 3 | 4 | 0 | 0.0 MB |
| 5 (mid) | 0 | 0 | 11 | 5 | 4 | 0 | 0.0 MB |
| teardown | 0 | 1 | 11 | 8 | 0 | 1 | 16.8 MB |

Blocks cluster at **25–75% live**; ZERO blocks are sparse (<25% live) at any
mid-eval trigger point.  Evacuation empties SPARSE blocks (move their few live
cells into dense blocks, munmap the emptied block).  With no sparse blocks, the
evacuable RSS is ~0 — only at teardown (C-stack unwound, most cells dead) does ONE
16.8 MB block become sparse.  Additionally, `evac-movability` showed **12–27% of
cells C-stack-PINNED** during mid-eval — unmovable, so they'd pin their blocks even
under evacuation.

## Why in-block reclaim is plentiful but irrelevant to peak RSS

The F1 Immix line-occupancy data (IMMIX_LINE_OCCUPANCY_2026-05-29: 46–50% dead
*lines* at 128 B) and this session's free-list reuse (reclaim% up to 56%) both
measure IN-BLOCK dead space — recyclable for in-process REUSE (which mid-eval
reuse already does), but it does NOT unmap the block (live lines remain).  Peak RSS
is set by mapped blocks, and blocks stay mapped as long as ANY cell is live.  The
two are consistent: lots of dead lines *within* blocks, ~0 fully-dead-or-sparse
*blocks*.

## VERDICT — #136 arena page-release: KILL

Both page-return mechanisms are empirically ~0 on firefox:
- whole-block-free: `blocksFreed=0` (no whole-dead blocks).
- evacuation: `sparseBlocks=0`, `evacuable_RSS≈0` (no sparse blocks; + 12–27%
  C-stack-pinned).

So mid-eval reclaim CANNOT become peak-RSS reduction — confirming the
MIDEVAL_REUSE_RCA finding (reclaims the bump for reuse, peak unchanged) with the
ROOT mechanism: the live set is too evenly distributed across blocks for either
munmap or evacuation to free pages.  Even the multi-week Immix evacuation project
(GC_DECISION_2026-05-29, already PAUSED below SHIP gate per GC_PAUSE_2026-05-29)
would free ~0 RSS at these density profiles — re-confirming that pause with fresh
mid-eval data.

This completes the RSS-lever sweep for the BEAT_TW campaign:
- #135 thunk-avoidance — KILL (0.7% avoidable).
- #134 ImportCache eviction — KILL (arena-no-release).
- #136 arena page-release — KILL (no whole-dead/sparse blocks).
- #139 MALLOC_SMALL CU-shrink — modest/foundational (descriptor repack + jemalloc).

**ALL reclaim-based RSS levers are dead.**  The only remaining RSS path is
ALLOCATE FEWER / SMALLER cells (FP-2/FP-3 mostly closed; thunk churn L3-hard) plus
the structural buckets (MALLOC_SMALL foundational; Boehm FFI).  Beating TW on RSS
requires shrinking the LIVE representation — v3's live working set + per-CU
structures + Boehm FFI are fundamentally heavier than TW's 16 B niche-tagged Value.
A broad foundational program, no single lever.

(Possible follow-up: confirm the density profile on M5 — but the mechanism
[allocation-order interleaving of live/dead cells] is general, and the line-888
comment in mark_sweep.cc already generalized "whole-block-free finds 0 blocks at
mid-eval trigger points.")

#138 (reduce GC mark cost) was gated on "M3 ships a default sweep" — with M3
killed, no default sweep ships, so #138 is moot/closed too.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
