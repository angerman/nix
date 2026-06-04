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

struct VMState;  // forward decl for maybeSamplePeriodicLiveFraction

/// Called at end of run (between dumpAllV3Roots and the final stats
/// flush in run.cc).  No-op unless `NIX_V3_LIVE_TRACE=1`.
///
/// Walks transitively from all precise roots; counts unique reached
/// objects per type; reports the LIVE-vs-ALLOCATED ratio.  Output
/// goes to stderr in a stable format compatible with grep-based bench
/// scripts (see `bench/m5-cron.sh` for the ledger convention).
void dumpV3LiveFraction() noexcept;

/// 2026-06-04: LIVE MEMORY BUCKETS — the GHC-style resident
/// decomposition.  Walks the precise-root graph eval-first and splits
/// arena LIVE bytes into EVAL working set vs CU-cache marginal
/// retention, then adds the non-arena buckets: CU-cache bytecode (libc
/// CompilationUnits), FFI/Boehm-live (heap−free), and the BC-cache
/// SQLite page cache — decomposed against CURRENT resident RSS (never
/// peak ru_maxrss, never the arena's cumulative bump counter).
///
/// Answers "are we counting wrong?": prints the arena LIVE total beside
/// the OLD cumulative `v3_arena` number and their delta (dead cells the
/// bump allocator never reclaimed).  No-op unless NIX_V3_MEM_BUCKETS=1.
///
/// Retirement criterion: when the precise GC ships default-on and the
/// arena counter becomes a live-bytes proxy, fold into NIX_VM_STATS.
void dumpV3MemoryBuckets() noexcept;

// (dumpV3BridgeRetention retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

/// Day 5 2026-05-28: per-block live-bytes probe for the Stage 6
/// generational tenured collector decision (per
/// `lode/STAGE_6_CHENEY_FALSIFIED_2026-05-27.md` alternative #2).
///
/// GHC RTS-style block-aware sweep can only free arena blocks that
/// are FULLY DEAD.  This probe walks all precise roots, marks
/// reached cells, attributes their bytes to containing arena blocks,
/// and reports the histogram of per-block fill ratios + the
/// freeable-block fraction.
///
/// Pre-committed SHIP threshold: ≥30% of arena bytes recoverable via
/// fully-dead block freeing.  If the measurement falls below this,
/// reconsider design (mark-compact instead of mark-sweep).
///
/// No-op unless `NIX_V3_BLOCK_PROBE=1`.
void dumpV3LiveBlockProbe() noexcept;

// ----------------------------------------------------------------------
// Periodic live-trace — L(t) time-series sampling.
// ----------------------------------------------------------------------
//
// Step 4 of the post-Phase-3.8 plan (2026-05-29).  Per
// `lode/L_MEASUREMENT_GAP_2026-05-28.md` §5: every "v3 has structurally
// high L" claim rests on ONE end-of-eval sample.  This API adds a
// time-series sample: walk the precise-root graph every K MB of
// arena allocation, record L(t) per sample, write CSV at end.
//
// Each periodic sample IS a full transitive walk (same machinery as
// dumpV3LiveFraction), so per-sample cost = O(reachable).  Gated
// default-OFF: each enabled run pays ~5-15% wall overhead per the
// L_MEASUREMENT_GAP §5.2 projection.
//
// Gate: NIX_V3_LIVE_TRACE_PERIODIC=<K>     (K in MB; default 64)
//       NIX_V3_LIVE_TRACE_PERIODIC_OUT=<f> (CSV path; default
//                                          /tmp/v3-live-periodic-<pid>.csv)
//
// Retirement criterion (per Rule 0 §2): delete the env-gate when
// the L(t) measurement is integrated into the bench harness as a
// default-OFF metric.  Removal tracked in the
// L_TIME_SERIES_DATA_2026-05-29 follow-up doc + Step 5 in
// post-Phase-3.8 plan.

/// True if NIX_V3_LIVE_TRACE_PERIODIC is set (cached at startup).
bool periodicLiveTraceEnabled() noexcept;

/// Dispatch-loop safepoint hook.  No-op unless gate enabled.  If the
/// arena's `bytesAllocated()` has crossed the next K-multiple since
/// the last sample, walks the precise-root transitive closure and
/// records one CSV row (alloc_offset_mb, resident_mb, live_mb,
/// L_resident, L_cumulative, wall_ms).  Called from vm.cc at
/// exitDepth==0 safepoints, next to the major-GC trigger.
void maybeSamplePeriodicLiveFraction(VMState & vm) noexcept;

/// End-of-run hook.  Writes accumulated CSV samples to
/// NIX_V3_LIVE_TRACE_PERIODIC_OUT (or default path).  No-op unless
/// gate enabled.  Called from run.cc after eval completes (alongside
/// dumpV3LiveFraction).
void flushPeriodicLiveTraceCsv() noexcept;

} // namespace nix::v3
