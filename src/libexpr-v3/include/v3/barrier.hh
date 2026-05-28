#pragma once
/// @file
/// v3 generational GC write barriers (Stage 3 Phase D).
///
/// Tracks inter-generational pointer writes: a write that puts a
/// nursery-resident object into a tenured container.  Without
/// barriers, the scavenger would either need to walk every tenured
/// object on each scavenge (O(tenured-size); the Phase C
/// implementation today) or miss the writes and corrupt the heap.
///
/// Design: per `lode/NURSERY_PHASE_D_DECISION_2026-05-21.md`.
/// Path α — thread-local dirty-container list.  No per-container
/// dirty bits (zero size overhead on Bindings/Pair/Closure/ListVec).
/// One field add: `Thunk::cellContainer` (so OP_RETURN's cell-write
/// can find its containing Bindings).
///
/// Workflow:
///   1. Caller about to do an inter-gen write (e.g. `b->entries[i]
///      .value = v` where `b` is tenured and `v` has a nursery payload)
///      goes through a barrier helper (e.g. `bindingsSetValue`).
///   2. Helper performs the write, then — gated on
///      `phaseDActive()` (cached env-var check at first use) — checks
///      `isNurseryPayload(v)`.
///   3. On hit, the helper appends a `DirtyEntry{kind, ptr}` to the
///      thread-local `tlDirtyContainers` vector.
///   4. On next scavenge, after walking the natural roots, the
///      scavenger drains `tlDirtyContainers` (visit each entry via
///      walkBindings / walkPair / walkThunk; existing `walked` set
///      dedups repeated containers).
///   5. After scavenge, the list is cleared.
///
/// Standalone cells (Thunk::cell pointing into an allocValue() cell,
/// not into a Bindings entry) use a separate `tlStandaloneCells`
/// registry (the Phase D decision §2.4 mechanism).
///
/// The barrier helpers are header-inline so the no-op branch
/// (phase D off) folds to a single predicted-not-taken branch.
/// The dirty-list append is a single push_back when on.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/alloc.hh"   // Bindings, Thunk, ListVec
#include "v3/closure.hh" // Thunk full layout
#include "v3/nursery.hh" // threadNursery() / contains()

#include <cstdint>
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// Dirty-container list
// ---------------------------------------------------------------------------

/// Kinds of container that can land on the dirty-list.  Used by the
/// scavenger drain step to dispatch to the right per-class walker.
enum class DirtyKind : uint8_t {
    Bindings = 0,
    Pair     = 1,  ///< ValuePair (Tag::App memo target)
    Thunk    = 2,  ///< Thunk header (Thunk::evaluated or Thunk::tail mutation)
    Closure  = 3,  ///< Closure (upvalues[] or capturedWiths mutation)
    List     = 4,  ///< ListVec (elems[] mutation; mostly write-once at build)
};

/// One dirty-list entry: which kind + raw container pointer.
/// Constructed at barrier-hit time; consumed by the scavenger's
/// dirty-drain pass.  Two pointer-sized fields → 16 bytes per
/// entry, packs well for the thread-local vector.
struct DirtyEntry {
    DirtyKind   kind;
    void *      ptr;
};

/// Thread-local list of tenured containers that received an
/// inter-gen pointer write since the last scavenge.  Walked + cleared
/// by `scavengeNursery`.
///
/// Defined in `barrier.cc`; referenced via these accessors so
/// header-inline code doesn't need to know the variable's
/// linkage.
std::vector<DirtyEntry> & dirtyContainers() noexcept;

/// Thread-local list of standalone `Value *` cells (i.e.
/// `Thunk::cell` pointers when `Thunk::cellContainer == nullptr`)
/// that received an inter-gen pointer write.  Walked + cleared by
/// `scavengeNursery` immediately before / after the dirty-list
/// drain.
std::vector<Value *> & standaloneCellRoots() noexcept;

/// Stage 6 Phase 3.7: registry of LambdaDescriptor::cachedSingletonClosure
/// pointers (libc-resident locations holding arena Closure pointers).
/// Mark phase walks this to keep cached singleton closures alive
/// across arena sweeps.
std::vector<Closure **> & singletonClosureRegistry() noexcept;

// ---------------------------------------------------------------------------
// Fast-path gate
// ---------------------------------------------------------------------------

/// Process-wide cached `NIX_V3_NURSERY != "0" && !empty()`.  Reads
/// the env var once at static-init time (before main).  When phase
/// D is off (the default), every barrier helper compiles to: do
/// the write + branch on this bool + fall through.  Predicted-not-
/// taken in production.
///
/// #767 (2026-05-22): exposed as an `inline const bool` so the
/// compiler can hoist the load across multiple barrier writes in
/// the same caller (e.g. `bindingsSetEntry` followed immediately
/// by another barrier emit).  The prior `static const bool`
/// function-local form forced a magic-static guard load on every
/// call.  Profile data (#765, hello.drvPath) showed the function
/// at ~2% self-time despite being a flag check — the guard load
/// was the cost.
namespace detail {
extern const bool g_phaseDActive;
}

[[gnu::always_inline]] inline bool phaseDActive() noexcept
{
    return detail::g_phaseDActive;
}

// ---------------------------------------------------------------------------
// isNurseryPayload — does this Value's payload point into the nursery?
// ---------------------------------------------------------------------------

/// Returns true iff `v.payload.{closure,thunk,bindings,list,pair,slot}`
/// points into the current thread's nursery.  Returns false for
/// non-pointer Tags (Int / Float / String / Path / Bool / Null /
/// Uninitialized / Blackhole / External / PrimOp / PrimOpApp).
///
/// `n.contains(p)` is an O(1) range check (`p >= base && p < end`).
/// The dispatch on tag is one switch; the compiler folds the
/// non-pointer branches to a constant `false`.  Hot-path cost when
/// called: ~2 compare instructions + 1 pointer-load.
[[gnu::always_inline]] inline bool isNurseryPayload(Value v, const Nursery & n) noexcept
{
    // Explicit case-per-Tag to satisfy -Wswitch-enum.  The compiler
    // folds the constant-false branches.
    switch (v.tag()) {
    case Tag::Closure:   return n.contains(v.payload.closure);
    case Tag::Thunk:     return n.contains(v.payload.thunk);
    case Tag::Attrs:     return n.contains(v.payload.bindings);
    case Tag::List:      return n.contains(v.payload.list);
    case Tag::App:       return n.contains(v.payload.pair);
    case Tag::App3:      return n.contains(v.payload.pair);
    case Tag::PrimOpApp: return n.contains(v.payload.pair);
    case Tag::Slot:      return n.contains(v.payload.slot);
    // Non-pointer payloads: scalar / interned-elsewhere / no-payload.
    case Tag::Uninitialized:
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::String:
    case Tag::Path:
    case Tag::PrimOp:
    case Tag::Blackhole:
    case Tag::External:
        return false;
    }
    return false;  // unreachable; silences -Wreturn-type
}

// ---------------------------------------------------------------------------
// Barrier helpers
// ---------------------------------------------------------------------------
//
// Each helper performs ONE primitive write and (under phaseDActive)
// records an inter-gen edge.  Caller replaces a raw assignment with a
// helper call:
//
//     b->entries[i].value = v;          // OLD (no barrier)
//     bindingsSetValue(b, i, v);        // NEW (barriered)
//
// All helpers are `noexcept` and have no error mode — they are
// pure side-effects on already-allocated memory.

/// Write `v` into `b->entries[i].value`.  Append `b` to
/// `dirtyContainers` if the write is inter-gen
/// (b is tenured AND v.payload is nursery).
[[gnu::always_inline]] inline void
bindingsSetValue(Bindings * b, uint32_t i, Value v) noexcept
{
    b->entries[i].value = v;
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        // b must be tenured AND v must carry a nursery payload.
        // Bindings are always tenured today (`Alloc::allocBindings`
        // calls threadArena() directly), so the `!n.contains(b)`
        // check is a defensive doublecheck — cheap.
        if (!n.contains(b) && isNurseryPayload(v, n))
            dirtyContainers().push_back({DirtyKind::Bindings, b});
    }
}

/// Write a full Bindings::Entry `{name, value}` at index i.  Covers
/// the struct-init form (`b->entries[i] = {sym, v}`) that some
/// primop code uses for simultaneous name + value assignment.
/// Semantically equivalent to `bindingsSetValue(b, i, e.value);
/// b->entries[i].name = e.name;`.
[[gnu::always_inline]] inline void
bindingsSetEntry(Bindings * b, uint32_t i, Bindings::Entry e) noexcept
{
    b->entries[i] = e;
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (!n.contains(b) && isNurseryPayload(e.value, n))
            dirtyContainers().push_back({DirtyKind::Bindings, b});
    }
}

/// Write `v` into `p->evaluated` (Tag::App memoization).  Append
/// `p` to `dirtyContainers` if inter-gen.  ValuePair is always
/// tenured (`Alloc::allocPair` calls threadArena()), so we skip the
/// container-residence check in the fast path.
[[gnu::always_inline]] inline void
pairSetEvaluated(ValuePair * p, Value v) noexcept
{
    p->evaluated = v;
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (isNurseryPayload(v, n))
            dirtyContainers().push_back({DirtyKind::Pair, p});
    }
}

/// Write `v` into `t->evaluated` (OP_FORCE result memoization).
/// Thunks are nurseryOrArena-allocated; the barrier fires only when
/// the Thunk is tenured AND v is nursery.  Same-gen writes (nursery
/// Thunk + nursery payload) are zero-overhead in the natural
/// scavenge — the Thunk is reachable and walkThunk visits
/// t->evaluated already.
[[gnu::always_inline]] inline void
thunkSetEvaluated(Thunk * t, Value v) noexcept
{
    t->evaluated = v;
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (!n.contains(t) && isNurseryPayload(v, n))
            dirtyContainers().push_back({DirtyKind::Thunk, t});
    }
}

// ---------------------------------------------------------------------------
// Post-construction batch barriers
// ---------------------------------------------------------------------------
//
// For Closure / Thunk / ListVec, individual element-write barriers
// (à la `bindingsSetValue`) would be costly because:
//   - Closure / Thunk / ListVec are often populated in tight loops
//     (one write per FAM slot during construction).
//   - Adding a check + dirty-list push per element write multiplies
//     per-element cost ~5-10×.
//   - Once constructed, these containers are largely write-once
//     (Closure upvalues immutable post-MAKE; ListVec elems likewise;
//     Thunk tail[] populated at MAKE_THUNK).
//
// Cleaner protocol: at the END of construction, scan the FAM array
// once.  If the container is tenured AND any field carries a nursery
// payload, push ONE DirtyEntry.  Cost: O(N) iteration + at most 1
// push.  Same correctness as per-write barriers for the
// write-once-then-immutable case.
//
// The mid-life mutations that DO happen post-construction
// (Thunk::evaluated, Thunk::cell rewrites) are handled by the
// per-write helpers above (`thunkSetEvaluated`, `cellWrite`).
// Closure-pool recycling (`recycleFakeClo`) returns a Closure that
// the caller IMMEDIATELY re-populates and re-walks; treat that as
// a fresh construction and call the post-construct helper there too.

/// Post-construct barrier for a Closure.  Iterates `upvalues[]` and
/// `capturedWiths` once; if any holds a nursery payload AND the
/// Closure is tenured, push one DirtyEntry.  Returns nothing; the
/// caller's contract is to call this AFTER finishing construction
/// of `c` (i.e., after writing all upvalues + setting capturedWiths
/// + cu + desc).
[[gnu::always_inline]] inline void
closurePostConstructBarrier(Closure * c) noexcept
{
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (n.contains(c)) return;  // nursery closure, no inter-gen
        // Scan upvalues + capturedWiths.  Single break on first nursery
        // payload — one push covers all entries since DirtyKind::Closure
        // re-walks the whole container.
        bool dirty = false;
        for (uint16_t i = 0; i < c->nUpvalues; ++i) {
            if (isNurseryPayload(c->upvalues[i], n)) { dirty = true; break; }
        }
        if (!dirty && c->capturedWiths && n.contains(c->capturedWiths))
            dirty = true;
        if (dirty) dirtyContainers().push_back({DirtyKind::Closure, c});
    }
}

/// Post-construct barrier for a Thunk.  Iterates `tail[]` (Suspended
/// state's captured upvalues) + `suspended.capturedWiths` and pushes
/// a DirtyEntry if any nursery payload + tenured-thunk.  Per-state
/// dispatch: only Suspended-class states have a meaningful FAM tail
/// to walk; Evaluated state's `evaluated` slot is handled by the
/// per-write `thunkSetEvaluated`; Bridge has no v3-payload tail
/// (just bridgeSrc which is TW-side).
[[gnu::always_inline]] inline void
thunkPostConstructBarrier(Thunk * t) noexcept
{
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (n.contains(t)) return;
        bool dirty = false;
        // tail[i] for Suspended / Native / Blackhole carries upvalues.
        // Native / Blackhole are rare; iterating tail is harmless if
        // nUpvalues == 0 (e.g. Bridge).
        for (uint16_t i = 0; i < t->nUpvalues; ++i) {
            if (isNurseryPayload(t->tail[i], n)) { dirty = true; break; }
        }
        // Suspended-capturedWiths.  Native / Bridge use the union for
        // other purposes; check the state byte.
        if (!dirty
            && (t->state == ThunkState::Suspended
                || t->state == ThunkState::Blackhole)
            && t->suspended.capturedWiths
            && n.contains(t->suspended.capturedWiths))
            dirty = true;
        if (dirty) dirtyContainers().push_back({DirtyKind::Thunk, t});
    }
}

/// Post-construct barrier for a ListVec.  Iterates `elems[]` and
/// pushes a DirtyEntry if any nursery payload + tenured list.
[[gnu::always_inline]] inline void
listPostConstructBarrier(ListVec * l) noexcept
{
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (n.contains(l)) return;
        for (uint32_t i = 0; i < l->size; ++i) {
            if (isNurseryPayload(l->elems[i], n)) {
                dirtyContainers().push_back({DirtyKind::List, l});
                break;
            }
        }
    }
}

/// Post-construct barrier for a ValuePair.  Iterates `left`,
/// `right`, and `evaluated` and pushes a DirtyEntry on first
/// inter-gen edge.  ValuePair is always tenured (`Alloc::allocPair`
/// uses threadArena directly), so the `!n.contains(p)` check
/// always falls through — kept as one branch for uniformity.
///
/// Used at Tag::App / Tag::PrimOpApp construction sites where the
/// caller writes `p->left = X; p->right = Y;` after `allocPair()`.
/// The per-write `pairSetEvaluated` helper covers the LATER
/// `evaluated` field memo writes; this batch barrier covers the
/// `left`/`right` initial assignment that pairs need at birth.
[[gnu::always_inline]] inline void
pairPostConstructBarrier(ValuePair * p) noexcept
{
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        // ValuePair always tenured by design (Alloc::allocPair
        // calls threadArena directly); defensive double-check.
        if (n.contains(p)) return;
        if (isNurseryPayload(p->left, n)
            || isNurseryPayload(p->right, n)
            || isNurseryPayload(p->evaluated, n))
        {
            dirtyContainers().push_back({DirtyKind::Pair, p});
        }
    }
}

/// #738 Phase E v0.2 (2026-05-21) — scan all Bindings entries and
/// add to dirty list if any entry holds a nursery payload.  Mirrors
/// the per-entry `bindingsSetValue`/`bindingsSetEntry` barriers but
/// for the post-walk case where the scavenger updated multiple
/// entries during a single walk.
///
/// Cost: O(b->size).  Acceptable in scavenge context where the
/// alternative would be per-entry tracking through the walk's
/// `visitValue`.  Called once per walked Bindings under Phase E.
[[gnu::always_inline]] inline void
bindingsPostConstructBarrier(Bindings * b) noexcept
{
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        // Defensive double-check: Bindings always tenured today.
        if (n.contains(b)) return;
        for (uint32_t i = 0; i < b->size; ++i) {
            if (isNurseryPayload(b->entries[i].value, n)) {
                dirtyContainers().push_back({DirtyKind::Bindings, b});
                return;
            }
        }
    }
}

/// Write `v` through the standalone-cell pointer `cell`, with
/// optional `cellContainer` (the owning Bindings if any).  Cleared
/// at the same site (read-and-zero on Thunk).
///
/// - If cellContainer is non-null: behaves like `bindingsSetValue`
///   — the containing Bindings goes on the dirty list (the cell
///   lives inside one of its entries).
/// - If cellContainer is null AND v is a nursery payload: the cell
///   pointer itself goes on the standalone-cell registry (so
///   scavenge walks it).
[[gnu::always_inline]] inline void
cellWrite(Value * cell, Value v, Bindings * cellContainer) noexcept
{
    *cell = v;
    if (__builtin_expect(phaseDActive(), 0)) [[unlikely]] {
        const Nursery & n = threadNursery();
        if (isNurseryPayload(v, n)) {
            if (cellContainer && !n.contains(cellContainer))
                dirtyContainers().push_back({DirtyKind::Bindings, cellContainer});
            else if (!cellContainer)
                standaloneCellRoots().push_back(cell);
        }
    }
}

} // namespace nix::v3
