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
#include "v3/bytecode.hh"

#include <memory>
#include <string>

namespace nix {
struct Expr;
class EvalState;
struct SourcePath;  // for runRootExprFromString's origin (by pointer)
}

namespace nix::v3 {
namespace ir { struct Module; }

/// Lower, compile, and run an already-parsed Expr through v3.
///
/// Lifetime: the result Value contains pointers into the
/// CompilationUnit's `stringConstants` (for OP_LIT_STR / OP_LIT_PATH
/// literals).  Caller MUST keep the returned `cu` alive for as long as
/// the Value is reachable; otherwise string/path payloads dangle.
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
/// Returns the compiled CompilationUnit and the result Value (in WHNF).
/// Caller is responsible for any further forcing / printing / bridging.
///
/// Pre-conditions:
///   - `e` must already have had `bindVars` applied against the
///     EvalState's static base env.  Without that, references like
///     `builtins.foo` haven't been resolved to (level, displ) and
///     the lowerer will fail.
///
/// #676 — the `cu` is held by `std::unique_ptr` (NOT by value) so its
/// address is stable across moves.  Closures created during `run()`
/// store `c->cu = &cu` at emit time; if RootResult itself were moved
/// (e.g. via `std::optional::emplace`), every embedded closure's `cu`
/// pointer would dangle.  Heap-allocating the CU and moving the
/// unique_ptr (not the CU) preserves the contract.
struct RootResult {
    std::unique_ptr<CompilationUnit> cu;
    Value value;
};
/// Run an already-LOWERED v3 IR module (the native path: lowerV3Ast →
/// here, with no nix::Expr).  Same side-effects + lifetime contract as
/// runRootExpr.  registerBuiltinPrimOps() must have run before lowering
/// (lower-time findPrimOp); this repeats the idempotent setup.
RootResult runRootExprModule(nix::EvalState & state, ir::Module module);

/// Native parse+lower+run from raw `.nix` source (no nix::Expr) — the
/// top-level entry for the CLI + any caller that has source text.
/// `basePath`/`homePath` resolve relative / `~` path literals.
/// `originPath` selects the source's position origin: non-null → a file
/// (`Pos::Origin(*originPath)`); null → an in-memory string
/// (`Pos::String`).  Positions then match TW.  Throws on a (provably-
/// impossible for parsed source) canLowerV3 miss.  (Takes the SourcePath
/// by pointer so run.hh needs no `nix/...` position header.)
RootResult runRootExprFromString(nix::EvalState & state, const std::string & source,
                                 const std::string & basePath, const std::string & homePath,
                                 const nix::SourcePath * originPath);

/// Convenience overload for SYNTHETIC sources (no relative/`~` path
/// literals) — e.g. the bytecode-primop wrapper installer.  Builds a
/// Pos::String origin internally + empty base/home, so the caller needs
/// no eval.hh / parser / position headers — just run.hh.
RootResult runRootExprFromString(nix::EvalState & state, const std::string & source);

} // namespace nix::v3
