# M2' (#139) — MALLOC_SMALL CU-shrink RCA — 2026-06-23

Task #139.  After #135 (thunk-avoidance) + #134 (ImportCache eviction) KILLs,
MALLOC_SMALL (690 MB M5, the #2 RSS bucket per M1) is the only un-explored RSS
lever — and it's libc-malloc, NOT arena-pinned, so the arena-no-release wall
doesn't apply.  Measure-first via a new per-field CU breakdown
(`importCachePrintFieldBreakdown`, printed under `NIX_V3_MEM_BUCKETS`).

## Finding 1 — the CU retains NO IR after lowering

`CompilationUnit` holds only runtime artifacts (bytecode + constants + symbolTable
+ lambdas + ICs); the `ir::*` nodes are freed post-emit.  No "free the IR" lever.

## Finding 2 — the CU footprint is dominated by LambdaDescriptor, not bytecode

firefox.drvPath, 596 CUs, **110,978 lambdas**, 17,668 force-sites:

| field | MB | irreducible? |
|---|---|---|
| **lambdas + formals (the array)** | **25.49** | mostly — but ~38% is diagnostic bloat (below) |
| code (bytecode) | 8.40 | yes |
| attrSelectIC (4-way) | 3.56 | runtime IC (shrinkable only at perf cost) |
| lambda names (`name` bodies) | 2.33 | **DROPPABLE (diagnostic)** |
| recSlotIC | 0.32 | runtime IC |
| forceEmitSites | 0.38 | **DROPPABLE (V3_DBG_FORCE_SITE only)** |
| consts / primops / fixed | ~1.5 | yes |
| **total CU footprint** | **~42 MB** | |

The LambdaDescriptor array (25.5 MB, **65%**) is the dominant cost — bigger than
the bytecode itself.

## Finding 3 — ~38% of each LambdaDescriptor is diagnostic / mostly-empty

`LambdaDescriptor` (~230 B each × 110,978) carries, per descriptor:
- `std::string name` + `std::string contextualName` = **64 B of headers**, EMPTY
  for the majority (anonymous) lambdas → ~7 MB ff of wasted empty-string headers.
  (`name` = V3_DBG_OPCYCLE/disassembler; `contextualName` = printNixValueRich
  lambda-name parity — the latter IS user-facing, so it needs a side-table, not a
  drop.)
- `forceCount` + `allocCount` + `callCount` = **24 B of counters**, WRITTEN only
  under `dbgForceStatsActive()` / `V3_DBG_ALLOC_DUMP` (default-off), READ only by
  atexit dumps → ~2.7 MB ff of always-resident dead weight in default builds.

Total diagnostic/mostly-empty: **~88 B/descriptor ≈ 10 MB ff / ~75 MB M5
(extrapolated)**.

## Finding 4 — the bigger MALLOC_SMALL chunk is malloc FRAGMENTATION

M5 MALLOC_SMALL 690 MB ≈ ~330 MB live CU structures + **~360 MB fragmentation**
(freed-but-resident, M1).  The fragmentation is libc-malloc-internal — only an
allocator swap (jemalloc) or `madvise` can return it; v3 code can't.

## VERDICT — modest, foundational; document-close (no quick single ship)

The reducible MALLOC_SMALL content is:
1. **LambdaDescriptor diagnostic shrink** — clean-but-small: move the 3 statistical
   counters (24 B, zero correctness risk — only written under the gate) + the 2
   diagnostic strings (`name` droppable; `contextualName` → side-table for the
   printNixValueRich parity readers) out of the always-resident descriptor.
   ~10 MB ff / ~75 MB M5 (2.5% of M5 RSS).  Also a tiny CPU/cache-locality win
   (smaller descriptor in the OP_MAKE_THUNK/OP_CALL hot paths).
2. **malloc fragmentation (~360 MB M5)** — jemalloc/madvise trial; speculative,
   platform-specific, build-system change.

Neither clears the ship bar alone against the 3× RSS gap (M5 2984→<982 needs
−2000 MB; this is ~75–435 MB).  Consistent with the session verdict: **beating TW
on RSS is a broad foundational program, not a single lever.**  Recommend bundling
the descriptor repack + jemalloc trial into a future foundational MALLOC_SMALL
sprint; the descriptor-counter removal is the lowest-risk first step if/when that
sprint is funded.  The per-field breakdown instrument stays (extends the existing
`NIX_V3_MEM_BUCKETS` report) for re-measurement.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
