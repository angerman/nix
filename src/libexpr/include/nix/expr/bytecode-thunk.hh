#pragma once
/// @file
/// Bridge types between the AST (tree-walking) world and the bytecode VM.
///
/// ExprBytecodeThunk is an Expr subclass that, when eval()'d, dispatches
/// into the bytecode VM rather than tree-walking.  This allows gradual
/// migration: some thunks can be bytecoded while others remain AST-based.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/nixexpr.hh"

namespace nix {

namespace bytecode {
struct CompilationUnit;
} // namespace bytecode


/// A thunk body that dispatches to bytecoded evaluation.
///
/// Allocated in BumpMemoryResource (same arena as all other Expr nodes).
/// When forceValue() encounters a thunk whose expr is an ExprBytecodeThunk,
/// the virtual eval() method dispatches to the bytecode VM.
///
/// This is the fundamental bridge that allows bytecoded and tree-walked
/// thunks to coexist: the existing forceValue() infrastructure works
/// unchanged because ExprBytecodeThunk IS an Expr.
struct ExprBytecodeThunk : Expr
{
    bytecode::CompilationUnit * unit;
    uint32_t thunkIdx;

    ExprBytecodeThunk(bytecode::CompilationUnit * unit, uint32_t thunkIdx)
        : unit(unit)
        , thunkIdx(thunkIdx)
    {}

    /// Dispatch to the bytecode VM.
    /// Declared here, defined in bytecode-thunk.cc (needs full EvalState).
    void eval(EvalState & state, Env & env, Value & v) override;

    void show(const SymbolTable & symbols, std::ostream & str) const override;

    void bindVars(EvalState &, const std::shared_ptr<const StaticEnv> &) override
    {
        // Already compiled; nothing to bind.
    }

    PosIdx getPos() const override;
};


/// A lambda proxy that dispatches its body evaluation to the bytecode VM.
///
/// When callFunction() calls lambda.body->eval(), and the body is an
/// ExprBytecodeThunk, it dispatches to the VM.  This proxy is what
/// OP_MAKE_CLOSURE creates: a Value with mkLambda(env, proxy) where
/// proxy->body points to an ExprBytecodeThunk.
///
/// Like ExprBytecodeThunk, this is allocated in BumpMemoryResource.
struct ExprLambdaBytecode : ExprLambda
{
    bytecode::CompilationUnit * unit;
    uint32_t lambdaIdx;

    /// Construct from a LambdaDescriptor in the compilation unit.
    /// Defined in bytecode-thunk.cc (needs full CompilationUnit/LambdaDescriptor).
    ExprLambdaBytecode(bytecode::CompilationUnit * unit, uint32_t lambdaIdx);
};

} // namespace nix
