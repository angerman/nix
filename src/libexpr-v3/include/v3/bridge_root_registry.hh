#pragma once
/// @file
/// Bridge-source side-table for arena deregistration.
///
/// Per `lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md`:
///
/// The v3 arena (`threadArena`) currently registers every block
/// with Boehm via `GC_add_roots(blk, blk + kBlockSize)`.  On a
/// 587 MB arena workload, Boehm's per-collection scan walks the
/// entire arena every time it considers collecting — which is why
/// Boehm chooses growth over collection (per
/// `BOEHM_TUNING_FALSIFIED_2026-05-27.md` periodic-GC probe).
///
/// The arena-registration was originally motivated by `Thunk::
/// bridgeSrc`: bridge thunks (state == `ThunkState::Bridge`) hold
/// a `void *` cast of `nix::Value *` from TW's heap.  Without the
/// arena being a Boehm root, Boehm doesn't see those `nix::Value *`
/// references and may reclaim them mid-evaluation.
///
/// This side-table provides a TARGETED alternative: when a bridge
/// thunk is allocated, its `bridgeSrc` is pushed into this registry
/// (thread-local `std::vector<void *>`).  The registry's BACKING
/// STORAGE is registered with Boehm via `GC_add_roots`.  Surface
/// area is small (one root region per thread, sized to the bridge
/// count) — Boehm collections scan only the bridge pointers, not
/// the full 587 MB arena.
///
/// With this side-table in place, `Arena::refill` can SKIP its
/// `GC_add_roots` call (gated on `NIX_V3_ARENA_NOROOT=1`).  Boehm's
/// per-collect cost drops from 40 ms (587 MB scan) to < 1 ms
/// (5 MB scan + registry), making auto-collection viable and
/// reducing the 400 MB Boehm watermark.
///
/// ## Audit conclusion (per bench/arena-dereg-audit.sh, 2026-05-27)
///
/// Tag::External is the OTHER potential Boehm-managed pointer in
/// v3 cells.  Verified absent on 4 anchor workloads (hello +
/// firefox + HNE + ackermann): 0 External-tagged Values reached
/// during the live-trace walk.  Arena dereg therefore needs ONLY
/// the bridge-source side-table for these workloads.
///
/// Future workloads must re-verify via
/// `bench/arena-dereg-audit.sh` before assuming External-clean.
///
/// ## Cost
///
/// * Per bridge-thunk alloc: one push_back on a thread-local
///   vector.  Amortized O(1).  Vector growth triggers
///   GC_remove_roots + GC_add_roots once; growth doubles capacity
///   so this is rare.
/// * Per Boehm collection: O(bridge_count) scan instead of
///   O(arena_size).  Typical bridge_count on HNE is ~10 (per the
///   v3-direct stats line); arena_size is ~600 MB.  ~50000× cost
///   reduction per collection.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace nix::v3 {

/// Register a Boehm-managed pointer (typically a `nix::Value *`
/// from `Thunk::bridgeSrc`) so Boehm sees it as a root.  Thread-
/// local; pushes onto the calling thread's registry.
///
/// Called from `Alloc::allocBridgeThunk` to ensure the bridged
/// nix::Value stays alive across Boehm collections regardless of
/// whether the arena itself is GC-registered.
///
/// Cost: amortized O(1).  On vector growth (capacity doubles)
/// the underlying root region is GC_remove_roots'd + re-added.
void pushBridgeRoot(void * src) noexcept;

/// Returns the current count of bridge sources in the calling
/// thread's registry.  Used by diagnostics + tests.
std::size_t bridgeRootCount() noexcept;

/// Returns a pointer to the registry's contiguous storage (or
/// nullptr if empty).  Used by diagnostics; the storage is
/// already Boehm-rooted so external readers don't need to
/// register it.
const void * const * bridgeRootData() noexcept;

} // namespace nix::v3
