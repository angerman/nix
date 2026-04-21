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
    // ExprVar::eval = lookupVar + forceValue.
    // Emit GET_LOCAL (lazy lookup) then FORCE.
    emitGetLocal(e);
    unit.emit(OP_FORCE);
}

/// Emit just the variable lookup instruction without forcing.
/// Used by compileVar (followed by FORCE) and by compileAsThunkOrEager
/// for variable references (no FORCE, matching ExprVar::maybeThunk).
void Compiler::emitGetLocal(ExprVar * e)
{
    unit.emitPos(e->pos);

    if (e->fromWith) {
        uint32_t exprIdx = unit.addExpr(e);
        unit.emit(OP_GET_WITH, exprIdx);
        return;
    }

    switch (e->level) {
        case 0: unit.emit(OP_GET_LOCAL_0, e->displ); break;
        case 1: unit.emit(OP_GET_LOCAL_1, e->displ); break;
        case 2: unit.emit(OP_GET_LOCAL_2, e->displ); break;
        case 3: unit.emit(OP_GET_LOCAL_3, e->displ); break;
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
    unit.emitPos(e->getPos());
    unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
}


// ---------------------------------------------------------------------------
// Phase 2: Let-bindings
// ---------------------------------------------------------------------------

void Compiler::compileLet(ExprLet * e)
{
    // Fall back to tree-walking for let-bindings with inherit(expr).
    // These require a separate inheritEnv that our bytecoded path
    // doesn't create. The ExprInheritFrom nodes have displacements
    // into the inheritEnv, not the let env.
    bool hasInheritFrom = e->attrs->inheritFromExprs
        && !e->attrs->inheritFromExprs->empty();

    if (hasInheritFrom) {
        unit.emitPos(e->attrs->pos);
        unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
        return;
    }

    // Simple let without inherit(expr):
    //   OP_ENTER_LET envSize
    //   <for each binding>
    //     <compile thunk or eager value>
    //     OP_SET_ENV_SLOT displ
    //   <compile body>
    //   OP_LEAVE_SCOPE

    uint32_t envSize = static_cast<uint32_t>(e->attrs->attrs->size());
    unit.emitPos(e->attrs->pos);
    unit.emit(OP_ENTER_LET, envSize);

    Displacement displ = 0;
    for (auto & [name, def] : *e->attrs->attrs) {
        compileAsThunkOrEager(def.e, def.pos);
        unit.emit(OP_SET_ENV_SLOT, displ);
        displ++;
    }

    compile(e->body);

    unit.emit(OP_LEAVE_SCOPE);
}


// ---------------------------------------------------------------------------
// Phase 2: Thunk-or-eager helper (mirrors maybeThunk)
// ---------------------------------------------------------------------------

void Compiler::compileAsThunkOrEager(Expr * expr, PosIdx pos)
{
    // Literals: compile directly, no thunk needed.
    if (dynamic_cast<ExprInt *>(expr)
        || dynamic_cast<ExprFloat *>(expr)
        || dynamic_cast<ExprString *>(expr)
        || dynamic_cast<ExprPath *>(expr)) {
        compile(expr);
        return;
    }

    // Variable references: emit just the lookup (no forcing).
    // This matches ExprVar::maybeThunk which returns the Value* directly.
    // The loaded value may itself be a thunk, but that's fine -- it will be
    // forced on demand when the consumer needs it.
    // IMPORTANT: do NOT call compile(expr) here -- that would emit
    // GET_LOCAL + FORCE, which eagerly forces and breaks recursive
    // fixed-points (lib.makeExtensible, rec {}, etc.).
    if (auto * var = dynamic_cast<ExprVar *>(expr)) {
        // Emit GET_LOCAL or GET_WITH (no forcing) for ALL variables.
        emitGetLocal(var);
        return;
    }

    // Lambda: compile to OP_MAKE_CLOSURE, no thunk needed (lambdas are values).
    if (dynamic_cast<ExprLambda *>(expr)) {
        compile(expr);
        return;
    }

    // General case: create a thunk.
    // 1. Compile the thunk body into a separate region of the code buffer.
    // 2. Jump over the thunk body at the definition site.
    // 3. Record a ThunkDescriptor for this region.
    // 4. Emit OP_MAKE_THUNK to create the thunk at runtime.

    // Jump over the thunk body.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);

    // Record the start of the thunk body.
    uint32_t thunkStart = static_cast<uint32_t>(unit.code.size());

    // Compile the thunk body (will be executed when forced).
    compile(expr);
    unit.emit(OP_RETURN);

    // Patch the jump to skip over the thunk body.
    unit.patchJump(jumpOver);

    // Register the thunk descriptor.
    uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
    unit.thunks.push_back(ThunkDescriptor{thunkStart, pos, expr});

    // Emit the thunk creation instruction.
    unit.emit(OP_MAKE_THUNK, thunkIdx);
}


// ---------------------------------------------------------------------------
// Phase 2: Lambda/closure compilation
// ---------------------------------------------------------------------------

void Compiler::compileLambda(ExprLambda * e)
{
    // Jump over the lambda body.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);

    // Record the start of the lambda body.
    uint32_t bodyStart = static_cast<uint32_t>(unit.code.size());

    // Compile the lambda body.
    compile(e->body);
    unit.emit(OP_RETURN);

    // Patch the jump.
    unit.patchJump(jumpOver);

    // Register the lambda descriptor.
    uint32_t lambdaIdx = static_cast<uint32_t>(unit.lambdas.size());

    auto formals = e->getFormals();
    uint16_t envSize = (!e->arg ? 0 : 1)
        + (formals ? static_cast<uint16_t>(formals->formals.size()) : 0);

    unit.lambdas.push_back(LambdaDescriptor{
        .codeOffset = bodyStart,
        .pos = e->pos,
        .name = e->name,
        .arg = e->arg,
        .formals = formals ? &*formals : nullptr,
        .envSize = envSize,
        .sourceExpr = e,
    });

    // Emit the closure creation instruction.
    unit.emitPos(e->pos);
    unit.emit(OP_MAKE_CLOSURE, lambdaIdx);
}


// ---------------------------------------------------------------------------
// Phase 2: Function calls
// ---------------------------------------------------------------------------

void Compiler::compileCall(ExprCall * e)
{
    // Compile the function expression.
    compile(e->fun);

    // Compile each argument as a thunk (lazy, matching tree-walker semantics).
    for (auto * arg : *e->args) {
        compileAsThunkOrEager(arg, e->pos);
    }

    // Emit the call instruction.
    unit.emitPos(e->pos);
    uint32_t nArgs = static_cast<uint32_t>(e->args->size());
    if (nArgs == 1)
        unit.emit(OP_CALL_1);
    else
        unit.emit(OP_CALL, nArgs);
}


// ---------------------------------------------------------------------------
// Stubs for Phase 3+ expression types
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Phase 3: Attribute selection
// ---------------------------------------------------------------------------

void Compiler::compileSelect(ExprSelect * e)
{
    auto attrPath = e->getAttrPath();

    // Fall back to tree-walking for complex cases:
    // - Dynamic attribute names (expr in path)
    // - Multi-level paths with 'or' default (complex jump logic)
    bool hasDynamic = false;
    for (auto & attr : attrPath)
        if (attr.expr) { hasDynamic = true; break; }

    if (hasDynamic) {
        unit.emitPos(e->pos);
        unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
        return;
    }

    // Multi-level paths with 'or' default still fall back (complex jump logic).
    if (e->def && attrPath.size() > 1) {
        unit.emitPos(e->pos);
        unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
        return;
    }

    // Compile the base expression.
    compile(e->e);

    // Emit select for each level in the path.
    for (size_t i = 0; i < attrPath.size(); i++) {
        uint32_t symIdx = unit.addSymbol(attrPath[i].symbol);
        bool isLast = (i == attrPath.size() - 1);

        if (e->def && isLast) {
            // Single-level 'or' default: a.x or default
            //   FORCE
            //   JUMP_IF_NOT_ATTRS -> defLabel    (no pop)
            //   DUP                               (stack: [attrs, attrs])
            //   HAS_ATTR sym                      (stack: [attrs, bool])
            //   JUMP_IF_FALSE -> defLabel2        (stack: [attrs])
            //   ATTR_SELECT sym                   (stack: [value])
            //   JUMP -> endLabel
            // defLabel:                           (stack: [non-attrs-value])
            // defLabel2:                          (stack: [attrs])
            //   POP                               (stack: [])
            //   <compile default>                 (stack: [default])
            // endLabel:
            unit.emitPos(e->pos);
            unit.emit(OP_FORCE);
            uint32_t jumpNotAttrs = unit.emit(OP_JUMP_IF_NOT_ATTRS, 0);
            unit.emit(OP_DUP);
            unit.emit(OP_HAS_ATTR, symIdx);
            uint32_t jumpNoAttr = unit.emit(OP_JUMP_IF_FALSE, 0);
            unit.emit(OP_ATTR_SELECT, symIdx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpNotAttrs);
            unit.patchJump(jumpNoAttr);
            unit.emit(OP_POP);
            compile(e->def);
            unit.patchJump(jumpEnd);
        } else {
            // No default: force base, select attr, force result.
            // Split into separate ops so OP_FORCE can trampoline
            // bytecoded thunks inline (avoiding vmExec recursion).
            unit.emitPos(e->pos);
            unit.emit(OP_FORCE);
            unit.emit(OP_ATTR_SELECT, symIdx);
            unit.emit(OP_FORCE);
        }
    }
}

void Compiler::compileHasAttr(ExprOpHasAttr * e)
{
    // Fall back for multi-level or dynamic paths.
    bool hasDynamic = false;
    for (auto & attr : e->attrPath)
        if (attr.expr) { hasDynamic = true; break; }

    if (hasDynamic || e->attrPath.size() > 1) {
        unit.emitPos(e->getPos());
        unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
        return;
    }

    // Single-level: { ... } ? attrName
    compile(e->e);
    uint32_t symIdx = unit.addSymbol(e->attrPath[0].symbol);
    unit.emitPos(e->getPos());
    unit.emit(OP_FORCE);
    // If not attrset, result is false.
    uint32_t jumpNotAttrs = unit.emit(OP_JUMP_IF_NOT_ATTRS, 0);
    unit.emit(OP_HAS_ATTR, symIdx);
    uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
    unit.patchJump(jumpNotAttrs);
    unit.emit(OP_POP);
    unit.emit(OP_FALSE);
    unit.patchJump(jumpEnd);
}

// ---------------------------------------------------------------------------
// Phase 3: Lists
// ---------------------------------------------------------------------------

void Compiler::compileList(ExprList * e)
{
    // Compile each element as a thunk-or-eager value, push onto stack.
    // Then OP_LIST_BUILD N pops N values and builds the list.
    for (auto * elem : e->elems) {
        compileAsThunkOrEager(elem, e->getPos());
    }
    unit.emitPos(e->getPos());
    unit.emit(OP_LIST_INIT, static_cast<uint32_t>(e->elems.size()));
}

// ---------------------------------------------------------------------------
// Phase 3: Attrsets, update, with
// ---------------------------------------------------------------------------

void Compiler::compileAttrs(ExprAttrs * e)
{
    // Fall back for recursive attrsets, inherit(expr), and dynamic attrs.
    // These require complex env setup, __overrides handling, etc.
    bool hasInheritFrom = e->inheritFromExprs && !e->inheritFromExprs->empty();
    bool hasDynamic = e->dynamicAttrs && !e->dynamicAttrs->empty();

    if (e->recursive || hasInheritFrom || hasDynamic) {
        unit.emitPos(e->pos);
        unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
        return;
    }

    // Non-recursive, no inherit(expr), no dynamic attrs.
    // Simple case: { a = e1; b = e2; ... }
    //
    // Compile: push capacity, then for each attr push its thunked value
    // along with symbol info.  OP_ATTRS_INIT capacity allocates a
    // BindingsBuilder, each attr is inserted via OP_ATTR_INSERT, and
    // OP_ATTRS_FINISH finalizes.

    uint32_t nAttrs = static_cast<uint32_t>(e->attrs->size());

    // Push each attribute value onto the stack (in iteration order,
    // which is sorted by symbol since AttrDefs is a std::map<Symbol,...>).
    for (auto & [name, def] : *e->attrs) {
        compileAsThunkOrEager(def.e, def.pos);
    }

    // OP_ATTRS_INIT nAttrs: pops nAttrs values, reads nAttrs symbol
    // indices from the following data words, builds the Bindings.
    unit.emitPos(e->pos);
    unit.emit(OP_ATTRS_INIT, nAttrs);

    // Emit (symbol index, position index) pairs as data words.
    // The VM reads these inline after OP_ATTRS_INIT.
    for (auto & [name, def] : *e->attrs) {
        uint32_t symIdx = unit.addSymbol(name);
        uint32_t posIdx = unit.addPos(def.pos);
        unit.emit(OP_NOP, symIdx); // data word: symbol index
        unit.emit(OP_NOP, posIdx); // data word: position index
    }
}

void Compiler::compileWith(ExprWith * e)
{
    // with attrs; body
    //
    // OP_PUSH_WITH creates a 1-slot env with the attrs thunk.
    // Variables in the body that come from `with` scope use OP_GET_WITH
    // which walks the env chain to find the with-env and looks up the
    // attribute dynamically.
    compileAsThunkOrEager(e->attrs, e->pos);
    unit.emitPos(e->pos);
    unit.emit(OP_PUSH_WITH);
    compile(e->body);
    unit.emit(OP_LEAVE_SCOPE);
}

void Compiler::compileUpdate(ExprOpUpdate * e)
{
    // The // operator's merge logic is complex (layered bindings,
    // sorted merge with RHS-wins duplicate resolution, optimization
    // heuristics).  Delegate to tree-walker for correctness.
    unit.emitPos(e->pos);
    unit.emit(OP_EVAL_EXPR, unit.addExpr(e));
}

void Compiler::compileConcatLists(ExprOpConcatLists * e)
{
    compile(e->e1);
    compile(e->e2);
    unit.emitPos(e->getPos());
    unit.emit(OP_LIST_CONCAT);
}

void Compiler::compileConcatStrings(ExprConcatStrings * e)
{
    // Compile each part expression, push on stack.
    // OP_STR_CONCAT_INIT nParts then pops them all and does the
    // type-dependent combining (int add, float promote, string concat
    // with context, path construction).
    //
    // The forceString flag is encoded in bit 23 of the operand.
    // nParts is in bits [0:22] (max 4M parts, more than enough).

    for (auto & [partPos, partExpr] : e->es) {
        compile(partExpr);
    }

    uint32_t nParts = static_cast<uint32_t>(e->es.size());
    uint32_t operand = nParts | (e->forceString ? (1u << 23) : 0);
    unit.emitPos(e->pos);
    unit.emit(OP_STR_CONCAT_INIT, operand);
}

} // namespace nix::bytecode
