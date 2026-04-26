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
        this->name = desc.name;
    }

    // Mark as bytecode proxy so OP_CALL_1 can detect v2 closures
    // without dynamic_cast (hot-path optimization matching isBytecodeThunk).
    isBytecodeProxy = true;
}

} // namespace nix
