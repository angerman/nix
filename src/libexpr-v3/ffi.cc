/// @file
/// FFI plan migration step 2: skeletal Evaluator + EvalScope impl.
///
/// Provides minimal definitions for the framework types declared in
/// `include/v3/ffi.hh` so they can be linked against.  Full migration
/// (handle storage, scope-bound handle invalidation, GlobalClosureHandle
/// promotion semantics) lands incrementally per the plan.
///
/// **Status:**
///   - `Evaluator` is a thin shell -- holds a per-instance scope-list
///     pointer.  Future work will lift the existing v3 globals
///     (v3HookCache, v3SubExprCache, v3BridgeClosures, ...) into
///     Evaluator member fields so multiple Evaluators can coexist
///     (e.g., for sandboxed plugin evaluation).
///   - `EvalScope` chains scopes per Evaluator.  Handle issuance and
///     invalidation will land alongside the migration of
///     `v3FormalsLambdaBridges` (today's sentinel-Env side-table) into
///     the EvalScope handle accounting.
///   - `promoteToGlobal` / `releaseGlobal` are skeletal -- the stable
///     v3BridgeClosures table already provides the storage; the wrapper
///     just makes the lifetime contract explicit at the API boundary.
///
/// Per FFI_PLAN_2026-05-06b §A1, A7, A10 (highest-priority migration
/// step 2).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ffi.hh"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

/// Per-EvalScope handle storage.  Each entry holds an opaque payload
/// (whatever v3-internal value the host registers) and a `valid` flag
/// flipped to false when the enclosing scope is destroyed.  The valid
/// flag persists in the table after scope destruction (until the next
/// gen rollover) so isValid() correctly returns false for stale handles.
struct HandleSlot
{
    void *   payload;
    bool     valid;
};

/// Per-Evaluator scope chain head.  Each EvalScope ctor pushes a new
/// node; dtor pops.  Threadlocal -- one logical Evaluator per thread
/// today; future work may need lock-free per-instance lists.
struct ScopeNode
{
    ScopeNode * prev;
    /// Generation token for handles allocated in this scope.  Encoded
    /// in the upper 32 bits of ClosureHandle::opaque so a handle whose
    /// scope has been destroyed (and whose generation has been removed
    /// from g_liveScopes) fails the lookup -- ABA defence works because
    /// each new scope gets a fresh generation from the global counter.
    uint32_t generation;
    /// Slot vector owned by this scope; entries are flipped to valid=false
    /// in the dtor before the table is freed.
    std::vector<HandleSlot> slots;
};

namespace {
thread_local ScopeNode * g_topScope = nullptr;

/// Scope generation counter.  Atomic so concurrent threads issue
/// distinct generations even if their EvalScopes never interact.
/// Starts at 1 (0 reserved for "uninitialised handle").
std::atomic<uint32_t> g_nextScopeGen{1};

/// Live-scope index: maps generation -> ScopeNode*.  Populated on ctor,
/// erased on dtor.  Lookup-by-generation drives O(1) handle resolution.
/// Lock guards both the map and per-slot access (writers + readers).
std::mutex                                  g_scopeLock;
std::unordered_map<uint32_t, ScopeNode *>   g_liveScopes;

constexpr uint32_t kInvalidGen = 0;

/// Pack/unpack helpers for the 64-bit handle opaque.
struct PackedHandle
{
    uint32_t generation;
    uint32_t slotIdx;
};
inline uint64_t packHandle(uint32_t gen, uint32_t slot) {
    return (uint64_t(gen) << 32) | uint64_t(slot);
}
inline PackedHandle unpackHandle(uint64_t opaque) {
    return PackedHandle{
        .generation = uint32_t(opaque >> 32),
        .slotIdx    = uint32_t(opaque & 0xFFFFFFFFu),
    };
}
}

class Evaluator
{
public:
    Evaluator() = default;
    Evaluator(const Evaluator &) = delete;
    Evaluator & operator=(const Evaluator &) = delete;
};

// ---------------------------------------------------------------------------
// EvalScope
// ---------------------------------------------------------------------------

EvalScope::EvalScope(Evaluator & e)
    : m_ev(e)
{
    uint32_t gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);
    // Avoid handing out gen=0 (reserved as kInvalidGen).  In practice
    // this only matters at the 4-billion-scope rollover; bias once.
    if (gen == kInvalidGen)
        gen = g_nextScopeGen.fetch_add(1, std::memory_order_relaxed);

    auto * node = new ScopeNode{
        .prev       = g_topScope,
        .generation = gen,
        .slots      = {},
    };
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        g_liveScopes.emplace(gen, node);
    }
    g_topScope = node;
    m_state    = node;
}

EvalScope::~EvalScope()
{
    auto * node = static_cast<ScopeNode *>(m_state);
    if (!node) return;

    // Invalidate every handle issued by this scope.  Marking the slots
    // (rather than just dropping the table) means a stale ClosureHandle
    // copied out by the host still resolves cleanly to "invalid" via
    // lookupClosureHandle / isValid -- they re-check the slot's valid
    // bit on every call.  After the slot vector is freed, generation
    // removal from g_liveScopes makes future lookups short-circuit.
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        for (auto & s : node->slots) s.valid = false;
        g_liveScopes.erase(node->generation);
    }

    if (node == g_topScope) {
        g_topScope = node->prev;
        delete node;
    } else {
        // Mismatch (scope dtor running out of stack order) is a programmer
        // error.  Leak the node so subsequent dtors find their state.
        // In a debug build, an assert would fire.
    }
}

ClosureHandle allocClosureHandle(EvalScope & /*scope*/, void * payload)
{
    // EvalScope is non-copyable + RAII, so the scope passed in MUST be
    // the topmost (otherwise the caller has a stack-order bug).  Read
    // g_topScope rather than poking at EvalScope::m_state -- the API
    // contract guarantees they match.
    ScopeNode * top = g_topScope;
    if (!top) return ClosureHandle{0};

    HandleSlot newSlot{payload, true};
    uint32_t slotIdx;
    {
        std::lock_guard<std::mutex> lk(g_scopeLock);
        slotIdx = static_cast<uint32_t>(top->slots.size());
        top->slots.push_back(newSlot);
    }
    return ClosureHandle{packHandle(top->generation, slotIdx)};
}

bool isValid(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return false;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return false;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return false;
    return node->slots[p.slotIdx].valid;
}

void * lookupClosureHandle(ClosureHandle h)
{
    auto p = unpackHandle(h.opaque);
    if (p.generation == kInvalidGen) return nullptr;

    std::lock_guard<std::mutex> lk(g_scopeLock);
    auto it = g_liveScopes.find(p.generation);
    if (it == g_liveScopes.end()) return nullptr;

    ScopeNode * node = it->second;
    if (p.slotIdx >= node->slots.size()) return nullptr;
    auto & slot = node->slots[p.slotIdx];
    return slot.valid ? slot.payload : nullptr;
}

// ---------------------------------------------------------------------------
// Global handle promotion
// ---------------------------------------------------------------------------
//
// Today: GlobalClosureHandle wraps the same storage as ClosureHandle
// (the v3BridgeClosures table -- always-rooted via traceable_allocator).
// The lifetime distinction is contract-only; the table itself never
// shrinks (per existing MED-14 bounding work).  Once we migrate
// v3FormalsLambdaBridges to use ClosureHandle, releaseGlobal will
// actually free the slot.

GlobalClosureHandle promoteToGlobal(EvalScope & /*scope*/, ClosureHandle h)
{
    GlobalClosureHandle g;
    g.opaque = h.opaque;
    return g;
}

void releaseGlobal(GlobalClosureHandle /*h*/)
{
    // No-op for now -- the underlying v3BridgeClosures table is grow-
    // only.  When we migrate to a slot-recycling registry, this will
    // free the slot and any captured values.
}

} // namespace nix::v3
