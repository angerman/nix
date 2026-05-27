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

#include <algorithm>
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

Value * MajorScavenger::fwdCell(Value * c) noexcept
{
    if (!c) return nullptr;
    if (!arena_.inActive(c)) return c;  // External / backup → leave
    auto it = forwardingCell_.find(c);
    if (it != forwardingCell_.end()) return it->second;

    Value * newC = static_cast<Value *>(
        arena_.allocInBackup(sizeof(Value)));
    *newC = *c;  // memcpy-equivalent; Value is trivially copyable
    forwardingCell_.emplace(c, newC);
    // Walk the cell's content INLINE.  No gray-work optimisation
    // for cells (sizeof(Value) — too small to amortise queueing).
    // visitValue may add gray Closure/Thunk/etc. work (drained
    // by the caller's fixed-point loop) or more pendingSlots_
    // (processed by subsequent resolvePendingSlots iterations).
    visitValue(*newC);
    return newC;
}

const char * MajorScavenger::fwdChars(const char * p) noexcept
{
    if (!p) return nullptr;
    // arena_.inActive takes a non-const void *; const_cast is safe
    // because inActive only reads (block range checks).
    if (!arena_.inActive(const_cast<char *>(p))) return p;
    auto it = forwardingChars_.find(p);
    if (it != forwardingChars_.end()) return it->second;

    // Length via strlen — allocChars's contract is null-terminated
    // (alloc.hh:1205-1207).  Include the terminating null byte in
    // the copy so callers that rely on string_view(p) (no explicit
    // length) continue to work.
    const size_t n = std::strlen(p);
    char * newP = static_cast<char *>(arena_.allocInBackup(n + 1));
    std::memcpy(newP, p, n + 1);
    forwardingChars_.emplace(p, newP);

    // String context side-table re-key.  stringContextSideTable
    // (alloc.hh:2374) is `unordered_map<const char *, vector<string>>`
    // keyed by the buffer address; after forwarding we need the
    // NEW address to retrieve the context.  Move the entry; the
    // OLD key will be unreachable post swap+free.
    auto & tbl = stringContextSideTable();
    auto cIt = tbl.find(p);
    if (cIt != tbl.end()) {
        tbl[newP] = std::move(cIt->second);
        tbl.erase(cIt);
    }

    ++stats_.charsCopied;
    stats_.bytesCopied += n + 1;
    return newP;
}

void MajorScavenger::visitString(const char * & s) noexcept
{
    s = fwdChars(s);
}

void MajorScavenger::visitPath(const char * & s) noexcept
{
    s = fwdChars(s);
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
    // Day 3 Steps 2/3/6: rewrite a Tag::Slot pointer to its NEW
    // post-scavenge target address.  Three cases:
    //
    // Case 1 — slot already forwarded (we've copied the cell in a
    // prior visitSlot or fwdCell call): direct lookup, rewrite.
    //
    // Case 2 — slot points into active_ but not yet forwarded.
    // Per Tag::Slot design (vm.cc:4618 / 8783 / 8823; emit.cc:968,
    // 1091, 1356; lower.cc:390 / 2889), the target is either
    // inside a Bindings::entries[i].value or a standalone
    // allocValue cell.  We can't reliably classify here at visit
    // time because some Bindings may not yet be forwarded.  Defer
    // both shapes to post-drain `resolvePendingSlots`.
    //
    // Case 3 — slot points at backup or external memory: leave.
    if (!slot) return;
    auto it = forwardingCell_.find(slot);
    if (it != forwardingCell_.end()) {
        // Case 1: already-forwarded standalone cell.
        slot = it->second;
    } else if (arena_.inActive(slot)) {
        // Case 2: defer classification + rewrite to post-drain.
        pendingSlots_.push_back(&slot);
        // Walk the OLD cell's content now while it's still
        // readable — forwards any nested arena pointers
        // (typed cells like Closure/Thunk reached through this
        // slot's payload).  The OLD address is valid until
        // arena.freeBackupBlocks fires after swapRegions.
        if (!cellsFollowed_.insert(slot).second) return;
        ++stats_.slotsFollowed;
        visitValue(*slot);
        return;
    }
    // For all other cases (Case 3 plus Case 1's post-forward): walk.
    if (!slot) return;
    if (!cellsFollowed_.insert(slot).second) return;
    ++stats_.slotsFollowed;
    visitValue(*slot);
}

// Day 4: rebuild sortedBindings_ index from forwardingBindings_.
// Called by resolvePendingSlots at the start of each iteration if
// forwardingBindings_ has grown since the last sort.  Sorted by
// OLD pointer so binary search can locate the owning Bindings in
// O(log B) instead of the naive O(B) linear scan that hung HNE.
void MajorScavenger::rebuildSortedBindings() noexcept
{
    sortedBindings_.clear();
    sortedBindings_.reserve(forwardingBindings_.size());
    for (auto & kv : forwardingBindings_) {
        Bindings * oldB = kv.first;
        Bindings * newB = kv.second;
        const size_t bytes = sizeof(Bindings)
            + sizeof(Bindings::Entry) * oldB->size;
        sortedBindings_.push_back({oldB, newB, static_cast<uint32_t>(bytes)});
    }
    std::sort(sortedBindings_.begin(), sortedBindings_.end(),
        [](const BindingsRange & a, const BindingsRange & b) {
            return reinterpret_cast<uintptr_t>(a.oldP)
                 < reinterpret_cast<uintptr_t>(b.oldP);
        });
}

void MajorScavenger::resolvePendingSlots() noexcept
{
    // Day 3 Step 3 + Step 6: each pending slot points at a Value
    // cell in active_.  Per Tag::Slot design semantics (vm.cc:4618 /
    // 8783 / 8823; emit.cc:968 / 1091 / 1356; lower.cc:390 / 2889),
    // the target is one of exactly two shapes:
    //
    //   (a) Inside `Bindings::entries[i].value` — produced by
    //       OP_REC_BINDING_SLOT_REF for rec-attrset entries.
    //   (b) A standalone `Alloc::allocValue()` cell — produced by
    //       OP_REC_SLOT_PUBLISH and OP_THUNK_SET_LOCAL_THROUGH_CELL.
    //
    // Resolution:
    //   (a) → byte-range search through forwardingBindings_,
    //         offset-forward to the NEW Bindings' entry.
    //   (b) → fwdCell (discover + copy + register).
    //
    // Why not a per-type byte-range search for Closure / Thunk
    // (the Day-4 evening attempt): cells do NOT live inside
    // Closure::upvalues[] or Thunk::tail[].  Closure / Thunk fields
    // are walked by their own walk*() and reached transitively via
    // fwd*() — they are never Tag::Slot targets.  Adding such
    // searches risks false-positive matches against memory
    // coincidences (a Value-sized region inside a Closure header
    // happening to coincide with a slot's byte offset), which on
    // HNE hung the major scavenge.  Falsified 2026-05-27.
    //
    // fwdCell's visitValue may add gray work (transitively reached
    // typed cells) or new pendingSlots_ (Tag::Slot inside the copied
    // cell).  Outer loop drains both per iteration; terminates
    // because forwardingCell_/forwardingBindings_/etc. dedup ensures
    // each arena cell is processed at most once.
    while (!pendingSlots_.empty()) {
        // Day 4: rebuild sorted index if forwardingBindings_ has
        // grown since the last sort (cheap O(B log B) re-sort once
        // per outer iteration; replaces O(P × B) linear scan
        // per slot per iteration).
        if (forwardingBindings_.size() != sortedBindingsCount_) {
            rebuildSortedBindings();
            sortedBindingsCount_ = forwardingBindings_.size();
        }

        std::vector<Value **> current = std::move(pendingSlots_);
        pendingSlots_.clear();
        bool didDiscover = false;

        for (Value ** slotAddr : current) {
            if (!slotAddr) continue;
            Value * oldSlot = *slotAddr;
            if (!oldSlot) continue;

            // Case (a-prime): already-forwarded standalone cell.
            // (A previous iteration may have moved it via fwdCell.)
            auto fIt = forwardingCell_.find(oldSlot);
            if (fIt != forwardingCell_.end()) {
                *slotAddr = fIt->second;
                continue;
            }

            // Case (a): Bindings-resident slot.  Binary search
            // (sorted by OLD pointer) for the owning Bindings.
            const char * slotCp = reinterpret_cast<const char *>(oldSlot);
            bool resolved = false;
            // upper_bound finds the first range with oldP > slotCp.
            // We want the range with oldP <= slotCp (and slotCp <
            // oldP + bytes), so back up one.
            auto upper = std::upper_bound(
                sortedBindings_.begin(), sortedBindings_.end(),
                slotCp,
                [](const char * cp, const BindingsRange & r) {
                    return cp < reinterpret_cast<const char *>(r.oldP);
                });
            if (upper != sortedBindings_.begin()) {
                auto & r = *(upper - 1);
                const char * oldBcp =
                    reinterpret_cast<const char *>(r.oldP);
                if (slotCp >= oldBcp && slotCp < oldBcp + r.bytes) {
                    ptrdiff_t offset = slotCp - oldBcp;
                    *slotAddr = reinterpret_cast<Value *>(
                        reinterpret_cast<char *>(r.newP) + offset);
                    resolved = true;
                }
            }
            if (resolved) continue;

            // Case (b): standalone allocValue cell — fwdCell.
            if (arena_.inActive(oldSlot)) {
                *slotAddr = fwdCell(oldSlot);
                didDiscover = true;
            }
            // else: external pointer (regionOf misclassification
            // would be a bug elsewhere; leave as-is).
        }

        // visitValue calls inside fwdCell may have enqueued gray
        // work (Closure / Thunk / Bindings / List / Pair) — drain.
        if (didDiscover) drain();
        // pendingSlots_ may have grown during this iteration's
        // fwdCell→visitValue chain; the outer while-loop handles it.
    }
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
    // Day 3 Step 5: forward Thunk::cell + Thunk::shapeCell.
    //
    // Two cases per the Day-3 analysis:
    //  (a) cellContainer != null — cell points INSIDE a Bindings::
    //      entries[i].value.  Offset-forward: compute offset within
    //      the OLD Bindings, look up the new Bindings via
    //      forwardingBindings_, compute new cell = new Bindings +
    //      offset.
    //  (b) cellContainer == null — cell is a standalone allocValue
    //      cell.  Direct lookup in forwardingCell_ (populated by
    //      Step 2's walkStandaloneCells).
    //
    // We MUST forward cellContainer FIRST so case (a) uses the
    // up-to-date new Bindings address.
    Bindings * oldCellContainer = t->cellContainer;
    if (t->cellContainer) t->cellContainer = fwdBindings(t->cellContainer);

    auto forwardCellPtr = [&](Value * & cellSlot) {
        if (!cellSlot) return;
        if (oldCellContainer) {
            // Case (a): offset-forward via cellContainer.
            auto it = forwardingBindings_.find(oldCellContainer);
            if (it != forwardingBindings_.end()) {
                Bindings * newB = it->second;
                ptrdiff_t offset =
                    reinterpret_cast<char *>(cellSlot)
                    - reinterpret_cast<char *>(oldCellContainer);
                cellSlot = reinterpret_cast<Value *>(
                    reinterpret_cast<char *>(newB) + offset);
            }
            // else: cellContainer wasn't in active (already moved
            // or external) — leave cell as-is.  Should be rare;
            // implies the Thunk's cellContainer was external while
            // its cell pointed into active — inconsistent state.
        } else {
            // Case (b): standalone — discover-and-copy.
            //
            // Day 3 Step 6 fix: previously this looked up only the
            // forwardingCell_ table populated by Step 2's
            // walkStandaloneCells.  But `Alloc::allocValue()`
            // (alloc.hh:999) doesn't push to `standaloneCellRoots()`
            // — the registry is empty in production.  Cells from
            // OP_THUNK_SET_LOCAL_THROUGH_CELL (vm.cc:8823) attached
            // via cellOwnRecordSet are reachable only via the
            // Thunk::cell pointer here.  Use fwdCell to copy on
            // discovery; subsequent fwdCell calls see the forwarding
            // entry and dedup.
            cellSlot = fwdCell(cellSlot);
        }
    };
    forwardCellPtr(t->cell);
    forwardCellPtr(t->shapeCell);

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
    // Day 3 Step 3 wired: post-drain pending-slot resolution.
    // Tag::Slot pointers into Bindings::entries[i].value are now
    // forwarded via byte-range search + offset arithmetic.  Must
    // run BEFORE swapRegions+freeBackupBlocks so oldBindings
    // remain readable (we read oldB->size for the byte-range).
    walkAllV3Roots(vm, mv);
    mv.drain();
    mv.resolvePendingSlots();
    arena.swapRegions();
    arena.freeBackupBlocks();

    // Status banner under NIX_VM_STATS=1.
    static const bool s_stats = std::getenv("NIX_VM_STATS") != nullptr;
    if (s_stats) {
        const auto & s = mv.stats();
        std::fprintf(stderr,
            "v3-direct major-scavenge: closures=%llu thunks=%llu bindings=%llu "
            "lists=%llu pairs=%llu chars=%llu bytesCopied=%.1fMB "
            "slotsFollowed=%llu\n",
            (unsigned long long)s.closuresCopied,
            (unsigned long long)s.thunksCopied,
            (unsigned long long)s.bindingsCopied,
            (unsigned long long)s.listsCopied,
            (unsigned long long)s.pairsCopied,
            (unsigned long long)s.charsCopied,
            s.bytesCopied / 1e6,
            (unsigned long long)s.slotsFollowed);
    }
}

} // namespace nix::v3
