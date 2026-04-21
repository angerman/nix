/// @file
/// Bytecode compiler implementation.
///
/// Translates the Nix AST into bytecode via a single post-order traversal.
/// The expression must have been through bindVars() before compilation.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode-compiler.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

namespace nix::bytecode {

// ---------------------------------------------------------------------------
// Top-level compile entry point
// ---------------------------------------------------------------------------

CompilationUnit * compile(EvalState & state, Expr * expr)
{
    auto * unit = new (GC) CompilationUnit();
    Compiler compiler(state, *unit);
    compiler.compile(expr);
    unit->emit(OP_RETURN);
    return unit;
}


// ---------------------------------------------------------------------------
// Expression dispatch
// ---------------------------------------------------------------------------

void Compiler::compile(Expr * expr)
{
    // Dispatch to the appropriate compilation method based on the
    // dynamic type of the expression.  This mirrors the virtual
    // Expr::eval() dispatch but at compile time.
    //
    // The order here matches the frequency of expression types in
    // typical Nix code (most common first for branch prediction).

    if (auto * e = dynamic_cast<ExprVar *>(expr))
        return compileVar(e);
    if (auto * e = dynamic_cast<ExprSelect *>(expr))
        return compileSelect(e);
    if (auto * e = dynamic_cast<ExprCall *>(expr))
        return compileCall(e);
    if (auto * e = dynamic_cast<ExprAttrs *>(expr))
        return compileAttrs(e);
    if (auto * e = dynamic_cast<ExprLet *>(expr))
        return compileLet(e);
    if (auto * e = dynamic_cast<ExprIf *>(expr))
        return compileIf(e);
    if (auto * e = dynamic_cast<ExprLambda *>(expr))
        return compileLambda(e);
    if (auto * e = dynamic_cast<ExprList *>(expr))
        return compileList(e);

    // Literals
    if (auto * e = dynamic_cast<ExprInt *>(expr))
        return compileLiteral(e);
    if (auto * e = dynamic_cast<ExprFloat *>(expr))
        return compileLiteral(e);
    if (auto * e = dynamic_cast<ExprString *>(expr))
        return compileLiteral(e);
    if (auto * e = dynamic_cast<ExprPath *>(expr))
        return compileLiteral(e);

    // Operators
    if (auto * e = dynamic_cast<ExprOpAnd *>(expr))
        return compileAnd(e);
    if (auto * e = dynamic_cast<ExprOpOr *>(expr))
        return compileOr(e);
    if (auto * e = dynamic_cast<ExprOpEq *>(expr))
        return compileEq(e);
    if (auto * e = dynamic_cast<ExprOpNEq *>(expr))
        return compileNEq(e);
    if (auto * e = dynamic_cast<ExprOpNot *>(expr))
        return compileNot(e);
    if (auto * e = dynamic_cast<ExprOpImpl *>(expr))
        return compileImpl(e);
    if (auto * e = dynamic_cast<ExprOpUpdate *>(expr))
        return compileUpdate(e);
    if (auto * e = dynamic_cast<ExprOpConcatLists *>(expr))
        return compileConcatLists(e);
    if (auto * e = dynamic_cast<ExprOpHasAttr *>(expr))
        return compileHasAttr(e);

    // String interpolation
    if (auto * e = dynamic_cast<ExprConcatStrings *>(expr))
        return compileConcatStrings(e);

    // Remaining
    if (auto * e = dynamic_cast<ExprWith *>(expr))
        return compileWith(e);
    if (auto * e = dynamic_cast<ExprAssert *>(expr))
        return compileAssert(e);
    if (auto * e = dynamic_cast<ExprPos *>(expr))
        return compilePos(e);

    // Fallback: if we encounter an expression type we don't handle yet,
    // throw a clear error rather than silently producing wrong code.
    throw Error("bytecode compiler: unhandled expression type at %s",
        state.positions[expr->getPos()]);
}


// ---------------------------------------------------------------------------
// Phase 1.1: Literals
// ---------------------------------------------------------------------------

void Compiler::compileLiteral(ExprInt * e)
{
    // Small integers fit in the 24-bit immediate field of OP_INT.
    auto n = e->v.integer().value;
    if (n >= 0 && n <= static_cast<int64_t>(kOperandMask)) {
        unit.emit(OP_INT, static_cast<uint32_t>(n));
    } else {
        // Large integers go through the constant pool.
        unit.emit(OP_CONST, unit.addConstant(&e->v));
    }
}

void Compiler::compileLiteral(ExprFloat * e)
{
    // Floats always go through the constant pool (can't encode in 24 bits).
    unit.emit(OP_CONST, unit.addConstant(&e->v));
}

void Compiler::compileLiteral(ExprString * e)
{
    // String Values live in the BumpMemoryResource arena, so &e->v is
    // stable for the entire evaluation lifetime.
    unit.emit(OP_CONST, unit.addConstant(&e->v));
}

void Compiler::compileLiteral(ExprPath * e)
{
    unit.emit(OP_CONST, unit.addConstant(&e->v));
}


// ---------------------------------------------------------------------------
// Phase 1.2: Arithmetic and comparison operators
// ---------------------------------------------------------------------------

void Compiler::compileBinOp(Expr * e1, Expr * e2, Op op, PosIdx pos)
{
    compile(e1);
    compile(e2);
    unit.emitPos(pos);
    unit.emit(op);
}


// ---------------------------------------------------------------------------
// Phase 1.2: Logic operators (short-circuit)
// ---------------------------------------------------------------------------

void Compiler::compileNot(ExprOpNot * e)
{
    compile(e->e);
    unit.emitPos(e->getPos());
    unit.emit(OP_NOT);
}

void Compiler::compileEq(ExprOpEq * e)
{
    compileBinOp(e->e1, e->e2, OP_EQ, e->pos);
}

void Compiler::compileNEq(ExprOpNEq * e)
{
    compileBinOp(e->e1, e->e2, OP_NEQ, e->pos);
}

void Compiler::compileAnd(ExprOpAnd * e)
{
    // Short-circuit: if left is false, result is false.
    //   <left>
    //   OP_JUMP_IF_FALSE -> shortCircuit
    //   <right>
    //   OP_JUMP -> end
    // shortCircuit:
    //   OP_FALSE
    // end:
    compile(e->e1);
    unit.emitPos(e->pos);
    uint32_t jumpFalse = unit.emit(OP_JUMP_IF_FALSE, 0);
    compile(e->e2);
    uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
    unit.patchJump(jumpFalse);
    unit.emit(OP_FALSE);
    unit.patchJump(jumpEnd);
}

void Compiler::compileOr(ExprOpOr * e)
{
    // Short-circuit: if left is true, result is true.
    compile(e->e1);
    unit.emitPos(e->pos);
    uint32_t jumpTrue = unit.emit(OP_JUMP_IF_TRUE, 0);
    compile(e->e2);
    uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
    unit.patchJump(jumpTrue);
    unit.emit(OP_TRUE);
    unit.patchJump(jumpEnd);
}

void Compiler::compileImpl(ExprOpImpl * e)
{
    // a -> b  is equivalent to  !a || b
    // Short-circuit: if a is false, result is true.
    compile(e->e1);
    unit.emitPos(e->pos);
    unit.emit(OP_NOT);
    uint32_t jumpTrue = unit.emit(OP_JUMP_IF_TRUE, 0);
    compile(e->e2);
    uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
    unit.patchJump(jumpTrue);
    unit.emit(OP_TRUE);
    unit.patchJump(jumpEnd);
}


// ---------------------------------------------------------------------------
// Phase 1.3: Variable access
// ---------------------------------------------------------------------------

void Compiler::compileVar(ExprVar * e)
{
    unit.emitPos(e->pos);

    if (e->fromWith) {
        // Dynamic with-scope lookup.
        uint32_t symIdx = unit.addSymbol(e->name);
        unit.emit(OP_GET_WITH, symIdx);
        return;
    }

    // Lexical variable: emit specialized opcode for common levels.
    switch (e->level) {
        case 0:
            unit.emit(OP_GET_LOCAL_0, e->displ);
            break;
        case 1:
            unit.emit(OP_GET_LOCAL_1, e->displ);
            break;
        case 2:
            unit.emit(OP_GET_LOCAL_2, e->displ);
            break;
        case 3:
            unit.emit(OP_GET_LOCAL_3, e->displ);
            break;
        default:
            unit.emit(OP_GET_LOCAL, packLevelDispl(
                static_cast<uint8_t>(e->level), static_cast<uint16_t>(e->displ)));
            break;
    }
}


// ---------------------------------------------------------------------------
// Phase 1.3: Control flow
// ---------------------------------------------------------------------------

void Compiler::compileIf(ExprIf * e)
{
    //   <cond>
    //   OP_JUMP_IF_FALSE -> elseLabel
    //   <then>
    //   OP_JUMP -> endLabel
    // elseLabel:
    //   <else>
    // endLabel:
    compile(e->cond);
    unit.emitPos(e->pos);
    uint32_t jumpElse = unit.emit(OP_JUMP_IF_FALSE, 0);
    compile(e->then);
    uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
    unit.patchJump(jumpElse);
    compile(e->else_);
    unit.patchJump(jumpEnd);
}

void Compiler::compileAssert(ExprAssert * e)
{
    compile(e->cond);
    unit.emitPos(e->pos);
    unit.emit(OP_ASSERT);
    compile(e->body);
}

void Compiler::compilePos(ExprPos * e)
{
    // __curPos is rarely used; emit a position-lookup opcode.
    // For now, fall through to the unhandled error.
    // TODO: implement OP_POS
    throw Error("bytecode compiler: ExprPos not yet implemented");
}


// ---------------------------------------------------------------------------
// Stubs for Phase 2+ expression types
// ---------------------------------------------------------------------------
// These will be implemented incrementally. For now they throw clear errors.

void Compiler::compileSelect(ExprSelect * e)
{
    throw Error("bytecode compiler: ExprSelect not yet implemented");
}

void Compiler::compileHasAttr(ExprOpHasAttr * e)
{
    throw Error("bytecode compiler: ExprOpHasAttr not yet implemented");
}

void Compiler::compileAttrs(ExprAttrs * e)
{
    throw Error("bytecode compiler: ExprAttrs not yet implemented");
}

void Compiler::compileList(ExprList * e)
{
    throw Error("bytecode compiler: ExprList not yet implemented");
}

void Compiler::compileLambda(ExprLambda * e)
{
    throw Error("bytecode compiler: ExprLambda not yet implemented");
}

void Compiler::compileCall(ExprCall * e)
{
    throw Error("bytecode compiler: ExprCall not yet implemented");
}

void Compiler::compileLet(ExprLet * e)
{
    throw Error("bytecode compiler: ExprLet not yet implemented");
}

void Compiler::compileWith(ExprWith * e)
{
    throw Error("bytecode compiler: ExprWith not yet implemented");
}

void Compiler::compileUpdate(ExprOpUpdate * e)
{
    throw Error("bytecode compiler: ExprOpUpdate not yet implemented");
}

void Compiler::compileConcatLists(ExprOpConcatLists * e)
{
    throw Error("bytecode compiler: ExprOpConcatLists not yet implemented");
}

void Compiler::compileConcatStrings(ExprConcatStrings * e)
{
    throw Error("bytecode compiler: ExprConcatStrings not yet implemented");
}

} // namespace nix::bytecode
