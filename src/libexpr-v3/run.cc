/// @file
/// v3 root-expression entry point implementation.
///
/// Mirrors the pipeline open-coded in `cli/v3-eval.cc:430-438`.  Lifted
/// to a shared helper so the integrated `nix` CLI can use the same
/// path under v3-direct (inversion phase 1).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/run.hh"
#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"

#include "nix/expr/eval.hh"

namespace nix::v3 {

RootResult runRootExpr(nix::EvalState & state, nix::Expr * e)
{
    // Idempotent: register the builtin primop table on first call.
    // Safe to call per-invocation — the underlying registry is global
    // and de-duplicates by name.  The registration cost is constant
    // (one-time map fill) so per-call overhead is negligible.
    registerBuiltinPrimOps();

    // Wire the global tlNixEvalState pointer so v3 primops that need
    // to reach back into TW (e.g. `import`, derivation strict-merge,
    // store-side path operations) can find it.  Caller is responsible
    // for keeping `state` alive for the lifetime of any returned Bridge
    // thunks; see `primops.cc treeWalkerToV3` nFunction case for the
    // address-stability contract.
    setNixEvalState(&state);

    // Lower the AST → IR → bytecode.  `lowerNixExpr` requires `e` to
    // have had `bindVars` applied; the caller's contract.
    auto module = lowerNixExpr(e, state.symbols, state.positions);

    // computeFreeVars: populates each `ir::Function::freeVars` from
    // `Function::vars`.  Required before `compile` so the emitter
    // knows which upvalues each closure captures.
    ir::computeFreeVars(module);

    // Compile IR to bytecode.  The CompilationUnit owns
    // `stringConstants` referenced by OP_LIT_STR / OP_LIT_PATH; the
    // resulting Value's string/path payloads point into that vector.
    // Return the cu by-move so caller keeps it alive alongside the
    // Value.
    RootResult out{compile(module), Value{}};

    // Run.  STG-10 (vm.cc:5530) automatically routes through
    // `runOnExistingVm` if we're re-entered from another v3 dispatch
    // loop — so calling `runRootExpr` from inside a primop is safe.
    out.value = run(out.cu);
    return out;
}

} // namespace nix::v3
