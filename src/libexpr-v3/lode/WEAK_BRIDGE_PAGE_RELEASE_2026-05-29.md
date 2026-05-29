# Weak-bridge page-release — design note (2026-05-29)

**Status:** design note only, no code.  Multi-week implementation needs user authorization before starting.

## Context

The #875 weak-bridge work landed three correctness-clean stages that all FAILED the memory SHIP gate:

| Stage | Commit | Memory Δ |
|---|---|---|
| Stage 1 — TW re-eval | `e72196dee` | 0 MB (inert, no fallback coverage) |
| Stage 1.5 — Expr-capture experiments | `2daee57c0` | falsified (divergent drvPath on HNE) |
| Stage 2 — serialize-on-sweep-evict | `70bc59fbe` | 0 MB (arena holds pages) |
| Stage 2b — serialize-at-creation (pre-evict) | `07399432d` | 0 MB (same) |

The shared root cause: v3 arena allocator doesn't release `mmap'd` pages on cell free.  `peak_rss` tracks the arena's high-water mark regardless of how aggressively bridges clear their `v3Value`.

## What needs to happen for memory ROI

For peak_rss to drop, the arena needs to:

1. **Track per-block live cell count.**  Today, blocks are appended to `active_.blocks` and never decrement.  Need: ref-count (or mark-sweep) so the arena knows which blocks are entirely empty.
2. **Release empty blocks back to the OS.**  Options:
   - `madvise(block, kBlockSize, MADV_DONTNEED)` — releases physical pages; keeps virtual mapping; subsequent reads zero-fill on demand.  Cost: zero per-block when not re-accessed.  Limitation: kernel may not honour on all platforms.
   - `free(block)` — full munmap.  Cleanest but requires removing the block from the arena's tracking + Boehm root registration.

## Two paths

### Path A: Lightweight ref-count + madvise

Minimal change to alloc.hh:

```cpp
class Arena {
    struct Block {
        char * data;
        uint64_t bytesAllocated = 0;   // running total from refill
        uint64_t bytesLive = 0;        // updated by external eviction
    };
    std::vector<Block> blocks_;

    void onCellFreed(void * cell, size_t bytes) noexcept {
        size_t blockIdx = findBlock(cell);
        if (blockIdx == SIZE_MAX) return;
        auto & b = blocks_[blockIdx];
        b.bytesLive -= bytes;
        if (b.bytesLive == 0 && blockIdx != activeBlockIdx_) {
            madvise(b.data, kBlockSize, MADV_DONTNEED);
            // Boehm scan over zero pages is fast + correct.
        }
    }
};
```

Caller hook: when `evictBridgeEntry` clears `v3Value`, also call `Arena::onCellFreed(...)`.  Each Bindings/ListVec/Closure cell knows its size from the allocation header (already tracked for Stage 6 cellStarts bitmap).

Effort: 2-3 d.  Pre-committed SHIP: HNE peak_rss ≥ 200 MB reduction under Stage 2 (sweep) or Stage 2b (pre-evict).

### Path B: Full Stage 6 production GC

Per `STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md`: dual-region arena + MoveGCVisitor + major-scavenge driver.  Mark-and-copy means even partially-live blocks get reclaimed: live cells are copied to a fresh region, the old region's pages are released wholesale.

Effort: 2-3 weeks.  SHIP threshold per design doc: ≥ 600 MB HNE.

## Recommendation

**Path A first** — small, targeted, complements the bridge-eviction work already landed.  If Path A measures ≥ 200 MB on HNE, we have a real lever with the existing bridge infrastructure.  If Path A measures < 100 MB, that's an empirical case for Path B (the only remaining lever).

Path A's narrow scope:
* No mark-sweep / dual-region / MoveGCVisitor
* Just: track per-block live bytes, madvise on empty
* Hook the existing bridge eviction (Stage 2 / 2b)

## Risks

1. **madvise on a Boehm-rooted region**: Boehm scans the region.  When pages are MADV_DONTNEED'd, they zero-fill on next access.  Boehm scanning sees zero words — safe (no pointer-shaped patterns).  But Boehm scanning EVERY access touches those pages, causing page faults.  Mitigation: deregister the block from Boehm before madvise (set `NIX_V3_ARENA_NOROOT=1` precondition).
2. **Tag::External in arena cells**: must be zero (per `bench/arena-dereg-audit.sh` PASS).  If a future tag introduces Boehm-pointer-holding cells, Path A breaks.  Mitigation: keep the audit script in CI.
3. **Cell-size tracking overhead**: needs to add bytes-per-cell to the alloc header.  ~8 bytes/cell × N cells.  Could be folded into existing alloc bookkeeping; needs design.

## Cross-references

* `WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md` — the design that documented Stage 2 as conditional on this work
* `ARENA_DEREG_FALSIFIED_2026-05-27.md` — falsified standalone Boehm-dereg
* `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — Path B's full design
* `STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` — Path B implementation day-by-day
* `[[falsification-rule]]` — every commit kills a hypothesis
* `[[measure-twice-cut-once]]` — pre-committed Path A SHIP threshold

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
