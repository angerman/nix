#pragma once
/// @file
/// Live-fraction tracer for the v3 arena — Stage 6 SPIKE for the
/// precise-root foundation (lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md).
///
/// **Purpose**: answer the load-bearing measure-twice question for
/// "ditch Boehm" before committing 1-2 weeks to Stage 6 implementation:
///
///   How much of the v3 arena is REACHABLE at end of eval (live)
///   versus TOTAL allocated (live + garbage-retained-by-bump-allocator)?
///
/// If 90 %+ of allocated bytes are still reachable, precise GC of the
/// arena recovers ≤ 10 % of arena RSS — falsifies the project as
/// scoped under [[memory-first-class]] ≥200 MB SHIP gate.  Project
/// must pivot to "allocate less" levers (Stage 4 strictness, nursery
/// promotion policy) instead.
///
/// If 50 % or less of allocated bytes are reachable, ≥50 % of arena
/// could in principle be reclaimed by precise GC — Stages 4-6 are
/// justified.
///
/// ## Mechanism
///
/// `dumpV3LiveFraction(NIX_V3_LIVE_TRACE=1)` does a transitive
/// mark-from-roots phase using `walkAllV3Roots` (Stage 3):
///
///   1. Push every root pointer into a worklist
///   2. For each worklist entry, walk its outgoing pointer fields
///      and enqueue newly-seen pointees
///   3. Continue until worklist empty (full transitive closure)
///   4. Report:
///        - per-type LIVE count + bytes
///        - allocStats() ALLOCATED count + bytes (the denominator)
///        - LIVE / ALLOCATED ratio per type + aggregate
///
/// ## Cost
///
/// One-shot diagnostic, called at end of run.  Cost is O(reachable
/// pointer count + outgoing edges).  On hello.drvPath the reachable
/// set is bounded by Boehm's live set ≈ 0.4 MB, so the trace itself
/// completes in milliseconds.  Heap usage: one unordered_set<void *>
/// sized to the reachable object count.
///
/// ## Retirement criterion
///
/// When Stage 6 lands (precise GC of v3 arena), the precise GC itself
/// IS this trace — `dumpV3LiveFraction` becomes a debug overlay on
/// top of the production marker.  Remove `NIX_V3_LIVE_TRACE` gate and
/// fold into NIX_VM_STATS at that point.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

namespace nix::v3 {

/// Called at end of run (between dumpAllV3Roots and the final stats
/// flush in run.cc).  No-op unless `NIX_V3_LIVE_TRACE=1`.
///
/// Walks transitively from all precise roots; counts unique reached
/// objects per type; reports the LIVE-vs-ALLOCATED ratio.  Output
/// goes to stderr in a stable format compatible with grep-based bench
/// scripts (see `bench/m5-cron.sh` for the ledger convention).
void dumpV3LiveFraction() noexcept;

} // namespace nix::v3
