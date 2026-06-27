# BiBOP campaign conclusion — foundation built + validated, peak-RSS payoff FALSIFIED

**Date:** 2026-06-27  **Branch:** angerman/2.35-eval-profiling-v2
**Scope:** B0.1–B2.2 (#176–186) built; B3 (#187–189) resolved by this conclusion.

## What BiBOP was

Per-CellType allocation "lanes" inside the arena → single-type blocks → cluster the
high-mortality types (Closure 81% / Bindings 75% / List 73% dead at firefox peak) so a
moving compactor can empty + munmap sparse blocks. B0.3 GO/NO-GO projected (assuming
PERFECT ceil(live/16MB) compaction): firefox 84–168MB reclaim (marginal), **M5 587MB
(STRONG GO)** → greenlit on the M5 case.

## What was built + validated (byte-id + --brute 22/22 throughout)

- B0.1 typed scavenger promotion; B0.2 mmap tenured blocks (real munmap);
- B1 per-CellType lanes (`laneFor`/`bumpInLane`/`blockLane`) + lane-repoint on free;
  **density variance CONFIRMED** (the hardest unknown — single-type blocks do cluster);
- B2.1 per-lane free-span rebuild (131MB recyclable confirmed under BiBOP);
- B2.2 recycle mechanism (`laneAdvanceToSpan`/`resetLanesForRecycle`) + whole-block-free
  under BiBOP + the moving recycle-dest evac (`prepareEvacRecycle`) + its SEGV fix
  (`evacClobberedMarks`: don't rebuild mutator spans from the verify's precise-only marks).

The foundation WORKS. The payoff does not.

## The payoff is FALSIFIED — four measured kills

1. **in-place recycle** (B2.2): byte-id, but peak NOT lowered — reuse fills dead space
   INSIDE mapped blocks, so the mapped high-water (= peak) doesn't shrink. firefox 352→386
   (lane frag). [B2_2_INPLACE_INSUFFICIENT]
2. **whole-block-free of fully-dead blocks** (B2.2): byte-id, frees only **1 block** —
   even single-type lanes rarely make a block fully dead (mortality spreads ACROSS blocks).
3. **moving evac + recycle-dest** (B2.2): byte-id firefox, frees 151MB, but peak RISES
   386→**537MB**. The arena grows to peak BETWEEN sweeps (live flat ~250); the evac fires
   AT the sweep AFTER the peak, and its moving copy-churn (Cheney 2× transient) adds fresh
   dest blocks. Aggressive triggering raises peak too (403/503 — more frag).
   [B2_2_MOVING_EVAC_PEAK_KILL]
4. **M5 ceiling** (B3.1, this doc): M5 baseline peakArena=1275MB, maxLive=592MB (byte-id).
   The recycle-dest evac SEGVs on M5 (a 2nd, M5-specific corruption — not fixed, see below).
   But the number is irrelevant: even reclaiming ALL 683MB of arena dead leaves v3 total RSS
   2984−683 = **2301MB vs TW 982MB = 2.34× TW**. The ~1.2GB non-arena RSS (Boehm /
   ImportCache / SQLite) + the 592MB live arena dominate; arena reclamation cannot make v3
   competitive on the very workload BiBOP was greenlit for.

## Why the M5 SEGV was NOT pursued

Fixing it is a multi-cycle corruption debug (the S2.1a precedent: M5 evac is the hardest
path). The ceiling math (#4) makes the resulting number unable to change the verdict
(perfect-case still 2.34× TW), and the firefox kill (#3) already proves the PRACTICAL
mechanism raises peak. Grinding the SEGV would violate measure-first discipline (debugging
toward an irrelevant number). The direct M5-evac peakArena is the one unmeasured cell, and
the verdict is robust to it.

## Verdict — B0.3 GO falsified by implementation; B3 = NO-GO / leave gated

B0.3's projection assumed perfect compaction. Real compaction (a) churns peak up via the
moving copy transient, (b) fires after the between-sweep peak, (c) recycles imperfectly.
And the M5 ceiling shows even the perfect case can't beat TW. BiBOP is the most thorough
reclamation attempt in the campaign (a full per-type moving compactor) and it CONFIRMS the
standing finding: **all reclaim-based RSS levers are dead. v3's RSS gap vs TW is the LIVE
representation** (v3 cells + CU structs + Boehm heavier than TW's 16B niche-tagged Value),
not reclaimable garbage. Beating TW on RSS requires shrinking the live representation — a
broad foundational program, not any GC/compaction lever.

- B3.1 ship gate: **NO-GO** (firefox peak rises; M5 ceiling 2.34× TW).
- B3.2 CPU cost: MOOT (won't ship).
- B3.3 default-flip: **leave gated NIX_V3_BIBOP/EVAC default-off** as the falsification
  evidence (Rule 0: the gated code + these docs ARE the kill record). Retire-able.
- B1.3 metadata shrink, B2.3 PCT tuning, B2.4 net-positive: MOOT (approach killed).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
