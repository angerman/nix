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
/// ## Cost when not invoked
///
/// The trigger in vm.cc dispatch-loop is gated `NIX_V3_MAJOR_GC=1`
/// (default OFF; production unaffected).  When gate is OFF this
/// function is never called and the implementation pays nothing.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

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
/// Phase 1+2+3 status: declared here, stubbed in mark_sweep.cc as a
/// no-op pending implementation.  vm.cc's dispatch-loop integration
/// can wire to this now (gated default-OFF); the gate remains OFF
/// until the implementation ships and passes the Phase 4 SHIP gate.
void runMajorMarkSweep(VMState & vm) noexcept;

} // namespace nix::v3
