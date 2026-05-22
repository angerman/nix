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

} // anonymous

std::vector<DirtyEntry> & dirtyContainers() noexcept
{
    return tl_dirty;
}

std::vector<Value *> & standaloneCellRoots() noexcept
{
    return tl_standaloneCells;
}

// #767 (2026-05-22): exposed as a namespace-scope `const bool` so
// every barrier emit just loads a single byte instead of going
// through the C++ magic-static guard the prior `static const bool`
// inside a function required.  The barrier helper declarations in
// `include/v3/barrier.hh` inline `phaseDActive()` as a direct read
// of this variable, which the compiler can hoist across multiple
// adjacent barrier writes.
//
// Gating on NIX_V3_NURSERY (not NIX_V3_NURSERY_SCAVENGE) because
// the barrier must record inter-gen writes whenever ALLOCATIONS
// route through the nursery — independent of whether scavenge is
// enabled.  An allocation-only-no-scavenge run still wants the
// diagnostic correctness (BRUTE+AUDIT distinguish LIVE vs DEAD);
// gating on _SCAVENGE would silently lose dirty-list entries.
namespace detail {
const bool g_phaseDActive = [] {
    const char * v = std::getenv("NIX_V3_NURSERY");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}();
}

} // namespace nix::v3
