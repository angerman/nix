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

    /// After the root walk, drain the worklist of copied cells.
    /// Each copied cell's pointer fields are visited transitively;
    /// new active→backup edges get forwarded the same way.
    void drain() noexcept;

    /// Statistics, captured during the scavenge.
    struct Stats {
        uint64_t closuresCopied = 0;
        uint64_t thunksCopied   = 0;
        uint64_t bindingsCopied = 0;
        uint64_t listsCopied    = 0;
        uint64_t pairsCopied    = 0;
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
    /// by Step 2's `walkStandaloneCells` (Day-3 follow-up).  Day-3
    /// Step 1 leaves this empty; Step 2 wires it.
    std::unordered_map<Value *,    Value *>    forwardingCell_;

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

    Stats stats_;

    // -- per-type forwarders (no recursion; just copy + queue) ----
    Closure   * fwdClosure (Closure   * c);
    Thunk     * fwdThunk   (Thunk     * t);
    Bindings  * fwdBindings(Bindings  * b);
    ListVec   * fwdList    (ListVec   * l);
    ValuePair * fwdPair    (ValuePair * p);

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
