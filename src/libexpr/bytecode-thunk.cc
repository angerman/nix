/// @file
/// Implementation of ExprBytecodeThunk and ExprLambdaBytecode.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode-thunk.hh"
#include "nix/expr/bytecode.hh"
#include "nix/expr/vm.hh"
#include "nix/expr/eval.hh"

namespace nix {

// ---------------------------------------------------------------------------
// ExprBytecodeThunk
// ---------------------------------------------------------------------------

void ExprBytecodeThunk::eval(EvalState & state, Env & env, Value & v)
{
    auto & desc = unit->thunks[thunkIdx];
    uint32_t offset = desc.codeOffset;
    // Track bytecoded thunk forcings for profiling.
    if (state.vmState)
        state.vmState->nrBytecodeThunkForces++;

    // v2 thunks store upvalues inline starting at env.values[1].
    Value ** upvalues = nullptr;
    if (desc.nUpvalues > 0)
        upvalues = &env.values[1];

    bytecode::vmExec(state, *unit, offset, env, v, upvalues);
}

void ExprBytecodeThunk::show(const SymbolTable & symbols, std::ostream & str) const
{
    str << "«bytecode-thunk@" << unit->thunks[thunkIdx].codeOffset << "»";
}

PosIdx ExprBytecodeThunk::getPos() const
{
    return unit->thunks[thunkIdx].pos;
}


// ---------------------------------------------------------------------------
// ExprLambdaBytecode
// ---------------------------------------------------------------------------

ExprLambdaBytecode::ExprLambdaBytecode(
    bytecode::CompilationUnit * unit,
    uint32_t lambdaIdx)
    : ExprLambda(
        unit->lambdas[lambdaIdx].pos,
        unit->lambdas[lambdaIdx].arg,
        nullptr)  // body set later by OP_MAKE_CLOSURE
    , unit(unit)
    , lambdaIdx(lambdaIdx)
{
    auto & desc = unit->lambdas[lambdaIdx];

    // Copy ALL ExprLambda fields from the original so that callFunction()
    // sees the correct formals, name, arg, ellipsis, etc.
    // We do this by memcpy'ing the ExprLambda portion from the original,
    // then overriding just the body pointer.
    if (desc.sourceExpr) {
        auto * orig = desc.sourceExpr;
        // Copy the ExprLambda base: pos, name, arg, hasFormals, ellipsis,
        // nFormals, formalsStart, body, docComment.
        // The ExprLambda fields start right after the Expr base class.
        std::memcpy(
            static_cast<ExprLambda *>(this),
            static_cast<ExprLambda *>(orig),
            sizeof(ExprLambda));
        // body will be overridden by OP_MAKE_CLOSURE after construction.
    } else {
        // Disk-cache load path: no AST.  Reconstruct just enough of the
        // ExprLambda public surface so `getFormals()`, `builtins.functionArgs`,
        // and `intersectAttrs` answer correctly.  The bytecode prologue
        // already binds formals at runtime; this is purely for runtime
        // introspection (issue #159 root cause: without setting hasFormals
        // here, functionArgs returned `{}` for cached formals lambdas,
        // causing callPackage to invoke them with `{}` and the prologue
        // then failed with `attribute 'X' missing`).
        this->name = desc.name;
        if (desc.sourceHasFormals) {
            // Allocate a Formal[] for the ExprLambda's formalsStart pointer.
            // GC-allocated since the lambda may outlive the CompilationUnit
            // in some flows; trivial Formal layout makes this safe under
            // Boehm.  `def` is set to a non-null sentinel for formals that
            // had a default in the source — this is the bit `functionArgs`
            // and `callPackageWith` care about.  The actual default value
            // is bound by the bytecoded prologue, never via `def->maybeThunk`.
            uint16_t n = static_cast<uint16_t>(desc.sourceFormals.size());
            // Allocated via Boehm GC so it survives until the proxy
            // ExprLambdaBytecode itself is reclaimed.  The CompilationUnit
            // is GC-traced; we don't need a separate ownership story.
            auto * arr = new (GC) Formal[n];
            // Sentinel non-null Expr* for "has default".  Cast a function
            // pointer through reinterpret_cast to a clearly-bogus address
            // — any non-null value works since the only reader is
            // `state.getBool(formal.def)` (bool conversion).
            static char defSentinel = 0;
            for (uint16_t i = 0; i < n; ++i) {
                auto sIdx = desc.sourceFormals[i].first;
                bool hasDef = desc.sourceFormals[i].second;
                arr[i].pos  = noPos;
                arr[i].name = unit->symbols.at(sIdx);
                arr[i].def  = hasDef ? reinterpret_cast<Expr *>(&defSentinel) : nullptr;
            }
            setBytecodeFormals(desc.sourceFormalsEllipsis, arr, n);
        }
    }

    // Mark as bytecode proxy so OP_CALL_1 can detect v2 closures
    // without dynamic_cast (hot-path optimization matching isBytecodeThunk).
    isBytecodeProxy = true;
}

} // namespace nix
