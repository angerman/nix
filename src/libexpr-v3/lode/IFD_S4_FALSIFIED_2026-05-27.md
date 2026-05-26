# #741 OP_IFD_PROBE S4 — FALSIFIED as a perf project — 2026-05-27

**Date:** 2026-05-27 (morning, post-AOT-SHIP)
**Status:** FALSIFIED via measurement-first spike; will NOT implement as a perf project
**Companion docs:** [`IFD_DEEP_DIVE_2026-05-21.md`](IFD_DEEP_DIVE_2026-05-21.md) (original S4 pitch), [`AOT_PHASE1_VERDICT_2026-05-27.md`](AOT_PHASE1_VERDICT_2026-05-27.md) (uses the same [[threshold-recalibration-rule]] for honest re-derivation)

## Headline

S4 cannot move cardano-node M5 wall. The bottleneck is pure-compute primops, not IFD or cache I/O. Phase 4b's drvHashCacheDisk (closest existing S4-like feature) is actually **net wall-NEGATIVE** in the fully-warm case (-2.6%).

Per [[falsification-rule]] + [[measure-twice-cut-once]]: do not implement S4 as a perf project. Track as deferred-with-data.

## Measurement (cardano-node M5, this host)

3-stage cache-warming sequence:

| Run | State | Wall | Notes |
|-----|-------|------|-------|
| Cold | CU cache empty, EvalResults empty, drvHashCacheDisk=off | ~34.7 s | Includes 4.4s compile + 1.2s lower + 2.1s optimise (8.2s total CU work) |
| Warm A | CU populated (3104 entries), EvalResults empty, drvHashCacheDisk=off | 26.45 s | CU disk cache saves 8.2 s as expected |
| Warm B | CU populated, EvalResults inserting (2299 inserts), drvHashCacheDisk=on | 25.95 s | First-time S4-like cache population |
| **Warm C** | CU populated, EvalResults populated, drvHashCacheDisk=on | **26.31 s** | Fully warm — should be best case for S4 |
| **Warm D** | CU populated, EvalResults populated but drvHashCacheDisk=off | **25.64 s** | Control: same cache state, S4 lookup path disabled |

**S4 effective wall: +670 ms (+2.6%) SLOWER vs no-S4 control.** The disk lookup + deserialize + value-allocate overhead exceeds the re-evaluation cost for these specific primDerivationStrict results.

## Where M5's wall actually goes

Per NIX_VM_STATS top primops (M5 warm cold-cache, full output):

```
v3-direct ifd probes: total=8940 import=8179 readFile=23 readDir=48 pathExists=688 findFile=2
v3-direct ifd probes (with-ctx, potential IFD): total=30 import=13 readFile=11 readDir=2 pathExists=4
v3-direct bridge telemetry total: 1.326 ms (0.004 % of 34.7 s wall)

v3 primop call counts (top 30 of 84):
  620556  genList       # PURE COMPUTE
  463451  match         # PURE COMPUTE
  441538  length        # PURE COMPUTE
  312138  __elemAt      # PURE COMPUTE
  305859  __addErrorContext  # PURE COMPUTE
  258903  elem          # PURE COMPUTE
  216621  isAttrs       # PURE COMPUTE
  ...
       805  readDir          # IFD candidate, ~0 of wall
       951  import           # cached via Phase 4b
```

**The dominant cost is pure-compute primops on long lists** (genList × 620k + length × 441k + elemAt × 312k). These have NO cache surface — they compute the same result every run because their inputs change every call.

The 30 with-ctx IFD probes are sub-ms total. Even perfect S4 caching delivers <1% wall.

## Why S4 fails the falsification gate

Per [[threshold-recalibration-rule]], applying the rigorous re-derivation:

**Original premise (IFD_DEEP_DIVE §6.5):**
- S4 captures the dominant time-cost (re-eval of the post-IFD attrset)
- Materialization workaround proves the existence of the time-cost
- Upside 10-100× on re-evals

**Falsified premise:**
- M5 measurement shows post-IFD attrset re-eval is NOT the dominant time-cost
- Top 7 primops are all pure-compute on lists/strings (genList, match, length, elemAt, addErrorContext, elem, isAttrs)
- 8179 imports / 8940 ifd probes are CACHED already by the in-memory caches; only 30 are actual IFDs
- The 30 IFDs contribute sub-ms to the 34.7 s wall

**Materialization comparison:** materialization's "50% eval-time win" applies to workloads where IFD re-eval IS the dominant cost (a different historical state of haskell.nix or different workload shape). Cardano-node M5 today is genList-dominated, not IFD-dominated.

## What does S4-as-pitched actually deliver?

`drvHashCacheDisk` is the existing in-tree implementation of S4-as-pitched (cross-process caching of post-IFD derivation eval results). Its measured impact on M5:

- Warm with cache hit: 26.31 s
- Warm with cache disabled: 25.64 s
- **Delta: +670 ms (-2.6 % wall) — drvHashCacheDisk SLOWS M5**

The cache lookup path is slower than recomputing on M5's specific workload shape. Implementing more S4 (extending to primReadFile/primPathExists/primReadDir) would add MORE lookup overhead, compounding the slowdown.

## What this DOES NOT mean

- **Not** that all IFD caching is bad. Phase 4b's primImport in-process cache (the `cache.results` map) delivers 5341 hits per M5 run — that's the dominant IFD cache and it works.
- **Not** that the original IFD_DEEP_DIVE analysis was wrong. It was correct for the workload shape that existed when written. Cardano-node M5 today is in a different regime.
- **Not** that cross-process IFD sharing is impossible. R8b (AOT distribution) DOES share IFD results across machines via the AOT cache. But the value there is CI-pool deduplication, not single-eval wall.

## What this DOES mean

- **S4 cannot ship as a perf project on cardano-node M5.** Per measure-twice-cut-once: no implementation without measured value.
- **S4 may have value for non-M5 workloads** (e.g., a haskell.nix workload that's genuinely IFD-dominated). If such a workload surfaces, re-measure first.
- **drvHashCacheDisk should be reviewed.** On M5 it's a net negative. The opt-in default-off status protects users from the slowdown. Making it default-on without further work would regress M5 by 2.6 %.

## Pre-committed retirement criterion (per [[falsification-rule]])

#741 as a perf project is FALSIFIED. Closing as completed-falsified.

For S4 to be revived, a workload measurement must show:
- ≥10 % of total eval wall in the 30 ifd-probes-with-ctx pathways
- AND that re-eval of post-IFD attrsets is genuinely on the critical path
- AND that lookup overhead is < re-eval cost (the current drvHashCacheDisk path violates this on M5)

None of the above is currently measured. Until they are, S4 implementation work is speculative and out of scope.

## Cross-references

- [[falsification-rule]] — Rule 0 applied; cheap measurement preceded multi-week implementation
- [[measure-twice-cut-once]] §3.8 — anti-pattern of implementing without measurement avoided
- [[threshold-recalibration-rule]] — re-derivation methodology applied here
- [[bridge-telemetry-2026-05-26]] — parallel finding (bridge wall = 0.018%) on HNE
- [[aot-phase1-ship-2026-05-27]] — AOT SHIP delivered 5% via different mechanism (parse residue)
- lode/IFD_DEEP_DIVE_2026-05-21.md §6.5 — original S4 pitch (now superseded by this measurement)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
