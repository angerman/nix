// v3 GC object child-pointer LAYOUT MANIFEST — single source of truth.
//
// The v3 heap object layouts (Closure / Thunk / Bindings / ListVec / ValuePair)
// have their child-pointer positions RE-ENCODED, by hand, in six separate walker
// sites:
//   W1  gc.cc          Scavenger::walk*/fwd*        (HOT nursery scavenge)
//   W2  gc.cc          Auditor::visit*              (cold post-scavenge audit)
//   W3  mark_sweep.cc  MarkSweep::walk*             (HOT gen-major mark)
//   W4  live_trace.cc  LiveTracer::walk*            (cold live-fraction trace)
//   W5  live_trace.cc  BucketTracer::walk*          (cold memory-bucket trace)
//   B   gc.cc          bruteScanSlotIsScalar        (cold raw-word classifier)
//
// Before this header the six agreed only by manual discipline: a layout reshape
// had to be mirrored into every site by hand, and a forgotten field is a silent
// missed-root use-after-free (the PhD-6 class — see src/libexpr-v3/CLAUDE.md §0).
//
// This manifest pins EVERY child-pointer offset + sizeof with `static_assert`,
// so a future layout reshape becomes a COMPILE error rather than a runtime UAF.
// It also provides ONE typed enumerator (`enumerateChildSlots`) over each
// object's child slots, so the cold walkers (W2/W4/W5) and the post-scavenge
// manifest cross-check tripwire enumerate from here instead of re-deriving the
// layout.  The two HOT walkers (W1 scavenger, W3 gen-major mark) stay hand-
// written and byte-identical for codegen reasons; this manifest DOCUMENTS and
// PINS their layout — the `static_assert`s below fire in every translation unit
// that includes this header, so a drifted hot walker cannot compile against a
// stale offset.
//
// SCOPE: "object child-pointer layout" only — the payload slots the GC must
// forward/trace.  The CU/inline-cache (`attrSelectCache`) walks that the hot and
// cold walkers ALSO perform are a side-effect keyed off a descriptor pointer, not
// an object child slot, and stay hand-written in each walker (they are not part
// of any object's layout).
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>   // std::size_t, offsetof
#include <cstdint>

#include "v3/value.hh"    // Value, ValuePair, Tag, tagIsPointer
#include "v3/closure.hh"  // Closure, Thunk, ThunkState, thunkCapturedWiths
#include "v3/alloc.hh"    // Bindings, ListVec, CellType

namespace nix::v3::gclayout {

// ===========================================================================
// PART 1 — offset / size manifest (COMPILE-TIME PINNED)
//
// Every child-pointer offset below is asserted against `offsetof`, and every
// object's `sizeof` against its known constant.  A layout reshape that moves a
// field, changes a header size, or re-strides a FAM will fail exactly one of
// these — forcing a review of all six walker sites + the classifier.
// ===========================================================================

// -- Closure (header 24 B; desc@0 is a CU pointer, NOT a GC child) ----------
//   desc@0 (const LambdaDescriptor*) | capturedWiths@8 (ListVec*) |
//   nUpvalues@16/_pad@18 (scalar) | upvalues[]@24 (FAM Value).
static_assert(sizeof(Closure) == 24, "Closure header must be 24 B (desc 8 + capturedWiths 8 + {nUpvalues,_pad} 8; FAM follows).");
static_assert(offsetof(Closure, desc) == 0);
static_assert(offsetof(Closure, capturedWiths) == 8);
static_assert(offsetof(Closure, nUpvalues) == 16);
static_assert(offsetof(Closure, _pad) == 18);
static_assert(offsetof(Closure, upvalues) == 24, "Closure upvalue FAM must start at sizeof(Closure).");
static_assert(offsetof(Closure, upvalues) == sizeof(Closure));

// -- Thunk (header 24 B; union@16 is desc/fn for non-Evaluated, NOT a GC child)
//   {state,hasWithsSlot,nUpvalues,forces}@0..7 (scalar) | cell@8 (Value*) |
//   union@16 (Evaluated: the cached Value; Suspended/Blackhole: desc pointer;
//   Native: fn pointer) | tail[]@24 (FAM Value; + optional capturedWiths raw
//   ListVec* at tail[nUpvalues] iff hasWithsSlot && state∈{Suspended,Blackhole}).
static_assert(sizeof(Thunk) == 24, "Thunk header must be 24 B (state-word 8 + cell 8 + union 8).");
static_assert(offsetof(Thunk, cell) == 8);
static_assert(offsetof(Thunk, evaluated) == 16, "Thunk union (Evaluated cached Value) must sit at offset 16, right after cell.");
static_assert(offsetof(Thunk, tail) == 24, "Thunk FAM tail must start at sizeof(Thunk).");
static_assert(offsetof(Thunk, tail) == sizeof(Thunk));

// -- Bindings (header 16 B) --------------------------------------------------
//   kind@0/_pad@1/size@4 (scalar) | parent@8 (const Bindings*, non-null iff
//   kind==Chain) | entries[]@16 (16-stride FAM: {SymbolId,PosIdx32}@+0 scalar,
//   value@+8 Value) | MapAttrs aux Value at &entries[size] iff kind==MapAttrs.
static_assert(sizeof(Bindings) == 16, "Bindings header must be 16 B (kind/pad/size 8 + parent 8).");
static_assert(offsetof(Bindings, kind) == 0);
static_assert(offsetof(Bindings, size) == 4);
static_assert(offsetof(Bindings, parent) == 8);
static_assert(offsetof(Bindings, entries) == 16, "Bindings entry FAM must start at sizeof(Bindings).");
static_assert(offsetof(Bindings, entries) == sizeof(Bindings));
static_assert(sizeof(Bindings::Entry) == 16, "Bindings::Entry must be 16 B (SymbolId 4 + PosIdx32 4 + Value 8).");
static_assert(offsetof(Bindings::Entry, value) == 8, "Bindings::Entry value (the only child pointer) must sit at +8.");

// -- ListVec (header 8 B) ----------------------------------------------------
//   size@0/_pad@4 (scalar) | elems[]@8 (FAM Value).
static_assert(sizeof(ListVec) == 8, "ListVec header must be 8 B (size 4 + _pad 4).");
static_assert(offsetof(ListVec, elems) == 8, "ListVec elem FAM must start at sizeof(ListVec).");
static_assert(offsetof(ListVec, elems) == sizeof(ListVec));

// -- ValuePair (32 B; all four slots are Values) ----------------------------
//   left@0 | right@8 | evaluated@16 | third@24  (all Value).
static_assert(sizeof(ValuePair) == 32, "ValuePair must be 4x8B = 32 B.");
static_assert(offsetof(ValuePair, left) == 0);
static_assert(offsetof(ValuePair, right) == 8);
static_assert(offsetof(ValuePair, evaluated) == 16);
static_assert(offsetof(ValuePair, third) == 24);

// The Thunk optional withs slot lives at tail[nUpvalues]; its byte offset is
// sizeof(Thunk) + nUpvalues*sizeof(Value) — asserted consistent with the tail.
inline constexpr std::size_t thunkWithsSlotOffset(std::uint16_t nUpvalues) noexcept
{
    return sizeof(Thunk) + std::size_t(nUpvalues) * sizeof(Value);
}
// The Bindings MapAttrs aux Value lives at &entries[size]; its byte offset is
// sizeof(Bindings) + size*sizeof(Entry) — mirrors Bindings::mapAttrsAux().
inline constexpr std::size_t bindingsMapAttrsAuxOffset(std::uint32_t size) noexcept
{
    return sizeof(Bindings) + std::size_t(size) * sizeof(Bindings::Entry);
}

// ===========================================================================
// PART 2 — raw-word SCALAR classifier (consumed by B: bruteScanSlotIsScalar)
//
// `slotIsScalar(ct, off, base)` is true iff the byte-offset `off` within a
// tenured cell of CellType `ct` lies in a provably-NON-POINTER scalar/metadata
// slot.  The conservative post-scavenge BRUTE raw-word scan SKIPS these so a
// scalar word whose 64-bit value coincidentally lands in the nursery's
// ASLR-varying range is not reported as a false-positive missed root (the #34
// flake).  A pointer never lives in a scalar slot, so skipping cannot hide a
// real missed root — the precise AUDIT + manifest tripwire stay the reachability
// checks.
//
// `base` (the cell start) is OPTIONAL: it is only needed to identify the
// Bindings MapAttrs aux Value (a POINTER slot at &entries[size] whose offset,
// modulo the entry stride, otherwise looks like an entry's scalar name/pos
// half).  When `base==nullptr` the pure-offset rules apply, which are correct
// for Sorted/Chain Bindings and for every other CellType.  All offsets derive
// from `sizeof`/`offsetof` above, so the classifier auto-tracks the layout.
// ===========================================================================
inline bool slotIsScalar(CellType ct, std::size_t off, const void * base = nullptr) noexcept
{
    switch (ct) {
    case CellType::Bindings: {
        // Defect #3 fix (2026-08): the MapAttrs aux Value at &entries[size] is a
        // POINTER slot.  Its offset is 16 + 16*size, so (off-16)%16 == 0 — which
        // the entry rule below would misclassify as an entry's {SymbolId,PosIdx32}
        // scalar half.  Recover the true classification from the live object.
        if (base) {
            const Bindings * b = static_cast<const Bindings *>(base);
            if (b->isMapAttrs() && off == bindingsMapAttrsAuxOffset(b->size))
                return false;
        }
        // header: [0, parent) = kind/pad/size SCALAR; parent@[8,16) POINTER.
        if (off < offsetof(Bindings, parent)) return true;
        if (off < sizeof(Bindings)) return false;
        // entry stride 16: {SymbolId,PosIdx32}@[+0,+8) SCALAR; value@[+8,+16) POINTER.
        const std::size_t rel = (off - sizeof(Bindings)) % sizeof(Bindings::Entry);
        return rel < offsetof(Bindings::Entry, value);
    }
    case CellType::Closure:
        // desc@0 + capturedWiths@8 POINTERS; {nUpvalues,_pad}@[16,24) SCALAR;
        // upvalues[]@24+ Values (pointer-capable).
        return off >= offsetof(Closure, nUpvalues) && off < sizeof(Closure);
    case CellType::Thunk:
        // {state,hasWithsSlot,nUpvalues,forces}@[0,8) SCALAR; cell@8, union@16,
        // tail@24+ all pointer-capable.
        return off < offsetof(Thunk, cell);
    case CellType::List:
        // {size,_pad}@[0,8) SCALAR; elems@8+ Values.
        return off < sizeof(ListVec);
    case CellType::Pair:
        // ValuePair is all-Value — never scalar.
    case CellType::Value:
        // standalone Value cell — the single word is a Value (pointer-capable).
    case CellType::Chars:
        // opaque char buffer — no pointer slots (never enters the pointer scan).
    case CellType::None:
        // unstamped / interior — opaque.
        return false;
    }
    return false;
}

// ===========================================================================
// PART 3 — typed child-slot enumerator (consumed by W2/W4/W5 + the tripwire)
//
// `enumerateChildSlots(ct, base, v)` invokes, in the SAME order the hot walkers
// use, one visitor callback per child slot of the object at `base`:
//
//   v.value(Value & slot, std::size_t off)          an inline Value child slot
//   v.listChild(ListVec * child, std::size_t off)   a raw ListVec* child
//                                                    (Closure/Thunk capturedWiths)
//   v.bindingsChild(const Bindings * child, off)    a raw Bindings* child (parent)
//   v.cellThrough(Value * cell, std::size_t off)    the Thunk cell (a Value* into
//                                                    another, tenured, object)
//
// `off` is the child slot's byte offset within the cell (for diagnostics /
// tripwire holder+offset reporting).  A visitor only needs the callbacks for the
// slot kinds a given object type produces; unused callbacks are never called.
//
// The union@16 of a Suspended/Blackhole/Native Thunk (desc/fn — a CU/PrimOp
// pointer, not a GC-managed child) is intentionally NOT enumerated, matching the
// hot walkers.
// ===========================================================================

template<class V>
inline void enumerateClosureChildren(Closure * c, V & v)
{
    v.listChild(c->capturedWiths, offsetof(Closure, capturedWiths));
    for (std::uint16_t i = 0; i < c->nUpvalues; ++i)
        v.value(c->upvalues[i], sizeof(Closure) + std::size_t(i) * sizeof(Value));
}

template<class V>
inline void enumerateThunkChildren(Thunk * t, V & v)
{
    v.cellThrough(t->cell, offsetof(Thunk, cell));
    switch (t->state) {
    case ThunkState::Suspended:
    case ThunkState::Blackhole:
        if (ListVec * w = thunkCapturedWiths(t))
            v.listChild(w, thunkWithsSlotOffset(t->nUpvalues));
        for (std::uint16_t i = 0; i < t->nUpvalues; ++i)
            v.value(t->tail[i], sizeof(Thunk) + std::size_t(i) * sizeof(Value));
        break;
    case ThunkState::Native:
        for (std::uint16_t i = 0; i < t->nUpvalues; ++i)
            v.value(t->tail[i], sizeof(Thunk) + std::size_t(i) * sizeof(Value));
        break;
    case ThunkState::Evaluated:
        v.value(t->evaluated, offsetof(Thunk, evaluated));
        break;
    }
}

template<class V>
inline void enumerateBindingsChildren(Bindings * b, V & v)
{
    if (b->isMapAttrs())
        v.value(*b->mapAttrsAux(), bindingsMapAttrsAuxOffset(b->size));
    for (std::uint32_t i = 0; i < b->size; ++i)
        v.value(b->entries[i].value,
                sizeof(Bindings) + std::size_t(i) * sizeof(Bindings::Entry)
                    + offsetof(Bindings::Entry, value));
    if (b->parent)
        v.bindingsChild(b->parent, offsetof(Bindings, parent));
}

template<class V>
inline void enumerateListChildren(ListVec * l, V & v)
{
    for (std::uint32_t i = 0; i < l->size; ++i)
        v.value(l->elems[i], sizeof(ListVec) + std::size_t(i) * sizeof(Value));
}

template<class V>
inline void enumeratePairChildren(ValuePair * p, V & v)
{
    v.value(p->left,      offsetof(ValuePair, left));
    v.value(p->right,     offsetof(ValuePair, right));
    v.value(p->evaluated, offsetof(ValuePair, evaluated));
    v.value(p->third,     offsetof(ValuePair, third));
}

/// Dispatch by CellType.  `base` must be the cell start of an object of that
/// type.  CellType::None/Value/Chars carry no enumerable child slots.
template<class V>
inline void enumerateChildSlots(CellType ct, void * base, V & v)
{
    switch (ct) {
    case CellType::Closure:  enumerateClosureChildren(static_cast<Closure *>(base), v); break;
    case CellType::Thunk:    enumerateThunkChildren(static_cast<Thunk *>(base), v); break;
    case CellType::Bindings: enumerateBindingsChildren(static_cast<Bindings *>(base), v); break;
    case CellType::List:     enumerateListChildren(static_cast<ListVec *>(base), v); break;
    case CellType::Pair:     enumeratePairChildren(static_cast<ValuePair *>(base), v); break;
    case CellType::None:
    case CellType::Value:
    case CellType::Chars:
        break;
    }
}

// ===========================================================================
// PART 4 — manifest cross-check tripwire (consumed by B's post-scavenge scan)
//
// `scanChildSlotsForYoung(ct, base, isYoung)` enumerates every manifest-listed
// child-pointer slot of the object at `base` and returns the FIRST slot whose
// pointer still lies in the young/nursery region per `isYoung`.  Run over the
// scavenger's live-object set AFTER a scavenge, a non-empty result means a hot
// walker forgot to forward a manifest field — a deterministic, precisely-located
// missed-root signal (holder type known by the caller, offset in `.off`) instead
// of a silent use-after-free.
//
// `isYoung` is a predicate `bool(const void *)` — production passes
// `Nursery::contains`; the unit test passes a sentinel-address predicate so the
// tripwire can be proven to FIRE on injected drift without a live nursery.
// ===========================================================================
struct TripwireHit {
    bool         hit  = false;
    std::size_t  off  = 0;      ///< byte offset of the offending slot within the cell
    const void * ptr  = nullptr;///< the residual young pointer found
    const char * kind = "";     ///< slot kind: "Value" / "ListVec*" / "Bindings*" / "cell"
};

template<class YoungPred>
inline TripwireHit scanChildSlotsForYoung(CellType ct, void * base, YoungPred isYoung)
{
    struct Vis {
        YoungPred & yp;
        TripwireHit h;
        void flag(const void * p, std::size_t off, const char * kind)
        {
            if (!h.hit && p && yp(p)) { h.hit = true; h.off = off; h.ptr = p; h.kind = kind; }
        }
        void value(Value & v, std::size_t off)
        {
            if (tagIsPointer(v.tag())) flag(v.asRaw(), off, "Value");
        }
        void listChild(ListVec * l, std::size_t off)      { flag(l, off, "ListVec*"); }
        void bindingsChild(const Bindings * b, std::size_t off) { flag(b, off, "Bindings*"); }
        void cellThrough(Value * c, std::size_t off)      { flag(c, off, "cell"); }
    } vis{isYoung, {}};
    enumerateChildSlots(ct, base, vis);
    return vis.h;
}

} // namespace nix::v3::gclayout
