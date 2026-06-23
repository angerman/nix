# M2 (#134) — ImportCache eviction RE-FALSIFIED + the arena-no-release reframe — 2026-06-23

Task #134.  M1 promoted "ImportCache eviction (un-pin arena subgraphs)" to the #1
RSS lever.  **That promotion was wrong** — it didn't account for the prior
falsification.  Measure-twice killed it again.

## The lever was ALREADY falsified (472871804, 2026-05-30)

`lode/PHASE_4B_LRU_FALSIFIED_2026-05-30.md`: results-only LRU eviction
(`NIX_V3_IMPORT_CACHE_MAX_ENTRIES`) on HNE (N=5 trimmed mean):

| config | Δ vs baseline |
|---|---|
| MAX_ENTRIES=1000 / 500 | +0.0 / +0.3 MB (no-op) |
| MAX_ENTRIES=100 | **+76.9 MB (regression)** |
| MAX_ENTRIES=50  | **+177.0 MB (regression)** |

## Re-confirmed on the CURRENT binary (2026-06-23, firefox, cache-off, no NIX_VM_STATS)

The 2026-05-30 result predates nursery default-on + gen-major + mid-eval, so I
re-ran it (measure-twice) on firefox with all of those active:

| config | peak RSS (median of 3) | Δ |
|---|---|---|
| MAX_ENTRIES=0 (no evict) | 675 MB | — |
| MAX_ENTRIES=256 | 727 MB | **+52 MB** |
| MAX_ENTRIES=64  | 890 MB | **+215 MB** |

Same shape: eviction makes RSS monotonically WORSE.  KILL confirmed.

## Root cause — the v3 arena never releases pages to the OS

Eviction drops the cache's reference, but (1) the evicted Bindings/Value cells'
arena pages stay mapped (no munmap), and (2) the next demand re-imports → allocates
FRESH cells → arena grows past the prior peak.  Freeing a reference cannot lower
peak RSS when the allocator doesn't return pages AND reclaim triggers fresh
allocation.

**This is the SAME pin that killed mid-eval reuse as an RSS lever this session**
(MIDEVAL_REUSE_RCA: reuse reclaims 36% of the M5 arena BUMP for in-process reuse,
but peak RSS is unchanged because the calloc'd blocks are never munmap'd).  Two
independent reclaim levers, one root cause.

## The reframe — what CAN move RSS (and the honest ceiling)

Reclaim-based levers are dead.  Only two paths remain:

1. **Allocate FEWER cells** (lowers peak directly; arena-no-release is irrelevant
   if the page is never allocated).  Mostly explored: FP-2 thunk header (done, 24B
   floor); FP-3 pair tax (mostly closed); the open one is thunk CHURN (62-67%
   unforced) — but that needs strictness/eager-eval with byte-id risk (L3-hard).
2. **Arena page release** (#136/M3): mmap'd blocks + whole-block-free/munmap +
   a collector that produces WHOLE free blocks.  HARD — a non-moving sweep frees
   scattered cells (fragmentation), not whole 16 MB blocks; needs compaction or
   luck.

**Honest ceiling — even perfect arena reclaim does NOT beat TW:**
- firefox: arena 352 MB, ~157 MB live → munmap'ing the ~195 MB dead gives
  675→~480 MB = 1.34× TW (from 1.89×).  Better, not winning.
- M5: live arena ~1023 MB ALONE ≈ TW's ENTIRE 982 MB RSS.  Plus MALLOC_SMALL
  690 MB + Boehm 402 MB + binary 310 MB.  Arena reclaim cannot close this.

So beating TW on RSS is a BROAD foundational program, not a single lever:
- **arena page-release (M3)** — the only arena RSS lever, hard, low ceiling alone;
- **MALLOC_SMALL CU shrink (690 MB M5, #2 bucket)** — free IR after lowering /
  shrink per-CU structures / malloc fragmentation; UN-RCA'd, a genuine new lever;
- **Boehm FFI reduction (402 MB)** — fewer TW-Value crossings (V3-NATIVE-ward);
- **cell representation** — v3's live cells are heavier than TW's 16 B niche-tagged
  Value (mostly explored).

## VERDICT — #134 ImportCache eviction: KILL (re-falsified)

The `NIX_V3_IMPORT_CACHE_MAX_ENTRIES` gate stays as opt-in infrastructure (it would
only ever activate PAIRED with M3 arena page-release — see the carcass rule in the
original falsification).  Re-scope the RSS work to: M3 (arena page-release, with
the low-ceiling caveat) + a NEW MALLOC_SMALL CU-shrink RCA (the un-explored #2
bucket).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
