#pragma once
/// @file
/// v3 root-expression entry point: parse-already-done → lower → compile
/// → run.
///
/// Wraps the pipeline that `v3-eval` and (under the inversion) the
/// integrated `nix` CLI both run.  The caller already has a parsed +
/// bind-vars'd `nix::Expr *`; this helper takes it from there to a
/// final v3 Value, with full primop registration + nix-EvalState
/// wiring done as a side-effect.
///
/// Use this rather than open-coding `lowerNixExpr → compile → run` so
/// the invocation stays the same across consumers.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0
#include "v3/value.hh"

namespace nix {
class Expr;
class EvalState;
}

namespace nix::v3 {

/// Lower, compile, and run an already-parsed Expr through v3.
///
/// Side-effects:
///   - calls `registerBuiltinPrimOps()` (idempotent — safe to call
///     repeatedly; the registry is global and de-duplicates).
///   - calls `setNixEvalState(&state)` so v3 primops that need to
///     reach back into TW (e.g. `import`, derivation strict-merge)
///     can find the nix::EvalState.
///   - allocates a fresh VMState internally (or reuses the active one
///     via STG-10 if we're being re-entered).
///
/// Returns the result Value in WHNF.  Caller is responsible for any
/// further forcing / printing / bridging.
///
/// Pre-conditions:
///   - `e` must already have had `bindVars` applied against the
///     EvalState's static base env.  Without that, references like
///     `builtins.foo` haven't been resolved to (level, displ) and
///     the lowerer will fail.
Value runRootExpr(EvalState & state, Expr * e);

} // namespace nix::v3
