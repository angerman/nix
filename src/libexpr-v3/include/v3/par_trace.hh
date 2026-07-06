#pragma once
/// @file
/// Parallel-potential trace instrument for the v3 evaluator.
///
/// **Purpose**: quantify the Amdahl ceiling on *intra-eval* parallelism
/// — the load-bearing "will parallelism help" measure-first input for
/// `lode/PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` §8 (proposed
/// measurement spike) and §5 (the Amdahl-cap hypothesis).
///
/// Nix eval is a lazy fork-join dependency DAG: forcing a thunk T
/// triggers forcing the thunks T depends on (its "children" — the
/// sub-values T needs to reach WHNF).  The idealized parallel speedup
/// ceiling on an ∞-core machine is the classic **work / span**:
///
///   - **work**  = Σ over all thunk-forces of that force's SELF cost
///                 (cost excluding nested child forces).
///   - **span**  = longest dependency chain (critical path):
///                 span(T) = self(T) + max over children C of span(C).
///   - **ceiling speedup = work / span**.
///       work/span ≈ 1  → fully serial (parallelism useless).
///       work/span = K  → an ∞-core machine could go K× faster;
///                         a real 8-core ≈ min(K, 8) minus overhead.
///
/// This is an IDEALIZED upper bound: it ignores synchronization, the
/// GC lock, FFI serialization, and per-spark overhead — all of which
/// the §5 table says cap real wins well below the ideal.  Treat the
/// number as a ceiling, not a forecast.
///
/// ## Two cost models
///
///   - COUNT-based (headline, robust): self = 1 per force that did
///     real work; work = #forces; span = deepest force-chain depth.
///     A memoized/blackholed hit is a ~zero-cost leaf (self = 0) — this
///     matters because sharing/memoization REMOVES parallelism (a
///     shared thunk is forced once, then reused).
///   - OP-weighted (secondary): self = #opcodes dispatched while this
///     force is top-of-stack (excluding nested forces' opcodes).  A
///     cost-weighted ceiling.
///
/// ## Mechanism
///
/// A `thread_local` stack of "currently-forcing" frames, each anchored
/// to the VM CallFrame index of the CFF_THUNK_RETURN frame that drives
/// the force (both the C-recursive `forceValue` path and the
/// frame-based `op_force_slow` path push such a frame — this anchor is
/// path-independent).  Reconciling against `vm.frames.size()` on entry
/// makes exception unwinds (tryEval, blackhole-as-value) self-healing:
/// stale frames whose VM frame has already been popped are folded away
/// before the next force pushes.
///
/// ## Correctness contract (byte-id neutral)
///
/// The instrument ONLY accumulates counters; it never touches Values,
/// thunk state, or control flow.  A drvPath computed with
/// `NIX_V3_PAR_TRACE` set MUST be byte-identical to one computed with
/// it unset.  All hooks are `inline` no-ops when the gate is off (a
/// single cached-bool branch, `__builtin_expect(..., 0)`).
///
/// ## Retirement criterion
///
/// This is a one-shot measurement spike for the parallel-eval GO/NO-GO
/// decision (PARALLEL_EVAL_CAPABILITIES §8 / §9 falsification table).
/// Once the parallel-potential question is decided (commit or kill the
/// multi-core candidate), DELETE this module and the `NIX_V3_PAR_TRACE`
/// gate — it has no production role.  Per Rule 0, it must not linger as
/// a permanent opt-in gate.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>

namespace nix::v3::partrace {

/// Cached gate.  True iff `NIX_V3_PAR_TRACE` is set in the environment.
/// Read once (function-local static); the hot hooks branch on it.
bool enabled() noexcept;

/// Force ENTER: a Suspended thunk just transitioned to Blackhole and a
/// CFF_THUNK_RETURN frame is about to be (or has just been) pushed.
/// `vmFrameIdx` = the index that frame occupies in `vm.frames` (i.e.
/// the size of `vm.frames` at the push).  `curVmFrames` = the current
/// `vm.frames.size()` used to reconcile away any stale frames left by a
/// prior exception unwind.
void enterForce(size_t vmFrameIdx, size_t curVmFrames) noexcept;

/// Force EXIT: a Blackhole thunk just transitioned to Evaluated at
/// OP_RETURN's CFF_THUNK_RETURN branch.  `poppedVmFrameIdx` = the index
/// of the frame being popped (== its position in `vm.frames`).  Pops
/// every force-frame at or above that index (there should be exactly
/// one under normal LIFO discipline, but reconcile defensively),
/// computing each frame's span and folding it into its parent.
void exitForce(size_t poppedVmFrameIdx) noexcept;

/// Memo HIT: a force landed on an already-Evaluated thunk (or was
/// otherwise resolved with no body execution).  A ~zero-cost leaf that
/// contributes no span — but is counted so we can report the memo-hit
/// fraction (the share of forces that sharing/memoization removed from
/// the parallel-work pool).
void memoHit() noexcept;

/// Per-opcode tick for the OP-weighted cost model.  Called once per
/// dispatched opcode; credits the innermost currently-forcing frame's
/// self-op counter.  No-op if no force is in progress (top-level code
/// outside any thunk body).
void opTick() noexcept;

/// Emit the end-of-run report to stderr.  No-op unless the gate is on.
/// Called from run.cc's diagnostics block (next to dumpV3LiveFraction).
void dumpReport() noexcept;

}  // namespace nix::v3::partrace
