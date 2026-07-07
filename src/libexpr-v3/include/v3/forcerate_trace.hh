#pragma once
/// @file
/// Per-creation-site force-rate histogram instrument for the v3
/// evaluator.
///
/// **Purpose**: the Rule-0 measure-first gate for "cheap-eagerness /
/// optimistic evaluation" (LITERATURE_SWEEP_2026-07-07 §2). v3 measures
/// 62-68% of thunks NEVER forced. Blind speculation would waste work on
/// those. Cheap-eagerness / optimistic-eval (Faxén; Ennals & PJ ICFP'03)
/// only pays if the per-CREATION-SITE force-rate is BIMODAL — some sites
/// always forced (→ speculate), some never (→ keep lazy) — so a per-site
/// classifier (static) or a per-site runtime profiler (adaptive) can sort
/// them without wasting work. This instrument measures that distribution.
///
/// ## What a "site" is
///
/// A creation site = the `OP_MAKE_THUNK` bytecode location, identified by
/// the `LambdaDescriptor *` the thunk is stamped with at creation
/// (`t->suspended.desc = &cu->lambdas[funcIdx]`). A given (CU, funcIdx)
/// maps to exactly one descriptor, so the descriptor pointer is a stable,
/// O(1) site key; `desc->cu`, `desc->codeOffset`, `desc->name`, and
/// `desc->posHandle` give human-readable attribution.
///
/// ## What is counted, per site
///
///   - **created** — bumped at `OP_MAKE_THUNK` for this descriptor.
///   - **forced**  — bumped at the Suspended→Blackhole transition (the
///                   FIRST force to WHNF). That transition is guarded
///                   (blackhole cycle-detect), so it fires exactly once
///                   per thunk lifetime — NOT once per force attempt, and
///                   NOT on memo-hits of an already-Evaluated thunk. So
///                   `forced` counts distinct first-forces.
///
/// Site force-rate = forced / created. The report buckets sites by
/// force-rate (created-weighted) and reports the aggregates the go/no-go
/// needs (bimodality, never-forced-at-low-rate-sites mass, always-forced
/// RHS-cheapness mass, total created/forced cross-check).
///
/// ## Correctness contract (byte-id neutral)
///
/// The instrument ONLY accumulates counters keyed by a descriptor
/// pointer; it never touches Values, thunk state, or control flow. A
/// drvPath computed with `NIX_V3_FORCERATE_TRACE` set MUST be byte-
/// identical to one computed with it unset. All hooks are `inline`
/// no-ops when the gate is off (a single cached-bool branch,
/// `__builtin_expect(..., 0)`).
///
/// ## Retirement criterion
///
/// This is a one-shot measurement spike for the cheap-eagerness /
/// optimistic-eval GO/NO-GO decision (LITERATURE_SWEEP_2026-07-07 §2's
/// Rule-0 histogram gate). Once that lever is decided (commit or kill
/// cheap-eagerness / optimistic-eval), DELETE this module and the
/// `NIX_V3_FORCERATE_TRACE` gate — it has no production role. Per Rule 0,
/// it must not linger as a permanent opt-in gate.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>

namespace nix::v3 {
struct LambdaDescriptor;
struct CompilationUnit;
}  // namespace nix::v3

namespace nix::v3::forcerate {

/// Cached gate. True iff `NIX_V3_FORCERATE_TRACE` is set in the
/// environment. Read once (function-local static); the hot hooks branch
/// on it.
bool enabled() noexcept;

/// Thunk CREATED at an `OP_MAKE_THUNK` whose funcIdx points at `desc`.
/// `cu` is the CU that owns the creating bytecode (== desc->cu, passed
/// explicitly so the hook needn't chase the mutable backpointer). No-op
/// unless the gate is on.
void created(const LambdaDescriptor * desc,
             const CompilationUnit * cu) noexcept;

/// First FORCE of a thunk: the Suspended→Blackhole transition, where
/// `desc` == `t->suspended.desc`. Fires exactly once per thunk lifetime.
/// No-op unless the gate is on.
void forcedFirst(const LambdaDescriptor * desc) noexcept;

/// Emit the end-of-run histogram + aggregates to stderr. No-op unless the
/// gate is on. Called from run.cc / v3-eval.cc diagnostics blocks (next
/// to dumpV3LiveFraction / partrace::dumpReport).
void dumpReport() noexcept;

}  // namespace nix::v3::forcerate
