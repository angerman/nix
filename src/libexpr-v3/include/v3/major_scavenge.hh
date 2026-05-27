#pragma once
/// @file
/// Stage 6 production GC — Major Scavenger (Cheney semispace
/// over the v3 tenured arena).
///
/// Per `lode/STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` Option A +
/// `lode/STAGE_6_IMPLEMENTATION_GUIDE_2026-05-27.md` Day 2.
///
/// ## Design
///
/// The MajorScavenger mirrors gc.cc's nursery `Scavenger` but
/// operates between the arena's `active_` and `backup_` regions
/// (per Stage 6 Day 1 + Day 2.1):
///
///   1. Walk all roots via `walkAllV3Roots(vm, mv)`.
///   2. For each visited pointer slot, classify via `arena.regionOf`:
///      * External — libc/Boehm/nursery/stack, skip
///      * Backup — already copied this scavenge, look up forwarding
///      * Active — needs forwarding: copy to backup, register
///        forwarding entry, enqueue for transitive walk, rewrite slot
///   3. Drain the worklist transitively (walk each copied cell's
///      pointer fields, forwarding as needed).
///   4. (Caller) `arena.swapRegions()`: active ↔ backup.
///   5. (Caller) Free the old-active blocks (now `backup_`).
///
/// ## Why this is a separate class from `gc.cc::Scavenger`
///
/// The nursery Scavenger has Phase D + Phase E nursery semantics
/// (`n.inYoung(c)`, `n.inActiveSurvivor(c)`, age-1 promotion to
/// survivor pool, age-2 promotion to tenured).  Major scavenge
/// doesn't have nursery concepts — it operates entirely within
/// the arena.  Mixing the two classes would muddy both
/// abstractions.
///
/// The per-type WALK functions (visit a Closure's fields, a
/// Bindings' entries, etc.) are structurally identical between
/// the two scavengers.  In the future, both could share an
/// abstract `Walker` interface.  For Day 2.2 we duplicate the
/// walk logic locally; abstraction is a follow-up optimization.
///
/// ## Cost when not invoked
///
/// Zero.  The MajorScavenger is constructed only by code that
/// explicitly invokes a major scavenge (no automatic firing in
/// this commit).  The class definition itself is header-only +
/// the move_gc.cc implementations are not linked unless called.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/precise_root.hh"
#include "v3/value.hh"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix::v3 {

struct VMState;
class Arena;
struct Closure;
struct Thunk;
struct Bindings;
struct ListVec;
struct ValuePair;

/// MajorScavenger — Cheney semispace over the v3 tenured arena.
///
/// Day-2.2 status: framework + per-type forwarding implemented;
/// caller-invoked only (no automatic dispatch-loop trigger yet —
/// that's Day 3).  Run via `runMajorScavenge(vm)` for the
/// canonical end-to-end path; standalone test access via the
/// public visit*/fwd* on a constructed instance.
class MajorScavenger : public RootVisitor
{
public:
    explicit MajorScavenger(Arena & arena) noexcept;

    // RootVisitor interface — called via walkAllV3Roots.  Each
    // visitor invocation classifies + forwards + rewrites the
    // slot if it points into active_.
    void visitClosure (Closure   * & slot) override;
    void visitThunk   (Thunk     * & slot) override;
    void visitBindings(Bindings  * & slot) override;
    void visitList    (ListVec   * & slot) override;
    void visitPair    (ValuePair * & slot) override;
    void visitSlot    (Value     * & slot) override;
    void visitString  (const char * & s) noexcept override;
    void visitPath    (const char * & s) noexcept override;

    /// After the root walk, drain the worklist of copied cells.
    /// Each copied cell's pointer fields are visited transitively;
    /// new active→backup edges get forwarded the same way.
    void drain() noexcept;

    /// Stage 6 Day 3 Step 2: forward standalone allocValue cells.
    /// Mutates `standaloneCellRoots()` so each entry pointing into
    /// active_ is moved to backup_ and the registry entry updated
    /// to the new address.  Populates `forwardingCell_` so
    /// subsequent visitSlot calls can resolve Tag::Slot pointers
    /// that aimed at the moved cells.
    ///
    /// Must be called BEFORE `walkAllV3Roots(vm, mv)` so the
    /// subsequent root walk dereferences the NEW cell addresses
    /// directly.  Cells' payloads are visited via the standard
    /// root walk (visitor.visitValue(*newCell)).
    void walkStandaloneCells() noexcept;

    /// Stage 6 Day 3 Steps 3 + 6: resolve pending Tag::Slot
    /// pointers deferred at visit time.  Two target shapes per
    /// Tag::Slot design:
    ///
    ///   (a) Inside `Bindings::entries[i].value` — owning Bindings
    ///       found by byte-range search through forwardingBindings_,
    ///       then offset-forward.
    ///   (b) Standalone `Alloc::allocValue()` cell — copied on
    ///       discovery via `fwdCell`, registered in forwardingCell_.
    ///       `fwdCell`'s `visitValue(*newCell)` may add gray work
    ///       (transitively reached cells) or more pendingSlots_;
    ///       the method's outer while-loop drains both per iteration
    ///       and terminates when no new work is generated.
    ///
    /// Must be called AFTER an initial drain so forwardingBindings_
    /// is complete for case (a), and BEFORE swapRegions so the OLD
    /// container memory is still readable for byte-range probing.
    ///
    /// O(pending_slots × forwardingBindings_count) worst case for
    /// case (a) per iteration.  Standalone (b) is O(1) per call.
    /// Total bounded by total Tag::Slot uses × per-slot cost; on
    /// hello.drvPath ~hundred slots, HNE ~thousand.  Sorted byte-
    /// range search is a future optimization if production
    /// cadence requires.
    void resolvePendingSlots() noexcept;

    /// Statistics, captured during the scavenge.
    struct Stats {
        uint64_t closuresCopied = 0;
        uint64_t thunksCopied   = 0;
        uint64_t bindingsCopied = 0;
        uint64_t listsCopied    = 0;
        uint64_t pairsCopied    = 0;
        uint64_t charsCopied    = 0;  // Day 4: strings + paths
        uint64_t bytesCopied    = 0;
        uint64_t slotsFollowed  = 0;
    };
    const Stats & stats() const noexcept { return stats_; }

private:
    Arena & arena_;

    /// Stage 6 Day 3 Step 1: typed forwarding tables.  Replaces
    /// the Day-2.2 type-erased `forwarding_` with per-type maps
    /// so post-drain slot resolution (per Day-3 analysis §"Step 3")
    /// can iterate Bindings-only forwardings.
    ///
    /// Each fwd*(p) consults / inserts its typed map.  Cycle
    /// traversal + double-copy elimination work identically to
    /// Day 2.2.
    std::unordered_map<Closure   *, Closure   *> forwardingClosure_;
    std::unordered_map<Thunk     *, Thunk     *> forwardingThunk_;
    std::unordered_map<Bindings  *, Bindings  *> forwardingBindings_;
    std::unordered_map<ListVec   *, ListVec   *> forwardingList_;
    std::unordered_map<ValuePair *, ValuePair *> forwardingPair_;

    /// Stage 6 Day 3 Step 1: standalone-cell forwarding.  Populated
    /// by Step 2's `walkStandaloneCells` AND Step 6's fwdCell
    /// discovery (resolvePendingSlots fallback for unregistered
    /// allocValue cells).
    std::unordered_map<Value *,    Value *>    forwardingCell_;

    /// Stage 6 Day 4: string/path buffer forwarding.  Tag::String
    /// + Tag::Path carry `const char *` buffers from
    /// `Alloc::allocChars` (alloc.hh:1208), allocated in the v3
    /// arena alongside typed cells.  Without forwarding, the
    /// buffers dangle after swap+free.  `fwdChars` copies on
    /// discovery and re-keys `stringContextSideTable()` entries
    /// (alloc.hh:2374) to the NEW buffer pointer.
    std::unordered_map<const char *, const char *> forwardingChars_;

    /// Worklist of copied cells whose fields need transitive
    /// forwarding.  Stored as (ptr, kind) pairs; the kind
    /// determines which walk* function processes it in drain().
    enum Kind : uint8_t {
        KClosure  = 0,
        KThunk    = 1,
        KBindings = 2,
        KList     = 3,
        KPair     = 4,
    };
    struct Gray { void * ptr; Kind kind; };
    std::vector<Gray> worklist_;

    /// Cells whose Tag::Slot deref we've followed; dedup against
    /// multiple Tag::Slot Values aliasing the same Value cell.
    std::unordered_set<Value *> cellsFollowed_;

    /// Day 3 Step 3: pending Tag::Slot pointers awaiting post-
    /// drain Bindings-owner resolution.  Each entry is the ADDRESS
    /// of a slot field inside a copied (backup_-resident) cell.
    /// resolvePendingSlots iterates this, finds the owning OLD
    /// Bindings via byte-range search in forwardingBindings_, and
    /// offset-forwards the slot value.
    std::vector<Value **> pendingSlots_;

    /// Day 4: sorted index of forwardingBindings_ entries (by OLD
    /// address) for O(log B) byte-range lookup during
    /// resolvePendingSlots.  Naive O(P × B) linear scan hangs on
    /// HNE-shape workloads (B ~ 500K, P ~ million, scan dominates
    /// the sample profile entirely).  Rebuilt whenever
    /// forwardingBindings_ grows.
    struct BindingsRange {
        Bindings * oldP;
        Bindings * newP;
        uint32_t   bytes;  // sizeof(Bindings) + size*sizeof(Entry)
    };
    std::vector<BindingsRange> sortedBindings_;
    size_t                     sortedBindingsCount_ = 0;

    Stats stats_;

    // -- per-type forwarders (no recursion; just copy + queue) ----
    Closure   * fwdClosure (Closure   * c);
    Thunk     * fwdThunk   (Thunk     * t);
    Bindings  * fwdBindings(Bindings  * b);
    ListVec   * fwdList    (ListVec   * l);
    ValuePair * fwdPair    (ValuePair * p);

    /// Day 3 Step 6: discover-and-copy a standalone allocValue cell.
    /// Used when a Tag::Slot pointer targets a Value cell that is
    /// NOT inside any forwarded Bindings (the only other arena-resident
    /// Value-slot carrier per the Tag::Slot design semantics) and is
    /// NOT registered in `standaloneCellRoots()` (the registry is in
    /// fact unused in production — `Alloc::allocValue()` does not
    /// push to it).
    ///
    /// Unlike per-type fwd* (which enqueue gray work for `drain()` to
    /// process later), `fwdCell` walks the cell's content INLINE via
    /// `visitValue(*newCell)` since cells are sized exactly
    /// `sizeof(Value)` — the gray-work optimisation buys nothing.  The
    /// inline walk may add more gray work (transitively reached cells)
    /// or more pending slots; the caller (`resolvePendingSlots`'s
    /// fixed-point loop) handles that.
    Value     * fwdCell    (Value     * c) noexcept;

    /// Stage 6 Day 4: forward an arena-allocated string/path buffer.
    /// Returns the NEW backup-resident address for `p`; nullptr if
    /// `p` is null; `p` unchanged if not in active_ (external /
    /// already-backup).  Uses `strlen` for length — v3's allocChars
    /// convention is null-terminated (alloc.hh:1205-1207).  Moves
    /// the matching `stringContextSideTable()` entry (if any) to
    /// the new key.
    const char * fwdChars  (const char * p) noexcept;

    /// Day 4: rebuild sortedBindings_ index from forwardingBindings_.
    void rebuildSortedBindings() noexcept;

    // -- per-type field walkers (called from drain) --------------
    void walkClosure (Closure   * c) noexcept;
    void walkThunk   (Thunk     * t) noexcept;
    void walkBindings(Bindings  * b) noexcept;
    void walkList    (ListVec   * l) noexcept;
    void walkPair    (ValuePair * p) noexcept;
};

/// Run a major scavenge: walk roots from `vm`, copy live arena
/// cells from active_ → backup_, swap regions, free the old
/// active (now backup) blocks.  Caller-invoked only (no automatic
/// trigger in Day 2.2; Day 3 will wire it into dispatch-loop
/// safe-points).
///
/// Stats are stderr-printed under `NIX_VM_STATS=1`.
///
/// Pre-conditions:
///   - `vm` is at a safe point (dispatch-loop top, no nested VMState).
///   - Caller has ensured no other in-flight scavenge.
///   - `vm.frames` `vm.valueStack` `vm.withStack` are consistent
///     with `ip` synced into the top frame.
void runMajorScavenge(VMState & vm) noexcept;

} // namespace nix::v3
