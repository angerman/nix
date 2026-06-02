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
#include <csetjmp>
#include <pthread.h>
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
            // Step 11′ (Immix): mark the line covering this Value
            // cell.  Standalone cells are 16 B (one cell-aligned
            // unit); Bindings-resident cells are 16 B but live
            // INSIDE a larger Bindings span — the Bindings's own
            // walkBindings line-mark covers them.  Either way,
            // marking 16 B at `p` is the minimal correct coverage.
            if (arenaSetForSlot_) {
                arenaSetForSlot_->markLinesForCell(p, sizeof(Value));
            }
            // Walk through the cell's content to mark transitively
            // reached pointers.  No type info at the slot target, so
            // visitValue dispatches on the tag.
            visitValue(*p);
        }
        // Phase 3.5: if the slot's target is INTERIOR of a larger
        // container (e.g., a Bindings's entries[i].value), mark the
        // owning container so drainConservative walks its bytes.
        // Without this the OTHER pointer-bearing entries in the
        // container don't get transitively marked — sweep would
        // free them, and the container's other fields would dangle.
        if (arenaSetForSlot_ && arenaSetForSlot_->inActive(p)
            && !arenaSetForSlot_->isCellStart(p))
        {
            const char * owner =
                arenaSetForSlot_->findContainingCellStart(p);
            if (owner) {
                markConservative(
                    const_cast<char *>(owner));
            }
        }
    }

    /// Phase 3.5: set the Arena reference used by visitSlot to
    /// identify interior slot targets.  Must be called BEFORE the
    /// root walk.
    void setArena(Arena & a) noexcept { arenaSetForSlot_ = &a; }
    void visitString(const char * & s) noexcept override
    {
        if (!s) return;
        // Mark just the byte-0 of the string.  Phase 2 sweep walks
        // the bitmap in address order; finding a marked bit at a
        // char-pool boundary identifies the string head.  Length
        // discovered via strlen at sweep time (allocChars guarantees
        // null-termination per alloc.hh:1205-1207).
        if (marker_.tryMark(s)) {
            ++statsChars_;
            // Step 11′ (Immix, 2026-05-29): mark the lines covering
            // the string payload.  Length = strlen(s) + 1 (null term).
            if (arenaSetForSlot_) {
                arenaSetForSlot_->markLinesForCell(s, std::strlen(s) + 1);
            }
        }
    }
    void visitPath(const char * & s) noexcept override
    {
        if (!s) return;
        if (marker_.tryMark(s)) {
            ++statsChars_;
            if (arenaSetForSlot_) {
                arenaSetForSlot_->markLinesForCell(s, std::strlen(s) + 1);
            }
        }
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

    /// Phase 3.5: conservative mark.  Sets the mark bit at `p` without
    /// enqueueing for transitive walk (we don't know `p`'s type since
    /// it came from a stack scan or byte-walk).  Subsequent
    /// `anyMarkInRange` in sweep keeps the containing cell alive.
    /// Records `p` in the conservative worklist so a follow-up
    /// `drainConservative` pass can walk its bytes if requested.
    bool markConservative(void * p) noexcept
    {
        if (!p) return false;
        if (marker_.tryMark(p)) {
            conservativeRoots_.push_back(p);
            ++statsConservative_;
            // Step 11′ (Immix): derive the cell's size from the
            // cell-start bitmap and line-mark the cell's full span.
            // For interior pointers, find the containing cell start;
            // for cell-start pointers, that's `p` itself.  Without
            // this, Immix could allocate over a conservatively-live
            // cell's tail bytes if they happen to fall in a "dead"
            // line per a precise-walk-only line-mark.  Conservative
            // overmark is the safe direction.
            if (arenaSetForSlot_) {
                const char * cellStart =
                    arenaSetForSlot_->findContainingCellStart(p);
                if (cellStart) {
                    const char * cellEnd =
                        arenaSetForSlot_->findNextCellStartOrBlockEnd(cellStart);
                    if (cellEnd && cellEnd > cellStart) {
                        arenaSetForSlot_->markLinesForCell(
                            cellStart,
                            static_cast<size_t>(cellEnd - cellStart));
                    }
                }
            }
            return true;
        }
        return false;
    }

    /// Phase 3.5: conservatively walk bytes of every conservatively-
    /// marked cell.  For each cell (containing-cell start determined
    /// via cell-start bitmap backward search), walk its bytes 8-byte
    /// at a time; for each word that falls in arena bounds, attempt
    /// to mark it.  Iterates to a fixed-point.
    ///
    /// `arena` is provided so we can resolve container starts +
    /// pre-compute arena bounds for fast filtering.
    void drainConservative(Arena & arena,
                           uintptr_t arenaMin,
                           uintptr_t arenaMax) noexcept
    {
        // Snapshot — drainConservative_ may grow during the walk.
        while (!conservativeRoots_.empty()) {
            void * p = conservativeRoots_.back();
            conservativeRoots_.pop_back();
            // Find the containing cell start.  `p` may be at a
            // cell-start OR an interior offset; backward scan finds
            // the nearest cell-start at or below `p`.
            const char * cellStart = arena.findContainingCellStart(p);
            if (!cellStart) continue;
            const char * cellEnd =
                arena.findNextCellStartOrBlockEnd(cellStart);
            if (!cellEnd || cellEnd <= cellStart) continue;
            const size_t cellSize = static_cast<size_t>(cellEnd - cellStart);
            // Walk bytes 8-aligned (v3 cells are 16-aligned; pointers
            // are 8-byte).  Conservative: every 8-byte word is a
            // potential pointer.
            for (size_t off = 0; off + sizeof(void *) <= cellSize;
                 off += sizeof(void *))
            {
                const uintptr_t val = *reinterpret_cast<const uintptr_t *>(
                    cellStart + off);
                if (val < arenaMin || val >= arenaMax) continue;
                // Range check passed; precise check + mark.
                void * candidate = reinterpret_cast<void *>(val);
                if (arena.inActive(candidate)) {
                    markConservative(candidate);
                    ++statsConservativeWalks_;
                }
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
    size_t statsConservative()      const noexcept { return statsConservative_; }
    size_t statsConservativeWalks() const noexcept { return statsConservativeWalks_; }

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
    size_t statsConservative_      = 0;
    size_t statsConservativeWalks_ = 0;
    std::vector<void *> conservativeRoots_;
    Arena * arenaSetForSlot_ = nullptr;

    void walkClosure(Closure * c) noexcept
    {
        // Step 11′ (Immix, 2026-05-29): mark the lines this Closure
        // occupies.  Cell size = sizeof(Closure) + nUpvalues * sizeof(Value).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(
                c, sizeof(Closure) + sizeof(Value) * c->nUpvalues);
        }
        if (c->capturedWiths)
            visitList(c->capturedWiths);
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            visitValue(c->upvalues[i]);
    }
    void walkThunk(Thunk * t) noexcept
    {
        // Step 11′ (Immix): mark lines for the Thunk.  Size depends
        // on state (Suspended/Native/Blackhole have FAM trailers;
        // Evaluated/Bridge are header-only).
        if (arenaSetForSlot_) {
            const size_t bytes =
                (t->state == ThunkState::Suspended ||
                 t->state == ThunkState::Native    ||
                 t->state == ThunkState::Blackhole)
                ? sizeof(Thunk) + sizeof(Value) * t->nUpvalues
                : sizeof(Thunk);
            arenaSetForSlot_->markLinesForCell(t, bytes);
        }
        // Thunk::cell + shapeCell point at Value cells (Bindings-
        // resident OR standalone allocValue).  Mark them via visitSlot
        // so the cell payload is walked transitively.
        if (t->cell)      visitSlot(t->cell);
        if (t->shapeCell) visitSlot(t->shapeCell);
        // Phase 3.5 safety: walk cellContainer precisely.  When cell
        // is Bindings-resident, cellContainer is the owning Bindings.
        // visitSlot above already triggers interior-owner walk for
        // cell, but doing visitBindings here is cheap insurance + the
        // explicit precise walk catches all Bindings entries (not
        // just conservatively).
        if (t->cellContainer)
            visitBindings(t->cellContainer);
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
        }
    }
    void walkBindings(Bindings * b) noexcept
    {
        // Step 11′: line-mark the Bindings cell (header + FAM entries).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(
                b, sizeof(Bindings) + sizeof(Bindings::Entry) * b->size);
        }
        for (uint32_t i = 0; i < b->size; ++i)
            visitValue(b->entries[i].value);
        if (b->parent)
            visitBindings(const_cast<Bindings *&>(b->parent));
    }
    void walkList(ListVec * l) noexcept
    {
        // Step 11′: line-mark the ListVec cell (header + elems FAM).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(
                l, sizeof(ListVec) + sizeof(Value) * l->size);
        }
        for (uint32_t i = 0; i < l->size; ++i)
            visitValue(l->elems[i]);
    }
    void walkPair(ValuePair * p) noexcept
    {
        // Step 11′: line-mark the Pair (uniform 64 B post-Tag::App3).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(p, sizeof(ValuePair));
        }
        visitValue(p->left);
        visitValue(p->right);
        visitValue(p->evaluated);
        visitValue(p->third);  // 2026-05-30 Tag::App3 arg2
    }
};

/// Phase 3.5: Conservative C-stack scan (Boehm-style).
///
/// Walks the current thread's C-stack from current SP to the stack
/// base, plus a `jmp_buf` (spills callee-saved registers).  For each
/// pointer-sized word, fast-filters against arena bounds; if within
/// range, runs a precise `arena.inActive` check; if confirmed, marks
/// the cell + enqueues for conservative byte-walk.
///
/// `outerSp` is the SP of the caller of runMajorMarkSweep — captured
/// via a local anchor in runMajorMarkSweep.  We pass it in (rather
/// than re-capture inside walkCStack) so the scan covers the
/// dispatch-loop's locals + all caller frames.
///
/// Why this is safe (vs precise-only mark):
///   * Conservative false positives keep dead cells alive (memory
///     overhead) — never a correctness issue.
///   * False negatives are impossible if the live pointer is on the
///     stack or in a callee-saved register (captured via setjmp).
///   * Caller-saved registers are spilled to the stack by the
///     compiler at every call boundary, so they're seen by the scan
///     before crossing into runMajorMarkSweep.
///   * Conservative byte-walk of marked cells handles transitive
///     reachability via X.upvalues[0] → Y where Y's pointer might
///     not be on the stack.
__attribute__((noinline))
static void walkCStackConservative(
    MarkVisitor & v,
    Arena & arena,
    const void * outerSp) noexcept
{
    // Spill callee-saved registers to stack-resident jmp_buf.
    // setjmp returns 0 on direct call; never longjmp back here.
    jmp_buf regsBuf;
    (void)setjmp(regsBuf);

    // Determine the SP region to scan.  We want everything from
    // `outerSp` (deepest caller frame's SP, passed by caller) UP TO
    // the stack base (high address).
    pthread_t self = pthread_self();
    const char * stackHi;
#ifdef __APPLE__
    stackHi = static_cast<const char *>(pthread_get_stackaddr_np(self));
#elif defined(__linux__)
    pthread_attr_t attr;
    void * sb;
    size_t ss;
    if (pthread_getattr_np(self, &attr) == 0
        && pthread_attr_getstack(&attr, &sb, &ss) == 0)
    {
        stackHi = static_cast<const char *>(sb) + ss;
        pthread_attr_destroy(&attr);
    } else {
        stackHi = nullptr;
    }
#else
    stackHi = nullptr;
    (void)self;
#endif
    if (!stackHi) return;

    const char * stackLo = static_cast<const char *>(outerSp);
    // Align stackLo down to pointer alignment.
    uintptr_t loU = reinterpret_cast<uintptr_t>(stackLo)
                    & ~(uintptr_t(sizeof(void *)) - 1);
    uintptr_t hiU = reinterpret_cast<uintptr_t>(stackHi)
                    & ~(uintptr_t(sizeof(void *)) - 1);
    if (loU > hiU) std::swap(loU, hiU);

    // Pre-compute arena bounds for fast filter.
    uintptr_t arenaMin, arenaMax;
    arena.activeBounds(arenaMin, arenaMax);
    if (arenaMin >= arenaMax) return;  // no active blocks

    // Walk stack.
    for (uintptr_t p = loU; p < hiU; p += sizeof(void *)) {
        const uintptr_t val =
            *reinterpret_cast<const uintptr_t *>(p);
        if (val < arenaMin || val >= arenaMax) continue;
        // Range hit; precise check.
        void * candidate = reinterpret_cast<void *>(val);
        if (arena.inActive(candidate)) {
            v.markConservative(candidate);
        }
    }

    // Also walk the jmp_buf (callee-saved registers).
    const uintptr_t bufLo =
        reinterpret_cast<uintptr_t>(&regsBuf);
    const uintptr_t bufHi = bufLo + sizeof(regsBuf);
    for (uintptr_t p = bufLo; p < bufHi; p += sizeof(void *)) {
        const uintptr_t val =
            *reinterpret_cast<const uintptr_t *>(p);
        if (val < arenaMin || val >= arenaMax) continue;
        void * candidate = reinterpret_cast<void *>(val);
        if (arena.inActive(candidate)) {
            v.markConservative(candidate);
        }
    }

    // Iterate the conservative cell-walk to a fixed-point.
    v.drainConservative(arena, arenaMin, arenaMax);
}

} // namespace

/// Stage 6 Phase 2: sweep stats.  Computed by the sweep loop;
/// reported under NIX_VM_STATS=1.
struct SweepStats {
    size_t deadCells     = 0;
    size_t liveCells     = 0;
    size_t deadBytes     = 0;
    size_t liveBytes     = 0;
    size_t blocksScanned = 0;
    size_t blocksFreed   = 0;
    size_t bytesFreed    = 0;

    // R2.1 (2026-06-02): per-block live-density histogram — the
    // evacuation-OPPORTUNITY measurement (measure-twice before the
    // risky moving-GC build).  whole-block-free finds 0 blocks at
    // mid-eval trigger points because live cells are SCATTERED across
    // every block; evacuation must MOVE the few live cells out of
    // sparse blocks to empty them.  This histogram quantifies how many
    // blocks are sparse (cheap to evacuate, big RSS win) vs dense
    // (not worth moving), and the live-byte copy cost.
    //   bins by live-byte fraction: [0-10) [10-25) [25-50) [50-75) [75-100]%
    size_t densityHist[5]  = {0, 0, 0, 0, 0};
    size_t sparseBlocks    = 0;  // < 25% live = good evacuation candidates
    size_t sparseLiveBytes = 0;  // live bytes in sparse blocks = copy cost
};

/// Sweep one arena block.  Walks the cell-start bitmap in address
/// order; for each cell, computes size from the distance to the next
/// cell-start; checks the mark bitmap; classifies as live or dead.
///
/// Phase 3: dead cells are added to the per-exact-size free-list in
/// the arena.  Their cell-start bits are cleared (they no longer
/// represent live cells).  Subsequent allocations check the free
/// list first; arena growth is capped at peak-live + slack.
/// Returns true if this block ended up FULLY DEAD (no live cells
/// found during sweep + no marker bits at all in the block's byte
/// range).  Caller may then `freeWholeBlock()` it.
static bool sweepOneBlock(
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

    if (starts.empty()) {
        // No allocations recorded.  If no mark bits either, this
        // block is fully dead (nothing references its contents).
        // Phase 3.8: signal caller to free the block.
        ++stats.blocksScanned;
        return !marker.anyMarkInRange(blockStart, 0, blockUsedBytes);
    }

    // Walk consecutive starts; size = next.offset - this.offset.
    // Last cell extends to blockUsedBytes.
    ++stats.blocksScanned;
    size_t blockLiveCells = 0;
    size_t blockLiveBytes = 0;
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
            ++blockLiveCells;
            blockLiveBytes += cellSize;
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
    // R2.1: bin this block's live-byte density (evacuation opportunity).
    if (blockUsedBytes > 0) {
        const double dens = double(blockLiveBytes) / double(blockUsedBytes);
        const int bin = dens < 0.10 ? 0 : dens < 0.25 ? 1
                      : dens < 0.50 ? 2 : dens < 0.75 ? 3 : 4;
        ++stats.densityHist[bin];
        if (dens < 0.25) {            // sparse → worth evacuating
            ++stats.sparseBlocks;
            stats.sparseLiveBytes += blockLiveBytes;
        }
    }
    // Phase 3.8: block is fully dead if no live cells AND no marker
    // bits in any byte range of the block.  The cell-level
    // classifications above only cover known cell-starts; we must
    // also verify NO interior marks (e.g., from Tag::Slot targets
    // pointing into other cells) are present.
    return blockLiveCells == 0
        && !marker.anyMarkInRange(blockStart, 0, blockUsedBytes);
}

/// DIAG-1 (2026-05-29 evening, per DIAGNOSTIC_AUDIT §6.1):
/// per-cycle GC CSV.  When `NIX_V3_GC_CYCLE_CSV=path` is set,
/// each runMajorMarkSweep invocation appends one CSV row.
///
/// Columns:
///   cycleIdx      — 0-based, per-process
///   trigger       — "arena_threshold" today (single trigger mechanism)
///   markMs        — real measured (mark_sweep.cc:872-874)
///   sweepMs       — real measured (mark_sweep.cc:875-877)
///   blocksScanned — sweep.blocksScanned
///   blocksFreed   — sweep.blocksFreed (whole-block-free wins)
///   liveCells     — sweep.liveCells
///   deadCells     — sweep.deadCells
///   liveBytes     — sweep.liveBytes
///   deadBytes     — sweep.deadBytes
///   bytesFreed    — sweep.bytesFreed
///   freeListEntries — arena.freeListEntryCount()
///   arenaBytesBefore — bytes_allocated at cycle entry
///   arenaBytesAfter  — bytes_allocated at cycle exit (after freelist install)
///   allocSincePrev   — arenaBytesBefore − arenaBytesPrev (or arenaBytesBefore for cycle 0)
///   wallSincePrev_ms — wall (ms) since prev cycle start (or process-start for cycle 0)
///   totalLines       — Step 11′ line-marks
///   deadLines        — Step 11′ line-marks
///   deadLinePct      — Step 11′ line-marks dead %
///
/// Pre-committed acceptance: a single hello.drvPath run under
/// NIX_V3_MAJOR_GC=1 + NIX_V3_GC_CYCLE_CSV=/tmp/gc.csv writes ≥1 row;
/// columns sum/agree per-row with the existing stderr banner.
namespace {

struct GcCycleCsvState {
    FILE * fp = nullptr;
    uint64_t cycleIdx = 0;
    size_t   arenaBytesPrev = 0;
    std::chrono::steady_clock::time_point tPrevStart =
        std::chrono::steady_clock::now();
    bool headerWritten = false;
};

inline GcCycleCsvState & gcCsvState() noexcept
{
    static GcCycleCsvState s;
    return s;
}

inline FILE * openGcCsvIfRequested() noexcept
{
    static const char * s_path = std::getenv("NIX_V3_GC_CYCLE_CSV");
    if (!s_path || !*s_path) return nullptr;
    auto & st = gcCsvState();
    if (!st.fp) {
        st.fp = std::fopen(s_path, "a");
        if (!st.fp) return nullptr;
        // Write header once per process (idempotent across appends —
        // a tail | head -1 will catch the first run's header).
        if (!st.headerWritten) {
            std::fprintf(st.fp,
                "cycleIdx,trigger,markMs,sweepMs,"
                "blocksScanned,blocksFreed,"
                "liveCells,deadCells,liveBytes,deadBytes,bytesFreed,"
                "freeListEntries,"
                "arenaBytesBefore,arenaBytesAfter,allocSincePrev,"
                "wallSincePrev_ms,"
                "totalLines,deadLines,deadLinePct\n");
            st.headerWritten = true;
        }
    }
    return st.fp;
}

} // anonymous

void runMajorMarkSweep(VMState & vm) noexcept
{
    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();

    Arena & arena = threadArena();
    const size_t arenaBytesBefore = arena.bytesAllocated();

    // -- Phase 1: precise mark --------------------------------------
    // Step 11′ (Immix, 2026-05-29): clear per-block line-mark
    // bitmap before mark walk populates it.  Zero-cost when major-GC
    // gate OFF (vector stays empty); ~50-200 µs for HNE's 94 blocks
    // when ON (memset of 94 × 16 KB = 1.5 MB).
    arena.clearAllLineMarks();
    BitmapMarker marker(arena);
    MarkVisitor visitor(marker);
    visitor.setArena(arena);  // for visitSlot interior-owner discovery
    walkAllV3Roots(vm, visitor);
    visitor.drain();

    // Phase 3.5 follow-up: drain conservative roots accumulated from
    // visitSlot's interior-owner discovery during precise mark.
    {
        uintptr_t arenaMin, arenaMax;
        arena.activeBounds(arenaMin, arenaMax);
        visitor.drainConservative(arena, arenaMin, arenaMax);
    }

    // -- Phase 3.5: conservative C-stack scan -----------------------
    // After precise marks finish, do a conservative scan of the
    // current thread's stack + callee-saved registers (via setjmp
    // spill).  Catches Value/cell pointers held in C-locals of
    // primop bodies that the precise walker can't see.  Anchor SP
    // here so the scan covers everything from our caller's frame
    // (dispatch-loop) up to the stack base.
    {
        char anchor;
        const void * sp = &anchor;
        walkCStackConservative(visitor, arena, sp);
    }

    const auto tMarkEnd = clock::now();

    // -- Phase 2 step 2: sweep (measurement-only; no free yet) ------
    SweepStats sweep;
    const auto & cellStarts = arena.cellStartBitmaps();
    const auto ranges = arena.blockRanges();

    // Filter regular blocks (skip huge); compute per-block used-bytes.
    // Phase 3.8: collect blocks that sweep classified fully-dead
    // first, then free them in a second pass (avoid invalidating
    // `ranges` mid-iteration).
    std::vector<const char *> blocksToFree;
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
        const bool fullyDead = sweepOneBlock(
            arena,
            r.begin,
            usedBytes,
            cellStarts[regularBlockIdx],
            marker,
            sweep);
        if (fullyDead) blocksToFree.push_back(r.begin);
        ++regularBlockIdx;
    }

    // Phase 3.8: free fully-dead blocks back to libc.  Done as a
    // second pass so we don't mutate active_.blocks during the sweep
    // iteration.  Each freeWholeBlock removes the block from arena
    // metadata + filters its free-list entries.
    for (const char * blk : blocksToFree) {
        const size_t freed = arena.freeWholeBlock(blk);
        if (freed > 0) {
            ++sweep.blocksFreed;
            sweep.bytesFreed += freed;
        }
    }

    // Step 12′ (Immix, 2026-05-29): rebuild free-line spans from the
    // post-mark line-mark bitmap.  Spans drive `Arena::alloc()` until
    // the next major GC.  Resets the immix bump-pointer state so
    // next alloc starts at span 0 of block 0.
    //
    // Must happen AFTER blocksToFree (freeWholeBlock removes
    // entries from lineMarks; this rebuild walks the surviving set).
    // No-op when V3_DBG_IMMIX_ALLOC=0 (the Arena alloc path skips
    // the Immix branch and the rebuilt freeSpans are unused).
    arena.rebuildFreeSpansFromLineMarks();

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
            "lists=%zu pairs=%zu cells=%zu chars=%zu cons=%zu consW=%zu "
            "markedCells=%zu blocks=%zu hugeMarked=%zu "
            "markMs=%.2f sweepMs=%.2f\n",
            visitor.statsClosures(), visitor.statsThunks(),
            visitor.statsBindings(), visitor.statsLists(),
            visitor.statsPairs(), visitor.statsCells(),
            visitor.statsChars(),
            visitor.statsConservative(),
            visitor.statsConservativeWalks(),
            marker.markedCells(),
            marker.blocksCovered(),
            marker.hugeBlocksMarked(),
            markMs, sweepMs);
        std::fprintf(stderr,
            "v3 sweep: blocksScanned=%zu liveCells=%zu deadCells=%zu "
            "liveBytes=%.1fMB deadBytes=%.1fMB reclaim%%=%.1f%% "
            "freelist_entries=%zu blocksFreed=%zu bytesFreed=%.1fMB\n",
            sweep.blocksScanned,
            sweep.liveCells, sweep.deadCells,
            sweep.liveBytes / 1e6,
            sweep.deadBytes / 1e6,
            (sweep.liveBytes + sweep.deadBytes) > 0
                ? 100.0 * double(sweep.deadBytes)
                          / double(sweep.liveBytes + sweep.deadBytes)
                : 0.0,
            arena.freeListEntryCount(),
            sweep.blocksFreed,
            sweep.bytesFreed / 1e6);
        // R2.1 (2026-06-02): evacuation-opportunity histogram.  Sparse
        // blocks (<25% live) are the evacuation candidates — moving
        // their few live bytes into dense blocks empties them for
        // munmap.  evacuable_RSS = sparseBlocks * 16 MB (RSS evacuation
        // could free this cycle); copy_cost = live bytes to relocate.
        std::fprintf(stderr,
            "v3 evac-opportunity: density[0-10|10-25|25-50|50-75|75-100]%%="
            "%zu|%zu|%zu|%zu|%zu  sparseBlocks=%zu(<25%%live) "
            "evacuable_RSS=%.1fMB copy_cost=%.1fMB\n",
            sweep.densityHist[0], sweep.densityHist[1], sweep.densityHist[2],
            sweep.densityHist[3], sweep.densityHist[4],
            sweep.sparseBlocks,
            double(sweep.sparseBlocks) * double(Arena::kBlockSize) / 1e6,
            double(sweep.sparseLiveBytes) / 1e6);
        // Step 11′ (Immix, 2026-05-29): line-mark bitmap summary.
        // Each block has 131,072 lines of 128 B; a line is "live"
        // if any byte of any marked cell falls in it.  Dead-line
        // fraction is the upper bound on bytes Immix's bump-realloc
        // could reclaim within blocks.
        //
        // Acceptance criterion (per GC_DECISION_2026-05-29 New Step
        // 11′): ≥30% lines fully dead.  Below that means the line-
        // mark scheme has bugged or Immix can't deliver on this
        // workload.  Empirical baseline from IMMIX_LINE_OCCUPANCY
        // measurement: 46.5% hello, 50.5% HNE.
        {
            const auto [totalLines, deadLines] = arena.countLineMarks();
            const double deadPct = totalLines > 0
                ? 100.0 * double(deadLines) / double(totalLines) : 0.0;
            std::fprintf(stderr,
                "v3 line-marks: blocks=%zu totalLines=%zu "
                "deadLines=%zu deadPct=%.2f%% "
                "(Immix acceptance ≥30%%)\n",
                arena.lineMarkBitmaps().size(),
                totalLines, deadLines, deadPct);
        }
        // Step 12′ (Immix, 2026-05-29): free-spans summary alongside
        // line-marks.  After rebuild, these spans drive the Immix
        // allocator until the next GC.
        {
            const auto [spanBytes, spanCount] = arena.countFreeSpanBytes();
            std::fprintf(stderr,
                "v3 free-spans: blocks=%zu spans=%zu spanBytes=%.1fMB\n",
                arena.freeSpansForBlocks().size(),
                spanCount, spanBytes / 1e6);
        }
        // Step 13′ (Immix recycle policy, 2026-05-29): per-cycle
        // skipped-vs-recyclable block counts.  Reflects whether the
        // NIX_V3_IMMIX_RECYCLE_PCT threshold filtered any blocks.
        {
            const auto & rs = immixRecycleStats();
            std::fprintf(stderr,
                "v3 recycle-policy: recyclable=%llu skipped=%llu "
                "recyclableDeadMB=%.1f skippedDeadMB=%.1f\n",
                (unsigned long long)rs.blocksRecyclable,
                (unsigned long long)rs.blocksSkipped,
                double(rs.recyclableDeadBytes) / 1e6,
                double(rs.skippedDeadBytes) / 1e6);
        }
    }

    // DIAG-1 (2026-05-29): per-cycle CSV row.  Independent of
    // NIX_VM_STATS — fires whenever NIX_V3_GC_CYCLE_CSV=path is set.
    if (FILE * fp = openGcCsvIfRequested()) {
        auto & st = gcCsvState();
        const size_t arenaBytesAfter = arena.bytesAllocated();
        const size_t allocSincePrev = (arenaBytesBefore >= st.arenaBytesPrev)
            ? (arenaBytesBefore - st.arenaBytesPrev)
            : 0;  // arena shrunk between cycles (whole-block-free); count 0
        // For cycle 0: state's tPrevStart was set when gcCsvState() was
        // first called (which happens AFTER tStart captured at function
        // entry) — would yield a negative value.  Report 0 for cycle 0
        // (meaningful CYCLE-TO-CYCLE time starts at cycle 1).
        const double wallSincePrevMs = (st.cycleIdx == 0)
            ? 0.0
            : std::chrono::duration<double, std::milli>(
                tStart - st.tPrevStart).count();
        const auto [totalLines, deadLines] = arena.countLineMarks();
        const double deadLinePct = totalLines > 0
            ? 100.0 * double(deadLines) / double(totalLines) : 0.0;
        std::fprintf(fp,
            "%llu,arena_threshold,%.3f,%.3f,"
            "%zu,%zu,"
            "%zu,%zu,%zu,%zu,%zu,"
            "%zu,"
            "%zu,%zu,%zu,"
            "%.3f,"
            "%zu,%zu,%.3f\n",
            (unsigned long long)st.cycleIdx,
            markMs, sweepMs,
            sweep.blocksScanned, sweep.blocksFreed,
            sweep.liveCells, sweep.deadCells,
            (size_t)sweep.liveBytes, (size_t)sweep.deadBytes,
            (size_t)sweep.bytesFreed,
            arena.freeListEntryCount(),
            arenaBytesBefore, arenaBytesAfter, allocSincePrev,
            wallSincePrevMs,
            totalLines, deadLines, deadLinePct);
        std::fflush(fp);
        ++st.cycleIdx;
        st.arenaBytesPrev = arenaBytesAfter;
        st.tPrevStart = tStart;
    }
}

} // namespace nix::v3
