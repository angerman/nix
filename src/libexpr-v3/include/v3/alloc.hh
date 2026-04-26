#pragma once
/// @file
/// v3 allocator: per-EvalState arena + simple refcount-free heap.
///
/// For the bring-up phase we use plain malloc/free under a thin wrapper.
/// The arena/refcount story is the architectural Phase A item — not yet
/// implemented; the wrapper exists so call-sites are stable when we
/// switch.
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

class EvalState;

/// Allocator surface.  All v3 heap allocations go through here so we can
/// swap the backend (currently malloc; future: arena).
struct Alloc
{
    /// Allocate a Value.  Returns a fresh, uninitialised Value*.
    static Value * allocValue() noexcept
    {
        return static_cast<Value *>(std::malloc(sizeof(Value)));
    }

    /// Allocate a Closure with `nUpvalues` upvalue slots.
    static Closure * allocClosure(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
        auto * c = static_cast<Closure *>(std::malloc(bytes));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->withEnv = nullptr;
        return c;
    }

    /// Allocate a Thunk for a Suspended thunk descriptor.
    /// `nUpvalues` upvalue slots follow inline in the FAM.
    static Thunk * allocThunkSuspended(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Thunk) + sizeof(Value) * nUpvalues;
        auto * t = static_cast<Thunk *>(std::malloc(bytes));
        t->state = ThunkState::Suspended;
        t->nUpvalues = nUpvalues;
        return t;
    }

    /// Allocate an Env with `nValues` slots.  Used for let / with scopes.
    static Env * allocEnv(uint16_t nValues) noexcept
    {
        const size_t bytes = sizeof(Env) + sizeof(Value) * nValues;
        auto * e = static_cast<Env *>(std::malloc(bytes));
        e->parent = nullptr;
        e->isWithEnv = false;
        e->nValues = nValues;
        return e;
    }
};

/// Allocation counters for diagnostics.  Always-on, gated by NIX_V3_STATS=1
/// for printing.  Cheap (single increment per alloc).
struct AllocStats
{
    uint64_t valuesAllocated   = 0;
    uint64_t closuresAllocated = 0;
    uint64_t thunksAllocated   = 0;
    uint64_t envsAllocated     = 0;
};

inline AllocStats & allocStats()
{
    static AllocStats stats;
    return stats;
}

} // namespace nix::v3
