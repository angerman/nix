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
#include "v3/barrier.hh"  // standaloneCellRoots()

#include <cstdio>
#include <cstring>

namespace nix::v3 {

MajorScavenger::MajorScavenger(Arena & arena) noexcept
    : arena_(arena)
{
    // Day 3 Step 1: pre-reserve typed maps.  Conservative
    // 1024-slot pre-size matches the Day-2.2 single-map default;
    // grow naturally as the workload demands.
    forwardingClosure_.reserve(1024);
    forwardingThunk_.reserve(1024);
    forwardingBindings_.reserve(1024);
    forwardingList_.reserve(1024);
    forwardingPair_.reserve(1024);
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
    auto it = forwardingClosure_.find(c);
    if (it != forwardingClosure_.end()) return it->second;

    const size_t bytes = sizeof(Closure) + sizeof(Value) * c->nUpvalues;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, c, bytes);
    Closure * newC = static_cast<Closure *>(dst);
    forwardingClosure_.emplace(c, newC);
    worklist_.push_back({newC, KClosure});

    ++stats_.closuresCopied;
    stats_.bytesCopied += bytes;
    return newC;
}

Thunk * MajorScavenger::fwdThunk(Thunk * t)
{
    if (!t) return nullptr;
    if (!arena_.inActive(t)) return t;
    auto it = forwardingThunk_.find(t);
    if (it != forwardingThunk_.end()) return it->second;

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
    Thunk * newT = static_cast<Thunk *>(dst);
    forwardingThunk_.emplace(t, newT);
    worklist_.push_back({newT, KThunk});

    ++stats_.thunksCopied;
    stats_.bytesCopied += bytes;
    return newT;
}

Bindings * MajorScavenger::fwdBindings(Bindings * b)
{
    if (!b) return nullptr;
    if (!arena_.inActive(b)) return b;
    auto it = forwardingBindings_.find(b);
    if (it != forwardingBindings_.end()) return it->second;

    const size_t bytes = sizeof(Bindings) + sizeof(Bindings::Entry) * b->size;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, b, bytes);
    Bindings * newB = static_cast<Bindings *>(dst);
    forwardingBindings_.emplace(b, newB);
    worklist_.push_back({newB, KBindings});

    ++stats_.bindingsCopied;
    stats_.bytesCopied += bytes;
    return newB;
}

ListVec * MajorScavenger::fwdList(ListVec * l)
{
    if (!l) return nullptr;
    if (!arena_.inActive(l)) return l;
    auto it = forwardingList_.find(l);
    if (it != forwardingList_.end()) return it->second;

    const size_t bytes = sizeof(ListVec) + sizeof(Value) * l->size;
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, l, bytes);
    ListVec * newL = static_cast<ListVec *>(dst);
    forwardingList_.emplace(l, newL);
    worklist_.push_back({newL, KList});

    ++stats_.listsCopied;
    stats_.bytesCopied += bytes;
    return newL;
}

ValuePair * MajorScavenger::fwdPair(ValuePair * p)
{
    if (!p) return nullptr;
    if (!arena_.inActive(p)) return p;
    auto it = forwardingPair_.find(p);
    if (it != forwardingPair_.end()) return it->second;

    const size_t bytes = sizeof(ValuePair);
    void * dst = arena_.allocInBackup(bytes);
    std::memcpy(dst, p, bytes);
    ValuePair * newP = static_cast<ValuePair *>(dst);
    forwardingPair_.emplace(p, newP);
    worklist_.push_back({newP, KPair});

    ++stats_.pairsCopied;
    stats_.bytesCopied += bytes;
    return newP;
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
    // Day 3 Step 2/3: Tag::Slot slot pointer may need rewriting.
    //
    // Case 1 — slot points at a standalone cell that we already
    // moved (Step 2): forward via cellForwarding_ table.
    //
    // Case 2 — slot points INSIDE a Bindings::entries[].value
    // (the common let-rec slot pattern): defer to Step 3's
    // post-drain resolution.  Day-3 Step 2 leaves this with the
    // OLD pointer; the slot is broken if Bindings moves but
    // Step 3 isn't yet implemented.  Production must wait for
    // Step 3 — runMajorScavenge stays caller-invoked only.
    //
    // Case 3 — slot points at backup (already forwarded) or
    // external: leave as-is.
    if (!slot) return;
    auto it = forwardingCell_.find(slot);
    if (it != forwardingCell_.end()) {
        // Case 1: standalone cell moved.  Update slot to new addr.
        slot = it->second;
    }
    // For all cases: dedup-walk the pointed-to Value's payload to
    // potentially forward THAT cell's contents.
    if (!slot) return;
    if (!cellsFollowed_.insert(slot).second) return;
    ++stats_.slotsFollowed;
    visitValue(*slot);
}

void MajorScavenger::walkStandaloneCells() noexcept
{
    auto & roots = standaloneCellRoots();
    for (size_t i = 0; i < roots.size(); ++i) {
        Value * oldCell = roots[i];
        if (!oldCell) continue;
        if (!arena_.inActive(oldCell)) continue;
        // Allocate a new Value cell in backup_, copy the contents.
        // sizeof(Value) is the canonical cell size; allocValue's
        // arena alignment is 16-byte so allocInBackup matches.
        Value * newCell = static_cast<Value *>(
            arena_.allocInBackup(sizeof(Value)));
        *newCell = *oldCell;
        forwardingCell_.emplace(oldCell, newCell);
        roots[i] = newCell;
        // The new cell's payload may itself carry an arena pointer
        // that needs forwarding.  Visit it via the standard
        // dispatch — visitValue will route to the correct fwd*.
        // This produces transitive worklist additions which drain
        // processes later.
        visitValue(*newCell);
    }
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
    // Day 3 Step 2: move standalone cells FIRST so subsequent
    // visitSlot lookups find the forwarding entries.  Mutates
    // standaloneCellRoots() in place.
    mv.walkStandaloneCells();
    // Step 3 (deferred): pre-walk Bindings to record entry
    // addresses for post-drain pending-slot resolution.  Until
    // Step 3 lands, Tag::Slot pointers into Bindings entries may
    // dangle after swap.  runMajorScavenge stays caller-invoked
    // only — see major_scavenge.hh class docstring.
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
