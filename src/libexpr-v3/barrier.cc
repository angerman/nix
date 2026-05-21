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

bool phaseDActive() noexcept
{
    // Cache the env-var read across the process lifetime.  The
    // `static const bool` init runs exactly once on first call;
    // every subsequent call is one load + one branch.
    //
    // Gating on NIX_V3_NURSERY (not NIX_V3_NURSERY_SCAVENGE) because
    // the barrier must record inter-gen writes whenever ALLOCATIONS
    // route through the nursery — independent of whether scavenge is
    // enabled.  An allocation-only-no-scavenge run still wants the
    // diagnostic correctness (BRUTE+AUDIT distinguish LIVE vs DEAD);
    // gating on _SCAVENGE would silently lose dirty-list entries.
    static const bool s_active = [] {
        const char * v = std::getenv("NIX_V3_NURSERY");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return s_active;
}

} // namespace nix::v3
