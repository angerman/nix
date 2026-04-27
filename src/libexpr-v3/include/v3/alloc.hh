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

struct EvalState;

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

/// 32-bit AST position handle.  Mirrors `nix::PosIdx`'s underlying
/// representation — we re-export it as a plain uint32_t to avoid
/// pulling the libexpr header into the v3 inner core.  0 means "no
/// position info known".
using PosIdx32 = uint32_t;
constexpr PosIdx32 kNoPos = 0;

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
        c->capturedWiths = nullptr;
        c->cu = nullptr;
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
        t->suspended.capturedWiths = nullptr;
        t->suspended.cu = nullptr;
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

// ---------------------------------------------------------------------------
// Per-attr position side-table.
//
// Tree-walker stores a PosIdx alongside every Bindings::Entry; v3 keeps
// the Entry slim (24 bytes) and uses this side-table instead.  The key
// `(bindings, name)` is uniquely defined: each Bindings sees exactly one
// PosIdx per name.  The side-table is populated by OP_ATTRS_INIT[_DYN] /
// OP_ATTRS_REC_INIT and looked up by `builtins.unsafeGetAttrPos`.
// Lifetime tracking is best-effort: we never explicitly free entries
// since Bindings live for the duration of the eval anyway.
// ---------------------------------------------------------------------------

struct PosKey
{
    const Bindings * bindings;
    SymbolId         name;
    bool operator==(const PosKey & o) const noexcept
    { return bindings == o.bindings && name == o.name; }
};

struct PosKeyHash
{
    size_t operator()(const PosKey & k) const noexcept
    {
        return std::hash<const Bindings *>{}(k.bindings) ^
               (std::hash<SymbolId>{}(k.name) << 1);
    }
};

} // namespace nix::v3

#include <unordered_map>

namespace nix::v3 {

inline std::unordered_map<PosKey, uint32_t, PosKeyHash> & attrPosTable()
{
    static std::unordered_map<PosKey, uint32_t, PosKeyHash> tbl;
    return tbl;
}

inline void recordAttrPos(const Bindings * b, SymbolId name, uint32_t pos)
{
    if (pos == 0) return;
    attrPosTable()[{b, name}] = pos;
}

inline uint32_t lookupAttrPos(const Bindings * b, SymbolId name)
{
    auto & tbl = attrPosTable();
    auto it = tbl.find({b, name});
    return it == tbl.end() ? 0 : it->second;
}

// Resolved AST source position: file path string, line, and column.
// The lowerer fills this snapshot pool from the EvalState's PosTable
// during compilation; runtime stores 1-based indices into this pool
// in the per-attr position side-table.  Pool slot 0 is reserved as
// "no position" so a 0 handle uniformly means "unknown".
struct PosSnapshot
{
    std::string file;
    uint32_t    line;
    uint32_t    column;
};

inline std::vector<PosSnapshot> & posSnapshotPool()
{
    static std::vector<PosSnapshot> pool = { PosSnapshot{} }; // index 0 = none
    return pool;
}

/// Push a snapshot into the pool and return its 1-based handle (0 = none).
inline uint32_t recordPosSnapshot(PosSnapshot s)
{
    auto & p = posSnapshotPool();
    p.push_back(std::move(s));
    return static_cast<uint32_t>(p.size() - 1);
}

inline const PosSnapshot * resolvePosSnapshot(uint32_t handle)
{
    if (handle == 0) return nullptr;
    auto & p = posSnapshotPool();
    if (handle >= p.size()) return nullptr;
    return &p[handle];
}

} // namespace nix::v3
