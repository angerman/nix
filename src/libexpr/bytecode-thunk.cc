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
    uint32_t offset = unit->thunks[thunkIdx].codeOffset;
    bytecode::vmExec(state, *unit, offset, env, v);
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
        nullptr)  // body set below
    , unit(unit)
    , lambdaIdx(lambdaIdx)
{
    auto & desc = unit->lambdas[lambdaIdx];

    // Set name for profiling and error messages.
    this->name = desc.name;

    // The body pointer will be set to an ExprBytecodeThunk by the
    // bytecode compiler, after this object is allocated.  We can't
    // create the thunk here because it would require the BumpMemoryResource
    // allocator which we don't have in this constructor.
    //
    // The compiler will do:
    //   auto * proxy = exprs.add<ExprLambdaBytecode>(unit, lambdaIdx);
    //   auto * bodyThunk = exprs.add<ExprBytecodeThunk>(unit, bodyThunkIdx);
    //   proxy->body = bodyThunk;
}

} // namespace nix
