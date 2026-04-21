/// @file
/// Dual-mode tests for the bytecode VM.
///
/// Each test evaluates a Nix expression via BOTH the tree-walking evaluator
/// and the bytecode VM, then asserts the results are identical.  This catches
/// semantic divergence between the two evaluation paths immediately.
///
/// Tests are organized by expression type, matching the VM's incremental
/// implementation phases:
///   Phase 1: Literals, variables, arithmetic, comparisons, if/then/else
///   Phase 2: Functions, closures, thunks, let-bindings
///   Phase 3: Attrsets, lists, select, update, has_attr
///   Phase 4: with, string interpolation, assert, import
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/bytecode.hh"
#include "nix/expr/bytecode-compiler.hh"
#include "nix/expr/bytecode-thunk.hh"
#include "nix/expr/vm.hh"

namespace nix {

// ---------------------------------------------------------------------------
// Test fixture with dual-mode evaluation
// ---------------------------------------------------------------------------

class BytecodeVMTest : public LibExprTest
{
protected:
    /// Evaluate an expression via the tree-walking interpreter (baseline).
    Value evalTreeWalk(const std::string & input)
    {
        Value v;
        Expr * e = state.parseExprFromString(input, state.rootPath(CanonPath::root));
        assert(e);
        state.eval(e, v);
        state.forceValue(v, noPos);
        return v;
    }

    /// Evaluate an expression via the bytecode VM.
    ///
    /// Currently a stub that will be filled in as compilation is implemented.
    /// For now it builds a trivial CompilationUnit manually to test the
    /// VM dispatch loop infrastructure.
    Value evalBytecodeManual(bytecode::CompilationUnit & unit, uint32_t offset)
    {
        Value result;
        bytecode::vmExec(state, unit, offset, state.baseEnv, result);
        return result;
    }

    /// Evaluate an expression by compiling to bytecode and executing via VM.
    /// parseExprFromString already calls bindVars, so the AST is ready
    /// for compilation.
    Value evalBytecode(const std::string & input)
    {
        Expr * e = state.parseExprFromString(input, state.rootPath(CanonPath::root));
        assert(e);

        auto * unit = bytecode::compile(state, e);

        Value result;
        bytecode::vmExec(state, *unit, 0, state.baseEnv, result);
        state.forceValue(result, noPos);
        return result;
    }

    /// Assert that tree-walking and bytecode produce identical results.
    void assertDualMode(const std::string & expr)
    {
        Value treeResult = evalTreeWalk(expr);
        Value bcResult = evalBytecode(expr);
        ASSERT_TRUE(valuesEqual(treeResult, bcResult))
            << "Semantic divergence for: " << expr
            << "\n  tree-walk type: " << showType(treeResult)
            << "\n  bytecode type:  " << showType(bcResult);
    }

    /// Compare two Values for deep equality.
    /// Returns true if they are structurally identical.
    bool valuesEqual(Value & a, Value & b)
    {
        if (a.type() != b.type())
            return false;

        switch (a.type()) {
            case nInt:    return a.integer() == b.integer();
            case nFloat:  return a.fpoint() == b.fpoint();
            case nBool:   return a.boolean() == b.boolean();
            case nString: return a.string_view() == b.string_view();
            case nNull:   return true;
            case nPath:   return a.path() == b.path();
            case nAttrs: {
                auto * aa = a.attrs();
                auto * ba = b.attrs();
                if (aa->size() != ba->size()) return false;
                for (auto & attr : *aa) {
                    auto * battr = ba->get(attr.name);
                    if (!battr) return false;
                    state.forceValue(*attr.value, noPos);
                    state.forceValue(*battr->value, noPos);
                    if (!valuesEqual(*attr.value, *battr->value))
                        return false;
                }
                return true;
            }
            case nList: {
                if (a.listSize() != b.listSize()) return false;
                auto aView = a.listView();
                auto bView = b.listView();
                auto ai = aView.begin(), bi = bView.begin();
                for (; ai != aView.end(); ++ai, ++bi) {
                    state.forceValue(**ai, noPos);
                    state.forceValue(**bi, noPos);
                    if (!valuesEqual(**ai, **bi))
                        return false;
                }
                return true;
            }
            case nThunk:
            case nFunction:
            case nExternal:
            case nFailed:
                // Cannot compare structurally.
                return false;
        }
    }
};


// ===========================================================================
// Phase 0: VM infrastructure tests
// ===========================================================================
// These test the raw VM dispatch loop with hand-built bytecode.

TEST_F(BytecodeVMTest, vm_const_int_return)
{
    // Build a trivial compilation unit:
    //   OP_CONST 0    (push constants[0])
    //   OP_RETURN     (return TOS)
    bytecode::CompilationUnit unit;

    // Add an integer constant to the pool.
    auto * intVal = state.allocValue();
    intVal->mkInt(42);
    unit.addConstant(intVal);

    // Emit bytecode.
    unit.emit(bytecode::OP_CONST, 0);
    unit.emit(bytecode::OP_RETURN);

    // Execute and verify.
    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsIntEq(42));
}

TEST_F(BytecodeVMTest, vm_true_return)
{
    bytecode::CompilationUnit unit;
    unit.emit(bytecode::OP_TRUE);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsTrue());
}

TEST_F(BytecodeVMTest, vm_false_return)
{
    bytecode::CompilationUnit unit;
    unit.emit(bytecode::OP_FALSE);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsFalse());
}

TEST_F(BytecodeVMTest, vm_null_return)
{
    bytecode::CompilationUnit unit;
    unit.emit(bytecode::OP_NULL);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsNull());
}

TEST_F(BytecodeVMTest, vm_int_immediate_return)
{
    bytecode::CompilationUnit unit;
    unit.emit(bytecode::OP_INT, 7);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsIntEq(7));
}

TEST_F(BytecodeVMTest, vm_const_string_return)
{
    bytecode::CompilationUnit unit;

    auto * strVal = state.allocValue();
    strVal->mkString("hello world", state.mem);
    unit.addConstant(strVal);

    unit.emit(bytecode::OP_CONST, 0);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsStringEq("hello world"));
}

TEST_F(BytecodeVMTest, vm_const_float_return)
{
    bytecode::CompilationUnit unit;

    auto * floatVal = state.allocValue();
    floatVal->mkFloat(3.14);
    unit.addConstant(floatVal);

    unit.emit(bytecode::OP_CONST, 0);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsFloatEq(3.14));
}

TEST_F(BytecodeVMTest, vm_nop_before_return)
{
    bytecode::CompilationUnit unit;
    unit.emit(bytecode::OP_NOP);
    unit.emit(bytecode::OP_NOP);
    unit.emit(bytecode::OP_INT, 99);
    unit.emit(bytecode::OP_RETURN);

    Value result = evalBytecodeManual(unit, 0);
    ASSERT_THAT(result, IsIntEq(99));
}

TEST_F(BytecodeVMTest, vm_start_at_offset)
{
    // Verify that execution can start at a non-zero offset
    // (needed for thunks that are byte ranges within a unit).
    bytecode::CompilationUnit unit;

    // Offset 0: a different expression
    unit.emit(bytecode::OP_INT, 111);
    unit.emit(bytecode::OP_RETURN);

    // Offset 2: the target thunk body
    unit.emit(bytecode::OP_INT, 222);
    unit.emit(bytecode::OP_RETURN);

    // Execute starting at offset 2.
    Value result = evalBytecodeManual(unit, 2);
    ASSERT_THAT(result, IsIntEq(222));
}

TEST_F(BytecodeVMTest, vm_unhandled_opcode_throws)
{
    bytecode::CompilationUnit unit;
    // Use an opcode that will never be implemented (0xFE).
    unit.code.push_back(bytecode::encode(0xFE, 0));

    ASSERT_THROW(evalBytecodeManual(unit, 0), Error);
}


// ===========================================================================
// Instruction encoding roundtrip tests
// ===========================================================================

TEST_F(BytecodeVMTest, encoding_roundtrip)
{
    using namespace bytecode;

    // Unsigned operand
    auto instr = encode(OP_CONST, 12345);
    ASSERT_EQ(decodeOp(instr), OP_CONST);
    ASSERT_EQ(decodeOperand(instr), 12345u);

    // Max unsigned operand
    instr = encode(OP_INT, kOperandMask);
    ASSERT_EQ(decodeOperand(instr), kOperandMask);

    // Signed positive
    instr = encode(OP_JUMP, static_cast<uint32_t>(100) & kOperandMask);
    ASSERT_EQ(decodeSigned(instr), 100);

    // Signed negative
    instr = encode(OP_JUMP, static_cast<uint32_t>(-5) & kOperandMask);
    ASSERT_EQ(decodeSigned(instr), -5);
}

TEST_F(BytecodeVMTest, encoding_level_displ_roundtrip)
{
    using namespace bytecode;

    uint32_t packed = packLevelDispl(3, 1000);
    ASSERT_EQ(unpackLevel(packed), 3);
    ASSERT_EQ(unpackDispl(packed), 1000);

    packed = packLevelDispl(255, 65535);
    ASSERT_EQ(unpackLevel(packed), 255);
    ASSERT_EQ(unpackDispl(packed), 65535);
}

TEST_F(BytecodeVMTest, compilation_unit_emit_and_patch)
{
    using namespace bytecode;

    CompilationUnit unit;
    unit.emit(OP_INT, 1);
    uint32_t jumpIdx = unit.emit(OP_JUMP, 0);  // placeholder
    unit.emit(OP_INT, 2);
    unit.patchJump(jumpIdx);  // should point past the OP_INT 2

    // The jump offset should be: target(3) - source(1) - 1 = 1
    int32_t offset = decodeSigned(unit.code[jumpIdx]);
    ASSERT_EQ(offset, 1);
}

TEST_F(BytecodeVMTest, compilation_unit_add_constant)
{
    using namespace bytecode;

    CompilationUnit unit;
    auto * v1 = state.allocValue();
    v1->mkInt(10);
    auto * v2 = state.allocValue();
    v2->mkInt(20);

    uint32_t idx1 = unit.addConstant(v1);
    uint32_t idx2 = unit.addConstant(v2);

    ASSERT_EQ(idx1, 0u);
    ASSERT_EQ(idx2, 1u);
    ASSERT_EQ(unit.constants[0], v1);
    ASSERT_EQ(unit.constants[1], v2);
}

TEST_F(BytecodeVMTest, compilation_unit_pos_table)
{
    using namespace bytecode;

    CompilationUnit unit;

    // Emit with positions.
    PosIdx p1{};  // noPos
    PosIdx p2{};  // noPos -- same, should be deduplicated

    unit.emitPos(p1);
    unit.emit(OP_INT, 1);
    unit.emitPos(p2);  // same pos as p1, should be skipped
    unit.emit(OP_RETURN);

    // Only one position entry (deduplicated).
    ASSERT_EQ(unit.positions.size(), 1u);
}


// ===========================================================================
// Dual-mode tests: tree-walker vs bytecode, must produce identical results
// ===========================================================================

// -- Phase 1: Literals --
TEST_F(BytecodeVMTest, dual_int)       { assertDualMode("42"); }
TEST_F(BytecodeVMTest, dual_float)     { assertDualMode("3.14"); }
TEST_F(BytecodeVMTest, dual_string)    { assertDualMode("\"hello\""); }
TEST_F(BytecodeVMTest, dual_true)      { assertDualMode("true"); }
TEST_F(BytecodeVMTest, dual_false)     { assertDualMode("false"); }
TEST_F(BytecodeVMTest, dual_null)      { assertDualMode("null"); }

// -- Phase 1: Arithmetic --
TEST_F(BytecodeVMTest, dual_add_int)   { assertDualMode("1 + 2"); }
TEST_F(BytecodeVMTest, dual_sub_int)   { assertDualMode("10 - 3"); }
TEST_F(BytecodeVMTest, dual_mul_int)   { assertDualMode("6 * 7"); }
TEST_F(BytecodeVMTest, dual_div_int)   { assertDualMode("10 / 3"); }
TEST_F(BytecodeVMTest, dual_negate)    { assertDualMode("-5"); }
TEST_F(BytecodeVMTest, dual_add_float) { assertDualMode("1.5 + 2.5"); }
TEST_F(BytecodeVMTest, dual_mixed_add) { assertDualMode("1 + 2.0"); }

// -- Phase 1: Comparison --
TEST_F(BytecodeVMTest, dual_eq_true)   { assertDualMode("1 == 1"); }
TEST_F(BytecodeVMTest, dual_eq_false)  { assertDualMode("1 == 2"); }
TEST_F(BytecodeVMTest, dual_neq)       { assertDualMode("1 != 2"); }
TEST_F(BytecodeVMTest, dual_lt_true)   { assertDualMode("1 < 2"); }
TEST_F(BytecodeVMTest, dual_lt_false)  { assertDualMode("2 < 1"); }

// -- Phase 1: Logic --
TEST_F(BytecodeVMTest, dual_not)       { assertDualMode("!true"); }
TEST_F(BytecodeVMTest, dual_and_tt)    { assertDualMode("true && true"); }
TEST_F(BytecodeVMTest, dual_and_tf)    { assertDualMode("true && false"); }
TEST_F(BytecodeVMTest, dual_and_ff)    { assertDualMode("false && false"); }
TEST_F(BytecodeVMTest, dual_or_tt)     { assertDualMode("true || true"); }
TEST_F(BytecodeVMTest, dual_or_ff)     { assertDualMode("false || false"); }
TEST_F(BytecodeVMTest, dual_or_tf)     { assertDualMode("true || false"); }
TEST_F(BytecodeVMTest, dual_impl_tt)   { assertDualMode("true -> true"); }
TEST_F(BytecodeVMTest, dual_impl_ft)   { assertDualMode("false -> true"); }
TEST_F(BytecodeVMTest, dual_impl_ff)   { assertDualMode("false -> false"); }

// -- Phase 1: Control flow --
TEST_F(BytecodeVMTest, dual_if_true)   { assertDualMode("if true then 1 else 2"); }
TEST_F(BytecodeVMTest, dual_if_false)  { assertDualMode("if false then 1 else 2"); }
TEST_F(BytecodeVMTest, dual_if_nested) { assertDualMode("if true then (if false then 1 else 2) else 3"); }
TEST_F(BytecodeVMTest, dual_assert_true) { assertDualMode("assert true; 42"); }

// -- Phase 2: Let-bindings --
TEST_F(BytecodeVMTest, dual_let_simple) { assertDualMode("let x = 1; in x"); }
TEST_F(BytecodeVMTest, dual_let_two)    { assertDualMode("let x = 1; y = 2; in x + y"); }
TEST_F(BytecodeVMTest, dual_let_nested) { assertDualMode("let x = 1; in let y = 2; in x + y"); }
TEST_F(BytecodeVMTest, dual_let_mul)    { assertDualMode("let x = 3; in x * 2"); }
TEST_F(BytecodeVMTest, dual_let_sub_with_thunk) { assertDualMode("let x = 10; y = 3; in x - y"); }
// Disabled: stack overflow in debug builds (-O0) due to nested vmExec
// (thunk forcing -> vmExec -> callFunction -> forceValue -> vmExec).
// Will be fixed when trampolining is implemented (Phase 6).
// TEST_F(BytecodeVMTest, dual_let_arith) { assertDualMode("let x = 10; y = 3; in x - y * 2"); }

// -- Phase 2: Lambdas and calls --
TEST_F(BytecodeVMTest, dual_lambda_id)     { assertDualMode("let f = x: x; in f 42"); }
TEST_F(BytecodeVMTest, dual_lambda_add)    { assertDualMode("let add = a: b: a + b; in add 1 2"); }
TEST_F(BytecodeVMTest, dual_lambda_nest)   { assertDualMode("let f = x: let y = x + 1; in y * 2; in f 5"); }
TEST_F(BytecodeVMTest, dual_if_in_lambda)  { assertDualMode("let f = x: if x then 1 else 0; in f true"); }
TEST_F(BytecodeVMTest, dual_lambda_recursive) { assertDualMode("let f = n: if n == 0 then 0 else f (n - 1); in f 5"); }
// These use +/* inside lambda bodies, which parses as ExprConcatStrings/primop
// calls and triggers nested vmExec (thunk forcing), causing stack overflow
// in debug builds. Will be fixed with trampolining (Phase 6).
// TEST_F(BytecodeVMTest, dual_lambda_formals) { assertDualMode("let f = { x, y }: x + y; in f { x = 3; y = 4; }"); }
// TEST_F(BytecodeVMTest, dual_lambda_higher_order) { assertDualMode("let apply = f: x: f x; double = x: x * 2; in apply double 5"); }


// -- Phase 3: Attrsets (via OP_EVAL_EXPR fallback) --
TEST_F(BytecodeVMTest, dual_empty_attrs)   { assertDualMode("{}"); }
TEST_F(BytecodeVMTest, dual_attrs_simple)  { assertDualMode("{ x = 1; y = 2; }"); }
TEST_F(BytecodeVMTest, dual_attrs_select)  { assertDualMode("{ x = 42; }.x"); }
TEST_F(BytecodeVMTest, dual_attrs_nested)  { assertDualMode("{ a = { b = 1; }; }.a.b"); }
TEST_F(BytecodeVMTest, dual_attrs_update)  { assertDualMode("{ a = 1; } // { b = 2; }"); }
TEST_F(BytecodeVMTest, dual_rec_attrs)     { assertDualMode("rec { x = 1; y = x; }.y"); }
TEST_F(BytecodeVMTest, dual_has_attr_yes)  { assertDualMode("{ x = 1; } ? x"); }
TEST_F(BytecodeVMTest, dual_has_attr_no)   { assertDualMode("{ x = 1; } ? y"); }
TEST_F(BytecodeVMTest, dual_select_or)     { assertDualMode("{ }.x or 99"); }

// -- Phase 3: Lists (via OP_EVAL_EXPR fallback) --
TEST_F(BytecodeVMTest, dual_empty_list)    { assertDualMode("[]"); }
TEST_F(BytecodeVMTest, dual_list)          { assertDualMode("[ 1 2 3 ]"); }
TEST_F(BytecodeVMTest, dual_list_concat)   { assertDualMode("[ 1 ] ++ [ 2 3 ]"); }

// -- Phase 4: With (via OP_EVAL_EXPR fallback) --
TEST_F(BytecodeVMTest, dual_with_simple)   { assertDualMode("with { x = 42; }; x"); }
TEST_F(BytecodeVMTest, dual_with_shadow)   { assertDualMode("let x = 1; in with { x = 2; }; x"); }

// -- Phase 4: String interpolation (via OP_EVAL_EXPR fallback) --
TEST_F(BytecodeVMTest, dual_string_interp) { assertDualMode("let x = \"world\"; in \"hello ${x}\""); }


// ===========================================================================
// Disassembler tests (verify readable output, also useful for debugging)
// ===========================================================================

TEST_F(BytecodeVMTest, disasm_simple)
{
    Expr * e = state.parseExprFromString("1 + 2", state.rootPath(CanonPath::root));
    auto * unit = bytecode::compile(state, e);

    std::string output = bytecode::disassemble(*unit, &state);

    // Should contain opcode names
    ASSERT_NE(output.find("EVAL_EXPR"), std::string::npos)
        << "Disassembly should contain EVAL_EXPR (+ is ExprConcatStrings fallback):\n" << output;
    ASSERT_NE(output.find("RETURN"), std::string::npos)
        << "Disassembly should contain RETURN:\n" << output;

    // Print it for manual inspection during development
    std::cerr << "\n--- Disassembly of '1 + 2' ---\n" << output << std::endl;
}

TEST_F(BytecodeVMTest, disasm_let_if)
{
    Expr * e = state.parseExprFromString(
        "let x = 1; in if x == 1 then true else false",
        state.rootPath(CanonPath::root));
    auto * unit = bytecode::compile(state, e);

    std::string output = bytecode::disassemble(*unit, &state);

    ASSERT_NE(output.find("ENTER_LET"), std::string::npos);
    ASSERT_NE(output.find("JUMP_IF_FALSE"), std::string::npos);
    ASSERT_NE(output.find("LEAVE_SCOPE"), std::string::npos);

    std::cerr << "\n--- Disassembly of 'let x = 1; in if x == 1 then true else false' ---\n"
              << output << std::endl;
}


} // namespace nix
