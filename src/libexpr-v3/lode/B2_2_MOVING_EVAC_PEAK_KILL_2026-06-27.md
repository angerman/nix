# B2.2 — moving evac + recycle-dest: byte-id + frees blocks, but KILLS BiBOP peak-RSS (firefox)

**Date:** 2026-06-27  **Branch:** angerman/2.35-eval-profiling-v2

Completed the recycling-dest moving evac — the mechanism B0.3 projected would realize
the BiBOP peak-RSS win. It works (byte-id, frees real blocks) but does NOT lower peak;
it RAISES it. This + the two earlier cheap-path kills falsifies the BiBOP peak-RSS
hypothesis for firefox.

## What was built

- `prepareEvacRecycle(excludeBlocks)` (alloc.hh): rebuild full-mark spans, clear the
  candidate blocks' spans (candidate-exclusion — a YIELD optimization, NOT safety: the
  evac verify re-marks, so a survivor in a candidate merely keeps it live), reset lanes.
- runEvacuation: under BiBOP, dest copies recycle via this instead of forceFreshBlock.
- SEGV FIX (the corruption the S2.1a precedent warned of): the post-evac span rebuild
  (mark_sweep.cc) rebuilt from the evac VERIFY's PRECISE-ONLY marks, which under-mark
  conservatively-live cells → the mutator recycled into live cells → firefox SEGV.
  Fix: `SweepStats.evacClobberedMarks` — skip the post-evac rebuild+reset when the evac
  ran its precise-only verify (prepareEvacRecycle already built sound full-mark spans for
  the dest; the lane cursors point past the consumed regions into still-dead space).
  RCA'd, not guessed: hello/git/gcc (full precise coverage) byte-id'd; only firefox
  (conservative-only-live cells) crashed.

## Result — byte-id ✓, frees 151MB, but peak RISES 386→537MB (firefox, PCT=0.5)

```
arena trajectory (sweep points):  101→151→235→386→537MB   live: 81→123→181→251→257MB
per-evac:  movedBytes=6.4 freed=16.8 | 6.9 freed=0 | 26.7 freed=0 | 47.5 freed=134.2MB
TOTAL blocksFreed=11  freedRSS=151MB   peakArena=537MB(+151 vs 386)  maxRSS=1004MB
```

The arena grows to its peak BETWEEN sweeps (386→537 while live stays flat at 251→257);
the evac fires AT the sweep — AFTER the peak — and its moving copy-churn adds fresh dest
blocks the recycling can't fully absorb. So it frees 151MB but the PEAK (what RSS
measures) was already hit and is even higher than the no-evac baseline.

## All four BiBOP reclamation configs fail to lower firefox peak

```
in-place recycle (default trigger)       peakArena 386MB   (baseline mideval 352 + lane frag)
in-place recycle (aggressive thr=128)    peakArena 403MB   (worse — more churn/frag)
in-place recycle (aggressive thr=64)     peakArena 503MB   (worse — more churn/frag)
moving evac + recycle-dest (PCT=0.5)     peakArena 537MB   (worst — moving copy churn)
```

The peak is set by the mutator's live high-water (~250MB) + unavoidable arena slack
(16MB-block rounding + lane fragmentation + between-sweep growth). No reclamation path
catches it; moving even raises it (the Cheney 2× transient: src+dest coexist during the
move). More-frequent triggering raises it too (more recycle/rebuild fragmentation).

## Verdict — B0.3 GO projection FALSIFIED by implementation (firefox)

B0.3 projected LANES reclaim by assuming perfect compaction to ceil(live/16MB). Real
compaction (a) churns the arena up via the moving copy transient, (b) fires after the
between-sweep peak, (c) recycles imperfectly (spans insufficient → fresh dest). firefox
peak RSS is UNREDUCIBLE by BiBOP — consistent with the broader campaign finding that all
reclaim-based RSS levers are dead (the arena tracks the live working set, which is v3's
real RSS cost vs TW's leaner 16B Values).

M5 (the B0.3 STRONG-GO case, projected 587MB) remains to be measured on darwin-4 (B3.1) —
but this firefox kill-mechanism (moving churn + between-sweep peak + frag) is GENERAL and
strongly predicts M5 fails too. The decisive M5 darwin-4 measurement is next.

The recycle-dest + SEGV-fix are byte-id (hello/git/gcc/firefox) + --brute 22/22; all gated
NIX_V3_BIBOP/NIX_V3_EVAC default-off (experimental evidence for the kill, retire-able).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
