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
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// Evaluator
// ---------------------------------------------------------------------------

/// Per-Evaluator scope chain head.  Each EvalScope ctor pushes a new
/// node; dtor pops.  Threadlocal -- one logical Evaluator per thread
/// today; future work may need lock-free per-instance lists.
struct ScopeNode
{
    ScopeNode * prev;
    /// Next-handle issuance counter scoped to this EvalScope.
    /// Wraps to ensure handles allocated in different scopes are
    /// distinguishable for invalidation purposes.
    uint64_t baseHandle;
};

namespace {
thread_local ScopeNode * g_topScope = nullptr;
std::atomic<uint64_t> g_nextScopeBase{1};
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
    auto * node = new ScopeNode{
        .prev       = g_topScope,
        .baseHandle = g_nextScopeBase.fetch_add(0x1000, std::memory_order_relaxed),
    };
    g_topScope = node;
    m_state    = node;
}

EvalScope::~EvalScope()
{
    auto * node = static_cast<ScopeNode *>(m_state);
    if (node && node == g_topScope) {
        g_topScope = node->prev;
        delete node;
    }
    // Mismatch (scope dtor running out of stack order) is a programmer
    // error.  We don't try to recover; leak the node so subsequent
    // dtors find their state.  In a debug build, an assert would fire.
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
