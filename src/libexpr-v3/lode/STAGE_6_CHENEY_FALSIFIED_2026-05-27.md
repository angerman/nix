# Stage 6 Cheney semispace design — Rule 0 FALSIFIED (2026-05-27)

Status: **Cheney semispace as implemented is NOT a viable Stage 6 production
design.** The Day 3 Step 7 SHIP-GREEN claim (commit `831d36e89`, "722 MB peak
RSS reduction on hello.drvPath") was based on a CORRUPTED measurement —
strings/paths in arena bytes were dangling post-scavenge, and the OS hadn't
yet reused those pages, so reads happened to return the old (correct) bytes.

When string/path forwarding is properly added, the real measurement reveals
that copying ARENA contents to backup doubles peak RSS during scavenge,
overwhelming any post-scavenge reduction.

## Hypothesis tested

Per `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` + `STAGE_6_IMPLEMENTATION_GUIDE`:

> Per-workload SHIP gates: ≥ 200 MB hello.drvPath, ≥ 600 MB HNE.

## Honest measurement (post string-forwarding fix)

hello.drvPath, default 256 MB threshold, growth=2.0:

| Mode      | peak_rss | v3_arena | drvPath status |
|-----------|---------:|---------:|----------------|
| gate-OFF  |  753 MB  |  587 MB  | byte-identical |
| gate-ON   | **1239 MB** |  436 MB | byte-identical |

**Delta**: **+486 MB peak_rss REGRESSION** (gate-ON is 1.65× gate-OFF).

HNE under gate: still SIGSEGVs after the first scavenge (~471 MB live set
copied, then crash). Different untracked pointer kind than the
allocValue-cell case fixed in this session.

## Why the claim was wrong

The Step 7 commit's claim of "peak_rss=31.4 MB under gate" was a measurement
of `getrusage(RUSAGE_SELF).ru_maxrss` AFTER the eval finished, taken at a
moment when:

* String/path pointers in live Tag::String / Tag::Path Values were dangling
  into freed OLD-active blocks.
* The OS hadn't yet reused those page slots, so `printValue(drvPath)` read
  the OLD bytes and produced the correct drvPath.
* But the process's resident-set never had to PAGE IN the live set + the
  copied backup simultaneously — because the strings were "still there"
  by accident.

When the same workload runs after this commit's `fwdChars` is wired,
strings + paths are properly copied to backup. The result is correct, and
the resident set during scavenge reflects:

* OLD-active (full arena, ~600 MB on hello)
* NEW-backup (live set copy, ~300-450 MB on hello)
* Combined = ~900 MB+ during scavenge → peak

After swap+free of OLD-active, the resident set drops, but the PEAK is
already locked in.

## Why Cheney is fundamentally wrong for this arena

The v3 arena is dominated (~84%) by `Bindings` allocations. The Cheney
loop copies all reachable Bindings to backup before freeing OLD. That
is a guaranteed 2× peak for the dominant allocation class.

Even with a perfect live-fraction (e.g., 50% reclaimable), peak is still:
`(1.0 × arena_size) + (0.5 × arena_size) = 1.5 × arena_size` — strictly
worse than gate-OFF.

The pre-Stage-6 spike (`LIVE_FRACTION_SPIKE_2026-05-27.md`) measured 239 MB
"freeable" on hello.drvPath at end-of-run. That measurement is a STEADY-STATE
post-collection size, not a PEAK during collection. Cheney's 2× peak negates
this delta.

## Next-step design alternatives

1. **Mark-sweep with block-aware freeing** — walk roots + mark reachable;
   sweep each arena block; free any block where ALL cells are unmarked.
   No 2× peak. Trade-off: fragmentation (no compaction) means worse
   post-collection arena size than Cheney.

2. **Generational copy** — only copy young objects (recently-allocated).
   The "old" generation (tenured) stays in place across most collections.
   Peak is 2× *young set* (small) rather than 2× *arena* (huge).
   Requires age tracking + write barriers (we have Phase D barriers).

3. **In-place mark + free-list** — keep arena's bump-allocator AND maintain
   per-block free lists for reclaimed cells. Allocations check free list
   first before bumping. Avoids both copy peak AND fragmentation cost.
   Significantly more complex implementation.

4. **Do nothing (revert Stage 6)** — accept current gate-OFF peak as the
   memory ceiling. Pursue other RSS-reduction strategies first (e.g.,
   string interning, structural sharing for Bindings, ImportCache LRU).

The session-arc memory `[Memory first-class]` says memory is a higher-slope
optimization target than wall. Pursuing alternative 4 (other strategies)
may be more productive than continuing to fight Cheney's fundamental peak
behavior.

## What was correctly built in this session (preserved)

The session DID land genuine correctness work:

* **walkThunk standalone-cell discover-on-need fix** — was lookup-or-leave;
  now uses fwdCell, copies standalone allocValue cells properly.
* **String/path forwarding via `fwdChars`** — Tag::String + Tag::Path
  payloads (allocChars bytes) properly copied + string-context side-table
  re-keyed.
* **Sorted-index optimization for Bindings byte-range lookup** —
  O(log B) per slot instead of O(B). Without this HNE hangs minutes
  in a linear scan.
* **Dynamic threshold (growth-factor)** — prevents tight-loop re-scavenge
  when live set > initial threshold. NIX_V3_MAJOR_GC_GROWTH=2.0 default.
* **visitString/visitPath RootVisitor callbacks** — default no-op; opt-in
  for moving visitors.

These are valuable building blocks for any future GC design (mark-sweep,
generational, etc.) regardless of whether Cheney itself ships.

## Decision

Stage 6 Cheney design is FALSIFIED for the v3 arena's bindings-dominated
allocation profile. The current implementation is retained behind the
opt-in `NIX_V3_MAJOR_GC=1` env gate (default-OFF; production unaffected)
so future work can measure pivots from the same code base.

Next investigation: prototype mark-sweep with block-aware freeing
(alternative 1 above) before committing to a full Stage 6 redesign.

Per Rule 0: this commit kills the hypothesis "Cheney semispace major
scavenge produces a net peak-RSS reduction on v3 workloads with the
current arena allocation profile."

Per measure-twice-cut-once §3.7: the Step 7 commit's SHIP claim was a
post-hoc reading of the actual measurement. The PRE-COMMITTED threshold
(≥ 200 MB hello.drvPath reduction) WAS NOT MET when measured honestly —
the corruption masked it. Step 7 should not have shipped as SHIP-GREEN.
This document corrects the record.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
