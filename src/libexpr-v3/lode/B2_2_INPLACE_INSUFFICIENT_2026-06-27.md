# B2.2 — in-place lane recycling is byte-id but does NOT lower peak; moving evac required

**Date:** 2026-06-27  **Branch:** angerman/2.35-eval-profiling-v2

Built the per-lane recycle MECHANISM (laneAdvanceToSpan + bumpInLane recycle path +
resetLanesForRecycle) and wired it into NORMAL allocation (bumpInLane reuses the lane's
post-sweep dead spans before refilling fresh).  Correct + safe (no moving, no
candidate-exclusion needed — just reuse genuinely-dead cells).

## Result — byte-id ✓, but peak NOT lowered

firefox.drvPath:
```
mid-eval-only : peakArena=352MB  maxRSS=721MB
BiBOP+recycle : peakArena=386MB  maxRSS=717MB
```
byte-id (hello/git/firefox) ✓ — recycling into swept dead cells is correct.

But the in-place recycle does NOT lower peak: peakArena is +34MB (BiBOP's 6 per-lane tail
blocks), maxRSS ~unchanged (within noise).  This confirms the NIX_V3_MIDEVAL_REUSE
free-list precedent + the mechanics: **in-place reuse fills dead space INSIDE already-mapped
blocks, so the mapped-block high-water (= peak RSS) doesn't shrink.**  The arena reaches its
peak (all blocks mapped) before/regardless of recycling; recycling only reuses dead cells in
those still-mapped blocks for FUTURE allocs.

## Conclusion — the peak-RSS win requires the MOVING evac (block-free)

To LOWER peak, whole blocks must be freed + munmap'd (B0.2 made munmap real).  A block frees
only when it has ZERO live cells.  BiBOP's high-mortality lane blocks are SPARSE (73-81%
dead) but not fully dead → their few live cells must be MOVED out (compaction) → block
empties → freeWholeBlock → munmap → peak drops.  That is the moving evac, and its dest
allocation IS the recycle mechanism built here (laneAdvanceToSpan), with the added
candidate-EXCLUSION (don't recycle survivors into a block being freed → UAF).

So: the recycle mechanism (committed, byte-id) is the building block; the remaining
moving-evac + candidate-exclusion is the corruption-prone final step that turns the 131MB
recyclable into actual peak-RSS reclaim.  In-place-alone is measured-insufficient (a real
kill of "in-place reuse lowers peak").

NOTE: BiBOP currently (lanes + in-place recycle, no moving evac) is net-neutral-to-slightly-
worse (+34MB arena frag, maxRSS within noise) — it only pays off WITH the moving evac.  All
gated NIX_V3_BIBOP default-off.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
