#pragma once
/// @file
/// v3/run_internal.hh — shared internal declarations for the run.cc TU family.
///
/// The per-eval DIAGNOSTIC helpers that `runRootExprModule` (run.cc) still
/// invokes at the phase boundaries + end-of-eval, but whose bodies now live in
/// run_diag.cc.  All of these are COLD, run once per eval, and sit behind
/// `getenv` gates (V3_TIMING / NIX_VM_STATS), so out-of-lining them carries no
/// hot-path cost — the driver keeps only a lean linear
/// register → limits → lower → optimise → strictness → compile → run → bridge
/// spine.
///
/// Mirrors the vm.cc → vm_debug.cc / vm_interning.cc / vm_applied_cache.cc /
/// vm_values.cc split (see v3/vm_internal.hh): symbols that were file-local in
/// run.cc's anonymous namespace are promoted to external linkage here so the
/// driver in run.cc and the diagnostic bodies in run_diag.cc can share them.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nix::v3 {

/// Phase-timing helper: present iff `V3_TIMING` env var is set.  RAII —
/// constructed at the top of `runRootExprModule`, `mark`'d at each phase
/// boundary (lower / optimise / compile / run), and its destructor emits the
/// "v3-direct timing (ms)" line + the #769 per-import phase breakdown.  A
/// no-op (zero overhead, `active == false`) when V3_TIMING is unset.
///
/// The ctor (which reads + caches the V3_TIMING gate once per process) and the
/// dtor (the dump) are out-of-line in run_diag.cc; only the trivial `mark`
/// stays inline here since it is called at the driver's phase boundaries.
struct PhaseTimer {
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;
    bool active;
    TP start;
    double lower_ms = 0, compile_ms = 0, optimise_ms = 0, run_ms = 0;
    PhaseTimer();       ///< run_diag.cc — caches the V3_TIMING gate.
    ~PhaseTimer();      ///< run_diag.cc — emits the timing dump when active.
    void mark(double & accum)
    {
        if (!active) return;
        TP now = Clock::now();
        accum += std::chrono::duration<double, std::milli>(now - start).count();
        start = now;
    }
};

/// DIAG-4 (per DIAGNOSTIC_AUDIT §6.4): per-phase ALLOCATION snapshot.  Captures
/// `threadArena().bytesAllocated()` + per-Tag byte totals so the NIX_VM_STATS
/// dump can attribute v3 arena growth to lower / optimise / compile / run.
/// Snapshots are cheap (a small struct copy) and taken at each phase boundary.
struct PhaseAllocSnap {
    size_t arenaBytes;
    uint64_t bytesValues;
    uint64_t bytesClosures;
    uint64_t bytesThunks;
    uint64_t bytesBindings;
    uint64_t bytesLists;
    uint64_t bytesPairs;
    uint64_t bytesChars;
};

/// Snapshot the current allocation counters into a `PhaseAllocSnap`.
/// (run_diag.cc)
PhaseAllocSnap takePhaseSnap();

/// The five phase-boundary snapshots the driver threads into `dumpVmStats`.
struct PhaseSnaps {
    PhaseAllocSnap start;
    PhaseAllocSnap afterLower;
    PhaseAllocSnap afterOptimise;
    PhaseAllocSnap afterCompile;
    PhaseAllocSnap afterRun;
};

/// NIX_VM_STATS=1 end-of-eval alloc/memory dump — the large cold diagnostic
/// block (per-phase arena bytes, per-Tag/-site attribution, RSS decomposition,
/// nursery survival, opcounts, IFD probes, the elsewhere-probe estimator,
/// free-list / immix / allocChars stats, etc.).  Called once (gated by the
/// caller) at the end of a successful `runRootExprModule`.  (run_diag.cc)
void dumpVmStats(const PhaseSnaps & snaps);

/// NIX_VM_STATS=1 ABORT dump — emitted (gated by the caller) when `run()`
/// throws, before the exception is re-thrown, so bounded-time probes can still
/// collect thunksForced / insns / per-site v3ToTreeWalker / IFD-probe counts.
/// (run_diag.cc)
void dumpVmAbortStats();

} // namespace nix::v3
