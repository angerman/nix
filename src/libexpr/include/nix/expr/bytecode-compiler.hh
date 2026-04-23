#pragma once
/// @file
/// Bytecode compiler: translates the Nix AST into a flat bytecode buffer.
///
/// The compiler performs a single post-order traversal of the AST after
/// the existing bindVars() pass has resolved all variable references.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"

namespace nix {

struct Expr;
struct ExprInt;
struct ExprFloat;
struct ExprString;
struct ExprPath;
struct ExprVar;
struct ExprSelect;
struct ExprOpHasAttr;
struct ExprAttrs;
struct ExprList;
struct ExprLambda;
struct ExprCall;
struct ExprLet;
struct ExprWith;
struct ExprIf;
struct ExprAssert;
struct ExprOpNot;
struct ExprOpEq;
struct ExprOpNEq;
struct ExprOpAnd;
struct ExprOpOr;
struct ExprOpImpl;
struct ExprOpUpdate;
struct ExprOpConcatLists;
struct ExprConcatStrings;
struct ExprPos;
class EvalState;

namespace bytecode {

/// Compile a Nix expression (which must have already been through
/// bindVars()) into a CompilationUnit.
///
/// The returned unit is GC-allocated and lives as long as any thunk
/// or closure references it.
CompilationUnit * compile(EvalState & state, Expr * expr);

/// Compiler state.  One instance per compile() call.
/// Not part of the public API -- exposed in the header only for testing.
class Compiler
{
    EvalState & state;
    CompilationUnit & unit;

    /// Track whether we are in tail position (for future OP_TAIL_CALL).
    /// Currently unused -- will be used when OP_TAIL_CALL is implemented.
    [[maybe_unused]] bool inTailPosition = false;

    /// Level offset applied to GET_LOCAL instructions.
    /// Used when the bytecoded env chain has an extra scope (e.g., the
    /// inherit-from env in non-rec attrsets) that bindVars didn't account
    /// for. When nonzero, emitGetLocal adds this offset to the variable's
    /// level before emitting the instruction.
    uint8_t levelOffset = 0;

    /// True when compiling inside a recursive binding loop (let/rec
    /// attrset).  Level-0 ExprVar references may be forward references
    /// to uninitialized env slots and must be wrapped in thunks.
    bool inRecursiveScope = false;

    /// Displacement offset for ExprInheritFrom nodes in the flattened
    /// let env approach.  When compiling `let inherit(expr) ...`, the
    /// let env is extended with extra slots for inherit-from sources.
    /// ExprInheritFrom with displ=D is remapped to displ=inheritDisplOffset+D.
    /// Zero means no remapping (not inside a let with inherit(expr)).
    uint32_t inheritDisplOffset = 0;

public:
    Compiler(EvalState & state, CompilationUnit & unit)
        : state(state)
        , unit(unit)
    {}

    /// Compile an expression, leaving one Value* on the VM stack.
    void compile(Expr * expr);

private:
    // -- Per-expression-type compilation methods --
    void compileLiteral(ExprInt * e);
    void compileLiteral(ExprFloat * e);
    void compileLiteral(ExprString * e);
    void compileLiteral(ExprPath * e);
    void compileVar(ExprVar * e);
    void compileSelect(ExprSelect * e);
    void compileHasAttr(ExprOpHasAttr * e);
    void compileAttrs(ExprAttrs * e);
    void compileList(ExprList * e);
    void compileLambda(ExprLambda * e);
    void compileCall(ExprCall * e);
    void compileLet(ExprLet * e);
    void compileWith(ExprWith * e);
    void compileIf(ExprIf * e);
    void compileAssert(ExprAssert * e);
    void compileNot(ExprOpNot * e);
    void compileEq(ExprOpEq * e);
    void compileNEq(ExprOpNEq * e);
    void compileAnd(ExprOpAnd * e);
    void compileOr(ExprOpOr * e);
    void compileImpl(ExprOpImpl * e);
    void compileUpdate(ExprOpUpdate * e);
    void compileConcatLists(ExprOpConcatLists * e);
    void compileConcatStrings(ExprConcatStrings * e);
    void compilePos(ExprPos * e);

    // -- Thunk-or-eager helper (mirrors Expr::maybeThunk) --
    void compileAsThunkOrEager(Expr * expr, PosIdx pos);

    // -- Variable lookup without forcing (mirrors ExprVar::maybeThunk) --
    void emitGetLocal(ExprVar * e);

    // -- Arithmetic binary op helper --
    void compileBinOp(Expr * e1, Expr * e2, Op op, PosIdx pos);
};

} // namespace bytecode
} // namespace nix
