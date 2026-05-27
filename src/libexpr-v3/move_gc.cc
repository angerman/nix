/// @file
/// Stage 6 production GC — MajorScavenger implementation.
///
/// Per `lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` Day 2.2.
///
/// ## Operation
///
/// MajorScavenger walks all v3 roots via `walkAllV3Roots`, copying
/// each reachable cell from `arena.active_` to `arena.backup_`,
/// then drains a worklist to recursively rewrite pointer fields
/// in the copied cells.  After drain + caller-invoked
/// swapRegions() + freeBackupBlocks(), the net effect is:
///   * Active region holds only live cells (compacted)
///   * Old-active memory returned to OS (or libc)
///   * Stage 6 RSS savings realized
///
/// Each visit() classifies via `arena.regionOf` + checks the
/// forwarding table.  Per-type sizes are computed from cell
/// metadata (Closure::nUpvalues, Bindings::size, ListVec::size, etc.).
///
/// ## NOT integrated with dispatch loop in this commit
///
/// `runMajorScavenge(vm)` is callable but not wired into v3's
/// dispatch loop in this commit (that's Day 3).  Production code
/// is unaffected.  Standalone test access via
/// `NIX_V3_MAJOR_GC_TEST=1` env-gate (Day 2.2 standalone test).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/major_scavenge.hh"
#include "v3/alloc.hh"
#include "v3/precise_root.hh"
#include "v3/vm.hh"
#include "v3/closure.hh"

#include <cstdio>
#include <cstring>

namespace nix::v3 {

MajorScavenger::MajorScavenger(Arena & arena) noexcept
    : arena_(arena)
{
    forwarding_.reserve(1024);
    worklist_.reserve(1024);
}

// ---------------------------------------------------------------------------
// Per-type forwarders.  Pattern (per gc.cc::fwd*):
//   1. nullptr → nullptr
//   2. Not in active → return unchanged (already backup OR external)
//   3. Forwarding table hit → return forwarded
//   4. Allocate dst in backup_, memcpy, register forwarding,
//      enqueue for transitive walk, return new ptr
// ---------------------------------------------------------------------------

Closure * MajorScavenger::fwdClosure(Closure * c)
{
    if (!c) return nullptr;
    if (!arena_.inActive(c)) return c;  // External / Backup → leave
    auto it = forwarding_.find(c);
    if (it != forwarding_.end()) return static_cast<Closure *>(it->second);

    const size_t bytes = sizeof(Closure) + sizeof(Value) * c->nUpvalues;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, c, bytes);
    forwarding_.emplace(c, dst);
    worklist_.push_back({dst, KClosure});

    ++stats_.closuresCopied;
    stats_.bytesCopied += bytes;
    return static_cast<Closure *>(dst);
}

Thunk * MajorScavenger::fwdThunk(Thunk * t)
{
    if (!t) return nullptr;
    if (!arena_.inActive(t)) return t;
    auto it = forwarding_.find(t);
    if (it != forwarding_.end()) return static_cast<Thunk *>(it->second);

    // Size depends on state per closure.hh::Thunk layout.
    size_t bytes;
    switch (t->state) {
    case ThunkState::Suspended:
    case ThunkState::Native:
    case ThunkState::Blackhole:
        bytes = sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
        break;
    case ThunkState::Evaluated:
    case ThunkState::Bridge:
        bytes = sizeof(Thunk);
        break;
    }
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, t, bytes);
    forwarding_.emplace(t, dst);
    worklist_.push_back({dst, KThunk});

    ++stats_.thunksCopied;
    stats_.bytesCopied += bytes;
    return static_cast<Thunk *>(dst);
}

Bindings * MajorScavenger::fwdBindings(Bindings * b)
{
    if (!b) return nullptr;
    if (!arena_.inActive(b)) return b;
    auto it = forwarding_.find(b);
    if (it != forwarding_.end()) return static_cast<Bindings *>(it->second);

    const size_t bytes = sizeof(Bindings) + sizeof(Bindings::Entry) * b->size;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, b, bytes);
    forwarding_.emplace(b, dst);
    worklist_.push_back({dst, KBindings});

    ++stats_.bindingsCopied;
    stats_.bytesCopied += bytes;
    return static_cast<Bindings *>(dst);
}

ListVec * MajorScavenger::fwdList(ListVec * l)
{
    if (!l) return nullptr;
    if (!arena_.inActive(l)) return l;
    auto it = forwarding_.find(l);
    if (it != forwarding_.end()) return static_cast<ListVec *>(it->second);

    const size_t bytes = sizeof(ListVec) + sizeof(Value) * l->size;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, l, bytes);
    forwarding_.emplace(l, dst);
    worklist_.push_back({dst, KList});

    ++stats_.listsCopied;
    stats_.bytesCopied += bytes;
    return static_cast<ListVec *>(dst);
}

ValuePair * MajorScavenger::fwdPair(ValuePair * p)
{
    if (!p) return nullptr;
    if (!arena_.inActive(p)) return p;
    auto it = forwarding_.find(p);
    if (it != forwarding_.end()) return static_cast<ValuePair *>(it->second);

    const size_t bytes = sizeof(ValuePair);
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, p, bytes);
    forwarding_.emplace(p, dst);
    worklist_.push_back({dst, KPair});

    ++stats_.pairsCopied;
    stats_.bytesCopied += bytes;
    return static_cast<ValuePair *>(dst);
}

// ---------------------------------------------------------------------------
// RootVisitor interface — each visit rewrites the slot to the
// forwarded address (if active) or leaves it (if external/backup).
// ---------------------------------------------------------------------------

void MajorScavenger::visitClosure(Closure * & slot)
{
    slot = fwdClosure(slot);
}

void MajorScavenger::visitThunk(Thunk * & slot)
{
    slot = fwdThunk(slot);
}

void MajorScavenger::visitBindings(Bindings * & slot)
{
    slot = fwdBindings(slot);
}

void MajorScavenger::visitList(ListVec * & slot)
{
    slot = fwdList(slot);
}

void MajorScavenger::visitPair(ValuePair * & slot)
{
    slot = fwdPair(slot);
}

void MajorScavenger::visitSlot(Value * & slot)
{
    // Tag::Slot dereferences a pointer to a Value in another cell.
    // The slot pointer itself doesn't move (it's into an arena cell
    // which IS being moved — but the cell's MEMBER address is what
    // changes; we rely on the recursive walk to update the cell's
    // internal slot pointers).  What we DO is walk through the
    // pointed-to Value's payload to potentially forward THAT.
    //
    // Dedup so multiple Tag::Slot Values aliasing the same cell
    // don't re-walk.
    if (!slot) return;
    if (!cellsFollowed_.insert(slot).second) return;
    ++stats_.slotsFollowed;
    visitValue(*slot);
}

// ---------------------------------------------------------------------------
// Field walkers — called from drain() per cell type.  Each rewrites
// pointer-bearing fields via the visit*/visitValue dispatch.
//
// Note: the cell we're walking is the COPIED cell in backup_.
// Its initial contents (from memcpy) point at active_; the walks
// rewrite those pointers to the new backup_ addresses.
// ---------------------------------------------------------------------------

void MajorScavenger::walkClosure(Closure * c) noexcept
{
    // External pointers — do NOT rewrite:
    //   c->desc — into CompilationUnit (libc, owned by ImportCache::cus deque)
    //   c->cu   — same
    // Arena pointers (do rewrite):
    //   c->capturedWiths (ListVec*)
    //   c->upvalues[]   (Value, each may carry an arena pointer)
    if (c->capturedWiths) c->capturedWiths = fwdList(c->capturedWiths);
    for (uint16_t i = 0; i < c->nUpvalues; ++i)
        visitValue(c->upvalues[i]);
}

void MajorScavenger::walkThunk(Thunk * t) noexcept
{
    // Thunk's slot pointers (cell + shapeCell) point INTO other
    // cells in active_ — those cells get copied separately; we
    // can't rewrite the cell pointer itself (it'd point at the
    // OLD cell address).  Instead, the cells those pointers
    // address get visited via Tag::Slot from their owning
    // Bindings::entries[].  For Day 2.2 safety: leave cell +
    // shapeCell as-is; they may dangle after swap.  This is a
    // KNOWN LIMITATION documented for Day 3+ to address via cell
    // forwarding logic.
    //
    // cellContainer is a Bindings* — that we CAN forward.
    if (t->cellContainer) t->cellContainer = fwdBindings(t->cellContainer);

    switch (t->state) {
    case ThunkState::Suspended:
    case ThunkState::Blackhole:
        // suspended.desc is CompilationUnit*-owned (libc) — skip.
        // suspended.cu — also libc-owned.
        if (t->suspended.capturedWiths)
            t->suspended.capturedWiths = fwdList(t->suspended.capturedWiths);
        for (uint16_t i = 0; i < t->nUpvalues; ++i)
            visitValue(t->tail[i]);
        break;
    case ThunkState::Evaluated:
        visitValue(t->evaluated);
        break;
    case ThunkState::Native:
        // native.fn is static PrimOp* — skip.
        for (uint16_t i = 0; i < t->nUpvalues; ++i)
            visitValue(t->tail[i]);
        break;
    case ThunkState::Bridge:
        // bridgeSrc is a `nix::Value *` from TW — separately
        // rooted via bridge_root_registry.  Not an arena pointer.
        break;
    }
}

void MajorScavenger::walkBindings(Bindings * b) noexcept
{
    for (uint32_t i = 0; i < b->size; ++i)
        visitValue(b->entries[i].value);
    // Chain bindings parent (per Phase A1a) — forward if Chain.
    if (b->parent)
        b->parent = fwdBindings(const_cast<Bindings *>(b->parent));
}

void MajorScavenger::walkList(ListVec * l) noexcept
{
    for (uint32_t i = 0; i < l->size; ++i)
        visitValue(l->elems[i]);
}

void MajorScavenger::walkPair(ValuePair * p) noexcept
{
    visitValue(p->left);
    visitValue(p->right);
    visitValue(p->evaluated);
}

// ---------------------------------------------------------------------------
// Drain — process the worklist transitively.
// ---------------------------------------------------------------------------

void MajorScavenger::drain() noexcept
{
    while (!worklist_.empty()) {
        Gray g = worklist_.back();
        worklist_.pop_back();
        switch (g.kind) {
        case KClosure:  walkClosure (static_cast<Closure   *>(g.ptr)); break;
        case KThunk:    walkThunk   (static_cast<Thunk     *>(g.ptr)); break;
        case KBindings: walkBindings(static_cast<Bindings  *>(g.ptr)); break;
        case KList:     walkList    (static_cast<ListVec   *>(g.ptr)); break;
        case KPair:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
        }
    }
}

// ---------------------------------------------------------------------------
// Top-level driver: walk roots + drain + swap + free.
// ---------------------------------------------------------------------------

void runMajorScavenge(VMState & vm) noexcept
{
    Arena & arena = threadArena();
    MajorScavenger mv(arena);
    walkAllV3Roots(vm, mv);
    mv.drain();
    arena.swapRegions();
    arena.freeBackupBlocks();

    // Status banner under NIX_VM_STATS=1.
    static const bool s_stats = std::getenv("NIX_VM_STATS") != nullptr;
    if (s_stats) {
        const auto & s = mv.stats();
        std::fprintf(stderr,
            "v3-direct major-scavenge: closures=%llu thunks=%llu bindings=%llu "
            "lists=%llu pairs=%llu bytesCopied=%.1fMB slotsFollowed=%llu\n",
            (unsigned long long)s.closuresCopied,
            (unsigned long long)s.thunksCopied,
            (unsigned long long)s.bindingsCopied,
            (unsigned long long)s.listsCopied,
            (unsigned long long)s.pairsCopied,
            s.bytesCopied / 1e6,
            (unsigned long long)s.slotsFollowed);
    }
}

} // namespace nix::v3
