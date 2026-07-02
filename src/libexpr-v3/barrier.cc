/// @file
/// v3 generational GC write barriers — implementation.
///
/// Provides:
///   - Thread-local `dirtyContainers` (the inter-gen write list).
///   - Thread-local `standaloneCellRoots` (standalone-cell registry).
///   - Process-wide `phaseDActive()` (cached `NIX_V3_NURSERY` gate).
///
/// All barrier helpers (`bindingsSetValue`, `pairSetEvaluated`,
/// `thunkSetEvaluated`, `cellWrite`) are header-inline in
/// `v3/barrier.hh`; this file only carries the thread-local storage
/// definitions + the gate's env-var cache.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/barrier.hh"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace nix::v3 {

namespace {

// Thread-local dirty-container list.  Defined here so multiple
// translation units share one storage per thread; the header
// publishes a `dirtyContainers()` accessor.
//
// Capacity: starts unbounded.  Reasonable steady-state under #702's
// hello.drvPath measurements: ~thousands of entries between
// scavenges.  std::vector's geometric growth means the
// initial-allocation cost is amortised.
//
// Cleared (`.clear()` keeping capacity) by `scavengeNursery` after
// drain.
thread_local std::vector<DirtyEntry> tl_dirty;

thread_local std::vector<Value *> tl_standaloneCells;

/// Stage 6 Phase 3.7 (2026-05-28): registry of lifted-singleton Closure
/// pointers cached on libc-resident LambdaDescriptors.  Each entry is
/// the ADDRESS of a `LambdaDescriptor::cachedSingletonClosure` field;
/// mark walks each address to keep the cached closure alive across
/// arena sweeps.  Without this, the closure is freed (mark doesn't
/// see the libc→arena pointer) and the next OP_MAKE_CLOSURE call to
/// the same lambda reads a stale cached pointer.
thread_local std::vector<Closure **> tl_singletonClosureRegistry;

/// Captured-withs singleton cache slots are process-global in vm.cc, so this
/// registry is process-global too.  The VM already treats that cache as a
/// single-threaded evaluation cache; matching its storage avoids missing slots
/// when a second thread touches the thread-local barrier state first.
std::vector<ListVec **> g_singletonCapturedWithsRegistry;

// PhD-6 last-writer instrument (gated): cell address -> the barrier setter that
// last wrote it.  Consulted by the post-scavenge AUDIT to report HOW an offending
// Bindings entry was last written (pins the missed-root write path vs reasoning).
thread_local std::unordered_map<const void *, const char *> tl_cellWriteSite;

} // anonymous

std::vector<DirtyEntry> & dirtyContainers() noexcept
{
    return tl_dirty;
}

std::vector<Value *> & standaloneCellRoots() noexcept
{
    return tl_standaloneCells;
}

std::vector<Closure **> & singletonClosureRegistry() noexcept
{
    return tl_singletonClosureRegistry;
}

std::vector<ListVec **> & singletonCapturedWithsRegistry() noexcept
{
    return g_singletonCapturedWithsRegistry;
}

// PhD-6 last-writer instrument: storage accessor + gate (on under the AUDIT
// config so it's free in production).  The map keys on cell addresses; under the
// moving nursery nursery-cell keys churn (stale), but the AUDIT-hit cells are
// TENURED (stable addresses), so the lookup is valid for the failing edges.
std::unordered_map<const void *, const char *> & cellWriteSiteMap() noexcept
{
    return tl_cellWriteSite;
}
namespace detail {
const bool g_dbgCellWriteSite = [] {
    const char * v = std::getenv("V3_DBG_NURSERY_AUDIT");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}();
}

// #767 (2026-05-22) → P3.5/§3.7 (2026-07-02): the process-wide Phase-D gate was
// once a namespace-scope `const bool g_phaseDActive` (read via the inline
// phaseDActive() in barrier.hh) so barrier emits loaded a byte instead of
// paying the magic-static guard the prior function-local form required.  The
// NIX_V3_NURSERY opt-out was RETIRED (2026-06-15; the flip soaked clean across
// all of nixpkgs on darwin-4, 24882 attrs / 0 divergence), so the gate is
// permanently true.  phaseDActive() is now `constexpr … return true`
// (barrier.hh), which folds the barrier guards at compile time across all TUs
// even without LTO — strictly better than the extern-const load — so the
// g_phaseDActive definition it depended on is removed here.

} // namespace nix::v3
