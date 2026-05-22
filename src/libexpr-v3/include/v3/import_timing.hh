#pragma once
/// @file
/// #769 (2026-05-22) — process-wide aggregator for primImport phase
/// timings.  The outer-expression PhaseTimer in run.cc captures
/// lower/optimise/compile/run only for the user's top-level expr,
/// which on import-heavy workloads (hello.drvPath, anything that
/// touches nixpkgs) is tiny (~0.1 ms).  All the actual compile
/// work happens INSIDE primImport per imported `.nix` file, which
/// V3_TIMING rolls into the outer `run` bucket.
///
/// This aggregator splits that bucket so V3_TIMING reports compile
/// vs eval honestly.  It's required input to the Stage 9 / disk
/// cache decision: if inner-compile is 5 % of total, caching can
/// save at most 5 %; if it's 30 %, caching is a big lever.
///
/// Cheap: a handful of `uint64_t` accumulators bumped under the
/// existing `V3_TIMING` gate.  Zero overhead when V3_TIMING is
/// unset (the timer struct's `active` member short-circuits).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace nix::v3 {

struct ImportTimingTotals
{
    /// Number of primImport invocations that reached the
    /// parse/lower/compile path (i.e. not served from the in-memory
    /// content cache and not served from the disk cache).
    uint64_t calls = 0;
    /// Number of primImport invocations served from the in-memory
    /// importCache().results (skips parse, lower, compile).
    uint64_t resultCacheHits = 0;
    /// Number of primImport invocations served from the in-memory
    /// importCache().byContent (skips parse, lower, compile, but
    /// re-runs the CU).
    uint64_t contentCacheHits = 0;
    /// Number of primImport invocations served from the disk cache
    /// (deserializeCU, skips parse + lower + compile).
    uint64_t diskCacheHits = 0;

    /// Nanoseconds accumulated PER PHASE across all imports.
    uint64_t parseNs    = 0;  ///< parseExprFromFile / parseExprFromString
    uint64_t lowerNs    = 0;  ///< lowerNixExpr → ir::Module
    uint64_t optimiseNs = 0;  ///< ir::optimise (+ strictness + freeVars)
    uint64_t compileNs  = 0;  ///< compile(module) → CompilationUnit
    uint64_t runNs      = 0;  ///< run(*cu) — recursive eval of the imported file
    uint64_t diskLookupNs = 0; ///< disk_cache::lookup
    uint64_t diskInsertNs = 0; ///< disk_cache::insert + serialize
};

/// Returns the process-wide accumulator.  Single-threaded VM, so
/// no locking needed.
ImportTimingTotals & importTimingTotals() noexcept;

/// Whether the timer should run (cached env-var check, mirrors
/// `s_active()` in run.cc).  Inlined so primImport's hot path
/// skips the bump entirely when V3_TIMING is unset.
bool importTimingEnabled() noexcept;

} // namespace nix::v3
