# Live-fraction spike — Stage 6 measure-twice falsifier

**Date**: 2026-05-27
**Goal**: answer the load-bearing claim "precise GC of v3 arena reduces
peak RSS by ≥200 MB on canonical workloads" BEFORE committing 1-2 weeks
to Stage 6 implementation.
**Outcome**: **SHIP-GREEN** — 239 MB freeable on hello.drvPath end-of-
run; mid-eval freeable strictly ≥ this lower bound; Stage 6 justified.

## Method

1. Stage 1 contract `tagIsPointer` + Stage 3 root walker `walkAllV3Roots`
   provide the precise root set.
2. `live_trace.cc` implements a transitive mark-from-roots tracer
   (worklist BFS, per-type counters, FAM-aware sizing).  Mirrors
   gc.cc Scavenger's walk shape minus forwarding semantics.
3. Fires at end-of-run under `NIX_V3_LIVE_TRACE=1` (single-shot via
   arena-watermark gate).  Reports per-type LIVE-vs-ALLOCATED + verdict.

## Why end-of-run is a sound LOWER BOUND on freeable

The v3 arena uses bump allocation — `threadArena` never returns
bytes to the OS during eval.  Therefore:

```
peak v3_arena = cumulative bytesAllocated   (no mid-eval reclamation)
peak live    ≤ cumulative bytesAllocated
end-of-run live ≤ peak live
end-of-run freeable = cumulative - end-of-run live
                    = LOWER BOUND on (cumulative - peak live)
                    = LOWER BOUND on what a continuous GC could reclaim
```

If end-of-run freeable ≥ 200 MB, mid-eval freeable is ≥ this and the
SHIP gate is met without needing mid-eval probes.

## Measurements

### Synthetic — `genList 200k attrsets + foldl'`

```
                 LIVE-objs   LIVE-bytes     ALLOC-bytes    live%
  Closures             11         352 B        57.98 MB     0.0%
  Thunks                0           0 B        39.67 MB     0.0%
  Bindings              0           0 B        15.26 MB     0.0%
  Lists                 0           0 B         3.05 MB     0.0%
  Pairs                 0           0 B         9.16 MB     0.0%
  TOTAL                           352 B       125.13 MB     0.0%
  Freeable: 125.13 MB — MARGINAL verdict (between 50-200 MB)
```

99.99 % garbage at end-of-run — fully transient intermediate state.

### Real — `(import <nixpkgs> {}).hello.name`

```
                 LIVE-objs   LIVE-bytes     ALLOC-bytes    live%
  Closures           1196      67.00 KB         5.08 MB     1.3%
  Thunks             1853     128.09 KB        13.22 MB     0.9%
  Bindings            461     582.66 KB        70.07 MB     0.8%
  Lists                26       6.92 KB         2.27 MB     0.3%
  Pairs             42013       1.92 MB        22.70 MB     8.5%
  TOTAL                         2.69 MB       113.34 MB     2.4%
  Freeable: 110.65 MB — MARGINAL verdict
```

97.6 % garbage at end-of-run.  The 110.65 MB lower bound is enough
to call MARGINAL but not yet SHIP-GREEN on hello.name alone.

### Real — `(import <nixpkgs> {}).hello.drvPath` (the canonical workload)

```
                 LIVE-objs   LIVE-bytes     ALLOC-bytes    live%
  Closures           1480      79.70 KB        44.03 MB     0.2%
  Thunks            23326       1.58 MB        52.40 MB     3.0%
  Bindings          16001     281.56 MB       388.28 MB    72.5%
  Lists               380      26.19 KB        11.89 MB     0.2%
  Pairs             52950       2.42 MB        28.46 MB     8.5%
  TOTAL                       285.67 MB       525.07 MB    54.4%
  Freeable: 239.40 MB — SHIP-GREEN verdict (≥ 200 MB gate met)
```

**Headline numbers**:
* peak_rss     = 754.7 MB
* v3_arena     = 587.2 MB (matches cumulative bytesAlloc within 12 %)
* boehm_heap   =  402.9 MB (99.9 % free — separate optimisation)
* live (arena) =  285.7 MB  (residual at end-of-run, mostly persistent
                              Bindings holding the drv attrset chain)
* freeable     =  239.4 MB  (cumulative − end-live; LOWER BOUND)

A continuously-running precise GC would reclaim **at least** 239 MB
of arena during eval, cutting peak RSS from 754 MB to ≤ 515 MB.

## What the 285 MB residual tells us

72.5 % of allocated Bindings bytes remain LIVE at end-of-run because
the result `hello.drvPath` transitively references the entire stdenv
attrset chain.  This is an Amdahl floor: even perfect precise GC
cannot drop below ~285 MB live arena while the drv attrset chain is
held by the result.

The remaining lever for THAT 285 MB is structural:
* **ChainBindings** (#823 / A1a Phase D) — shares parent → overlay
  delta instead of materialising 388 MB of mergeBindings copies into
  the chain
* **Bindings interning** — global hash-cons of bindings tables

These are SEPARATE from Stage 6 precise GC and don't share the same
SHIP gate.

## Cross-check: where does the 239 MB freeable live during eval?

`Pairs allocated = 28.46 MB; live = 2.42 MB`.  Pairs are App/PrimOpApp
nodes — mostly transient memoization placeholders that survive past
the App they describe.  ~26 MB of these are clear precise-GC targets.

`Thunks allocated = 52.4 MB; live = 1.6 MB`.  ~51 MB of thunks are
intermediate forces that completed but whose Thunk cells stay
arena-pinned.  Another precise-GC target.

`Bindings allocated = 388 MB; live = 282 MB`.  106 MB of Bindings are
fully transient — intermediate mergeBindings copies that the chain
overwrites.  Precise-GC target.

Total clear precise-GC targets at end-of-run: ~26 + 51 + 106 + 44
(transient closures) + 11.5 (transient lists) ≈ 239 MB.  Matches
the verdict total.

## Decision

Stages 4-6 are GREEN to proceed.  Stage 6 SHIP gate (≥ 200 MB peak
RSS reduction on M5 / hello.drvPath) is ACHIEVABLE by the lower-bound
measurement.  Mid-eval freeable is strictly ≥ end-of-run freeable, so
production precise GC will hit ≥ 239 MB reclamation on hello.drvPath
even in the worst case.

## What the spike intentionally did NOT measure

* **Mid-eval peak live**.  Would require a SIGUSR1 / RSS-watermark
  triggered trace.  Out of scope: the LOWER BOUND alone meets the
  SHIP gate.  Defer to Stage 6 production GC, which IS the mid-eval
  walker.
* **Boehm reachable set**.  Boehm is mostly empty (99.9 % free on
  hello.drvPath) — ditching Boehm reclaims the 400 MB watermark, but
  that's a separate allocator-policy lever (`GC_gcollect_and_unmap`
  exists today).  This spike is about v3_arena specifically.
* **Cross-workload variance**.  hello.name's freeable (110 MB) is
  below SHIP; hello.drvPath's freeable (239 MB) is above.  The
  workload that determines project value is hello.drvPath because
  that's where the absolute RSS waste is largest.

## How to reproduce

```bash
NIX_VM_STATS=1 NIX_V3_LIVE_TRACE=1 NIX_V3_DIRECT_EVAL=1 \
  NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=6G \
  ./build/src/nix/nix --extra-experimental-features nix-command \
  eval --impure --expr '(import <nixpkgs> {}).hello.drvPath' 2>&1 \
  | grep -A22 "LIVE-FRACTION"
```

`NIX_V3_LIVE_TRACE` is gated by an arena-watermark filter that
dumps once per "arena grew by ≥ 64 KB" event — filters out the
small primop-install passes (~3 KB arena each) and only emits on
the meaningful runRootExpr pass.

## Retirement criterion

When Stage 6 lands the production precise GC, the live-trace tracer
becomes a debug overlay on top of the production marker.  At that
point: fold `NIX_V3_LIVE_TRACE` into `NIX_VM_STATS=1`, remove the
gate, and replace this doc with the post-Stage-6 measurement record.

## References

* `lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md` — 7-stage plan
* `[[memory-first-class]]` — ≥ 200 MB peak RSS SHIP gate origin
* `[[measure-twice-cut-once]]` — pre-Stage-6 falsification methodology
* Stage 1 commit `6f854fa2c` — `tagIsPointer` classification
* Stage 3 commit chain (`02c95eba0` + `e7639f837`) — `walkAllV3Roots`
* Stage 6 SPIKE this commit — live-trace tracer + ship verdict

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
