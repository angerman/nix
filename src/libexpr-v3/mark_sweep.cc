/// @file
/// Stage 6 production GC — flat mark-sweep implementation.
///
/// Replaces the Cheney semispace MajorScavenger (move_gc.cc) which was
/// falsified at commit eda44711a + 9bf527714 (3 pivots on the
/// generational-copying premise).  Per
/// `lode/GC_DESIGN_POST_CHENEY_2026-05-28.md` §4-§6.
///
/// ## Phase plan
///
///   * Phase 0 (this commit) — runMajorMarkSweep is a no-op stub; Cheney
///     carcass removal in progress.  vm.cc dispatch-loop wires here.
///   * Phase 1 (next commit) — mark phase with per-block bitmap.
///   * Phase 2 — sweep + per-size-class free lists.
///   * Phase 3 — allocation slow path uses free lists.
///   * Phase 4 — honest measurement against SHIP gate.
///   * Phase 5 — production hardening + default-on.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/mark_sweep.hh"

namespace nix::v3 {

struct VMState;

void runMajorMarkSweep(VMState & vm) noexcept
{
    // Phase 0: no-op stub.  Dispatch-loop integration wires the call
    // path; actual implementation lands in Phases 1-3.
    (void)vm;
}

} // namespace nix::v3
