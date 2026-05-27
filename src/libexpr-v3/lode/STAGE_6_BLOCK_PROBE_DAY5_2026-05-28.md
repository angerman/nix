# Stage 6 Day 5 — block-fill probe measurement results (2026-05-28)

Per `STAGE_6_CHENEY_FALSIFIED_2026-05-27.md` alternative #2: prototype
generational mark-sweep with block-aware freeing. Cheap measure-twice
proxy (`NIX_V3_BLOCK_PROBE=1`, `live_trace.cc::BlockProbe`): walk all
precise roots, attribute marked-cell bytes to containing arena blocks,
report per-block fill histogram.

## Pre-committed SHIP threshold

≥30% of arena recoverable via fully-dead block freeing.

## Results

### hello.drvPath

| Metric                       | Value         |
|------------------------------|--------------:|
| Reachable closures           | 1,484         |
| Reachable thunks             | 23,391        |
| Reachable bindings           | 16,032        |
| Reachable lists              | 379           |
| Reachable pairs              | 53,220        |
| Reachable Tag::Slot cells    | 18,456        |
| Reachable strings/paths      | 53,960        |
| Regular blocks               | 34 (570.4 MB) |
| Live bytes                   | 301.5 MB      |
| Avg fill                     | 52.9%         |
| **Fully-dead blocks**        | **1 (2.9%)**  |
| **Sweepable bytes**          | **16.8 MB (2.9% of arena)** |

Fill histogram:
* empty (<1%): 13 blocks
* very-low (<10%): 2
* low (<25%): 1
* mid (<50%): 0
* high (<75%): 0
* full (75%+): 18

**Verdict: FAIL (2.9% < 30%).**

### HNE (haskell-nix-example `.packages.x86_64-linux.hello.drvPath`)

| Metric                       | Value          |
|------------------------------|---------------:|
| Reachable closures           | 80,472         |
| Reachable thunks             | 1,422,270      |
| Reachable bindings           | 313,630        |
| Reachable lists              | 591,190        |
| Reachable pairs              | 2,215,186      |
| Reachable Tag::Slot cells    | 1,295,062      |
| Reachable strings/paths      | 1,172,648      |
| Regular blocks               | 94 (1577.1 MB) |
| Live bytes                   | 681.7 MB       |
| Avg fill                     | 43.2%          |
| **Fully-dead blocks**        | **0 (0.0%)**   |
| **Sweepable bytes**          | **0.0 MB (0.0% of arena)** |

Fill histogram:
* empty (<1%): 0 blocks
* very-low (<10%): 0
* low (<25%): 30
* mid (<50%): 25
* high (<75%): 39
* full (75%+): 0

**Verdict: FAIL (0% < 30%).**

## Interpretation

The HNE distribution is fundamentally hostile to naive block-aware
sweep. Every 16 MB block contains at least 10% live data — no block
is fully dead, none even nearly so. The reason is structural:
haskell.nix's overlay machinery interleaves long-lived attrset
metadata with ephemeral derivation-construction cells; in a bump
allocator that produces uniformly-mixed blocks.

The hello.drvPath distribution is bimodal (13 empty + 18 full + a few
in between), reflecting cleaner temporal separation of phases. Block-
aware sweep would still recover only 16.8 MB (the one truly-dead
block); the 13 "empty" blocks have a tiny live tail (< 1% fill each)
that pins them.

## Therefore — simple block-aware sweep is FALSIFIED for v3

Same Rule 0 falsifier as Cheney: the measurement falls below the
pre-committed threshold. Implementation effort is not justified.

## What the data DOES suggest

A more selective design works:

* **Skip dense blocks** (75%+ fill): no copy, no peak addition
* **Evacuate sparse blocks** (< 25% fill): copy live cells to a
  compaction target; free the source block

For HNE:
* 30 low-fill blocks × 16 MB = 480 MB to scan
* Live in those: ~10-20% × 480 MB = 50-100 MB to copy
* Recover: ~380-430 MB (~24-27% of arena)
* Peak during compaction: 480 + 100 = ~580 MB scanned-plus-target
  (vs Cheney's full 1577 + 681 = ~2.3 GB)

For hello.drvPath:
* 16 low-fill blocks × 16 MB = 256 MB to scan
* Live in those: tiny tails, ~5-15 MB total
* Recover: ~240 MB (~42% of arena)
* Peak: ~270 MB (vs Cheney's ~900 MB)

This is SELECTIVE COMPACTION — formally a mark-region collector with
per-region evacuation policy. Closer to G1 (per-region selection)
than to plain GHC RTS (which still copies via Cheney within a gen).

**Pre-committed SHIP threshold cannot be met by simple block-aware
sweep on HNE. Selective compaction is the next falsifier target,
not naive sweep.**

## Recommended next step

Don't yet implement either mark-sweep or selective compaction.
Instead, build a second probe layer:

1. **Per-block age tracking** — when was each block first allocated?
   Long-lived cells correlate with old blocks; ephemeral with new.
2. **Block density correlation** — does sparse-fill correlate with
   block age? (If old blocks are dense, we know promotion already
   sorted them; sparse-fill blocks are the recent ones with lots of
   ephemerals mixed in.)
3. **Allocation site attribution** — which alloc sites contribute to
   sparse vs dense blocks? Could allocator routing be biased to put
   ephemerals in dedicated "young" blocks (a true generational
   nursery for tenured allocator)?

That measurement would tell us whether ALLOCATION-TIME ROUTING
(allocator hint based on alloc site) is a viable alternative to
selective compaction. Cheaper to implement, similar effect.

## What this probe IS valuable for

The probe itself is small (~250 LoC in live_trace.cc) and is now in
the tree behind `NIX_V3_BLOCK_PROBE=1`. It pays for itself across
any future Stage 6 design iteration:

* Detect when an allocator change improves block locality
* Bound any future GC design's reclaim ceiling
* Diff arena profiles workload-to-workload

Keep the probe; retire the Cheney scavenger entry point (or leave
gated default-OFF as a scaffold; no production cost).

## Decision

Per measure-twice-cut-once §3.1: SHIP threshold not met.
Per Rule 0: this measurement kills the hypothesis "block-aware sweep
of the v3 tenured arena recovers ≥30% of bytes via fully-dead block
freeing." HNE delivers 0%.

Per Rule 0 (preserved finding): the hello/HNE distribution split
HINTS at a viable selective-compaction design but that itself is
unmeasured. Don't commit to selective compaction until a similar
proxy measurement shows it would clear the SHIP threshold.

Genuine next-step proposal: implement block-age + alloc-site probes
(~1 day) before any GC implementation work. The cheapest experiments
have the highest information return.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
