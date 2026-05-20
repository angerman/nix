/// @file
/// V3-native callFlake — load + compile + run `call-flake.nix` on v3's
/// VM instead of delegating to TW's evaluator.
///
/// See `lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md` for the
/// architectural rationale.  TL;DR: post-#697 the bridge-side perf
/// gap is closed, but TW still EVALUATES call-flake.nix when v3
/// calls `builtins.getFlake`.  That's a V3-NATIVE violation: pure
/// Nix code (no FFI inside call-flake.nix) should run on v3's VM.
///
/// This file implements `v3::callFlakeV3(state, lockedFlake)` which:
///   1. Loads call-flake.nix from the canonical libflake source
///      (shared via the generated header).
///   2. Parses (via TW's `parseExprFromString` — parsing IS TW's
///      responsibility per V3-NATIVE; v3 wraps the AST → IR pipeline).
///   3. Lowers + compiles in v3 (cached on a static so subsequent
///      getFlake calls reuse the same CU).
///   4. Builds the args (vLocks, vOverrides, vFetchTreeFinal) by
///      mirroring libflake's `callFlake` body — these are TW Values.
///   5. Bridges the TW args to v3 Values via `treeWalkerToV3`.
///   6. Applies the 3-arg call-flake.nix lambda via `callClosure`.
///   7. Returns the resulting v3 Value.
///
/// The v3-compiled call-flake.nix internally calls
/// `import (outPath + "/flake.nix")`, which routes through v3's
/// `primImport` (post-#696 supports IFD too).  So the entire
/// post-FFI evaluation chain (call-flake.nix + each flake's
/// outputs lambda) runs on v3's VM.  TW handles only:
///   - `parseFlakeRef` (string → FlakeRef)
///   - `lockFlake` (fetch + write lockfile)
///   - `fetchTreeFinal` (fetch a single source)
///   - Parsing .nix file sources (TW owns the parser)
///
/// All of those are FFI leaves per the V3-NATIVE rule.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/primop.hh"
#include "v3/closure.hh"
#include "v3/lower.hh"
#include "v3/value.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/flake/flake.hh"
#include "nix/flake/settings.hh"

#include <mutex>

namespace nix::v3 {

namespace {

/// The canonical call-flake.nix source, generated from
/// src/libflake/call-flake.nix at build time (see meson.build).
constexpr const char * callFlakeSource =
#include "call-flake.nix.gen.hh"
    ;

/// Lazily-initialised v3-compiled call-flake.nix closure.  The CU is
/// kept alive via the importCache's `cus` deque (similar to
/// primImport's caching) so closures captured here remain valid.
///
/// First-call cost: parse + lower + compile (~tens of ms for this
/// 105-line file).  Subsequent calls: O(1) lookup.
///
/// Thread safety: called_once initialises atomically.  After that,
/// the Value is immutable (the CU is GC-rooted via cache).
struct CachedCallFlake {
    std::once_flag flag;
    Value closureValue;

    Value get(nix::EvalState & ns) {
        std::call_once(flag, [&] {
            // (1) Parse call-flake.nix as a Nix expression.  We use
            // the TW parser since v3 doesn't have its own.
            nix::Expr * e = ns.parseExprFromString(
                callFlakeSource, ns.rootPath("/«v3-call-flake»"));
            e->bindVars(ns, ns.staticBaseEnv);

            // (2) Lower + optimise + compile into a v3 CU.
            // TODO: actual implementation.  Skeleton currently
            // throws to surface that this path isn't wired yet.
            //
            // Steps once implemented:
            //   auto module = lowerNixExpr(e, ns.symbols, ns.positions);
            //   ir::optimise(module);
            //   ir::computeFreeVars(module);
            //   auto cu = std::make_unique<CompilationUnit>(compile(module));
            //   // Hold cu alive via a static or via importCache.
            //   closureValue = run(*cu);
            //   if (closureValue.tag() != Tag::Closure)
            //       throw std::runtime_error("call-flake.nix did not compile to a closure");

            (void)e;  // suppress unused warning until wired
            throw std::runtime_error(
                "v3::callFlakeV3: PHASE 1 SCAFFOLD — implementation pending "
                "(see lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md)");
        });
        return closureValue;
    }
};

CachedCallFlake g_cachedCallFlake;

} // namespace

/// Public entry point — invoked from `primGetFlake` (when the
/// `NIX_V3_NATIVE_CALL_FLAKE` gate is on; in a follow-up commit
/// this becomes the default).
///
/// `state.nixEvalState` must be wired.
///
/// Returns a v3 Value representing the flake's outputs attrset
/// (with `outputs`, `inputs`, `sourceInfo`, `outPath`, `_type`).
///
/// PHASE 1: skeleton — will throw a "scaffold pending" exception
/// when invoked.  Phase 2 fills in the body per the design doc.
Value callFlakeV3(EvalState & state, const nix::flake::LockedFlake & lockedFlake)
{
    if (!state.nixEvalState)
        throw std::runtime_error("v3::callFlakeV3: no TW EvalState wired");
    auto & ns = *state.nixEvalState;

    // PHASE 1: surface the cached-compile attempt so we know the
    // generated header + meson wiring is correct.  When this throws
    // "PHASE 1 SCAFFOLD", parse + bind succeeded; the rest of the
    // skeleton is what Phase 2 fills in.
    Value vCallFlake = g_cachedCallFlake.get(ns);

    // PHASE 2 work (sketched below):
    //
    //   auto [lockFileStr, keyMap] = lockedFlake.lockFile.to_string();
    //
    //   // Build TW args (replicate libflake/flake.cc:callFlake lines 932-969)
    //   nix::Value vLocks; vLocks.mkString(lockFileStr, ns.mem);
    //   nix::Value vOverrides = buildOverrides(state, lockedFlake, keyMap);
    //   auto * pFetchTreeFinal = nix::get(ns.internalPrimOps, "fetchFinalTree");
    //   if (!pFetchTreeFinal || !*pFetchTreeFinal)
    //       throw std::runtime_error("v3::callFlakeV3: fetchFinalTree primop missing");
    //
    //   // Bridge args to v3 (shallow)
    //   Value v3Locks       = treeWalkerToV3(state, vLocks);
    //   Value v3Overrides   = treeWalkerToV3(state, vOverrides);
    //   Value v3FetchFinal  = treeWalkerToV3(state, **pFetchTreeFinal);
    //
    //   // Apply args one at a time via the existing callClosure API.
    //   // We reuse the active VMState (stronger than spawning a fresh
    //   // one — avoids cross-VM thunk-Black-mark issues, per STG-10).
    //   VMState * vm = activeV3VM();
    //   if (!vm) throw std::runtime_error("v3::callFlakeV3: no active VMState");
    //   Value r1 = callClosure(*vm, vCallFlake, v3Locks);
    //   Value r2 = callClosure(*vm, r1, v3Overrides);
    //   Value r3 = callClosure(*vm, r2, v3FetchFinal);
    //   return r3;
    //
    // For Phase 1, surface the not-yet-implemented marker.
    (void)vCallFlake;
    (void)lockedFlake;
    throw std::runtime_error(
        "v3::callFlakeV3: PHASE 2 implementation pending — "
        "see lode/V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md for the plan");
}

} // namespace nix::v3
