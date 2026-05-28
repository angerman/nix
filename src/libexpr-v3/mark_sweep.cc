/// @file
/// Stage 6 production GC — flat mark-sweep implementation.
///
/// Replaces the Cheney semispace MajorScavenger (move_gc.cc) which was
/// falsified at commit eda44711a + 9bf527714.  Per
/// `lode/GC_DESIGN_POST_CHENEY_2026-05-28.md` §4-§6.
///
/// ## Phase plan
///
///   * Phase 0 (commit 84e33bd93) — runMajorMarkSweep stub; carcass removal.
///   * Phase 1 (this commit) — mark phase with per-block bitmap.
///   * Phase 2 — sweep + per-size-class free lists.
///   * Phase 3 — allocation slow path uses free lists.
///   * Phase 4 — honest measurement against SHIP gate.
///   * Phase 5 — production hardening + default-on.
///
/// ## Phase 1 design
///
/// The mark phase visits each reachable cell from the precise root
/// walker (Stage 3) and records a bit in a per-block bitmap.  No cell
/// is moved.  The bitmap uses 1 bit per 16-byte slot of arena.  For a
/// 16 MB block, the bitmap is 128 KB (16 MB / 16 / 8).  For an HNE
/// arena of ~1.6 GB across ~94 blocks, total bitmap size during GC is
/// ~12 MB — small compared to the arena it covers.
///
/// `BitmapMarker` owns the bitmap; lifetime is single-GC-cycle.
/// `MarkVisitor` is the RootVisitor that interfaces with the precise
/// walker.  The drain loop processes a typed worklist (Closure, Thunk,
/// Bindings, ListVec, ValuePair) until empty, with each cell walked
/// for outgoing pointers.
///
/// String/path payloads (allocChars) are marked at byte-0 only — the
/// sweep phase (Phase 2) handles span lookup via per-block stride
/// dispatch.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/mark_sweep.hh"
#include "v3/alloc.hh"
#include "v3/precise_root.hh"
#include "v3/vm.hh"
#include "v3/closure.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace nix::v3 {

namespace {

/// Bit-granularity: each bit covers `kAlign` arena bytes.  v3's
/// arena aligns allocations to 16 bytes (alloc.hh:642).  Cells of
/// any type start on a 16-byte boundary.
constexpr size_t kAlign        = 16;
constexpr size_t kBitsPerWord  = 64;
constexpr size_t kBytesPerWord = kAlign * kBitsPerWord;  // 1024

/// Per-block mark bitmap entry.  Sorted by `blockStart` for binary
/// lookup during mark.
struct BlockBitmap {
    const char *           blockStart;
    std::vector<uint64_t>  bits;  // [block_size / kBytesPerWord] words

    // For 16 MB block: bits.size() = 16 MB / 1024 = 16384 words = 128 KB.
};

/// BitmapMarker — per-arena, single-GC-cycle bitmap manager.  Each
/// arena block gets its own bitmap; lookups are binary-search.
class BitmapMarker {
public:
    explicit BitmapMarker(Arena & arena) noexcept
        : arena_(arena)
    {
        const auto ranges = arena.blockRanges();
        // Pre-allocate per regular 16 MB block.  Huge blocks tracked
        // separately as a presence-set.
        for (const auto & r : ranges) {
            const size_t rangeBytes =
                static_cast<size_t>(r.end - r.begin);
            if (rangeBytes != Arena::kBlockSize) continue;
            BlockBitmap bb;
            bb.blockStart = r.begin;
            bb.bits.assign(
                Arena::kBlockSize / kBytesPerWord, 0ULL);
            blockBitmaps_.push_back(std::move(bb));
        }
        std::sort(blockBitmaps_.begin(), blockBitmaps_.end(),
            [](const BlockBitmap & a, const BlockBitmap & b) {
                return a.blockStart < b.blockStart;
            });
    }

    /// Mark address `p`.  Returns true if the cell was not previously
    /// marked (i.e., the caller should walk its outgoing edges).
    /// Returns false if `p` is null, not in active arena, already
    /// marked, or a huge-block address (those are marked separately).
    bool tryMark(const void * p) noexcept
    {
        if (!p) return false;
        if (!arena_.inActive(p)) return false;
        BlockBitmap * blk = findBlock(p);
        if (!blk) {
            // Huge block: track via set; first occurrence returns true.
            return hugeMarked_.insert(p).second;
        }
        const size_t offset =
            static_cast<size_t>(static_cast<const char *>(p)
                                - blk->blockStart);
        const size_t bitIndex = offset / kAlign;
        const size_t wordIndex = bitIndex / kBitsPerWord;
        const uint64_t mask    = 1ULL << (bitIndex % kBitsPerWord);
        if (blk->bits[wordIndex] & mask) return false;
        blk->bits[wordIndex] |= mask;
        ++markedCells_;
        return true;
    }

    /// Test whether `p` is currently marked (without setting).  Used
    /// by sweep (Phase 2) to decide if a swept cell is alive.
    bool isMarked(const void * p) const noexcept
    {
        if (!p) return false;
        if (!arena_.inActive(p)) return false;
        const BlockBitmap * blk = findBlockConst(p);
        if (!blk) return hugeMarked_.count(p) > 0;
        const size_t offset =
            static_cast<size_t>(static_cast<const char *>(p)
                                - blk->blockStart);
        const size_t bitIndex = offset / kAlign;
        const size_t wordIndex = bitIndex / kBitsPerWord;
        const uint64_t mask    = 1ULL << (bitIndex % kBitsPerWord);
        return (blk->bits[wordIndex] & mask) != 0;
    }

    size_t markedCells() const noexcept { return markedCells_; }
    size_t blocksCovered() const noexcept { return blockBitmaps_.size(); }
    size_t hugeBlocksMarked() const noexcept { return hugeMarked_.size(); }

    // For Phase 2 sweep: iterate bitmaps in order.
    const std::vector<BlockBitmap> & blockBitmaps() const noexcept
        { return blockBitmaps_; }

    /// Phase 3 correctness: check whether ANY mark bit is set in the
    /// byte range `[startOffset, endOffset)` within the block starting
    /// at `blockBegin`.  Used by sweep to conservatively classify a
    /// cell as ALIVE if any of its 16-byte slots was marked (which
    /// includes interior slot-target marks from `visitSlot`).  This
    /// covers the case where the owning container (e.g., a Bindings)
    /// was reached ONLY via a Tag::Slot pointer to its entries[i]
    /// interior — without this check sweep would treat the container
    /// as dead and free it, dangling the slot.
    bool anyMarkInRange(const char * blockBegin,
                        size_t startOffset,
                        size_t endOffset) const noexcept
    {
        const BlockBitmap * blk = findBlockConst(blockBegin);
        if (!blk) return false;
        const size_t startBit = startOffset / kAlign;
        const size_t endBit   = (endOffset + kAlign - 1) / kAlign;
        const size_t startWord = startBit / kBitsPerWord;
        const size_t endWord   =
            std::min<size_t>((endBit + kBitsPerWord - 1) / kBitsPerWord,
                             blk->bits.size());
        for (size_t w = startWord; w < endWord; ++w) {
            uint64_t word = blk->bits[w];
            if (word == 0) continue;
            // Mask only bits within [startBit, endBit) of this word.
            const size_t wStartBit = w * kBitsPerWord;
            const size_t wEndBit   = (w + 1) * kBitsPerWord;
            const size_t lo =
                (startBit > wStartBit) ? (startBit - wStartBit) : 0;
            const size_t hi =
                (endBit < wEndBit) ? (endBit - wStartBit) : kBitsPerWord;
            // Bits [lo, hi) within this word.
            const uint64_t mask = (hi - lo >= kBitsPerWord)
                ? uint64_t(-1)
                : (((uint64_t(1) << (hi - lo)) - 1) << lo);
            if (word & mask) return true;
        }
        return false;
    }

private:
    Arena &                  arena_;
    std::vector<BlockBitmap> blockBitmaps_;  // sorted by blockStart
    std::unordered_set<const void *> hugeMarked_;
    size_t                   markedCells_ = 0;

    BlockBitmap * findBlock(const void * p) noexcept
    {
        // Binary search by blockStart; check end after.
        auto it = std::upper_bound(
            blockBitmaps_.begin(), blockBitmaps_.end(), p,
            [](const void * cp, const BlockBitmap & b) {
                return cp < (const void *)b.blockStart;
            });
        if (it == blockBitmaps_.begin()) return nullptr;
        --it;
        if (p >= (const void *)it->blockStart
            && p < (const void *)(it->blockStart + Arena::kBlockSize))
            return &*it;
        return nullptr;
    }
    const BlockBitmap * findBlockConst(const void * p) const noexcept
    {
        auto it = std::upper_bound(
            blockBitmaps_.begin(), blockBitmaps_.end(), p,
            [](const void * cp, const BlockBitmap & b) {
                return cp < (const void *)b.blockStart;
            });
        if (it == blockBitmaps_.begin()) return nullptr;
        --it;
        if (p >= (const void *)it->blockStart
            && p < (const void *)(it->blockStart + Arena::kBlockSize))
            return &*it;
        return nullptr;
    }
};

/// MarkVisitor — RootVisitor implementation that records mark bits.
/// Cells are not moved.  Outgoing pointer fields are walked via a
/// typed worklist (per-type-tagged Gray entries).
class MarkVisitor : public RootVisitor {
public:
    explicit MarkVisitor(BitmapMarker & m) noexcept : marker_(m) {}

    void visitClosure(Closure * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KClosure});
            ++statsClosures_;
        }
    }
    void visitThunk(Thunk * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KThunk});
            ++statsThunks_;
        }
    }
    void visitBindings(Bindings * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KBindings});
            ++statsBindings_;
        }
    }
    void visitList(ListVec * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KList});
            ++statsLists_;
        }
    }
    void visitPair(ValuePair * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KPair});
            ++statsPairs_;
        }
    }
    void visitSlot(Value * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            ++statsCells_;
            // Walk through the cell's content to mark transitively
            // reached pointers.  No type info at the slot target, so
            // visitValue dispatches on the tag.
            visitValue(*p);
        }
    }
    void visitString(const char * & s) noexcept override
    {
        if (!s) return;
        // Mark just the byte-0 of the string.  Phase 2 sweep walks
        // the bitmap in address order; finding a marked bit at a
        // char-pool boundary identifies the string head.  Length
        // discovered via strlen at sweep time (allocChars guarantees
        // null-termination per alloc.hh:1205-1207).
        if (marker_.tryMark(s)) ++statsChars_;
    }
    void visitPath(const char * & s) noexcept override
    {
        if (!s) return;
        if (marker_.tryMark(s)) ++statsChars_;
    }

    /// Drain the worklist.  Each entry's outgoing pointer fields are
    /// visited (recurse via visit* / visitValue dispatch).
    void drain() noexcept
    {
        while (!worklist_.empty()) {
            Gray g = worklist_.back();
            worklist_.pop_back();
            switch (g.kind) {
            case KClosure:  walkClosure (static_cast<Closure  *>(g.ptr)); break;
            case KThunk:    walkThunk   (static_cast<Thunk    *>(g.ptr)); break;
            case KBindings: walkBindings(static_cast<Bindings *>(g.ptr)); break;
            case KList:     walkList    (static_cast<ListVec  *>(g.ptr)); break;
            case KPair:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
            }
        }
    }

    // Stats getters
    size_t statsClosures() const noexcept { return statsClosures_; }
    size_t statsThunks()   const noexcept { return statsThunks_; }
    size_t statsBindings() const noexcept { return statsBindings_; }
    size_t statsLists()    const noexcept { return statsLists_; }
    size_t statsPairs()    const noexcept { return statsPairs_; }
    size_t statsCells()    const noexcept { return statsCells_; }
    size_t statsChars()    const noexcept { return statsChars_; }

private:
    BitmapMarker & marker_;
    enum GrayKind : uint8_t {
        KClosure = 0, KThunk, KBindings, KList, KPair
    };
    struct Gray { void * ptr; GrayKind kind; };
    std::vector<Gray> worklist_;

    size_t statsClosures_ = 0;
    size_t statsThunks_   = 0;
    size_t statsBindings_ = 0;
    size_t statsLists_    = 0;
    size_t statsPairs_    = 0;
    size_t statsCells_    = 0;
    size_t statsChars_    = 0;

    void walkClosure(Closure * c) noexcept
    {
        if (c->capturedWiths)
            visitList(c->capturedWiths);
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            visitValue(c->upvalues[i]);
    }
    void walkThunk(Thunk * t) noexcept
    {
        // Thunk::cell + shapeCell point at Value cells (Bindings-
        // resident OR standalone allocValue).  Mark them via visitSlot
        // so the cell payload is walked transitively.
        if (t->cell)      visitSlot(t->cell);
        if (t->shapeCell) visitSlot(t->shapeCell);
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Blackhole:
            if (t->suspended.capturedWiths)
                visitList(t->suspended.capturedWiths);
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i]);
            break;
        case ThunkState::Evaluated:
            visitValue(t->evaluated);
            break;
        case ThunkState::Native:
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i]);
            break;
        case ThunkState::Bridge:
            // bridgeSrc is a TW nix::Value*; tracked via
            // bridge_root_registry, not via arena pointers.
            break;
        }
    }
    void walkBindings(Bindings * b) noexcept
    {
        for (uint32_t i = 0; i < b->size; ++i)
            visitValue(b->entries[i].value);
        if (b->parent)
            visitBindings(const_cast<Bindings *&>(b->parent));
    }
    void walkList(ListVec * l) noexcept
    {
        for (uint32_t i = 0; i < l->size; ++i)
            visitValue(l->elems[i]);
    }
    void walkPair(ValuePair * p) noexcept
    {
        visitValue(p->left);
        visitValue(p->right);
        visitValue(p->evaluated);
    }
};

} // namespace

/// Stage 6 Phase 2: sweep stats.  Computed by the sweep loop;
/// reported under NIX_VM_STATS=1.
struct SweepStats {
    size_t deadCells     = 0;
    size_t liveCells     = 0;
    size_t deadBytes     = 0;
    size_t liveBytes     = 0;
    size_t blocksScanned = 0;
};

/// Sweep one arena block.  Walks the cell-start bitmap in address
/// order; for each cell, computes size from the distance to the next
/// cell-start; checks the mark bitmap; classifies as live or dead.
///
/// Phase 3: dead cells are added to the per-exact-size free-list in
/// the arena.  Their cell-start bits are cleared (they no longer
/// represent live cells).  Subsequent allocations check the free
/// list first; arena growth is capped at peak-live + slack.
static void sweepOneBlock(
    Arena &                       arena,
    const char *                  blockStart,
    size_t                        blockUsedBytes,
    const std::vector<uint64_t> & cellStartBits,
    const BitmapMarker &          marker,
    SweepStats &                  stats) noexcept
{
    // Collect cell-start offsets in this block, in address order.
    // Pre-allocate to avoid per-cell std::vector growth.
    std::vector<size_t> starts;
    starts.reserve(blockUsedBytes / 32);  // rough estimate
    const size_t maxWord =
        std::min(cellStartBits.size(),
                 (blockUsedBytes + kBytesPerWord - 1) / kBytesPerWord);
    for (size_t w = 0; w < maxWord; ++w) {
        uint64_t bits = cellStartBits[w];
        while (bits) {
            // count trailing zeros of bits → bit index within word
            const int b = __builtin_ctzll(bits);
            const size_t offset =
                (w * kBitsPerWord + size_t(b)) * kAlign;
            if (offset < blockUsedBytes)
                starts.push_back(offset);
            bits &= bits - 1;  // clear lowest set bit
        }
    }

    if (starts.empty()) return;  // no allocations in this block

    // Walk consecutive starts; size = next.offset - this.offset.
    // Last cell extends to blockUsedBytes.
    ++stats.blocksScanned;
    for (size_t i = 0; i < starts.size(); ++i) {
        const size_t offset    = starts[i];
        const size_t endOffset = (i + 1 < starts.size())
            ? starts[i + 1]
            : blockUsedBytes;
        const size_t cellSize = endOffset - offset;
        const void * cellAddr =
            static_cast<const void *>(blockStart + offset);
        // Phase 3 correctness: a cell is ALIVE if any mark bit in its
        // byte range is set (covers Bindings reached only via a slot
        // pointer into its entries[i]).  Strict cell-start check
        // alone misses the owning container.
        if (marker.anyMarkInRange(blockStart, offset, offset + cellSize)) {
            ++stats.liveCells;
            stats.liveBytes += cellSize;
        } else {
            ++stats.deadCells;
            stats.deadBytes += cellSize;
            // Phase 3: route dead cell to the per-exact-size free
            // list and clear its cell-start bit.  Subsequent allocs
            // of this size will reuse the freed slot.
            arena.freeListAdd(
                const_cast<void *>(cellAddr), cellSize);
            arena.clearCellStartBitFor(cellAddr);
        }
    }
}

void runMajorMarkSweep(VMState & vm) noexcept
{
    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();

    Arena & arena = threadArena();

    // -- Phase 1: mark ----------------------------------------------
    BitmapMarker marker(arena);
    MarkVisitor visitor(marker);
    walkAllV3Roots(vm, visitor);
    visitor.drain();

    const auto tMarkEnd = clock::now();

    // -- Phase 2 step 2: sweep (measurement-only; no free yet) ------
    SweepStats sweep;
    const auto & cellStarts = arena.cellStartBitmaps();
    const auto ranges = arena.blockRanges();

    // Filter regular blocks (skip huge); compute per-block used-bytes.
    size_t regularBlockIdx = 0;
    for (const auto & r : ranges) {
        const size_t rangeBytes =
            static_cast<size_t>(r.end - r.begin);
        if (rangeBytes != Arena::kBlockSize) continue;  // huge
        if (regularBlockIdx >= cellStarts.size()) break;
        // blockUsedBytes: for the LAST block (current bump cursor),
        // we sweep up to the current bump; for older blocks, up to
        // kBlockSize.  `r.end` from blockRanges already encodes this.
        const size_t usedBytes =
            static_cast<size_t>(r.end - r.begin);
        sweepOneBlock(
            arena,
            r.begin,
            usedBytes,
            cellStarts[regularBlockIdx],
            marker,
            sweep);
        ++regularBlockIdx;
    }

    const auto tSweepEnd = clock::now();
    const double markMs =
        std::chrono::duration<double, std::milli>(tMarkEnd - tStart)
            .count();
    const double sweepMs =
        std::chrono::duration<double, std::milli>(tSweepEnd - tMarkEnd)
            .count();

    // Phase 3 (TODO): allocator slow path uses the free lists computed
    // here.  Phase 2 step 2 only MEASURES; cells are not actually
    // returned to free lists or the OS.  Workload behaviour under
    // gate-ON is identical to gate-OFF.

    static const bool s_stats = std::getenv("NIX_VM_STATS") != nullptr;
    if (s_stats) {
        std::fprintf(stderr,
            "v3 major-mark-sweep: closures=%zu thunks=%zu bindings=%zu "
            "lists=%zu pairs=%zu cells=%zu chars=%zu "
            "markedCells=%zu blocks=%zu hugeMarked=%zu "
            "markMs=%.2f sweepMs=%.2f\n",
            visitor.statsClosures(), visitor.statsThunks(),
            visitor.statsBindings(), visitor.statsLists(),
            visitor.statsPairs(), visitor.statsCells(),
            visitor.statsChars(),
            marker.markedCells(),
            marker.blocksCovered(),
            marker.hugeBlocksMarked(),
            markMs, sweepMs);
        std::fprintf(stderr,
            "v3 sweep: blocksScanned=%zu liveCells=%zu deadCells=%zu "
            "liveBytes=%.1fMB deadBytes=%.1fMB reclaim%%=%.1f%% "
            "freelist_entries=%zu\n",
            sweep.blocksScanned,
            sweep.liveCells, sweep.deadCells,
            sweep.liveBytes / 1e6,
            sweep.deadBytes / 1e6,
            (sweep.liveBytes + sweep.deadBytes) > 0
                ? 100.0 * double(sweep.deadBytes)
                          / double(sweep.liveBytes + sweep.deadBytes)
                : 0.0,
            arena.freeListEntryCount());
    }
}

} // namespace nix::v3
