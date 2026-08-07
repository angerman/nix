#pragma once
/// @file
/// Per-site IFD (import-from-derivation) tracing instrument for the v3
/// evaluator — the Phase-1 falsifier for the IFD frontier (PROGRESSION_PLAN
/// 2026-08-07 §3).
///
/// **Purpose**: route the IFD decision tree. WS-2 already gives default-on
/// aggregate IFD visibility (candidate counts + realise-blocked total wall,
/// `run.cc emitIfdEndOfEvalSummary`). This instrument adds, GATED behind
/// `NIX_V3_IFD_TRACE=1`, the five per-realise signals Phase 2 (concurrency)
/// and Phase 3 (cache) live or die on:
///
///   1. **Per-site attribution** — the nearest source `file:line:col` of the
///      lambda body that triggered each realise (from the current VM frame's
///      `LambdaDescriptor::posHandle`, same mechanism as forcerate_trace).
///   2. **Arguments** — the derivation / store-path string handed to
///      `realisePath` (what is being built).
///   3. **Per-realise wall-time** — each realise timed individually (WS-2's
///      `ifdRealiseNanos` is only a SUM).
///   4. **Cache/build signal** — did the realise return fast (already
///      valid / substituted / v1-cache-served) or slow (a real build), plus
///      whether it threw (build unavailable — the sandbox case). The
///      definitive built-vs-substituted split still needs a real store /
///      `profile-import-from-derivation` on the deploy; per-realise wall +
///      the repeat-arg rate here size the Phase-3 prize structurally.
///   5. **Independence signal** — the realise NESTING depth at entry. A
///      realise entered while another realise is already in flight
///      (depth > 0) is STRICTLY data-dependent on the outer one (the outer
///      build's output was needed to reach it — "must sequence"). A realise
///      entered at depth 0 (no realise in flight) is a candidate for overlap
///      ("independent-candidate"). depth-0 is an UPPER BOUND on the
///      independent set (two sequential depth-0 realises can still be
///      data-dependent through ordinary eval between them); nested is a sound
///      LOWER bound on the dependent set. This is the number Phase 2 needs.
///
/// ## Correctness contract (byte-id neutral)
///
/// The instrument ONLY reads/counts: a source position, the realise arg
/// string, a steady_clock delta, and a thread-local depth counter. It never
/// touches Values, thunk state, the store, or control flow. A drvPath
/// computed with `NIX_V3_IFD_TRACE` set MUST be byte-identical to one
/// computed with it unset. Each `RealiseScope` is emplaced into a
/// `std::optional` ONLY when `enabled()`, so the OFF path constructs nothing
/// (no arg-string copy, no position walk) — a single cached-bool branch.
///
/// ## Retirement criterion (Rule 0 / repo rule 4)
///
/// This is a one-shot measurement spike for the Phase-1 IFD routing decision.
/// Once that decision is recorded (GO-Phase-2 / GO-Phase-3 / KILL), DELETE
/// this module, the `RealiseScope` emplacements at the realise sites in
/// primops.cc, the `ifdtrace::dumpReport()` call in run.cc, and the
/// `NIX_V3_IFD_TRACE` gate. It has no production role and must not linger as
/// a permanent opt-in gate. (WS-2's default-on aggregate summary STAYS — this
/// only removes the gated per-realise detail.)
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>

namespace nix::v3::ifdtrace {

/// Cached gate. True iff `NIX_V3_IFD_TRACE` is set. Read once
/// (function-local static); the realise-site branch tests it.
bool enabled() noexcept;

/// RAII scope wrapping a single `realisePath` call. Construct it (via a
/// `std::optional<RealiseScope>::emplace`, guarded by `enabled()`) IMMEDIATELY
/// before the realise, next to the existing WS-2 `IfdRealiseTimer`. On
/// construction it captures the callsite + arg + entry nesting-depth and
/// starts the timer; on destruction (incl. exception unwind) it records the
/// per-realise wall-time, whether the realise threw, and appends one trace
/// record. Non-overlapping with `IfdRealiseTimer` in placement discipline
/// (caller-level, not inside `ffi::realisePath`) so realises are not
/// double-counted.
struct RealiseScope
{
    /// `kind` is an `IfdProbeKind` (bytecode.hh); `arg` is the realise
    /// argument (derivation / store-path string). The arg is copied + the
    /// callsite walked here (both allocate), so only construct this when
    /// `enabled()`. Not `noexcept` (an OOM under the measurement flag should
    /// propagate, not terminate); the OFF path never constructs one.
    RealiseScope(uint8_t kind, std::string arg);
    ~RealiseScope();

    RealiseScope(const RealiseScope &) = delete;
    RealiseScope & operator=(const RealiseScope &) = delete;

  private:
    uint64_t                              seq_;
    uint8_t                               kind_;
    uint32_t                              depthAtEntry_;
    uint64_t                              parentSeq_;   // seq of enclosing realise; 0 if depth 0
    std::string                           arg_;
    std::string                           site_;
    std::chrono::steady_clock::time_point t0_;
    int                                   excBase_;
};

/// Emit the end-of-eval structured IFD trace report to stderr: one line per
/// realise (seq, kind, depth-class, ms, threw, site, arg) plus a summary
/// (counts, independent-candidate vs must-sequence wall split, repeat-arg
/// rate). No-op unless `enabled()` and at least one realise was recorded.
/// Called from run.cc next to forcerate::dumpReport(); cumulative +
/// last-report-per-process authoritative (mirrors the other trace dumps).
void dumpReport() noexcept;

}  // namespace nix::v3::ifdtrace
