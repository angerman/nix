# S2.2 — why the (correct) moving compactor doesn't free blocks / realize RSS

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Context:** S2.1b made the mid-eval moving compactor CORRECT (byte-id, validated A/B).
But `freedRSS=16.8MB` despite moving 342MB / 5.4M cells (`blocksFreed=1` of 43
candidates). This RCA finds why — and overturns the obvious theory.

All measurements: firefox.drvPath, full-compaction evac (NIX_V3_EVAC + PRECISE_ONLY +
PCT=1.0 + MIDEVAL_GC + NO_CONSERV_SCAN), inside `nix develop -c` (deterministic).

## Instrument: declined-by-type + block-pin decomposition

Added `v3 evac-declined` (per-CellType count of cells `fwdRaw` declines to move →
they pin their candidate block, blocking whole-block-free). Result on firefox:

```
v3 evac-declined (unmovable→block-pin): None=0 Value=0 Closure=0 Thunk=0
                                        Bindings=0 List=0 Pair=0 Env=0 Chars=0
```

→ **Every TYPED cell (Closure/Thunk/Bindings/List/Pair) is moved** (declined=0); only
7 blackhole thunks (the S2.1b pin) + Chars (via the separate `evacChars`, skipped) + Env
(non-moving by env-sharing design) are unmoved.

## The decisive decomposition — Chars/Env unmovability is NOT the block-free blocker

| config (firefox) | blocksFreed | freedRSS |
|---|---:|---:|
| baseline (Chars+Env pinned) | 1 | 16.8MB |
| Env movable (NIX_V3_NO_ENV_SHARING=1) | 2 | 33.6MB |
| Chars movable (NIX_V3_EVAC_RELOC_CHARS=1) | 1 | 16.8MB |
| both movable | 0* | 0* |

(* Chars-reloc corrupts the eval — S2.1a bug-class-2 still live — so that row is an
artifact of a corrupted run, discount it.)

**Making Chars and/or Env movable does NOT increase block-free** (flat at 1-2 of 43).
So the ~128MB ceiling is NOT gated by scattered unmovable Chars/Env, as assumed.

## Root cause: full-compaction moves to FRESH blocks = churn, not reduction

`fwdRaw` allocates each dest via `threadArena().alloc()` — a FRESH block. At PCT=1.0 the
evac moves ~all live cells (342MB) into ~21 fresh dest blocks while freeing only the
source blocks that end up fully empty (≈1). Net: the arena does NOT shrink — the same
live data just relocates from source blocks to an equal number of fresh dest blocks
(churn). A moving compactor reduces RSS only when it packs SPARSE-block cells into
EXISTING PARTIAL blocks (Immix recycling / hole-filling) and frees the emptied sparse
blocks — which this evac does NOT do.

Compounding it: firefox's blocks cluster UNIFORMLY 25-75% live (density histogram
`0|0|11|5|4`, sparseBlocks=0-1 — the #136/S2.3 finding). There is no sparse/dense split
to exploit; the only gain is pairing ~50%-live blocks (2→1), a modest ceiling, and only
if dest allocation recycles into partial blocks.

## Verdict + path

S2.2's RSS realization is gated by the evac's DEST-ALLOCATION STRATEGY (fresh-block bump),
NOT cell movability. To realize even the modest firefox ceiling:
1. **Dest recycling**: allocate evac dest cells into EXISTING partial blocks (the Immix
   line-mark machinery already computes free spans — `v3 free-spans` line — but the evac
   ignores it and bumps fresh). Wire fwdRaw's dest alloc to recycle partial blocks.
2. Only then do sparse/half-full source blocks actually empty + free + munmap.
3. (evacChars correctness — S2.1a bug-class-2 — is a SEPARATE prerequisite if Chars need
   to move for a block to empty; but per the table above it's not the current blocker.)

This is substantial foundational allocator work (Immix evacuation-into-recycled-blocks),
with a MODEST ceiling on firefox (uniform 25-75% density → ~pair-and-pack, well under the
naive 128MB perfect-packing figure). Consistent with the campaign's standing verdict: the
RSS win needs a real Immix recycling evacuator + likely BiBOP segregation, not a single
lever. The compactor is CORRECT (S2.1b); making it PROFITABLE is the next foundational sub-
project (#171).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
