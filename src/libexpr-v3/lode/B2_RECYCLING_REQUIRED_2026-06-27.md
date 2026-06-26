# B2 — measure-first: BiBOP makes sources sparse, but recycling is STILL required

**Date:** 2026-06-27  **Branch:** angerman/2.35-eval-profiling-v2

Before building the per-lane recycling evac (B2.1/B2.2), tested the cheap hypothesis:
"BiBOP single-type sparse blocks + the EXISTING fresh-dest evac → net-positive RSS"
(since the S2.2 net-negative was blamed on *mixed* blocks never emptying).

## Result — still net-negative (firefox, BiBOP + EVAC + PRECISE_ONLY, byte-id OK)

```
PCT=0.25 : movedBytes=13MB  freedRSS=0MB   NET=-13MB
PCT=0.5  : movedBytes=123MB freedRSS=34MB  NET=-89MB
PCT=0.75 : movedBytes=185MB freedRSS=50MB  NET=-135MB
```

BiBOP DOES help the source side: `freedRSS` is 34-50MB (vs S2.2's ~16MB on mixed blocks)
— the sparse single-type source blocks actually empty + free now. BUT the existing evac
still bump-allocates dest into FRESH blocks (`forceFreshBlock`), so `movedBytes` (123-185MB
of fresh dest) dwarfs the freed sources → net-negative at every PCT.

## Conclusion — recycling is mandatory (not optional)

The fresh-dest churn is the residual blocker REGARDLESS of source sparsity: the evac
re-allocates everything it moves. Net = freedRSS − movedBytes is negative until the dest
stops consuming fresh blocks. So B2.1/B2.2 (rebuild per-lane free spans + dest recycles
into the lane's EXISTING partial blocks' dead space) is the REQUIRED mechanism, not a
tuning nicety. With recycling, moved cells fill existing dead space (no new blocks) →
movedBytes adds ~0 arena → net ≈ freedRSS (positive).

Also landed: the lane-repoint fix (alloc.hh freeWholeBlock) — re-points lanes_[].blockIdx
after the block-index shift a free causes (the B1.1-noted hazard); byte-id held across the
BiBOP+EVAC PCT sweep (freeWholeBlock fired + lanes re-pointed correctly).

NEXT: B2.1 per-lane free-span rebuild → B2.2 evac dest → lane partial spans (drop
forceFreshBlock for the lane path; lane-scoped Immix span alloc) → B2.4 confirm net-positive.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
