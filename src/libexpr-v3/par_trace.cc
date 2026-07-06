/// @file
/// Parallel-potential trace instrument — implementation.
///
/// See par_trace.hh for the work/span model and the byte-id-neutral
/// correctness contract.  Retirement: delete once the parallel-eval
/// GO/NO-GO is decided (PARALLEL_EVAL_CAPABILITIES_2026-05-18 §8/§9).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/par_trace.hh"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nix::v3::partrace {

namespace {

/// One "currently forcing" frame on the per-thread force stack.
struct ForceFrame
{
    /// The VM CallFrame index this force is anchored to.  Used to
    /// reconcile against `vm.frames.size()` so exception unwinds don't
    /// desynchronize the force stack.
    size_t   vmFrameIdx;
    /// COUNT-model span accumulator: the max span among children forced
    /// during this force's body.  This force's span = 1 + childSpanMax.
    uint64_t childSpanMax = 0;
    /// OP-model self cost: opcodes dispatched while this frame is the
    /// innermost (top of the force stack).
    uint64_t selfOps = 0;
    /// OP-model span accumulator: the max op-span among children.  This
    /// force's op-span = selfOps + childOpSpanMax.
    uint64_t childOpSpanMax = 0;
};

/// Per-thread aggregate accumulators + the live force stack.  All
/// thread_local: the instrument is single-threaded today (v3 eval is
/// single-threaded), and per-thread state keeps it correct if a future
/// caller re-enters on another thread.  Kept out of a struct so the
/// hot-path increments touch plain thread_locals directly.
thread_local std::vector<ForceFrame> g_stack;

// COUNT model ---------------------------------------------------------
/// Total real-work forces (self = 1 each).  This is the parallel "work".
thread_local uint64_t g_workForces = 0;
/// Span (critical-path depth) of the deepest completed root force, and
/// the running max across all top-level (depth-0) forces in the run.
/// The eval as a whole is a sequence of top-level forces; the run's
/// span is the SUM of the top-level force spans (they run one after
/// another at the root), while work is the SUM of all work.  We track
/// both the per-root spans (summed into g_spanForces) so work/span
/// reflects the whole run's fork-join structure.
thread_local uint64_t g_spanForces = 0;
/// Deepest single force-chain observed (max span of any one root force)
/// — reported as "deepest-chain length".
thread_local uint64_t g_deepestChain = 0;

// OP model ------------------------------------------------------------
thread_local uint64_t g_workOps = 0;   // Σ selfOps over all forces
thread_local uint64_t g_spanOps = 0;   // Σ op-span over top-level forces
thread_local uint64_t g_deepestOpChain = 0;

// Diagnostics ---------------------------------------------------------
/// Forces that hit an already-Evaluated thunk (near-zero-cost leaves).
/// Sharing/memoization removed these from the parallel-work pool.
thread_local uint64_t g_memoHits = 0;
/// Peak depth of the force stack (deepest simultaneous nesting).
thread_local size_t   g_peakStackDepth = 0;
/// Number of top-level (depth-0) root forces.  A drvPath eval is driven
/// by top-level bytecode (not a single enclosing thunk), so it emits
/// MANY sequential root forces.  These roots are themselves mutually
/// independent, so a second, more-optimistic ceiling treats them as
/// parallelizable too: work / (deepest single chain).  Reporting the
/// count lets the reader see which regime the workload is in.
thread_local uint64_t g_rootForces = 0;

}  // namespace

bool enabled() noexcept
{
    // Retirement criterion (see par_trace.hh): delete this gate once the
    // parallel-eval candidate is decided.  Diagnostic-only, byte-id
    // neutral — never affects eval results.
    static const bool s_on = std::getenv("NIX_V3_PAR_TRACE") != nullptr;
    return s_on;
}

void enterForce(size_t vmFrameIdx, size_t curVmFrames) noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;

    // Reconcile: fold away any force-frames whose VM frame has already
    // been unwound (by an exception that reverted Blackhole→Suspended
    // WITHOUT a clean OP_RETURN exit — tryEval, blackhole-as-value).
    // Such a frame's `vmFrameIdx >= curVmFrames`, i.e. it sits at or
    // above the current top of the VM frame stack, so it can't be a live
    // ancestor of the force we're about to enter.  Discard its partial
    // accounting (an aborted force does no completed work) but keep the
    // parent chain consistent.
    while (!g_stack.empty() && g_stack.back().vmFrameIdx >= curVmFrames)
        g_stack.pop_back();

    g_stack.push_back(ForceFrame{ .vmFrameIdx = vmFrameIdx });
    if (g_stack.size() > g_peakStackDepth) g_peakStackDepth = g_stack.size();
}

void exitForce(size_t poppedVmFrameIdx) noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    if (g_stack.empty()) return;

    // Under normal LIFO discipline the frame being popped is the top of
    // the force stack.  Pop every frame at or above the popped VM frame
    // index (defensive: reconciles any residual stale frames), folding
    // each completed force's span/work into the run + parent.
    while (!g_stack.empty() && g_stack.back().vmFrameIdx >= poppedVmFrameIdx) {
        ForceFrame f = g_stack.back();
        g_stack.pop_back();

        // COUNT model: this force's self cost is 1; its span is
        // 1 + max child span.
        uint64_t span   = 1 + f.childSpanMax;
        // OP model: self is the opcodes it dispatched directly; its
        // op-span is selfOps + max child op-span.
        uint64_t opSpan = f.selfOps + f.childOpSpanMax;

        g_workForces += 1;
        g_workOps    += f.selfOps;

        if (!g_stack.empty()) {
            // Fold into the parent's child-span accumulators.
            auto & parent = g_stack.back();
            if (span   > parent.childSpanMax)   parent.childSpanMax   = span;
            if (opSpan > parent.childOpSpanMax) parent.childOpSpanMax = opSpan;
        } else {
            // Top-level (root) force: its span/op-span is a segment of
            // the whole run's critical path.  Root forces execute
            // sequentially at the top, so the SERIAL-ROOT span is their
            // sum; the deepest single chain is the ceiling if the roots
            // are treated as parallel too (they are independent).
            ++g_rootForces;
            g_spanForces += span;
            g_spanOps    += opSpan;
            if (span   > g_deepestChain)   g_deepestChain   = span;
            if (opSpan > g_deepestOpChain) g_deepestOpChain = opSpan;
        }
    }
}

void memoHit() noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    // A hit on an already-Evaluated thunk: zero self cost, zero span,
    // no children.  It contributes nothing to work OR span — it's the
    // parallelism that sharing/memoization already removed.  We only
    // count it so the report can show the memo-hit fraction.
    ++g_memoHits;
}

void opTick() noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    if (!g_stack.empty()) ++g_stack.back().selfOps;
}

void dumpReport() noexcept
{
    if (__builtin_expect(!enabled(), 1)) return;
    // Self-gate on activity (mirrors appliedCacheStatsDump): runRootExpr
    // is called recursively by primop-bytecode installers, so this fires
    // on every module exit.  Suppress the empty installer reports so only
    // reports that did real forcing print.  Counters are CUMULATIVE and
    // never reset, so the LAST printed report per process is authoritative.
    if (g_workForces == 0) return;

    uint64_t work     = g_workForces;
    uint64_t span     = g_spanForces;
    uint64_t memoHits = g_memoHits;
    uint64_t totalForceAttempts = work + memoHits;

    double wsCount = span > 0 ? (double)work / (double)span : 0.0;
    double wsOps   = g_spanOps > 0 ? (double)g_workOps / (double)g_spanOps : 0.0;
    double memoFrac = totalForceAttempts > 0
        ? (double)memoHits / (double)totalForceAttempts : 0.0;
    // Optimistic ceiling: if the (independent) root forces are ALSO run
    // in parallel, the critical path collapses to the single deepest
    // force-chain.  This is the true ∞-core bound for a driver that
    // launches many independent root forces (the drvPath shape).
    double wsCountRootPar = g_deepestChain > 0
        ? (double)work / (double)g_deepestChain : 0.0;
    double wsOpsRootPar = g_deepestOpChain > 0
        ? (double)g_workOps / (double)g_deepestOpChain : 0.0;

    std::fprintf(stderr,
        "\n=== v3 PARALLEL-POTENTIAL TRACE (NIX_V3_PAR_TRACE) ===\n"
        "  work/span model — idealized ∞-core fork-join ceiling.\n"
        "  NOTE: this IGNORES sync / GC-lock / FFI serialization; real\n"
        "        8-core wins are min(work/span, 8) MINUS that overhead.\n"
        "\n"
        "  root forces (top-level, independent) : %llu\n"
        "\n"
        "  COUNT model (self = 1 per real-work force; robust headline):\n"
        "    work  (total real-work forces) : %llu\n"
        "    span  A: serial-roots (Σ root spans) : %llu\n"
        "      work/span  (CEILING, serial roots) : %.3f\n"
        "    span  B: deepest single chain (roots parallel) : %llu\n"
        "      work/span  (CEILING, roots parallel) : %.3f\n"
        "\n"
        "  OP-weighted model (self = opcodes dispatched, excl. nested):\n"
        "    work  (total self-ops)         : %llu\n"
        "    span  A: serial-roots (op)     : %llu\n"
        "      work/span  (CEILING, op-wtd, serial roots) : %.3f\n"
        "    span  B: deepest op-chain (roots parallel) : %llu\n"
        "      work/span  (CEILING, op-wtd, roots parallel) : %.3f\n"
        "\n"
        "  Sharing / memoization:\n"
        "    memo-hits (forces on Evaluated): %llu\n"
        "    total force attempts           : %llu\n"
        "    memo-hit fraction              : %.4f  (parallelism sharing removed)\n"
        "    peak force-stack nesting depth : %zu\n"
        "=== end parallel-potential trace ===\n\n",
        (unsigned long long)g_rootForces,
        (unsigned long long)work,
        (unsigned long long)span,
        wsCount,
        (unsigned long long)g_deepestChain,
        wsCountRootPar,
        (unsigned long long)g_workOps,
        (unsigned long long)g_spanOps,
        wsOps,
        (unsigned long long)g_deepestOpChain,
        wsOpsRootPar,
        (unsigned long long)memoHits,
        (unsigned long long)totalForceAttempts,
        memoFrac,
        g_peakStackDepth);
    std::fflush(stderr);
}

}  // namespace nix::v3::partrace
