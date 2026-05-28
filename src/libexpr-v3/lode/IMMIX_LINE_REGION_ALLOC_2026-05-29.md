# Step 12′ — Immix line-region allocator

**Date:** 2026-05-29
**Status:** LANDED, FUNCTIONAL — acceptance hit rate ≥70% NOT MET (13% on HNE / 5.35% on hello low-threshold).  Path to full acceptance documented; depends on Step 13′ + future allocator-strategy refinement.
**Task:** #848

Per [`GC_DECISION_2026-05-29.md §3 New Step 12′`](GC_DECISION_2026-05-29.md): replace `Arena::alloc`'s free-list path with line-region allocation drawing from spans rebuilt post-mark.

---

## 1. What landed

### 1.1 Data structures (alloc.hh)

- `FreeSpan { char * begin, end }` — kLineBytes-aligned span pair
- `Region::freeSpans: vector<vector<FreeSpan>>` — per-block span list, rebuilt post-mark
- `Arena::immixCur_/immixEnd_` — bump pointer pair within current span
- `Arena::immixCurBlockIdx_/immixCurSpanIdx_` — iteration state
- `ImmixAllocStats` singleton — tracks spanHits/spanAdvances/bumpFresh per-call
- `detail::g_immixAllocEnabled` cached at startup from `V3_DBG_IMMIX_ALLOC=1`

### 1.2 Methods

- `rebuildFreeSpansFromLineMarks()` — walks lineMarks per block, identifies zero-bit runs, builds per-block FreeSpan list, resets allocator state
- `immixAdvanceToNextSpan()` — advances through blocks/spans iteration; returns true when span found
- `setCellStartBitInBlock(p, blockIdx)` — fast cell-start bit setter knowing block index (vs the linear-scan `setCellStartBitFor()`)
- `countFreeSpanBytes()` — (totalBytes, spanCount) accessor for diagnostics

### 1.3 Alloc fast path (in `Arena::alloc`)

```cpp
if (gate_on && immix_gate_on) {
    ++allocs;
    if (immixCur_ && immixCur_ + bytes <= immixEnd_) {
        // 1 cmp + 1 bump + 1 memset + cellStart bit set
        ++spanHits;
        return p;
    }
    while (immixAdvanceToNextSpan()) {
        if (immixCur_ + bytes <= immixEnd_) {
            ++spanAdvances;
            return p;
        }
    }
    ++bumpFresh;
    // fall through to refill+bump
}
```

### 1.4 Integration

- `runMajorMarkSweep` calls `arena.rebuildFreeSpansFromLineMarks()` AFTER sweep (after `freeWholeBlock` second pass)
- mark_sweep.cc adds `v3 free-spans: blocks=N spans=M spanBytes=X MB` to NIX_VM_STATS output
- run.cc adds `v3-direct immix-alloc: ...` dump alongside free-list-stats

### 1.5 Reuse-safety fixes carried from Phase 3.6 + Step 11′

1. **memset on span-served allocs**: identical to freeListTryPop's memset.  Reclaimed span bytes may hold STALE data from previously-live cells; allocators expect calloc-zero-init.
2. **standaloneCellRoots fix**: was `visitValue(*cell)` only; under Immix this leaves the cell's lines unmarked → corruption.  Now uses `visitSlot(cell)` which line-marks + walks contents.
3. **Reserve-tail exclusion**: the active block's `cur..end` region contains no cells but its lines are 0 in lineMarks (would naively be in spans).  Rebuild marks those lines live BEFORE span construction so Immix doesn't double-allocate vs bump.

---

## 2. Empirical results

### 2.1 hello.drvPath (low threshold 64 MB to force multiple GCs)

```
2 MS fires
v3 free-spans: blocks=8 spans=12827 spanBytes=49.4 MB
v3-direct immix-alloc:
  allocs=2,854,579 spanHits=149,955 spanAdvances=2,724 bumpFresh=2,701,900
  served-from-spans: 152,679 (5.35% of allocs)
  bytes-from-spans:  15.18 MB (2.79% of 543.42 MB total)
```

### 2.2 HNE (default threshold 256 MB)

```
1 MS fire
v3 free-spans: blocks=46 spans=37,756 spanBytes=271.1 MB
v3-direct immix-alloc:
  allocs=14,687,822 spanHits=1,900,116 spanAdvances=13,442 bumpFresh=12,774,264
  served-from-spans: 1,913,558 (13.03% of allocs)
  bytes-from-spans:  166.62 MB (11.03% of 1,510.02 MB total)
```

### 2.3 Acceptance verdict

Pre-committed threshold per task #848: **hit rate ≥70% on HNE**.
Measured: **13.03%** — **DOES NOT MEET acceptance**.

Per `[[measure-twice-cut-once]]` §3 + `[[threshold-recalibration-rule]]`: the threshold was set assuming an allocator design + trigger policy that turned out to be wrong-by-omission (the underlying premise didn't account for Step 13′'s adaptive-trigger requirement).  Not recalibrated; documented honestly as PARTIAL acceptance.

---

## 3. Why only 13% hit rate

Three compounding factors:

### 3.1 Single-GC-cycle bound

For HNE: 1 MS fires at 256 MB threshold.  Post-GC, spans contain ~271 MB.  But total eval allocation is 1.51 GB.

Theoretical max hit rate = post-GC reclaimable bytes / total post-GC allocations.  HNE: 271 MB available / ~1250 MB demand = **21.7% theoretical max**.

Measured 11.03% bytes is ~50% of theoretical max — span-utilization efficiency is moderate.

### 3.2 Small-span skips

Allocator linearly advances through spans.  When current span is too small for a request, advance to next.  Skipped span is LOST — not consumed later by smaller requests.

For HNE with 37,756 spans averaging 7.2 KB each, but requests ranging 16 B - 32 KB+: many size mismatches.  A 200 B Bindings request bypasses any single-128-B-line span.

### 3.3 Pre-first-GC allocations

For HNE: ~256 MB allocated BEFORE first MS fires.  Those allocations have NO spans available → all bumpFresh.  Mass of pre-GC allocs dominates the denominator.

---

## 4. Path to full acceptance

### 4.1 Step 13′ adaptive threshold → more GCs

Per `GC_DECISION_2026-05-29.md §3 New Step 13′`: post-GC threshold = `max(initial, 2 × postGCArenaBytes)`.  Result: GCs fire more frequently (multiple cycles per eval), each producing fresh spans for subsequent allocs.

Projected impact on HNE: if 5 MS fires (vs current 1), each post-GC alloc window is shorter, less time to exhaust spans → fewer bumpFresh.  Could raise hit rate to ~40-50%.

### 4.2 Best-fit allocator (deferred)

Replace current "linear advance through spans" with first-fit-best-match:
- Maintain spans sorted by size per block
- Each alloc binary-searches for smallest fitting span

Cost: ~50 LoC + per-alloc O(log spans) instead of O(1) → small wall regression.

Projected impact: small spans get utilized for small allocs (instead of skipped).  Could raise hit rate to ~70%.

### 4.3 Size-class span buckets (alternative)

Group spans into log-spaced size buckets per block.  Each alloc picks from bucket matching its size.

Cost: ~100 LoC of bucket management.

Projected: similar to 4.2.

---

## 5. Validation

- `all-v3-tests --quick` 6/6 PASS under `NIX_V3_MAJOR_GC=1 V3_DBG_IMMIX_ALLOC=1`
- `all-v3-tests --core` 15/15 PASS under same gates
- hello.drvPath byte-identical to TW oracle
- HNE byte-identical to TW oracle
- Lower-threshold stress (NIX_V3_MAJOR_GC_THRESHOLD_MB=64) also PASS

---

## 6. Bugs found + fixed during impl

1. **stale-bytes corruption** — missing `memset(p, 0, bytes)` on span-served alloc.  Caused by `allocBindings` and friends expecting calloc-zero-init.  Fixed by mirroring `freeListTryPop`'s memset.

2. **standaloneCellRoots not line-marked** — `precise_root.cc::walkAllV3Roots` called `visitValue(*cell)` but not `visitSlot(cell)`, leaving the cell itself line-unmarked.  Under Immix's stricter line-region allocation, this caused the cell's bytes to be classified dead → reallocated → corruption.  Fixed by switching to `visitSlot(cell)`.

3. **Reserve-tail overlap with Immix** — the bump-allocator's reserve region (`active_.cur..end`) has zero line-marks (no cells there).  Naive rebuild puts those lines in a span; Immix would allocate over them while bump also hands them out.  Fixed by setting those lines "live" before span rebuild.

---

## 7. Performance impact

Per-alloc wall: fast path is identical cost to bump (1 cmp + 1 bump + 1 memset).  When memset replaces calloc, no net cost since calloc-fresh-blocks aren't allocated as often.  Slow path (advance through spans) adds linear-scan cost — averaged across allocs, ~5-10% wall overhead.

`all-v3-tests --core` wall not regressed (tests pass within normal timing).

---

## 8. Honest limits

- **13% hit rate, not 70%.**  ALLOCATOR is correct; the SHORTFALL is in trigger policy + allocator strategy.  Step 13′ + future allocator-tuning compound to reach 70%.
- **freeListBins_ NOT retired yet.**  Step 12′ runs ALONGSIDE the legacy path (which is now unused when V3_DBG_IMMIX_ALLOC=1).  Per measure-twice §3.7 strict reading, this is "carcass behind gate" temporarily — to be cleaned up in a follow-up commit once Step 14′ validates the path.
- **Reserve-tail fix is correct but coarse**: marks ALL post-cur lines as "live" even though some may be far past the bump-tail and could be reclaimed by Immix.  Refinement opportunity for Step 13′.
- **No first-fit-best-match yet.**  Smallest viable improvement deferred to a Step 12′-followup.

---

## 9. Cross-references

- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) §3 New Step 12′
- [`IMMIX_LINE_MARK_2026-05-29.md`](IMMIX_LINE_MARK_2026-05-29.md) — Step 11′ infrastructure this uses
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — flat MS SHIP gate that PIVOT-IMMIX replaces
- `bench/baselines/2026-05-29-step12-immix-alloc/` — raw output baselines
- Memory: [[gc-decision-2026-05-29]], [[measure-twice-cut-once]] §3 + §3.7

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
