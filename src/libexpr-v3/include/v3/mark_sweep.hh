#pragma once
/// @file
/// Stage 6 production GC — flat mark-sweep over the v3 tenured arena.
///
/// Per `lode/GC_DESIGN_POST_CHENEY_2026-05-28.md` §4-§6 (the design
/// post-Cheney falsification).  Replaces the Cheney semispace
/// machinery (move_gc.cc + major_scavenge.hh) which was falsified at
/// commit eda44711a — Cheney's 1.5-1.7x peak factor at v3's high
/// live-fraction made it strictly worse than the no-GC baseline.
///
/// ## Design summary (Day 6, 2026-05-28)
///
///   1. Walk all precise roots via `walkAllV3Roots` (Stage 3), marking
///      each reachable cell in a per-block mark bitmap (Phase 1).
///   2. Sweep each arena block: for each unmarked cell address, insert
///      into a per-size-class free list.  Cells stay IN PLACE (no
///      copy, no 2x peak).  Free blocks (those with zero marked cells)
///      get returned to libc (Phase 2).
///   3. Allocator fast path: bump within current block; slow path:
///      check size-class free list first (Phase 3).
///
/// Falsifier results justifying this design (per
/// `STAGE_6_FALSIFIERS_DAY6_2026-05-28.md`):
///   * F#1 line-occupancy: 46.5% hello / 50.5% HNE lines fully dead;
///     both flat MS and Immix viable.
///   * F#2 mark cost (realistic bitmap): 0.8% hello / 1.8% HNE wall.
///   * Flat MS reclaims 12% more bytes per cycle than Immix
///     (cell-granularity vs line-granularity).
///   * Implementation budget 1.5-2 KLoC, 2-3 weeks.
///
/// ## When invoked
///
/// `runMajorMarkSweep` is reached from the vm.cc dispatch-loop major-GC
/// safepoint, which fires for EITHER (a) the opt-in `NIX_V3_MAJOR_GC=1`
/// gate OR (b) the default-on generational-major path (`g_genMajorEnabled`,
/// a hard constant, active whenever a nursery exists — i.e. always).  In
/// both cases the safepoint runs only at `exitDepth == 0` once the arena
/// passes the dynamic threshold, and the precise mark itself is skipped
/// unless `Arena::cellMetaEnabled()` (RC-2 2026-08-04: no cell metadata ⇒
/// nothing to reclaim ⇒ inert).  The flat mark-sweep keeps cells IN PLACE
/// (non-moving; no 2x peak).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>

namespace nix::v3 {

struct VMState;

/// Run a major mark-sweep GC: mark phase walks all precise roots
/// from `vm`; sweep phase walks the arena rebuilding per-size-class
/// free lists and freeing fully-dead blocks.
///
/// Pre-conditions:
///   - `vm` is at a safe point (dispatch-loop top, `exitDepth == 0`).
///   - Caller has ensured no nested VMState distinct from `vm`.
///   - `vm.frames` `vm.valueStack` `vm.withStack` are consistent with
///     `ip` synced into the top frame.
///
/// Side effects:
///   - Frees unreached arena cells back to per-block free lists.
///   - May free entire blocks back to libc (fully-dead blocks).
///   - Stats banner emitted on stderr under NIX_VM_STATS=1.
///
/// Result of one major mark-sweep cycle, consumed by the dispatch-loop
/// trigger's adaptive-backoff policy (PLAN_BEAT_TW_V2 §1.1b).  `heapBytes`
/// is the arena's `bytesAllocated()` at cycle entry (the "freed < 5 % of
/// heap" denominator + the backoff anchor); `bytesFreed` is what the
/// sweep returned to libc (whole-block-free + huge-block reclaim).
struct MajorGcResult {
    std::size_t bytesFreed = 0;
    std::size_t heapBytes  = 0;
};

/// Phase 1+2+3 status: declared here, stubbed in mark_sweep.cc as a
/// no-op pending implementation.  vm.cc's dispatch-loop integration
/// can wire to this now (gated default-OFF); the gate remains OFF
/// until the implementation ships and passes the Phase 4 SHIP gate.
MajorGcResult runMajorMarkSweep(VMState & vm) noexcept;

} // namespace nix::v3
