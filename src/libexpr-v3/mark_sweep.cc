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
#include "v3/primop.hh"  // M2.1: importCacheColdBytes / importCacheCuCount / BytecodeBytes
#include "v3/precise_root.hh"
#include "v3/nursery.hh"  // MIDEVAL_GC: threadNursery().forEachUsedRange conservative scan
#include "v3/barrier.hh"  // MIDEVAL_GC: dirtyContainers() remembered-set root walk
#include "v3/bytecode.hh"  // MIDEVAL_GC: CompilationUnit::attrSelectCache IC walk
#include "v3/vm.hh"
#include "v3/closure.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csetjmp>
#include <execinfo.h>  // S1.2 PIN-FRAMES: backtrace at the mid-eval safepoint
#include <pthread.h>
#include <unordered_map>
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
        } else enqueueNurseryCell(p, KClosure);  // MIDEVAL_GC: traverse nursery cell
    }
    void visitThunk(Thunk * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KThunk});
            ++statsThunks_;
        } else enqueueNurseryCell(p, KThunk);
    }
    void visitBindings(Bindings * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KBindings});
            ++statsBindings_;
        } else enqueueNurseryCell(p, KBindings);
    }
    void visitList(ListVec * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KList});
            ++statsLists_;
        } else enqueueNurseryCell(p, KList);
    }
    void visitPair(ValuePair * & p) override
    {
        if (!p) return;
        if (marker_.tryMark(p)) {
            worklist_.push_back({p, KPair});
            ++statsPairs_;
        } else enqueueNurseryCell(p, KPair);
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
        } else if (nursery_ && nursery_->contains(p)
                   && nurseryVisited_.insert(p).second) {
            // MIDEVAL_GC: a nursery-resident Value cell — visitValue decodes its
            // boxed payload so a tenured pointee is marked.  Deduped via the set.
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
                char * o = const_cast<char *>(owner);
                // Lever 3 (R2.4d): if the owner's type is known (R2.1'
                // metadata), mark+walk it PRECISELY at its exact pointer
                // fields (via the typed worklist) instead of the O(bytes)
                // conservative byte-scan in drainConservative — which was
                // 153 s / 4.3 B words on HNE cycle2 (byte-scanning huge
                // Bindings reached via interior Tag::Slot pointers).  This
                // is ALSO more precise (no false-positive marks).  Fall
                // back to conservative only for unstamped owners.
                //
                // ONLY for the SWEEP's mark (typedInteriorOwners_=true):
                // the evac VERIFY must NOT owner-walk — it already pins a
                // candidate via the slot-TARGET mark above; owner-walking
                // there marks the owner's other entries and falsely pins
                // candidates (blocksFreed→0).  Verify keeps the deferred-
                // conservative path (a no-op there since verify doesn't
                // call drainConservative).
                if (!typedInteriorOwners_) markConservative(o);
                else switch (arenaSetForSlot_->cellTypeAt(owner)) {
                case CellType::Bindings: { Bindings * b = reinterpret_cast<Bindings *>(o); visitBindings(b); break; }
                case CellType::Closure:  { Closure  * c = reinterpret_cast<Closure  *>(o); visitClosure(c);  break; }
                case CellType::Thunk:    { Thunk    * t = reinterpret_cast<Thunk    *>(o); visitThunk(t);    break; }
                case CellType::List:     { ListVec  * l = reinterpret_cast<ListVec  *>(o); visitList(l);     break; }
                case CellType::Pair:     { ValuePair * pr = reinterpret_cast<ValuePair *>(o); visitPair(pr); break; }
                case CellType::Value:
                case CellType::Chars:
                case CellType::None:
                    markConservative(o); break;  // unstamped → fallback
                }
            }
        }
    }

    /// Phase 3.5: set the Arena reference used by visitSlot to
    /// identify interior slot targets.  Must be called BEFORE the
    /// root walk.
    void setArena(Arena & a) noexcept { arenaSetForSlot_ = &a; }
    // MIDEVAL_GC: enable precise traversal of the resident nursery (see member).
    void setNursery(Nursery * n) noexcept { nursery_ = n; }
    // Lever 3 (R2.4d): enable typed interior-owner walk (mark only).
    void setTypedInteriorOwners(bool b) noexcept { typedInteriorOwners_ = b; }
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
                // S1.2 PROVENANCE: attribute this conservative-only pin to its
                // cell type (one nibble read; reuses the cellStart the line-mark
                // already resolves).  Precise marking finishes BEFORE the
                // conservative C-stack scan, so every tryMark success here is a
                // cell the precise root graph did NOT reach = a genuine direct
                // C-stack ROOT pin.  Gated on NIX_VM_STATS (read once) so
                // production pays nothing; only accrues when it will be reported.
                static const bool s_prov = std::getenv("NIX_VM_STATS") != nullptr;
                if (__builtin_expect(s_prov, 0) && cellStart) {
                    auto ct = static_cast<uint8_t>(arenaSetForSlot_->cellTypeAt(cellStart));
                    if (ct < 9) ++statsConservByType_[ct];
                }
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
        // Fixed-point over BOTH the typed worklist and the conservative
        // roots: a typed-walk below pushes children to worklist_, and a
        // byte-scan pushes more conservativeRoots_.
        for (;;) {
            drain();  // typed worklist (precise walks)
            if (conservativeRoots_.empty()) break;
            void * p = conservativeRoots_.back();
            conservativeRoots_.pop_back();
            // Find the containing cell start.  `p` may be at a
            // cell-start OR an interior offset; backward scan finds
            // the nearest cell-start at or below `p`.
            const char * cellStart = arena.findContainingCellStart(p);
            if (!cellStart) continue;
            // R2.4d (2026-06-04): if the owning cell is TYPED (R2.1'
            // metadata — now including huge ≥4 MB cells), TYPED-walk its
            // exact pointer fields instead of an O(bytes) conservative
            // byte-scan.  Byte-scanning a huge (>4 MB, 170k-entry)
            // Bindings reached via a C-stack pointer was the conservative
            // drain's 2236 s / 7.4e9-word blow-up on M5 (flaky multi-
            // minute mark pauses → wall-time timeouts under default-ON GC).
            // Only genuinely-untyped cells (Value/Chars/None) still
            // need the byte-scan; those are small.
            char * cs = const_cast<char *>(cellStart);
            switch (arena.cellTypeAt(cellStart)) {
            case CellType::Bindings: walkBindings(reinterpret_cast<Bindings *>(cs)); continue;
            case CellType::Closure:  walkClosure(reinterpret_cast<Closure *>(cs));   continue;
            case CellType::Thunk:    walkThunk(reinterpret_cast<Thunk *>(cs));       continue;
            case CellType::List:     walkList(reinterpret_cast<ListVec *>(cs));      continue;
            case CellType::Pair:     walkPair(reinterpret_cast<ValuePair *>(cs));    continue;
            case CellType::Value:
            case CellType::Chars:
            case CellType::None:
                break;  // unknown layout → conservative byte-scan below
            }
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
                const uintptr_t word = *reinterpret_cast<const uintptr_t *>(
                    cellStart + off);
                const uintptr_t val = word;
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
    /// S1.2 PROVENANCE: per-CellType count of conservative-only pins (0-8).
    const size_t * statsConservByType() const noexcept { return statsConservByType_; }

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
    // S1.2 PROVENANCE (NON_JIT_LEVER / safepoint-foundation): per-CellType
    // breakdown of the cells the conservative scan marks as NEW (tryMark
    // success).  Since precise marking finishes BEFORE the conservative C-stack
    // scan runs (runMajorMarkSweep captures preciseMarkedCells first), every
    // tryMark success here is a cell the precise root graph did NOT reach — i.e.
    // a genuine conservative-ONLY pin.  Indexed by CellType (0-8).  Read-only
    // diagnostic (reported under NIX_VM_STATS); zero hot-path cost beyond the
    // increment.  Tells us WHICH cell type dominates firefox's 12.5% pinned set
    // → which construction/eval/FFI paths to migrate to precise handles next
    // (guessing the targets from a static census kept missing — args[], list
    // primops both measured-unchanged).
    size_t statsConservByType_[9] = {0,0,0,0,0,0,0,0,0};
    std::vector<void *> conservativeRoots_;
    Arena * arenaSetForSlot_ = nullptr;
    bool typedInteriorOwners_ = false;  // Lever 3: mark-only typed walk
    // MIDEVAL_GC_DESIGN_2026-06-22: when set (mid-eval non-moving sweep), the mark
    // traverses RESIDENT nursery cells PRECISELY — the typed walkX → visitValue
    // decode the v8nan box, so tenured cells reachable ONLY through the nursery
    // are marked (a flat conservative byte-scan leaks nested boxed pointers).
    // `tryMark` can't mark/dedup nursery cells (not in the arena bitmap), so the
    // dedup is this side set.  arena helpers (markLinesForCell/findContaining…)
    // all no-op on non-arena addresses, so the typed walkX are nursery-safe.
    Nursery * nursery_ = nullptr;
    std::unordered_set<const void *> nurseryVisited_;
    bool enqueueNurseryCell(void * p, GrayKind kind) noexcept {
        if (!nursery_ || !nursery_->contains(p)) return false;
        if (!nurseryVisited_.insert(p).second) return false;
        worklist_.push_back({p, kind});
        return true;
    }

    // MIDEVAL_GC (RCA fix, 2026-06-23): mirror the scavenger (gc.cc:641) — a live
    // closure/thunk's CompilationUnit attrSelect IC pins `Bindings*` that are
    // reachable ONLY via the IC (transient attrsets cached by an attr-select).
    // The moving scavenger FORWARDS those entries; the non-moving mid-eval mark
    // must MARK them, else they're swept + reused = UAF (the apply-overrides
    // divergence: an override Bindings zeroed → empty attrset).  Runs only when
    // `nursery_` is set — no longer set since the mid-eval GC was retired, so
    // walkCuIC is inert; gen-major/Phase-S clear the IC before their mark, so
    // they never relied on this.  Dedup CUs via walkedCUs_.
    std::unordered_set<const CompilationUnit *> walkedCUs_;
public:
    /// M2.1 (BOUNDED_MEMORY_PLAN): the set of CUs referenced by a live thunk/closure
    /// reached in THIS mark (mid-eval only; walkCuIC gates on nursery_).  A cached CU
    /// absent from this set is COLD = no live reference = evictable.
    const std::unordered_set<const CompilationUnit *> & walkedCUs() const noexcept
        { return walkedCUs_; }
private:
    void walkCuIC(const CompilationUnit * cu) noexcept {
        if (!nursery_ || !cu || !walkedCUs_.insert(cu).second) return;
        for (const auto & ic : cu->rt.attrSelectCache)
            for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w)
                if (Bindings * b = const_cast<Bindings *>(ic.entries[w].bindings))
                    visitBindings(b);
    }

    void walkClosure(Closure * c) noexcept
    {
        // Step 11′ (Immix, 2026-05-29): mark the allocated Closure range
        // (header + inline upvalue FAM).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(c, closureAllocatedSize(c));
        }
        if (c->capturedWiths)
            visitList(c->capturedWiths);
        walkCuIC(closureCU(c));  // P1b: was c->cu. MIDEVAL_GC: IC-pinned Bindings (mirror scavenger)
        // Upvalues live inline in the FAM.  (The env-sharing path — upvalues in a
        // shared tenured Env, marked with its parent chain — was retired 2026-08
        // with the Closure::upvalEnv field.)
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            visitValue(c->upvalues[i]);
    }
    void walkThunk(Thunk * t) noexcept
    {
        // Step 11′ (Immix): mark lines for the Thunk.  Size depends
        // on state (Suspended/Native/Blackhole have FAM trailers;
        // Evaluated/Bridge are header-only).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(t, thunkScanSize(t));  // FP-2b: incl. withs slot
        }
        // Thunk::cell points at a Value cell (Bindings-resident OR standalone
        // allocValue).  Mark it via visitSlot so the cell payload is walked
        // transitively.  (M-8: the shapeCell field was removed — see closure.hh.)
        if (t->cell)      visitSlot(t->cell);
        // Phase 3.5 safety: precisely walk the owning Bindings when `cell` is
        // Bindings-resident.  M-8 (CODEBASE_REVIEW_2026-06-11) removed the
        // stored Thunk::cellContainer; per the review it is DERIVED on demand
        // via findContainingCellStart(cell) + a Bindings type-check.  visitSlot
        // above already triggers the interior-owner walk, so this is the same
        // "cheap insurance" precise walk it was before, now without the 8 B
        // per-thunk field.
        if (t->cell && arenaSetForSlot_) {
            const char * owner = arenaSetForSlot_->findContainingCellStart(t->cell);
            if (owner && arenaSetForSlot_->cellTypeAt(owner) == CellType::Bindings) {
                // visitBindings takes Bindings*& (it may forward under a moving
                // GC); bind a local lvalue.  We discard any forwarding since we
                // don't store the derived owner.
                Bindings * ownerB = reinterpret_cast<Bindings *>(const_cast<char *>(owner));
                visitBindings(ownerB);
            }
        }
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Blackhole:
            walkCuIC(thunkCU(t));  // MIDEVAL_GC: IC-pinned Bindings (mirror scavenger)
            if (ListVec * w = thunkCapturedWiths(t))  // FP-2b: tail slot
                visitList(w);
            // Upvalues live inline in the tail.  (The env-sharing path — upvalues
            // in a shared tenured Env at tail[0], marked with its parent chain —
            // was retired 2026-08 with the Closure::upvalEnv field.)
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
        // Step 11′: line-mark the Bindings cell (header + FAM entries + the
        // MapAttrs aux tail, via allocBytes()).
        if (arenaSetForSlot_) {
            arenaSetForSlot_->markLinesForCell(b, b->allocBytes());
        }
        if (b->isMapAttrs())
            visitValue(*b->mapAttrsAux());
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
/// Classify one scanned word as a conservative root: if it points into the
/// active arena, mark the cell it references.  Over-approximate ⇒ safe (a
/// non-pointer word whose value lands in arena range only over-retains).
static inline void conservativeMarkWord(
    MarkVisitor & v, Arena & arena, uintptr_t word,
    uintptr_t arenaMin, uintptr_t arenaMax) noexcept
{
    if (word < arenaMin || word >= arenaMax) return;
    void * candidate = reinterpret_cast<void *>(word);
    if (arena.inActive(candidate)) v.markConservative(candidate);
}

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

    // Walk stack.  De-box each word (covers boxed Value C-locals too).
    for (uintptr_t p = loU; p < hiU; p += sizeof(void *)) {
        conservativeMarkWord(v, arena,
            *reinterpret_cast<const uintptr_t *>(p), arenaMin, arenaMax);
    }

    // Also walk the jmp_buf (callee-saved registers).
    const uintptr_t bufLo =
        reinterpret_cast<uintptr_t>(&regsBuf);
    const uintptr_t bufHi = bufLo + sizeof(regsBuf);
    for (uintptr_t p = bufLo; p < bufHi; p += sizeof(void *)) {
        conservativeMarkWord(v, arena,
            *reinterpret_cast<const uintptr_t *>(p), arenaMin, arenaMax);
    }

    // MIDEVAL_GC_DESIGN_2026-06-22: conservatively scan the RESIDENT nursery.
    // The non-moving mid-eval mark-sweep runs with a non-empty nursery; the
    // precise walk SKIPS nursery cells (tryMark rejects non-arena pointers,
    // mark_sweep.cc:111), so a tenured cell reachable ONLY through a nursery
    // cell would be missed → swept → UAF.  Scan every used nursery byte for
    // tenured-arena pointers (same word-scan + arena-bounds filter as the
    // C-stack scan above) and conservatively mark them.  No-op when the
    // young region is empty (gen-major's post-forceScavenge path), so this is
    // free on the default collector.  Over-approximate ⇒ safe.
    // Nursery cells hold NaN-boxed Values; conservativeMarkWord de-boxes each
    // word so boxed tenured pointers aren't missed (the swept-live-cell UAF).
    threadNursery().forEachUsedRange([&](const char * lo, const char * hi) noexcept {
        uintptr_t a = reinterpret_cast<uintptr_t>(lo)
                      & ~(uintptr_t(sizeof(void *)) - 1);
        uintptr_t b = reinterpret_cast<uintptr_t>(hi)
                      & ~(uintptr_t(sizeof(void *)) - 1);
        for (uintptr_t p = a; p < b; p += sizeof(void *)) {
            conservativeMarkWord(v, arena,
                *reinterpret_cast<const uintptr_t *>(p), arenaMin, arenaMax);
        }
    });

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

    // R2.1′ (2026-06-03): live-cell tally by CellType (index = CellType
    // value 0..8).  Validates the per-cell type stamping AND shows how
    // much of the live set is now typed (movable by the metadata-aware
    // mover) vs None (unstamped → pinned).  None (index 0) counts
    // interior/huge/non-bump cells the mover can't directly type.
    size_t cellTypeHist[9] = {0,0,0,0,0,0,0,0,0};

    // P1/P2 sizing (2026-07-06, REPRESENTATION_REWRITE Phase 0): DEAD-cell
    // tally by CellType, mirroring cellTypeHist but for the swept-dead branch.
    // cellTypeHist is live-only, so total-tenured(type) = cellTypeHist[t] +
    // deadCellTypeHist[t].  Pins P1a's ceiling (8B * total-tenured Bindings =
    // the header-shrink peak-RSS bound) and sizes P2 (per-type reclaimable
    // dead bytes/count).  Measurement-only; reported under NIX_VM_STATS.
    size_t deadCellTypeHist[9]  = {0,0,0,0,0,0,0,0,0};
    size_t deadCellTypeBytes[9] = {0,0,0,0,0,0,0,0,0};

    // R2.4b: per regular-block (start, live-byte fraction).  The
    // evacuator filters this to the sparse candidate set.  Populated
    // in sweepOneBlock's density section.
    std::vector<std::pair<const char *, double>> blockDensities;
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
    const std::vector<uint8_t> &  cellTypeBytes,   // R2.1′: per-granule type
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
            // R2.1′ + M-9: tally the live cell by its stamped type.  The
            // type array is nibble-packed (two granules per byte); unpack via
            // the shared helper (bounds-checked, returns None when out of
            // range).  `cellTypeBytes` here is the per-block nibble array.
            const size_t gran = offset >> 4;
            const uint8_t ty =
                static_cast<uint8_t>(nix::v3::cellTypeUnpack(cellTypeBytes, gran));
            stats.cellTypeHist[ty < 9 ? ty : 0]++;
        } else {
            ++stats.deadCells;
            stats.deadBytes += cellSize;
            // P1/P2 sizing: tally the DEAD cell by its stamped type (mirror of
            // the live branch's cellTypeHist unpack).  Measurement-only.
            {
                const size_t gran = offset >> 4;
                const uint8_t ty = static_cast<uint8_t>(
                    nix::v3::cellTypeUnpack(cellTypeBytes, gran));
                const size_t ti = ty < 9 ? ty : 0;
                stats.deadCellTypeHist[ti]++;
                stats.deadCellTypeBytes[ti] += cellSize;
            }
            // Phase 3: route dead cell to the per-exact-size free
            // list and clear its cell-start bit.  Subsequent allocs
            // of this size will reuse the freed slot.
            //
            // PLAN_BEAT_TW Phase 0.5 (2026-06-12): the freeListBins_ push is
            // ONLY ever consumed by the legacy reuse path in Arena::alloc()
            // (alloc.hh:1315 — gated `g_freeListReuseEnabled &&
            // !g_immixAllocEnabled`).  On the DEFAULT path (both gates off)
            // and on the Immix path (line-region reuse, not bins) nothing
            // pops freeListBins_, so the push was pure RSS bloat (8 B/cell +
            // unordered_map<vector> growth) and sweep CPU with zero benefit.
            // Gate it to the only condition that consumes it.  Retirement:
            // folds away with the freeListBins_ legacy path at the Immix SHIP
            // gate (GC_DECISION_2026-05-29 §6).  clearCellStartBitFor stays
            // unconditional — it is bitmap-only (no growth) and keeps the
            // sweep-iteration semantics identical to before this change.
            if (nix::v3::detail::g_freeListReuseEnabled
                && !nix::v3::detail::g_immixAllocEnabled) {
                arena.freeListAdd(
                    const_cast<void *>(cellAddr), cellSize);
            }
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
        stats.blockDensities.emplace_back(blockStart, dens);  // R2.4b
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

/// Review #2: erase string-context side-table entries whose char-buffer
/// key lives in a now-freed (munmapped) block range.  The side-table is
/// keyed by char-buffer pointer; without this, a later alloc at a
/// recycled virtual address inherits the stale context → e.g. a glibc.drv
/// store-path context attaches to an unrelated literal ("flags") →
/// "not allowed to refer to a store path".  Used by the whole-block-free /
/// huge-reclaim path in runMajorMarkSweep so blocks freed there don't leak
/// stale context.  `freedRanges` is sorted in place.
static void sweepStringContextRanges(
    std::vector<std::pair<uintptr_t, uintptr_t>> & freedRanges) noexcept
{
    if (freedRanges.empty()) return;
    std::sort(freedRanges.begin(), freedRanges.end());
    auto & tbl = stringContextSideTable();
    for (auto it = tbl.begin(); it != tbl.end(); ) {
        const uintptr_t k = reinterpret_cast<uintptr_t>(it->first);
        auto rit = std::upper_bound(
            freedRanges.begin(), freedRanges.end(), k,
            [](uintptr_t key, const std::pair<uintptr_t, uintptr_t> & r) {
                return key < r.first;
            });
        bool inFreed = false;
        if (rit != freedRanges.begin()) { --rit; inFreed = (k < rit->second); }
        if (inFreed) it = tbl.erase(it);
        else ++it;
    }
}

} // anonymous

MajorGcResult runMajorMarkSweep(VMState & vm) noexcept
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
    visitor.setTypedInteriorOwners(true);  // Lever 3: fast mark (not verify)
    const auto tm0 = clock::now();
    walkAllV3Roots(vm, visitor);
    visitor.drain();
    const auto tm1 = clock::now();

    // Phase 3.5 follow-up: drain conservative roots accumulated from
    // visitSlot's interior-owner discovery during precise mark.
    {
        uintptr_t arenaMin, arenaMax;
        arena.activeBounds(arenaMin, arenaMax);
        visitor.drainConservative(arena, arenaMin, arenaMax);
    }
    const auto tm2 = clock::now();

    // R2.4a (2026-06-02): evacuation safety precondition.  Cells
    // reachable by the PRECISE walk (above) hold rewritable slots, so
    // evacuation can move them and fix up the references.  Cells
    // reachable ONLY by the conservative C-stack scan (below) are
    // pinned by an un-rewritable C-local pointer — moving them would
    // dangle that pointer.  The delta (conservative-only marked cells)
    // is the un-evacuatable set; their containing blocks must be
    // PINNED, not evacuated.  Small delta => evacuation can move ~all
    // sparse-block live cells; large delta => yield shrinks.
    const size_t preciseMarkedCells = marker.markedCells();

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
        // S1.2 PIN-FRAME ATTRIBUTION (NIX_V3_PIN_FRAMES): at a mid-eval safepoint
        // the C-stack literally CONTAINS the suspended re-entrant primop frames
        // whose C-locals are about to be conservatively pinned.  Symbolize the
        // call chain here (once per sweep, capped) so the conserv-provenance
        // TYPE breakdown gains FRAME provenance — names the exact primops whose
        // Value/string/Bindings locals must migrate to GcRoot.  Confirms the
        // migration target BEFORE the work (args[]+list-primops were both
        // measured-unchanged from a static census → frames remove the guessing).
        // Gated; one-shot read; capped at 8 dumps to bound stderr.
        static const bool s_pinFrames = std::getenv("NIX_V3_PIN_FRAMES") != nullptr;
        if (__builtin_expect(s_pinFrames, 0)) {
            static int s_dumps = 0;
            if (s_dumps++ < 8) {
                void * fr[64];
                int n = ::backtrace(fr, 64);
                char ** syms = ::backtrace_symbols(fr, n);
                std::fprintf(stderr, "[pin-frames] sweep#%d C-stack at safepoint (%d frames):\n",
                             s_dumps, n);
                // Skip the top GC frames (this fn + caller); print the chain so
                // primNNN / *Strict / merge* / mapAttrs frames are visible.
                for (int i = 0; i < n; ++i)
                    if (syms && syms[i]) std::fprintf(stderr, "[pin-frames]   #%02d %s\n", i, syms[i]);
                if (syms) ::free(syms);
            }
        }
        // S1.2 LIVE-vs-STALE MEASUREMENT (NIX_V3_NO_CONSERV_SCAN): skip the
        // conservative C-stack scan to test whether the conservativeOnly pins are
        // LIVE-but-precise-missed (→ removing the scan sweeps a live cell → UAF /
        // divergence → handle migration is the path) or STALE/DEAD (→ byte-id
        // survives → the scan over-pins dead cells, and the RSS win is removing
        // the scan, NOT migrating handles).  MEASUREMENT ONLY: this is unsafe by
        // construction (it deliberately drops a root source); the gate exists to
        // answer the live-vs-stale question on a throwaway eval.  RETIREMENT: once
        // the question is answered + recorded (CONSERV_PIN_PROVENANCE doc), delete
        // this gate — it must NEVER ship enabled (a real eval would UAF if any pin
        // is live).  Compare the output drvPath to TW to detect corruption.
        static const bool s_noConservScan = std::getenv("NIX_V3_NO_CONSERV_SCAN") != nullptr;
        if (!__builtin_expect(s_noConservScan, 0))
            walkCStackConservative(visitor, arena, sp);
    }

    const size_t conservativeOnlyCells =
        marker.markedCells() - preciseMarkedCells;

    const auto tMarkEnd = clock::now();
    {
        // T-4 (CODEBASE_REVIEW_2026-06-11): gate this per-GC timing line behind
        // NIX_VM_STATS.  It used to fire unconditionally on every major GC,
        // polluting stderr-capturing byte-identity harnesses (the measurement
        // gate compares stderr) and any tool that diffs v3 vs TW output.
        static const bool s_markStats = std::getenv("NIX_VM_STATS") != nullptr;
        if (__builtin_expect(s_markStats, 0)) {
            auto ms = [](clock::time_point a, clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            std::fprintf(stderr,
                "v3 mark-split: preciseWalk=%.0fms drainConservative=%.0fms "
                "cStackConservative=%.0fms\n",
                ms(tm0, tm1), ms(tm1, tm2), ms(tm2, tMarkEnd));
            // M2.1 (BOUNDED_MEMORY_PLAN): size the CU-eviction win at THIS safepoint.
            // A cached CU absent from the mark's referenced-CU set (visitor.walkedCUs())
            // has no live thunk/closure → COLD = evictable now.
            // walkedCUs() is populated by walkCuIC, which gates on `nursery_`; since
            // the mid-eval GC was retired `nursery_` is never set, so this reports
            // all CUs cold (the M2.1 sizing is inert on the surviving paths).
            {
                size_t coldCount = 0;
                const size_t cuCount  = importCacheCuCount();
                const size_t coldB    = importCacheColdBytes(
                    [&](const CompilationUnit * cu) {
                        return visitor.walkedCUs().count(cu) == 0; },
                    coldCount);
                const size_t totB = importCacheBytecodeBytes();
                std::fprintf(stderr,
                    "v3 M2.1 cold-CU: %zu/%zu CUs cold, %.0f/%.0fMB evictable "
                    "(%.0f%% of CU-bytecode; referenced=%zu)\n",
                    coldCount, cuCount, coldB / 1e6, totB / 1e6,
                    totB ? 100.0 * coldB / totB : 0.0, visitor.walkedCUs().size());
            }
        }
    }

    // -- Phase 2 step 2: sweep (measurement-only; no free yet) ------
    SweepStats sweep;
    const auto & cellStarts = arena.cellStartBitmaps();
    const auto & cellTypes  = arena.cellTypeArrays();  // R2.1′
    const auto ranges = arena.blockRanges();
    static const std::vector<uint8_t> emptyTypes;

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
            regularBlockIdx < cellTypes.size()
                ? cellTypes[regularBlockIdx] : emptyTypes,
            marker,
            sweep);
        if (fullyDead) blocksToFree.push_back(r.begin);
        ++regularBlockIdx;
    }

    // Phase 3.8: free fully-dead blocks back to libc.  Done as a
    // second pass so we don't mutate active_.blocks during the sweep
    // iteration.  Each freeWholeBlock removes the block from arena
    // metadata + filters its free-list entries.
    // Collect the freed (begin,end) ranges so we can sweep the string-
    // context side-table for them too (review #2): the previous code only
    // swept the EVAC-freed ranges, leaking stale context for blocks freed
    // by this default whole-block-free path.
    // MIDEVAL_GC_DESIGN_2026-06-22: whole-block-free munmaps the block
    // (freeWholeBlock assumes refill() mmap'd it).  Under the mid-eval gate the
    // legacy major GC is OFF, so blocks are CALLOC'd (cellMetaEnabled gates only
    // the cell-start/type metadata, NOT the block alloc method) — munmap'ing a
    // calloc'd block would crash.  So whole-block-free runs ONLY when the legacy
    // major GC is on; under mid-eval the fully-dead blocks' CELLS are still binned
    // by the sweep above for in-block free-list reuse (the firefox win — its dead
    // is scattered → ~0 fully-dead blocks anyway).  Huge blocks (below) are safe:
    // freeHugeBlock picks std::free vs munmap to match how they were allocated.
    std::vector<std::pair<uintptr_t, uintptr_t>> freedRanges;
    if (Arena::majorGcEnabled())
    for (const char * blk : blocksToFree) {
        const size_t freed = arena.freeWholeBlock(blk);
        if (freed > 0) {
            ++sweep.blocksFreed;
            sweep.bytesFreed += freed;
            freedRanges.emplace_back(
                reinterpret_cast<uintptr_t>(blk),
                reinterpret_cast<uintptr_t>(blk) + Arena::kBlockSize);
        }
    }

    // Review #4: reclaim DEAD huge blocks (>4 MB, e.g. M5's 170k-entry
    // Bindings).  A huge block IS a single cell; if its begin address was
    // not marked, it is unreachable.  munmap returns its RSS (the alloc
    // path mmaps huge blocks under the major-GC gate).  Without this, huge
    // blocks accumulated for the whole eval (unbounded RSS) — the dominant
    // cost on M5 and directly counter to the no-Boehm / peak-RSS goal.
    // Iterate a snapshot (freeHugeBlock mutates the live vector).
    for (auto & [hbeg, hsz] : arena.hugeBlockRanges()) {
        if (!marker.isMarked(hbeg)) {
            const size_t freed = arena.freeHugeBlock(hbeg);
            if (freed > 0) {
                ++sweep.blocksFreed;
                sweep.bytesFreed += freed;
                freedRanges.emplace_back(
                    reinterpret_cast<uintptr_t>(hbeg),
                    reinterpret_cast<uintptr_t>(hbeg) + freed);
            }
        }
    }

    // Review #2: sweep stale string-context entries for the whole-block +
    // huge freed ranges.
    sweepStringContextRanges(freedRanges);

    // M-2 (CODEBASE_REVIEW_2026-06-11): also sweep the side-table against the
    // MARK BITMAP.  The range-sweep above only drops context for WHOLE freed
    // blocks; a string buffer that is DEAD (unmarked) but sits in a STILL-LIVE
    // block keeps its char* valid, so once granule reuse is on (Immix line
    // reuse / free-list, the M-7 prerequisite) a NEW string allocated at that
    // recycled granule would inherit the dead string's context (#682 family).
    // visitString() marks every reachable string buffer, so an unmarked
    // in-arena key is genuinely dead.  Erase only `inActive && !isMarked` keys
    // (a non-arena key — e.g. a static string — is left untouched).
    {
        auto & sctbl = stringContextSideTable();
        for (auto it = sctbl.begin(); it != sctbl.end(); ) {
            const void * k = static_cast<const void *>(it->first);
            if (arena.inActive(k) && !marker.isMarked(k))
                it = sctbl.erase(it);
            else
                ++it;
        }
    }

    // Step 12′ (Immix, 2026-05-29): rebuild free-line spans from the
    // post-mark line-mark bitmap.  Spans drive `Arena::alloc()` until
    // the next major GC.  Resets the immix bump-pointer state so
    // next alloc starts at span 0 of block 0.
    //
    // Must happen AFTER blocksToFree (freeWholeBlock removes
    // entries from lineMarks; this rebuild walks the surviving set).
    // P-8 (CODEBASE_REVIEW_2026-06-11): only rebuild the free-line spans when
    // Immix alloc actually consumes them.  With V3_DBG_IMMIX_ALLOC=0 (default)
    // the Arena alloc path skips the Immix branch, so the rebuilt freeSpans
    // were computed and immediately ignored — per-GC dead work over every
    // surviving block.
    if (nix::v3::detail::g_immixAllocEnabled)
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
        // R2.4a: evacuation movability — precise-reachable (rewritable,
        // movable) vs conservative-only (pinned).  conservativePct high
        // => many cells can't be moved => evacuation pins their blocks.
        std::fprintf(stderr,
            "v3 evac-movability: preciseMarked=%zu conservativeOnly=%zu "
            "(%.1f%% pinned by C-stack)\n",
            preciseMarkedCells, conservativeOnlyCells,
            (preciseMarkedCells + conservativeOnlyCells) > 0
                ? 100.0 * double(conservativeOnlyCells)
                          / double(preciseMarkedCells + conservativeOnlyCells)
                : 0.0);
        // S1.2 PROVENANCE: per-CellType breakdown of the conservative-ONLY pins
        // (cells the C-stack scan marked NEW, after the precise mark).  This is
        // the actionable signal: the dominant type names the construction/eval/
        // FFI path whose C++-local Values must be migrated to precise handles
        // (GcRoot/…) next.  args[] + the 6 list-fold primops both measured
        // firefox-unchanged at 12.5% → this localizes the ACTUAL sources instead
        // of guessing from a static census.
        {
            const size_t * c = visitor.statsConservByType();
            std::fprintf(stderr,
                "v3 conserv-provenance: None=%zu Value=%zu Closure=%zu Thunk=%zu "
                "Bindings=%zu List=%zu Pair=%zu Env=%zu Chars=%zu\n",
                c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
        }
        // R2.1′: live-cell tally by stamped CellType — validates the
        // per-cell type metadata + shows the typed (movable) fraction.
        {
            const size_t * h = sweep.cellTypeHist;
            const size_t typed = h[1]+h[2]+h[3]+h[4]+h[5]+h[6]+h[7]+h[8];
            std::fprintf(stderr,
                "v3 evac-celltypes: None=%zu Value=%zu Closure=%zu Thunk=%zu "
                "Bindings=%zu List=%zu Pair=%zu Env=%zu Chars=%zu "
                "(typed/movable=%.1f%%)\n",
                h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8],
                (typed + h[0]) > 0 ? 100.0 * double(typed)
                                     / double(typed + h[0]) : 0.0);
        }
        // P1/P2 sizing (REPRESENTATION_REWRITE Phase 0): DEAD-cell tally by
        // type + total-tenured (live+dead) per type.  total-tenured Bindings
        // * 8B = P1a header-shrink peak-RSS ceiling; per-type dead bytes size
        // the P2 GC-reclaim lever.  Measurement-only.
        {
            const size_t * lh = sweep.cellTypeHist;
            const size_t * dh = sweep.deadCellTypeHist;
            const size_t * db = sweep.deadCellTypeBytes;
            std::fprintf(stderr,
                "v3 dead-celltypes: None=%zu Value=%zu Closure=%zu Thunk=%zu "
                "Bindings=%zu List=%zu Pair=%zu Env=%zu Chars=%zu\n",
                dh[0], dh[1], dh[2], dh[3], dh[4], dh[5], dh[6], dh[7], dh[8]);
            std::fprintf(stderr,
                "v3 dead-celltype-MB: Closure=%.1f Thunk=%.1f Bindings=%.1f "
                "List=%.1f Pair=%.1f (Bindings total-tenured=%zu -> "
                "P1a aux-shrink ceiling=%.2fMB)\n",
                db[2]/1e6, db[3]/1e6, db[4]/1e6, db[5]/1e6, db[6]/1e6,
                lh[4] + dh[4],
                double(lh[4] + dh[4]) * 8.0 / 1e6);
        }
        // P2 falsifier (REPRESENTATION_REWRITE Phase 2): block live-density
        // distribution at this (near-peak) sweep + whole-block-free / evac
        // ceilings.  The GO gate is: reclaim >=300MB TO OS at peak.  Two
        // mechanisms: (a) whole-block-free (non-moving) reclaims only FULLY
        // dead blocks = blocksFreed*blockSize; (b) evacuation (moving) could
        // reclaim SPARSE blocks (<25% live) but must copy their live bytes
        // (churn that raises peak).  densityHist bins blocks by live fraction
        // [0-10)[10-25)[25-50)[50-75)[75-100]%; sparseBlocks = the <25% ones
        // (evacuation candidates), sparseLiveBytes = the copy cost. If blocks
        // cluster 25-75% (dense-scattered), BOTH mechanisms reclaim ~0.
        {
            const size_t * dHist = sweep.densityHist;
            const size_t nblk = sweep.blocksScanned;
            const double evacCeilingMB =
                double(nblk ? (dHist[0] + dHist[1]) : 0) * (512.0 / 1024.0);  // sparse blocks * ~512KB
            std::fprintf(stderr,
                "v3 P2-density: blocks=%zu bins[<10%%=%zu 10-25=%zu 25-50=%zu "
                "50-75=%zu 75-100=%zu] sparse(<25%%)=%zu sparseLiveMB=%.1f\n",
                nblk, dHist[0], dHist[1], dHist[2], dHist[3], dHist[4],
                sweep.sparseBlocks, double(sweep.sparseLiveBytes)/1e6);
            std::fprintf(stderr,
                "v3 P2-reclaim-to-OS: whole-block-free blocksFreed=%zu bytesFreed=%.1fMB "
                "(evac-ceiling ~%.0fMB sparse-blocks * 512KB, minus %.1fMB copy-churn) "
                "[GO gate >=300MB]\n",
                sweep.blocksFreed, double(sweep.bytesFreed)/1e6,
                evacCeilingMB, double(sweep.sparseLiveBytes)/1e6);
        }
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

    // PLAN_BEAT_TW_V2 §1.1b: hand the trigger what it needs for the
    // adaptive backoff — bytes the sweep actually returned to libc, and
    // the heap size at cycle entry (the "freed < 5 % of heap" test + the
    // backoff anchor).
    return { sweep.bytesFreed, arenaBytesBefore };
}

} // namespace nix::v3
