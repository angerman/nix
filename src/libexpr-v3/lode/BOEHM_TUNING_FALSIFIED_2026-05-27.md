# Boehm tuning spike (§6.2) — FALSIFIED on macOS aarch64

**Date**: 2026-05-27
**Setup**: cppnix on macOS aarch64 with Boehm 8.2.8 from nixpkgs
**Workload**: `(import <nixpkgs> {}).hello.drvPath` under v3-direct
**Goal**: per IDEAL_GC_DESIGN §6.2, attempt the "1 day, 100-300 MB
recovery" Boehm-tuning spike before committing to Stages 5-6 effort.

## Hypothesis

Boehm's heap shows 402.9 MB total, 402.8 MB free (99.9 %) on
hello.drvPath.  Tuning Boehm's free-space divisor and forcing
explicit unmap should release the 400 MB watermark back to the OS.

## Procedure

Three runtime knobs tried (each is the documented Boehm API for the
suspected mechanism):

1. `GC_set_free_space_divisor(30)` — target only ~3 % free instead
   of default 33 %.  Should drive more frequent collection +
   smaller watermark.
2. `GC_gcollect_and_unmap()` — explicit "collect + unmap" call,
   documented in Boehm 8.2.8 as the unconditional unmap path.
3. `GC_set_force_unmap_on_gcollect(1)` — flag that gates whether
   explicit collects also unmap (defaults off).

Wired into `limits.cc::initLimits` and `run.cc`'s end-of-eval stats
path, with `boehm_unmapped = GC_get_unmapped_bytes()` added to the
"v3-direct memory:" report line for direct verification.

## Result

```
                                             boehm_heap  boehm_free  boehm_unmapped  peak_rss  gc_count
BASELINE                                       402.9 MB    402.8 MB        0.0 MB     753.8 MB   1
FREE_DIV=30 (target 3 % free)                  402.9 MB    402.8 MB        0.0 MB     753.5 MB   1
FORCE_UNMAP=1 (GC_gcollect_and_unmap, no flag) 402.9 MB    402.8 MB        0.0 MB     770.4 MB  13
FORCE_UNMAP=1 + force_unmap_on_gcollect(1)     402.9 MB    402.8 MB        0.0 MB     770.0 MB  13
```

**`boehm_unmapped` never moved off zero**, even with the documented
"force unmap + explicit unmap-collect" combination.  The 402.9 MB
heap watermark is structural — Boehm in this build sees 402.8 MB
free, but does not release the pages to the OS.

## Why the mechanism doesn't fire

The library is linked with `_munmap` (verified via `nm`) and exports
`GC_get_unmapped_bytes` (returns 0), so munmap support is compiled
in.  But every arena block of `threadArena` is registered with Boehm
via `GC_add_roots` (`alloc.hh::refill`, lines 698-708).  Boehm
conservatively scans those arena blocks for pointers into its heap;
on a multi-GB arena that scan likely finds ambiguous pointer-shaped
bit patterns on the order of "small enough to think a Boehm chunk
is referenced."  Even with `GC_set_force_unmap_on_gcollect(1)` and
13 explicit collections, no chunk crossed the internal
`GC_UNMAP_THRESHOLD` of 6 consecutive empty cycles required to
mark it unmappable.

In other words: the cheap-knob fix doesn't work because the v3
arena is BOTH huge AND registered as a Boehm root — Boehm
permanently sees enough false-positive pointer alias risk to
pin all its empty chunks.

## Decision per Rule 0

This is the falsifier.  The §6.2 "cheap-knob 100-300 MB recovery"
path is **FALSIFIED on this platform / library combination**.

What remains live:
* **§6.1 precise-root infrastructure** (Stages 4-6 in the foundation
  doc) — confirmed SHIP-GREEN via the live-fraction spike
  (`LIVE_FRACTION_SPIKE_2026-05-27.md`).  239 MB freeable on
  hello.drvPath; that path is the one that delivers.
* **Architectural Boehm replacement** — bigger project.  Not in
  reach this turn.

What's removed:
* The Boehm-tuning sub-track is closed.  Future "let's just tune
  Boehm" suggestions need to first demonstrate `boehm_unmapped`
  moves off zero on macOS aarch64.

## What this turn LANDS regardless

The instrumentation is no-regret:
* `boehm_unmapped` is now in the `v3-direct memory:` stats line —
  any future Boehm tuning attempt can verify mechanism behaviour
  in one run.
* `NIX_V3_BOEHM_FREE_DIV` env-gate remains in `initLimits` — costs
  one cached-bool branch per process, future-proofs against
  newer Boehm versions where the mechanism might differ.
* `NIX_V3_BOEHM_FORCE_UNMAP` env-gate remains as the diagnostic
  switch — falsifies any reopening of the §6.2 hypothesis.

Both gates carry inline retirement criteria.

## Cross-references

* `lode/LIVE_FRACTION_SPIKE_2026-05-27.md` — Stage 6 SHIP-GREEN
  via precise-root path, the alternative that DID deliver.
* `lode/IDEAL_GC_DESIGN_2026-05-26.md` §6.2 — original spike
  proposal, now falsified.
* `lode/GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md` — wall-perf
  falsifier (this is the memory-perf falsifier counterpart).
* [[falsification-rule]] — what hypothesis does this commit kill?
  Cheap Boehm tuning as a path to peak RSS reduction.
* [[measure-twice-cut-once]] — exit gate per pre-committed
  thresholds: spike found 0 MB recovered vs 100-300 MB needed →
  hypothesis falsified.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
