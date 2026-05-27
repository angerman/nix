/// @file
/// Bridge-source side-table — implementation.
///
/// See include/v3/bridge_root_registry.hh for design + rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bridge_root_registry.hh"

#include "nix/expr/config.hh"
#if NIX_USE_BOEHMGC
#  include <gc/gc.h>
#endif

#include <vector>

namespace nix::v3 {

namespace {

/// Per-thread registry state.  Held in a thread_local to match
/// the threadArena ownership model.
struct Impl
{
    std::vector<void *> srcs;
    // Cached pointer range currently registered with Boehm.
    // Updated whenever the vector's storage moves or grows.
    // Initial nullptr/nullptr means "nothing registered yet".
    void * curStart = nullptr;
    void * curEnd   = nullptr;
};

inline Impl & impl() noexcept
{
    thread_local Impl s;
    return s;
}

/// Synchronize Boehm's view of the registry with the vector's
/// current storage range.  Called whenever push_back may have
/// moved the storage.
void resyncBoehmRoots() noexcept
{
#if NIX_USE_BOEHMGC
    Impl & st = impl();
    // Unregister the previously-registered range if any.
    if (st.curStart) {
        GC_remove_roots(st.curStart, st.curEnd);
        st.curStart = nullptr;
        st.curEnd   = nullptr;
    }
    // Register the current range if non-empty.
    if (!st.srcs.empty()) {
        st.curStart = static_cast<void *>(&st.srcs[0]);
        st.curEnd   = static_cast<char *>(st.curStart)
                    + st.srcs.size() * sizeof(void *);
        GC_add_roots(st.curStart, st.curEnd);
    }
#else
    // No Boehm: registry is a memoization hint with no GC
    // interaction.  Caller can still push but it's a no-op for
    // root tracking.
    (void)impl();
#endif
}

} // anon ns

void pushBridgeRoot(void * src) noexcept
{
    if (!src) return;  // No nursery / Boehm interaction for null.
    Impl & st = impl();
    // Capture pre-push storage identity so we can detect
    // re-allocation (vector growing past capacity moves storage).
    void * oldData = st.srcs.empty() ? nullptr : &st.srcs[0];
    std::size_t oldCap = st.srcs.capacity();
    st.srcs.push_back(src);
    void * newData = &st.srcs[0];
    std::size_t newCap = st.srcs.capacity();
    if (newData != oldData || newCap != oldCap || st.curStart == nullptr) {
        // Storage moved / capacity changed / first push:
        // re-register the new range with Boehm.
        resyncBoehmRoots();
    } else {
        // Same storage, same capacity: just need to extend the
        // registered end.  Cheap path — single GC_add_roots over
        // the newly-occupied slot.  But Boehm tolerates
        // overlapping GC_add_roots calls, so we can simply
        // re-add the FULL range and let Boehm dedup internally.
        // (Per the Boehm docs `GC_add_roots` is idempotent over
        // overlapping regions.)  Avoids the per-push
        // GC_remove_roots cost.
        //
        // Actually simpler: just re-call resyncBoehmRoots.  At
        // ~10 ns per call this is negligible — bridge thunks are
        // rare (HNE: ~10).
        resyncBoehmRoots();
    }
}

std::size_t bridgeRootCount() noexcept
{
    return impl().srcs.size();
}

const void * const * bridgeRootData() noexcept
{
    Impl & st = impl();
    if (st.srcs.empty()) return nullptr;
    return st.srcs.data();
}

} // namespace nix::v3
