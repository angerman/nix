#pragma once
/// @file
/// v3 allocator: per-EvalState arena + simple refcount-free heap.
///
/// For the bring-up phase we use plain malloc/free under a thin wrapper.
/// The arena/refcount story is the architectural Phase A item — not yet
/// implemented; the wrapper exists so call-sites are stable when we
/// switch.
///
/// Heap-allocated runtime objects (besides Value):
///   - Closure  : LambdaDescriptor* + FAM upvalues
///   - Thunk    : state + descriptor + FAM upvalues / args
///   - Env      : parent + FAM values (let/with scopes)
///   - ListVec  : size + FAM Value elements
///   - Bindings : size + FAM (SymbolId, Value) pairs (sorted)
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/closure.hh"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <new>

namespace nix::v3 {

using SymbolId = uint32_t;
constexpr SymbolId kInvalidSymbol = 0;

class EvalState;

// ---------------------------------------------------------------------------
// ListVec — flat array of Values with a length prefix.
// ---------------------------------------------------------------------------

struct ListVec
{
    uint32_t size;
    uint32_t _pad;
    Value    elems[]; // FAM
};

// ---------------------------------------------------------------------------
// Bindings — sorted (SymbolId, Value) pairs with binary search.
//
// For the bring-up phase this is the only Bindings shape.  The v3 design doc
// envisages Empty/Single/Small/Sorted polymorphism, but a single Sorted form
// is correct and lets us defer the polymorphism work until the perf gap
// motivates it.
// ---------------------------------------------------------------------------

struct Bindings
{
    struct Entry { SymbolId name; Value value; };

    uint32_t size;
    uint32_t _pad;
    Entry    entries[]; // FAM, sorted ascending by name

    /// Binary search.  Returns nullptr if not found.
    const Value * lookup(SymbolId name) const noexcept
    {
        uint32_t lo = 0, hi = size;
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            SymbolId midName = entries[mid].name;
            if (midName == name) return &entries[mid].value;
            if (midName < name) lo = mid + 1;
            else                hi = mid;
        }
        return nullptr;
    }

    bool has(SymbolId name) const noexcept { return lookup(name) != nullptr; }
};

// ---------------------------------------------------------------------------
// Allocator surface
// ---------------------------------------------------------------------------

struct Alloc
{
    static Value * allocValue() noexcept
    {
        return static_cast<Value *>(std::malloc(sizeof(Value)));
    }

    static Closure * allocClosure(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
        auto * c = static_cast<Closure *>(std::malloc(bytes));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->withEnv = nullptr;
        return c;
    }

    /// Allocate a Suspended thunk with `nUpvalues` captured upvalues
    /// stored in the FAM tail.
    static Thunk * allocThunkSuspended(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Thunk) + sizeof(Value) * nUpvalues;
        auto * t = static_cast<Thunk *>(std::malloc(bytes));
        t->state = ThunkState::Suspended;
        t->nUpvalues = nUpvalues;
        return t;
    }

    static Env * allocEnv(uint16_t nValues) noexcept
    {
        const size_t bytes = sizeof(Env) + sizeof(Value) * nValues;
        auto * e = static_cast<Env *>(std::malloc(bytes));
        e->parent = nullptr;
        e->isWithEnv = false;
        e->nValues = nValues;
        return e;
    }

    static ListVec * allocList(uint32_t n) noexcept
    {
        const size_t bytes = sizeof(ListVec) + sizeof(Value) * n;
        auto * l = static_cast<ListVec *>(std::malloc(bytes));
        l->size = n;
        return l;
    }

    static Bindings * allocBindings(uint32_t n) noexcept
    {
        const size_t bytes = sizeof(Bindings) + sizeof(Bindings::Entry) * n;
        auto * b = static_cast<Bindings *>(std::malloc(bytes));
        b->size = n;
        return b;
    }
};

// ---------------------------------------------------------------------------
// Allocation counters
// ---------------------------------------------------------------------------

struct AllocStats
{
    uint64_t valuesAllocated   = 0;
    uint64_t closuresAllocated = 0;
    uint64_t thunksAllocated   = 0;
    uint64_t envsAllocated     = 0;
    uint64_t listsAllocated    = 0;
    uint64_t attrsetsAllocated = 0;
};

inline AllocStats & allocStats()
{
    static AllocStats stats;
    return stats;
}

} // namespace nix::v3
