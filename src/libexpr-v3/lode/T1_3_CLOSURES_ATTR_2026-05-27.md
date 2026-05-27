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
* avg-nUp for fakeClo sites is 2.16 / 1.91 — well within historical
  pool's per-bucket range (kPoolMaxBuckets=16).

## Root cause — the fakeClo pool was RETIRED, nursery hasn't replaced it by default

Followed up on 2026-05-27 by checking the existing `alloc.hh:1069-
1230` fakeClo pool's hit rate (added counters, ran HNE).  Result:
**hits=0, misses=0, recycles=0** — the pool is DEAD CODE.

Source: `vm.cc:6803` comment block (Phase D Step 12, 2026-05-21):

> Phase D Step 12 (2026-05-21): retired the fakeClo / closure-pool
> sentinel infrastructure.  Pool reuse saved a hand-rolled allocation,
> but the Cheney nursery (NIX_V3_NURSERY=1) provides real generational
> reclamation: fresh closures land in nursery, scavenge collects
> unreferenced ones at next cycle.  Pool was load-bearing only before
> nursery + Phase D landed.

In production code, `vm.cc:6803` + `vm.cc:12085` call
`Alloc::allocClosure(t->nUpvalues)` directly — bypassing the pool
entirely.  `Alloc::allocFakeClo` and `Alloc::recycleFakeClo` have ZERO
callers anywhere in the codebase.  The infrastructure sits in
`alloc.hh:1069-1230` as dormant code.

**The 144 MB overhead exists BECAUSE**:
* Pool retired Phase D Step 12
* Nursery (its replacement) is opt-in via `NIX_V3_NURSERY=1`,
  default-OFF per CLAUDE.md §6.3 "Phase E v0.2 stress-mode missed-
  root resolution" — known blocker

## The actual remaining levers

(Updated 2026-05-27 after Day-2 mortality measurement + user
pushback on fakeClo revival.  Original draft proposed pool
revival as a tactical mitigation; user correctly rejected this
as reversing Phase D Step 12's intentional architectural
retirement.)

1. **Resolve Phase E v0.2 ship-readiness** (multi-session per
   `PHASE_E_V02_DAY2_FALSIFIED_2026-05-27.md` Paths A + B) —
   architectural direction.  Day-1 stress validation PASSED on
   all 3 anchor workloads; Day-2 mortality measurement showed
   the simple "flip default-on" path does NOT meet the SHIP gate
   (peak RSS +129-252 MB regression on hello/HNE).  Iteration
   paths: tune scavenge trigger (Path A) + audit what bypasses
   nursery (Path B).  When Phase E v0.2 ships at acceptable
   wall+RSS, the 144 MB fakeClo overhead closes automatically.

2. **Stage 6 production precise GC** (2-3 wk per
   `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`) — the next
   architectural step if Phase E v0.2 cannot reach ship-readiness
   after Paths A + B.

3. **Make the closure smaller per-instance** — orthogonal to GC
   strategy; modest yield independent of (1) + (2).

**EXPLICITLY NOT a lever** (per user pushback 2026-05-27):
reviving `Alloc::allocFakeClo` / `recycleFakeClo` to bypass
nursery.  This would reverse Phase D Step 12's intentional
retirement of hand-rolled pools in favor of generational
reclamation.  The 144 MB is a SYMPTOM of Phase E not shipping,
not a problem to route around with hand-rolled pools.  Fix the
architecture (Phase E ship-readiness), don't patch over it.

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
