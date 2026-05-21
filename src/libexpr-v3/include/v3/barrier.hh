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

// ---------------------------------------------------------------------------
// Fast-path gate
// ---------------------------------------------------------------------------

/// Process-wide cached `NIX_V3_NURSERY != "0" && !empty()`.  Reads
/// the env var once on first call.  When phase D is off, every
/// barrier helper compiles to: do the write + branch on this bool +
/// fall through.  Predicted-not-taken in production.
bool phaseDActive() noexcept;

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
