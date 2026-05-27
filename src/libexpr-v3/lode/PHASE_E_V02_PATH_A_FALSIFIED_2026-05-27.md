# Phase E v0.2 Path A (scavenge threshold tuning) — FALSIFIED

**Date**: 2026-05-27 (Day-2 follow-up)
**Per**: `PHASE_E_V02_DAY2_FALSIFIED_2026-05-27.md` Path A — "Tune
scavenge trigger so mortality rises above 50% at acceptable wall"
**Outcome**: ✗ **Path A doesn't move the needle on HNE.**
Threshold tuning from 75% → 50% → 25% increases hit rate on
hello.drvPath (31% → 50%) but has ZERO effect on HNE (19% → 19%).
And on hello, RSS gets WORSE not better.

## What landed (measurement infrastructure)

`NIX_V3_NURSERY_TRIGGER_PCT=N` env-var override added to
`nursery.hh::shouldScavenge`.  Replaces the hard-coded 75% fill
threshold with a configurable percentage in [1, 99].

Used to characterize how scavenge frequency responds to the
threshold.  Default remains 75% (legacy behavior).

## Measurements

### hello.drvPath sweep

| TRIGGER_PCT | scavenges | hit_rate | mortality | RSS    |
|-------------|-----------|----------|-----------|--------|
| 75 (default)|     1     |  31.1 %  |  56.9 %   | 882.6 MB|
| 50          |     2     |  50.1 %  |  39.8 %   |1250.3 MB|
| 25          |     2     |  50.1 %  |  39.8 %   |1243.6 MB|

PCT=50 and PCT=25 produce **identical results** (2 scavenges).
The dispatch-loop safe-point cadence is the upper bound on
scavenge frequency, not the threshold.  Lower than ~50% threshold
gets clipped by the safe-point model.

### HNE sweep

| TRIGGER_PCT | scavenges | hit_rate | mortality | RSS    |
|-------------|-----------|----------|-----------|--------|
| 75 (default)|     4     |  19.1 %  |  38.3 %   |2816.8 MB|
| 50          |     4     |  19.1 %  |  38.3 %   |2474.8 MB|

**Identical hit rate, identical scavenge count.**  On HNE the
threshold doesn't matter at all between 50% and 75%.

## Why Path A fails

Per nursery.hh:88-92 + vm.cc:2596-2617: the scavenger fires from
the DISPATCH-LOOP TOP, between bytecode opcodes.  `shouldScavenge`
is checked once per dispatch iteration, not per allocation.

The problem: single opcode bodies (especially primop bodies)
allocate MANY objects before returning to dispatch.  A primop
like `listToAttrs` allocates N Pairs + 1 Bindings + N entries
within ONE opcode body.  If those overflow the nursery, all
overflows go to arena REGARDLESS of threshold — by the time
control returns to dispatch and shouldScavenge() is checked, the
overflow has already happened.

So:
* Lower threshold → fires scavenge slightly earlier → at most 1-2
  more scavenges over the run → marginal improvement
* But each lower threshold also means smaller usable nursery
  per opcode → MORE bypass for the same workload pattern
* Net: hit rate caps at ~50% on hello, ~19% on HNE — the workload's
  primop-body allocation density determines the ceiling

## What this kills

* "Path A trigger tuning unlocks Phase E v0.2 default-on" — FALSIFIED.
  Threshold tuning is bounded by the safe-point cadence.
* "Phase E v0.2 can be tuned to ship at acceptable RSS budget" —
  FALSIFIED by the cross-workload measurement.  The architectural
  safe-point model prevents the mortality-amplification needed.

## What remains valid

* The `NIX_V3_NURSERY_TRIGGER_PCT=N` env-gate is kept as a
  measurement tool for future explorations.  Default 75%
  preserves legacy behavior.  Per [[falsification-rule]] +
  measurement-anchored design, this is NOT "carcass behind gate"
  — it's the diagnostic that produced this falsification.
* The `v3-direct nursery routing` diagnostic (Path B) remains
  the canonical signal for any future Phase-E iteration.

## What's next architecturally

Per `PHASE_E_V02_DAY2_FALSIFIED_2026-05-27.md` Path B finding +
this Path A falsification, the next architectural step is:

**Stage 6 production precise GC** (per
`STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`, 2-3 weeks).  Mark-sweep
or compacting GC over the v3 arena doesn't depend on the
dispatch-loop safe-point model for arena reclamation — it can
scan + reclaim regardless of primop-body state because the arena
holds tenured cells (post-OP_RETURN write-back, fully realized).

Per the Stage 6 SHIP-GREEN spike: 239 MB freeable on hello.drvPath
+ 797 MB freeable on HNE.  This is the lever Phase E v0.2 cannot
deliver but Stage 6 can.

The Phase E v0.2 infrastructure REMAINS opt-in and useful for
small + transient allocations (the 31% hit rate on hello still
saves 33.6 MB).  But it's NOT the path to default-on on HNE-class
workloads.

## Closing the 144 MB fakeClo lever (per user pushback)

Per T1_3_CLOSURES_ATTR + 2026-05-27 user pushback against fakeClo
pool revival: the 144 MB fakeClo overhead remains.  The correct
architectural path is Stage 6 production GC, NOT a pool revival.

Path A falsification + Path B data don't change this conclusion —
they REFINE the strategy:
* Phase E v0.2 default-on does NOT close the fakeClo lever in
  current shape (Path A falsified).
* Stage 6 production GC IS the path that closes the fakeClo lever
  by reclaiming dead arena cells (including expired fakeClos)
  regardless of primop-body cadence.

## Cross-references

* `lode/PHASE_E_V02_DAY1_PASS_2026-05-27.md` — Day-1 stress PASS
* `lode/PHASE_E_V02_DAY2_FALSIFIED_2026-05-27.md` — Day-2 +
  Path B data
* `lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — the next
  architectural step
* `lode/T1_3_CLOSURES_ATTR_2026-05-27.md` — the 144 MB lever
* `nursery.hh::shouldScavenge` — where the threshold is checked
* `vm.cc:2596-2617` — where the dispatch-loop trigger fires
* `[[falsification-rule]]` — every commit kills a hypothesis;
  this one kills Path A
* `[[measure-twice-cut-once]]` — Path A's failure mode (no
  improvement at lower thresholds) is exactly what measurement
  is for

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
