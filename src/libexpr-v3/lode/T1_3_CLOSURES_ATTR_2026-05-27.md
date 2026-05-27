# T1.3 Closures attribution — 56 % of Closure bytes on HNE is fakeClo overhead

**Date**: 2026-05-27
**Origin**: `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 T1.3.
Templated from #746 BINDINGS_ATTR + extension of
T1_3_THUNKS_ATTR_2026-05-27.
**Status**: infrastructure landed, HNE measurement complete,
**REAL ACTIONABLE LEVER IDENTIFIED**.

## TL;DR

Unlike Thunks (single dominant site at OP_MAKE_THUNK per
T1_3_THUNKS_ATTR), Closures are dispersed across 4 vm.cc sites on
HNE.  The headline:

```
4 distinct origins, 4079320 total allocs, 259.1 MB tracked
  vm.cc:3412   1765735  115.08 MB  avg-nUp=2.27  max-nUp=8  -- OP_MAKE_CLOSURE general
  vm.cc:6803   1595947  101.31 MB  avg-nUp=2.16  max-nUp=8  -- fakeClo (OP_FORCE thunk body)
  vm.cc:12085   714910   42.65 MB  avg-nUp=1.91  max-nUp=9  -- fakeClo (OP_TAIL_CALL)
  vm.cc:3379      2728    0.08 MB  avg-nUp=0.00  max-nUp=0  -- allocClosureTenured singleton
```

* **115 MB / 259 MB (44 %)** — USER closures (real `Foo: ...` lambdas)
* **144 MB / 259 MB (56 %)** — VM-internal **fakeClo** wrappers

`fakeClo` is a synthetic Closure the VM allocates to dispatch a
THUNK body through the same dispatch loop the OP_CALL path uses.
It carries the thunk's upvalues + capturedWiths + cu so the dispatch
code can read them uniformly.  Per the #558 Phase 4 recycling pool
design (`alloc.hh:1069-1230`), these should be aggressively recycled
at OP_RETURN.

## Why this is a real lever

* **144 MB on HNE is VM-internal overhead**, not user-visible
  allocation.
* avg-nUp for fakeClo sites is 2.16 / 1.91 — well within the pool's
  per-bucket range (kPoolMaxBuckets=16).
* Yet 2.3 M fresh allocations were recorded, suggesting the existing
  pool (kPoolPerBucket=128 × kPoolMaxBuckets=16 = 2048 slots total)
  isn't catching enough recycles.

Three possible mechanisms for the leak:
1. **Pool size too small for HNE concurrency** — bursts of
   simultaneous thunk forces overflow the 128-slot per-bucket
   limit; spillover allocates fresh.
2. **Recycle gate too narrow** — recycle path may have early-out
   conditions (per the alloc.hh comments, "fakeClos eligible for
   pooling; OP_CALL frames' closures... not pooled").
3. **Exception unwind paths skip recycle** — if a thunk body throws,
   the OP_RETURN-side recycle doesn't fire, and the fakeClo leaks
   to (effective) tenured arena.

Each is a focused 1-day debugging task with measurable yield.

## Cross-comparison: Thunks (T1_3_THUNKS) vs Closures (this doc)

| Type | Sites | Top-site % | Lever |
|------|-------|-----------|-------|
| Thunks   | 1 (OP_MAKE_THUNK)        | 99.999% | No C++-site lever; per-LambdaDescriptor via V3_DBG_ALLOC_DUMP |
| Closures | 4 (mostly OP_MAKE / fakeClo) | 44 / 39 / 16 / 0.03% | **fakeClo pool tuning** (144 MB recoverable on HNE) |

Closures' multi-site dispersion is what makes T1.3 informative here
in a way it wasn't for Thunks.

## Recommended follow-up (multi-session)

1. **Audit fakeClo pool effective hit rate** — instrument
   `Alloc::allocFakeClo` to count hits vs misses; dump under
   `NIX_V3_FAKECLO_STATS=1`.
2. **Identify which fakeClo allocation site overflows** — separate
   the 1.6 M / 0.7 M counts (OP_FORCE vs OP_TAIL_CALL) into hit /
   miss buckets per source.
3. **Tune `kPoolPerBucket`** — pre-committed: ship if ≥ 50 MB peak
   RSS reduction on HNE + no `--core` regression.
4. **Audit exception-unwind path** — confirm OP_RETURN recycle runs
   on the throw path (or document why not).

The fakeClo machinery already exists; tuning is a SMALL CHANGE with
real measurable yield.  This is a high-ROI Stage 6.5 / Phase 4b-style
deliverable that's INDEPENDENT of arena dereg / precise GC.

## Cross-references

* `lode/MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 — T1.3
  closes for Closures (with a real lever) + Thunks (confirming)
* `lode/T1_3_THUNKS_ATTR_2026-05-27.md` — Thunks parallel (no lever)
* `lode/HNE_BUCKET_DECOMP_2026-05-27.md` — Closures = 272 MB context
* `lode/SESSION_ARC_2026-05-27.md` — overall session arc
* `include/v3/alloc.hh:1069-1230` — fakeClo recycling pool (#558 Phase 4)
* `include/v3/alloc.hh:1453-1510` — `ClosureOrigin` + table API
* `include/v3/alloc.hh:2231-2330` — `dumpClosuresAttribution`
* `vm.cc:6803` — fakeClo at OP_FORCE (101 MB / 1.6 M allocs)
* `vm.cc:12085` — fakeClo at OP_TAIL_CALL (43 MB / 0.7 M allocs)
* `[[falsification-rule]]` — this commit IDENTIFIES a new lever
  (rather than falsifying / confirming an existing hypothesis)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
