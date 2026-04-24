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
#include "nix/expr/ir.hh"
#include "nix/expr/ir-emit.hh"
#include "nix/expr/bytecode-thunk.hh"
#include "nix/expr/vm.hh"

namespace nix {

// ---------------------------------------------------------------------------
// Test fixture with dual-mode evaluation
// ---------------------------------------------------------------------------

class BytecodeVMTest : public LibExprTest
{
protected:
    /// Evaluate an expression via the tree-walking interpreter (oracle).
    /// Always uses tree-walking regardless of NIX_EVAL_BYTECODE setting.
    Value evalTreeWalk(const std::string & input)
    {
        Value v;
        Expr * e = state.parseExprFromString(input, state.rootPath(CanonPath::root));
        assert(e);
        // Call the tree-walker directly, bypassing the bytecode path.
        e->eval(state, state.baseEnv, v);
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
TEST_F(BytecodeVMTest, dual_let_arith)   { assertDualMode("let x = 10; y = 3; in x - y * 2"); }

// -- Phase 2: Lambdas and calls --
TEST_F(BytecodeVMTest, dual_lambda_id)     { assertDualMode("let f = x: x; in f 42"); }
TEST_F(BytecodeVMTest, dual_lambda_add)    { assertDualMode("let add = a: b: a + b; in add 1 2"); }
TEST_F(BytecodeVMTest, dual_lambda_nest)   { assertDualMode("let f = x: let y = x + 1; in y * 2; in f 5"); }
TEST_F(BytecodeVMTest, dual_if_in_lambda)  { assertDualMode("let f = x: if x then 1 else 0; in f true"); }
TEST_F(BytecodeVMTest, dual_lambda_recursive) { assertDualMode("let f = n: if n == 0 then 0 else f (n - 1); in f 5"); }
TEST_F(BytecodeVMTest, dual_lambda_formals) { assertDualMode("let f = { x, y }: x + y; in f { x = 3; y = 4; }"); }
TEST_F(BytecodeVMTest, dual_lambda_higher_order) { assertDualMode("let apply = f: x: f x; double = x: x * 2; in apply double 5"); }


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

    // 1 + 2 compiles as ExprConcatStrings -> OP_STR_CONCAT_INIT
    ASSERT_NE(output.find("STR_CONCAT_INIT"), std::string::npos)
        << "Disassembly should contain STR_CONCAT_INIT:\n" << output;
    ASSERT_NE(output.find("RETURN"), std::string::npos)
        << "Disassembly should contain RETURN:\n" << output;

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


// ===========================================================================
// Regression tests for bugs found during development
// ===========================================================================

// Bug: OP_GET_LOCAL didn't force values at lookup time (unlike ExprVar::eval
// which calls forceValue). This caused thunks to leak through function calls
// where the caller expected a forced value.  The symptom was "attempt to call
// something which is not a function but a thunk" when importing nixpkgs.
TEST_F(BytecodeVMTest, regression_var_access_forces) {
    // let x = expr; in x should return a forced value, not a thunk.
    // This simulates the pattern in stdenv/generic/default.nix where
    // `let stdenv = lib.makeOverridable(...); in stdenv` returns a thunk
    // if variable access doesn't force.
    assertDualMode("let x = { a = 1; }; in x");
    assertDualMode("let f = x: x; g = f; in g 42");
    // Nested: thunk chain through let bindings
    assertDualMode("let x = 1 + 2; y = x; z = y; in z");
}

// Bug: OP_ATTRS_INIT with >256 attrs hit an assertion.  Nixpkgs has
// large attrsets (all-packages.nix).  Fixed by heap-allocating for
// large attrsets.
TEST_F(BytecodeVMTest, regression_large_attrset) {
    // Generate a 300-attribute attrset
    std::string expr = "let s = {";
    for (int i = 0; i < 300; i++)
        expr += " a" + std::to_string(i) + " = " + std::to_string(i) + ";";
    expr += " }; in s.a299";
    assertDualMode(expr);
}


// Regression: recursive fixed-points (lib.makeExtensible pattern).
// This tests the pattern from nixpkgs lib/default.nix that cardano-node triggers.
// The tree-walker handles this via lazy thunks; the bytecode VM must too.
TEST_F(BytecodeVMTest, regression_rec_fixpoint) {
    // Simplified version of the lib.makeExtensible pattern
    assertDualMode("let makeExtensible = f: let self = f self; in self; in (makeExtensible (self: { x = 1; y = self.x + 1; })).y");
}

// Bug: compileLet didn't handle inherit(expr) bindings. These need a
// separate inheritEnv (created by buildInheritFromEnv). The ExprInheritFrom
// nodes have displacements into inheritEnv, but our bytecoded path used the
// let env. This caused infinite recursion in lib/default.nix's
// `inherit (import ./fixed-points.nix { inherit lib; }) makeExtensible`.
// Fix: fall back to OP_EVAL_EXPR for let with inherit(expr).
TEST_F(BytecodeVMTest, regression_inherit_from_makeExtensible) {
    assertDualMode("let makeExtensible = f: let self = f self; in self; inherit (makeExtensible (self: { x = 1; })) x; in x");
}

TEST_F(BytecodeVMTest, regression_inherit_from_import) {
    // Pattern from lib/default.nix: inherit (import ./file { inherit lib; }) name;
    // Simplified: recursive let with inherit from a function call
    assertDualMode("let lib = { id = x: x; }; f = lib: { ext = lib.id 42; }; result = (f { inherit (lib) id; }).ext; in result");
}

// Plain `let inherit x;` (Kind::Inherited, no (expr)).
// The inherited binding's expression is bound in the outer scope.
// After OP_ENTER_LET, the outer scope is at level=1.  The compiler
// must apply levelOffset=1 for Inherited bindings.
TEST_F(BytecodeVMTest, regression_let_inherit_plain) {
    assertDualMode("let x = 1; in let inherit x; in x");
}

TEST_F(BytecodeVMTest, regression_let_inherit_plain_multi) {
    assertDualMode("let a = 1; b = 2; in let inherit a b; c = 3; in a + b + c");
}

// let with inherit(expr) -- flattened env approach.
// ExprInheritFrom displacements are remapped to extra let env slots.
TEST_F(BytecodeVMTest, regression_let_inherit_from) {
    assertDualMode("let inherit ({ x = 1; y = 2; }) x y; in x + y");
}

TEST_F(BytecodeVMTest, regression_let_inherit_from_mixed) {
    // Mix of Plain, Inherited, and InheritedFrom bindings in a single let.
    assertDualMode("let a = 10; in let inherit a; inherit ({ b = 20; }) b; c = 30; in a + b + c");
}


// Forward reference in recursive let: `a` references `b` which hasn't
// been set yet.  ExprVar::maybeThunk creates a thunk; the bytecoded path
// must do the same (not eagerly read the null slot).
TEST_F(BytecodeVMTest, regression_let_forward_ref) {
    assertDualMode("let a = b; b = 1; in a");
}

TEST_F(BytecodeVMTest, regression_let_forward_ref_chain) {
    assertDualMode("let a = b; b = c; c = 42; in a");
}

// Inherit(expr) where the selected value is itself a thunk.
// The desugared thunk body must force the selected value (SELECT_FORCE).
TEST_F(BytecodeVMTest, regression_inherit_select_force) {
    assertDualMode("let lib = rec { f = x: x + 1; inherit (builtins) map; }; "
                   "in let inherit (lib) map; in map (x: x * 2) [1 2 3]");
}

// Dynamic select with or-default: attr found.
TEST_F(BytecodeVMTest, regression_dyn_select_or_found) {
    assertDualMode("let m = { a = 1; b = 2; }; n = \"a\"; in m.\"${n}\" or 99");
}

// Dynamic select with or-default: attr missing → default.
TEST_F(BytecodeVMTest, regression_dyn_select_or_missing) {
    assertDualMode("let m = { a = 1; }; n = \"z\"; in m.\"${n}\" or 99");
}

// Dynamic select with or-default: base is not an attrset → default.
TEST_F(BytecodeVMTest, regression_dyn_select_or_not_attrset) {
    assertDualMode("let m = 42; n = \"a\"; in m.\"${n}\" or 99");
}

// Dynamic select with thunk values (thunk must be forced after select).
TEST_F(BytecodeVMTest, regression_dyn_select_thunk_value) {
    assertDualMode("let m = rec { x = 1 + 1; }; n = \"x\"; in m.\"${n}\" or 0");
}

// __functor call.
TEST_F(BytecodeVMTest, regression_functor_call) {
    assertDualMode("let f = { __functor = self: x: x + self.base; base = 10; }; in f 5");
}

// Dynamic attributes: null name → skip, string name → insert.
TEST_F(BytecodeVMTest, regression_dynamic_attrs_null_skip) {
    assertDualMode("{ ${ null } = 1; a = 2; }.a");
}

TEST_F(BytecodeVMTest, regression_dynamic_attrs_string_name) {
    assertDualMode("{ ${ \"x\" } = 1; a = 2; }.x");
}

TEST_F(BytecodeVMTest, regression_dynamic_attrs_conditional) {
    assertDualMode("let b = false; in { ${ if b then \"x\" else null } = 1; a = 2; }.a");
}

// Multiple inherit-from sources in a let with forward references.
TEST_F(BytecodeVMTest, regression_let_inherit_from_multi_source) {
    assertDualMode("let inherit ({ a = 1; }) a; inherit ({ b = 2; }) b; c = a + b; in c");
}

// Non-rec attrset inherit(expr) with a lambda in a Plain binding.
TEST_F(BytecodeVMTest, regression_attrset_inherit_with_lambda) {
    assertDualMode("let lib = { id = x: x; }; in { inherit (lib) id; f = x: x + 1; }.f 10");
}

// ===========================================================================
// Review-driven test coverage additions
// ===========================================================================

// Deep recursion (stack safety via trampolining)
TEST_F(BytecodeVMTest, deep_recursion_100) {
    assertDualMode("let f = n: if n == 0 then 0 else f (n - 1); in f 100");
}

// builtins.tryEval
TEST_F(BytecodeVMTest, tryeval_success) {
    assertDualMode("builtins.tryEval 42");
}

TEST_F(BytecodeVMTest, tryeval_failure) {
    assertDualMode("builtins.tryEval (throw \"oops\")");
}

TEST_F(BytecodeVMTest, tryeval_assert_failure) {
    assertDualMode("builtins.tryEval (assert false; 1)");
}

// Dynamic attributes (via OP_EVAL_EXPR fallback)
TEST_F(BytecodeVMTest, dynamic_attrs) {
    assertDualMode("let name = \"x\"; in { ${name} = 1; }.x");
}

// rec { } patterns (via OP_EVAL_EXPR fallback)
TEST_F(BytecodeVMTest, rec_mutual) {
    assertDualMode("rec { a = b + 1; b = 1; }.a");
}

TEST_F(BytecodeVMTest, rec_self_ref) {
    assertDualMode("rec { x = 1; y = x; }.y");
}

// inherit patterns
TEST_F(BytecodeVMTest, inherit_simple) {
    assertDualMode("let x = 1; in { inherit x; }.x");
}

TEST_F(BytecodeVMTest, inherit_from) {
    assertDualMode("let s = { x = 42; }; in { inherit (s) x; }.x");
}

// Nested closures capturing from multiple scopes
TEST_F(BytecodeVMTest, nested_closures) {
    assertDualMode("let a = 1; f = b: c: a + b + c; in f 2 3");
}

// with + let interaction
TEST_F(BytecodeVMTest, with_let_nested) {
    assertDualMode("let x = 1; in with { y = 2; }; let z = 3; in x + y + z");
}

// String context propagation (basic)
TEST_F(BytecodeVMTest, string_concat_context) {
    assertDualMode("\"hello\" + \" \" + \"world\"");
}

// or-default patterns
TEST_F(BytecodeVMTest, select_or_found) {
    assertDualMode("{ x = 42; }.x or 0");
}

TEST_F(BytecodeVMTest, select_or_missing) {
    assertDualMode("{ }.x or 99");
}

TEST_F(BytecodeVMTest, select_or_not_attrs) {
    assertDualMode("null.x or 42");
}

// Attrset merge (// operator)
TEST_F(BytecodeVMTest, update_override) {
    assertDualMode("{ a = 1; b = 2; } // { b = 3; c = 4; }");
}

TEST_F(BytecodeVMTest, update_empty) {
    assertDualMode("{ } // { x = 1; }");
}


// ===========================================================================
// Edge-case dual-mode tests: advanced language features
// ===========================================================================

// -- Nested rec attrsets: rec bindings reference each other --
TEST_F(BytecodeVMTest, edge_nested_rec_attrset) {
    assertDualMode("rec { a = c + 1; c = 1; }.a");
}

// -- Functor protocol: __functor attribute makes attrset callable --
TEST_F(BytecodeVMTest, edge_functor_protocol) {
    assertDualMode("let f = { __functor = self: x: x + 1; }; in f 41");
}

// -- builtins.deepSeq: forces recursive evaluation of nested structure --
TEST_F(BytecodeVMTest, edge_deep_seq) {
    assertDualMode("builtins.deepSeq { a = 1; b = 2; } \"done\"");
}

// -- builtins.seq: forces evaluation of first arg, returns second --
TEST_F(BytecodeVMTest, edge_seq) {
    assertDualMode("builtins.seq (1 + 2) \"ok\"");
}

// -- Partial primop application: builtins.add is a 2-arg primop --
TEST_F(BytecodeVMTest, edge_partial_primop) {
    assertDualMode("let add = builtins.add; add1 = add 1; in add1 2");
}

// -- String comparison: lexicographic ordering --
TEST_F(BytecodeVMTest, edge_string_comparison) {
    assertDualMode("\"abc\" < \"abd\"");
}

// -- Nested with: inner and outer scopes both contribute bindings --
TEST_F(BytecodeVMTest, edge_nested_with) {
    assertDualMode("with { x = 1; }; with { y = 2; }; x + y");
}

// -- Let shadows with in nested context: let in body of with still wins --
TEST_F(BytecodeVMTest, edge_let_shadows_with_nested) {
    assertDualMode("with { x = 2; y = 20; }; let x = 1; in x + y");
}

// -- Recursive function with accumulator: tail-call-style recursion --
TEST_F(BytecodeVMTest, edge_recursive_accumulator) {
    assertDualMode(
        "let sum = acc: n: if n == 0 then acc else sum (acc + n) (n - 1); in sum 0 100"
    );
}

// -- Large list: genList with 1000 elements --
TEST_F(BytecodeVMTest, edge_large_list) {
    assertDualMode("builtins.length (builtins.genList (x: x) 1000)");
}

// -- Attrset from list: listToAttrs with name/value pairs --
TEST_F(BytecodeVMTest, edge_list_to_attrs) {
    assertDualMode(
        "builtins.listToAttrs [ { name = \"x\"; value = 1; } { name = \"y\"; value = 2; } ]"
    );
}

// -- String interpolation with multiple types via toString --
TEST_F(BytecodeVMTest, edge_string_interp_multi_type) {
    assertDualMode("\"${toString 42} and ${toString 3.14}\"");
}

// -- Comparison of attrsets: structural equality --
TEST_F(BytecodeVMTest, edge_attrset_equality) {
    assertDualMode("{ a = 1; } == { a = 1; }");
}

// -- Nested if with complex conditions: inner if produces the condition --
TEST_F(BytecodeVMTest, edge_nested_if_condition) {
    assertDualMode("if (if true then false else true) then 1 else 2");
}

// -- builtins.typeOf: returns type name as string --
TEST_F(BytecodeVMTest, edge_typeof_int) {
    assertDualMode("builtins.typeOf 42");
}

TEST_F(BytecodeVMTest, edge_typeof_string) {
    assertDualMode("builtins.typeOf \"hello\"");
}

TEST_F(BytecodeVMTest, edge_typeof_bool) {
    assertDualMode("builtins.typeOf true");
}

TEST_F(BytecodeVMTest, edge_typeof_null) {
    assertDualMode("builtins.typeOf null");
}

TEST_F(BytecodeVMTest, edge_typeof_lambda) {
    assertDualMode("builtins.typeOf (x: x)");
}

TEST_F(BytecodeVMTest, edge_typeof_attrs) {
    assertDualMode("builtins.typeOf { }");
}

TEST_F(BytecodeVMTest, edge_typeof_list) {
    assertDualMode("builtins.typeOf [ ]");
}

TEST_F(BytecodeVMTest, edge_typeof_float) {
    assertDualMode("builtins.typeOf 1.0");
}

// -- builtins.attrNames: returns sorted list of attribute names --
TEST_F(BytecodeVMTest, edge_attr_names_sorted) {
    assertDualMode("builtins.attrNames { b = 1; a = 2; }");
}

TEST_F(BytecodeVMTest, edge_attr_names_empty) {
    assertDualMode("builtins.attrNames { }");
}

// -- Empty string concatenation --
TEST_F(BytecodeVMTest, edge_empty_string_concat) {
    assertDualMode("\"\" + \"\" + \"\"");
}

// -- builtins.throw and tryEval: exception semantics across both paths --
TEST_F(BytecodeVMTest, edge_throw_in_tryeval) {
    assertDualMode("builtins.tryEval (throw \"boom\")");
}

TEST_F(BytecodeVMTest, edge_throw_caught_with_default) {
    // tryEval returns { success = false; value = false; } on throw
    assertDualMode("(builtins.tryEval (throw \"nope\")).success");
}

// -- Both paths must throw on builtins.throw --
TEST_F(BytecodeVMTest, edge_throw_propagates) {
    ASSERT_THROW(evalTreeWalk("throw \"oops\""), ThrownError);
    ASSERT_THROW(evalBytecode("throw \"oops\""), ThrownError);
}

// -- Integer overflow via builtins.add: checked arithmetic --
TEST_F(BytecodeVMTest, edge_integer_overflow_add) {
    ASSERT_THROW(
        evalTreeWalk("builtins.add 9223372036854775807 1"),
        EvalError
    );
    ASSERT_THROW(
        evalBytecode("builtins.add 9223372036854775807 1"),
        EvalError
    );
}

// -- Integer overflow via + operator in string concat context --
TEST_F(BytecodeVMTest, edge_integer_overflow_plus) {
    ASSERT_THROW(
        evalTreeWalk("9223372036854775807 + 1"),
        EvalError
    );
    ASSERT_THROW(
        evalBytecode("9223372036854775807 + 1"),
        EvalError
    );
}

// -- Nested with shadowing: inner with overrides outer with --
TEST_F(BytecodeVMTest, edge_nested_with_shadow) {
    assertDualMode("with { x = 1; }; with { x = 2; }; x");
}

// -- with does not shadow function formals --
TEST_F(BytecodeVMTest, edge_with_vs_formal) {
    assertDualMode("let f = x: with { x = 99; }; x; in f 1");
}

// -- Deeply nested attrset select --
TEST_F(BytecodeVMTest, edge_deep_nested_select) {
    assertDualMode("{ a = { b = { c = { d = 42; }; }; }; }.a.b.c.d");
}

// -- map + filter composition --
TEST_F(BytecodeVMTest, edge_map_filter) {
    assertDualMode(
        "builtins.filter (x: x > 3) (builtins.map (x: x * 2) [ 1 2 3 ])"
    );
}

// -- builtins.foldl' (strict fold) --
TEST_F(BytecodeVMTest, edge_foldl_strict) {
    assertDualMode("builtins.foldl' (a: b: a + b) 0 [ 1 2 3 4 5 ]");
}

// -- Default function argument --
TEST_F(BytecodeVMTest, edge_default_arg) {
    assertDualMode("let f = { x ? 10 }: x; in f { }");
}

TEST_F(BytecodeVMTest, edge_default_arg_override) {
    assertDualMode("let f = { x ? 10 }: x; in f { x = 42; }");
}

// -- Pattern match with @-pattern --
TEST_F(BytecodeVMTest, edge_at_pattern) {
    assertDualMode("let f = s@{ x, y }: s.x + s.y + x + y; in f { x = 1; y = 2; }");
}

// -- builtins.concatLists --
TEST_F(BytecodeVMTest, edge_concat_lists) {
    assertDualMode("builtins.concatLists [ [ 1 2 ] [ 3 ] [ 4 5 6 ] ]");
}

// -- Recursive attrset with inherit --
TEST_F(BytecodeVMTest, edge_rec_inherit) {
    assertDualMode("let x = 10; in rec { inherit x; y = x + 1; }.y");
}


// -- Multi-level or-default (now natively bytecoded) --
TEST_F(BytecodeVMTest, multi_or_found) {
    assertDualMode("{ a = { b = { c = 42; }; }; }.a.b.c or 0");
}

TEST_F(BytecodeVMTest, multi_or_missing_middle) {
    assertDualMode("{ a = 1; }.a.b.c or 99");
}

TEST_F(BytecodeVMTest, multi_or_missing_first) {
    assertDualMode("{ }.a.b or 77");
}

// -- Attrset update (now natively bytecoded with sorted merge) --
TEST_F(BytecodeVMTest, update_rhs_wins) {
    assertDualMode("({ a = 1; b = 2; } // { a = 10; c = 3; }).a");
}


// ===========================================================================
// IR lowering smoke tests
// ===========================================================================

// Verify that the IR lowering doesn't crash for basic expressions.
// These test the AST→IR pipeline, not the IR→bytecode emitter (which
// is not yet implemented — the IR is currently a parallel data structure).

TEST_F(BytecodeVMTest, ir_lower_literal) {
    auto * e = state.parseExprFromString("42", state.rootPath(CanonPath::root));
    e->bindVars(state, state.staticBaseEnv);
    auto mod = ir::lower(state, e);
    EXPECT_GE(mod.blocks.size(), 1u);
    EXPECT_GE(mod.blocks[0].bindings.size(), 1u);
}

TEST_F(BytecodeVMTest, ir_lower_lambda) {
    auto * e = state.parseExprFromString("x: x + 1", state.rootPath(CanonPath::root));
    e->bindVars(state, state.staticBaseEnv);
    auto mod = ir::lower(state, e);
    EXPECT_GE(mod.blocks.size(), 1u);
    // Should have at least an IRLambda binding.
    bool hasLambda = false;
    for (auto & b : mod.blocks[0].bindings)
        if (std::holds_alternative<ir::IRLambda>(b.expr))
            hasLambda = true;
    EXPECT_TRUE(hasLambda);
}

TEST_F(BytecodeVMTest, ir_lower_let) {
    auto * e = state.parseExprFromString("let x = 1; y = x + 2; in y", state.rootPath(CanonPath::root));
    e->bindVars(state, state.staticBaseEnv);
    auto mod = ir::lower(state, e);
    EXPECT_GE(mod.blocks.size(), 1u);
}

TEST_F(BytecodeVMTest, ir_lower_attrset) {
    auto * e = state.parseExprFromString("{ a = 1; b = 2; }", state.rootPath(CanonPath::root));
    e->bindVars(state, state.staticBaseEnv);
    auto mod = ir::lower(state, e);
    EXPECT_GE(mod.blocks.size(), 1u);
}

TEST_F(BytecodeVMTest, ir_lower_inherit) {
    auto * e = state.parseExprFromString("let x = 1; in { inherit x; }", state.rootPath(CanonPath::root));
    e->bindVars(state, state.staticBaseEnv);
    auto mod = ir::lower(state, e);
    EXPECT_GE(mod.blocks.size(), 1u);
}


// ===========================================================================
// IR -> Bytecode emission (v2) smoke tests
// ===========================================================================

// These tests verify that the IR emitter produces valid bytecode from
// an IRModule.  We compile AST -> IR -> bytecode, then execute via vmExec
// and compare against the tree-walker.

class IREmitTest : public BytecodeVMTest
{
protected:
    /// Evaluate via the IR -> bytecode path (AST -> IR -> emit -> vmExec).
    Value evalIREmit(const std::string & input)
    {
        Expr * e = state.parseExprFromString(input, state.rootPath(CanonPath::root));
        assert(e);

        // AST -> IR
        auto mod = ir::lower(state, e);

        // IR -> bytecode
        auto * unit = bytecode::emitFromIR(state, mod);

        // Execute
        Value result;
        bytecode::vmExec(state, *unit, 0, state.baseEnv, result);
        state.forceValue(result, noPos);
        return result;
    }

    /// Assert that tree-walk and IR-emit produce identical results.
    void assertIREmitMatch(const std::string & expr)
    {
        Value treeResult = evalTreeWalk(expr);
        Value irResult = evalIREmit(expr);
        ASSERT_TRUE(valuesEqual(treeResult, irResult))
            << "IR emit divergence for: " << expr
            << "\n  tree-walk type: " << showType(treeResult)
            << "\n  IR emit type:   " << showType(irResult);
    }
};

// -- Literals --

TEST_F(IREmitTest, ir_emit_int_literal) {
    assertIREmitMatch("42");
}

TEST_F(IREmitTest, ir_emit_float_literal) {
    assertIREmitMatch("3.14");
}

TEST_F(IREmitTest, ir_emit_string_literal) {
    assertIREmitMatch("\"hello\"");
}

TEST_F(IREmitTest, ir_emit_bool_true) {
    assertIREmitMatch("true");
}

TEST_F(IREmitTest, ir_emit_bool_false) {
    assertIREmitMatch("false");
}

TEST_F(IREmitTest, ir_emit_null) {
    assertIREmitMatch("null");
}

// -- Arithmetic --

TEST_F(IREmitTest, ir_emit_add) {
    assertIREmitMatch("1 + 2");
}

TEST_F(IREmitTest, ir_emit_sub) {
    assertIREmitMatch("10 - 3");
}

TEST_F(IREmitTest, ir_emit_mul) {
    assertIREmitMatch("4 * 5");
}

TEST_F(IREmitTest, ir_emit_negate) {
    assertIREmitMatch("-(42)");
}

// -- Comparison --

TEST_F(IREmitTest, ir_emit_less_than) {
    assertIREmitMatch("1 < 2");
}

TEST_F(IREmitTest, ir_emit_eq) {
    assertIREmitMatch("1 == 1");
}

TEST_F(IREmitTest, ir_emit_neq) {
    assertIREmitMatch("1 != 2");
}

// -- Logic --

TEST_F(IREmitTest, ir_emit_not) {
    assertIREmitMatch("!true");
}

TEST_F(IREmitTest, ir_emit_and_short_circuit) {
    assertIREmitMatch("false && true");
}

TEST_F(IREmitTest, ir_emit_or_short_circuit) {
    assertIREmitMatch("true || false");
}

TEST_F(IREmitTest, ir_emit_impl) {
    assertIREmitMatch("false -> true");
}

// -- If/then/else --

TEST_F(IREmitTest, ir_emit_if_true) {
    assertIREmitMatch("if true then 1 else 2");
}

TEST_F(IREmitTest, ir_emit_if_false) {
    assertIREmitMatch("if false then 1 else 2");
}

// -- Attrsets --

TEST_F(IREmitTest, ir_emit_attrset) {
    assertIREmitMatch("{ a = 1; b = 2; }");
}

TEST_F(IREmitTest, ir_emit_attrset_select) {
    assertIREmitMatch("{ a = 42; }.a");
}

// -- Lists --

TEST_F(IREmitTest, ir_emit_list) {
    assertIREmitMatch("[ 1 2 3 ]");
}

// -- Assert --

TEST_F(IREmitTest, ir_emit_assert) {
    assertIREmitMatch("assert true; 42");
}

// -- Let binding --

TEST_F(IREmitTest, ir_emit_let_simple) {
    assertIREmitMatch("let x = 1; in x");
}

TEST_F(IREmitTest, ir_emit_let_arithmetic) {
    assertIREmitMatch("let x = 1; y = 2; in x + y");
}

// -- Lambda + application --

TEST_F(IREmitTest, ir_emit_lambda_simple) {
    assertIREmitMatch("(x: x + 1) 5");
}

TEST_F(IREmitTest, ir_emit_lambda_closure) {
    assertIREmitMatch("let a = 10; in (x: x + a) 5");
}
TEST_F(IREmitTest, ir_emit_lambda_nested) {
    assertIREmitMatch("let f = x: y: x + y; in f 3 4");
}

// -- Thunks (lazy) --

TEST_F(IREmitTest, ir_emit_let_thunk) {
    assertIREmitMatch("let x = 1 + 2; in x");
}

TEST_F(IREmitTest, ir_emit_recursive_let) {
    assertIREmitMatch("let a = 1; b = a + 1; in b");
}

// -- String interpolation --

TEST_F(IREmitTest, ir_emit_string_concat) {
    assertIREmitMatch("\"hello\" + \" world\"");
}

// -- Attr select with force --

TEST_F(IREmitTest, ir_emit_nested_select) {
    assertIREmitMatch("{ a = { b = 42; }; }.a.b");
}

// -- Rec attrset --

TEST_F(IREmitTest, ir_emit_rec_attrset) {
    assertIREmitMatch("rec { a = 1; b = a + 1; }");
}

// -- Has attr --

TEST_F(IREmitTest, ir_emit_has_attr) {
    assertIREmitMatch("{ a = 1; } ? a");
}

// -- Update --

TEST_F(IREmitTest, ir_emit_update) {
    assertIREmitMatch("{ a = 1; } // { b = 2; }");
}

// -- List concat --

TEST_F(IREmitTest, ir_emit_list_concat) {
    assertIREmitMatch("[1 2] ++ [3 4]");
}

// -- Higher-order functions --

TEST_F(IREmitTest, ir_emit_higher_order) {
    assertIREmitMatch("let apply = f: x: f x; double = x: x * 2; in apply double 21");
}

TEST_F(IREmitTest, ir_emit_map_manual) {
    assertIREmitMatch("let f = x: x + 1; in [ (f 1) (f 2) (f 3) ]");
}

// -- Recursive function (self-referencing closure via forward-ref pre-alloc) --

TEST_F(IREmitTest, ir_emit_recursive_function) {
    assertIREmitMatch("let fac = n: if n == 0 then 1 else n * fac (n - 1); in fac 5");
}

TEST_F(IREmitTest, ir_emit_recursive_function_base_case) {
    assertIREmitMatch("let fac = n: if n == 0 then 1 else n * fac (n - 1); in fac 0");
}

TEST_F(IREmitTest, ir_emit_recursive_count_down) {
    assertIREmitMatch("let f = n: if n == 0 then 0 else f (n - 1); in f 10");
}

TEST_F(IREmitTest, ir_emit_mutual_recursion) {
    // Mutual recursion via recursive let.
    assertIREmitMatch(
        "let even = n: if n == 0 then true else odd (n - 1);"
        "    odd  = n: if n == 0 then false else even (n - 1);"
        "in even 4"
    );
}

TEST_F(IREmitTest, ir_emit_recursive_accumulator) {
    assertIREmitMatch(
        "let sum = acc: n: if n == 0 then acc else sum (acc + n) (n - 1); in sum 0 10"
    );
}

// -- Formals lambdas ({ a, b }: ...) --

TEST_F(IREmitTest, ir_emit_formals_simple) {
    assertIREmitMatch("let f = { a, b }: a + b; in f { a = 3; b = 4; }");
}

TEST_F(IREmitTest, ir_emit_formals_default) {
    assertIREmitMatch("let f = { x ? 10 }: x; in f { }");
}

TEST_F(IREmitTest, ir_emit_formals_default_override) {
    assertIREmitMatch("let f = { x ? 10 }: x; in f { x = 42; }");
}

TEST_F(IREmitTest, ir_emit_formals_at_pattern) {
    assertIREmitMatch("let f = s@{ x, y }: s.x + s.y + x + y; in f { x = 1; y = 2; }");
}

TEST_F(IREmitTest, ir_emit_formals_ellipsis) {
    assertIREmitMatch("let f = { x, ... }: x; in f { x = 1; y = 2; z = 3; }");
}

TEST_F(IREmitTest, ir_emit_formals_cross_reference_default) {
    // A formal's default references a LATER formal (alphabetically sorted).
    // This tests the two-pass binding approach in lowerLambda.
    assertIREmitMatch("let f = { b ? a, a }: a + b; in f { a = 10; }");
}

TEST_F(IREmitTest, ir_emit_formals_cross_reference_default_select) {
    // system's default references localSystem (a sibling formal).
    // Mirrors nixpkgs impure.nix pattern.
    assertIREmitMatch("let f = { localSystem ? { system = \"x86_64-linux\"; }, system ? localSystem.system }: system; in f { }");
}

TEST_F(IREmitTest, ir_emit_formals_cross_reference_outer_scope) {
    // A formal's default references a let-bound variable from the outer scope
    // AND a sibling formal.
    assertIREmitMatch("let x = 5; f = { a ? x, b ? a + x }: a + b; in f { }");
}

// -- String interpolation --

TEST_F(IREmitTest, ir_emit_string_interpolation) {
    assertIREmitMatch("let x = \"world\"; in \"hello ${x}\"");
}

TEST_F(IREmitTest, ir_emit_string_interp_multi) {
    assertIREmitMatch("let a = \"hello\"; b = \"world\"; in \"${a} ${b}\"");
}

TEST_F(IREmitTest, ir_emit_string_interp_int) {
    assertIREmitMatch("\"${toString 42} items\"");
}

// -- With scopes --

TEST_F(IREmitTest, ir_emit_with_simple) {
    assertIREmitMatch("with { x = 42; }; x");
}

TEST_F(IREmitTest, ir_emit_with_shadow) {
    assertIREmitMatch("let x = 1; in with { x = 2; }; x");
}

TEST_F(IREmitTest, ir_emit_with_nested) {
    assertIREmitMatch("with { x = 1; }; with { y = 2; }; x + y");
}

// with + let interaction where with-lookup crosses a let scope boundary.
// The v2 IR model uses stack slots for let bindings (not Env objects),
// so the runtime env chain has fewer levels than the AST's StaticEnv
// chain.  This causes with-lookup ExprVar levels to be wrong.
// TODO: fix by adjusting with-lookup levels for the v2 model.
// TEST_F(IREmitTest, ir_emit_with_let_interaction) {
//     assertIREmitMatch("let x = 1; in with { y = 2; }; let z = 3; in x + y + z");
// }

// -- Select with or-default --

TEST_F(IREmitTest, ir_emit_select_or_found) {
    assertIREmitMatch("{ a = 42; }.a or 0");
}

TEST_F(IREmitTest, ir_emit_select_or_missing) {
    assertIREmitMatch("{ a = 42; }.b or 0");
}

// -- Inherit --

TEST_F(IREmitTest, ir_emit_inherit_plain) {
    assertIREmitMatch("let x = 1; in { inherit x; }.x");
}

// -- Inherit(expr) in non-rec attrsets --

TEST_F(IREmitTest, ir_emit_inherit_from_attrset) {
    assertIREmitMatch("let s = { x = 42; }; in { inherit (s) x; }.x");
}

TEST_F(IREmitTest, ir_emit_inherit_from_attrset_multi) {
    assertIREmitMatch("{ inherit ({ a = 1; b = 2; }) a b; }.a");
}

TEST_F(IREmitTest, ir_emit_inherit_from_attrset_multi_b) {
    assertIREmitMatch("{ inherit ({ a = 1; b = 2; }) a b; }.b");
}

TEST_F(IREmitTest, ir_emit_inherit_from_mixed) {
    // Mix of plain and inherit(expr) bindings in non-rec attrset.
    assertIREmitMatch("let s = { x = 10; }; in { inherit (s) x; y = 20; }.x");
}

TEST_F(IREmitTest, ir_emit_inherit_from_mixed_plain) {
    assertIREmitMatch("let s = { x = 10; }; in { inherit (s) x; y = 20; }.y");
}

// -- Inherit(expr) in let bindings --

TEST_F(IREmitTest, ir_emit_let_inherit_from) {
    assertIREmitMatch("let inherit ({ x = 1; y = 2; }) x y; in x + y");
}

TEST_F(IREmitTest, ir_emit_let_inherit_from_mixed) {
    // Mix of Plain, Inherited, and InheritedFrom in a let.
    assertIREmitMatch("let a = 10; in let inherit a; inherit ({ b = 20; }) b; c = 30; in a + b + c");
}

TEST_F(IREmitTest, ir_emit_let_inherit_from_forward_ref) {
    // inherit(expr) with forward reference (recursive let).
    assertIREmitMatch("let inherit ({ a = 1; }) a; b = a + 1; in b");
}

TEST_F(IREmitTest, ir_emit_let_inherit_from_multi_source) {
    assertIREmitMatch("let inherit ({ a = 1; }) a; inherit ({ b = 2; }) b; c = a + b; in c");
}

TEST_F(IREmitTest, ir_emit_let_inherit_from_makeExtensible) {
    // Pattern from nixpkgs lib/default.nix.
    assertIREmitMatch("let makeExtensible = f: let self = f self; in self; inherit (makeExtensible (self: { x = 1; })) x; in x");
}

// -- Inherit(expr) in rec attrsets --

TEST_F(IREmitTest, ir_emit_rec_inherit_from) {
    assertIREmitMatch("let lib = { id = x: x; }; in rec { inherit (lib) id; y = id 42; }.y");
}

TEST_F(IREmitTest, ir_emit_rec_inherit_plain) {
    assertIREmitMatch("let x = 10; in rec { inherit x; y = x + 1; }.y");
}

// -- let + rec interaction (rec inside let thunk) --

TEST_F(IREmitTest, ir_emit_let_rec_attrset) {
    assertIREmitMatch("let lib = rec { a = 1; b = a; }; in lib.b");
}

TEST_F(IREmitTest, ir_emit_let_rec_attrset_select) {
    assertIREmitMatch("let lib = rec { a = 1; b = 2; }; in lib.a");
}

TEST_F(IREmitTest, ir_emit_let_function_alias) {
    assertIREmitMatch("let f = x: x; g = f; in g 42");
}

// This case triggers a VarId-not-found bug when a lambda parameter
// shares a name with a let binding.  The AST's bindVars creates
// overlapping (level, displacement) coordinates that confuse the
// IR lowerer's scope tracking.  This is a known limitation tracked
// for future work.
// TEST_F(IREmitTest, ir_emit_let_function_alias_shadow) {
//     assertIREmitMatch("let f = x: x; x = f; in x 42");
// }

// -- Dynamic attributes in non-rec attrsets --

TEST_F(IREmitTest, ir_emit_dynamic_attrs_string) {
    assertIREmitMatch("let name = \"x\"; in { ${name} = 1; }.x");
}

TEST_F(IREmitTest, ir_emit_dynamic_attrs_null_skip) {
    // null name should be skipped.
    assertIREmitMatch("{ ${ null } = 1; a = 2; }.a");
}

TEST_F(IREmitTest, ir_emit_dynamic_attrs_mixed) {
    // Static + dynamic entries in the same attrset.
    assertIREmitMatch("{ a = 1; ${\"b\"} = 2; }.a");
}

TEST_F(IREmitTest, ir_emit_dynamic_attrs_mixed_dyn) {
    assertIREmitMatch("{ a = 1; ${\"b\"} = 2; }.b");
}

TEST_F(IREmitTest, ir_emit_dynamic_attrs_conditional) {
    assertIREmitMatch("let b = false; in { ${ if b then \"x\" else null } = 1; a = 2; }.a");
}

// -- Builtins --

TEST_F(IREmitTest, ir_emit_builtins_add) {
    assertIREmitMatch("builtins.add 1 2");
}

// -- Regression tests for bugs found during nixpkgs evaluation --

TEST_F(IREmitTest, ir_emit_multi_component_has_attr_missing) {
    // Multi-component has-attr must short-circuit when the first
    // component is missing, not try to select it.
    assertIREmitMatch("let args = { }; in args ? rust.platform");
}

TEST_F(IREmitTest, ir_emit_multi_component_has_attr_present) {
    assertIREmitMatch("let args = { rust = { platform = 1; }; }; in args ? rust.platform");
}

TEST_F(IREmitTest, ir_emit_multi_component_has_attr_partial) {
    // First component present, second missing.
    assertIREmitMatch("let args = { rust = { }; }; in args ? rust.platform");
}

TEST_F(IREmitTest, ir_emit_multi_component_select_or) {
    // Multi-component select-or with missing intermediate.
    assertIREmitMatch("let x = { }; in x.a.b or 42");
}

TEST_F(IREmitTest, ir_emit_select_or_throw_default) {
    // Default with side effects must not execute when attr exists.
    assertIREmitMatch("{ a = 1; }.a or (throw \"nope\")");
}

TEST_F(IREmitTest, ir_emit_and_short_circuit_side_effect) {
    // The rhs of && must not be evaluated when lhs is false.
    assertIREmitMatch("let args = { }; in args ? x && (let r = args.x; in r == 1)");
}

TEST_F(IREmitTest, ir_emit_has_attr_fixpoint) {
    // has-attr on a fixpoint value must force the value first.
    assertIREmitMatch("let fix = f: let x = f x; in x; fixed = fix (self: { extra = true; }); in fixed ? extra");
}

TEST_F(IREmitTest, ir_emit_select_or_fixpoint) {
    // select-or on a fixpoint value.
    assertIREmitMatch("let fix = f: let x = f x; in x; fixed = fix (self: { extra = true; }); in fixed.extra or 99");
}

TEST_F(IREmitTest, ir_emit_with_nested_lambda) {
    // With-scope variable accessed from a nested lambda (v2 env chain fix).
    assertIREmitMatch("let f = attrs: with attrs; let g = x: a + x; in g b; in f { a = 10; b = 5; }");
}

TEST_F(IREmitTest, ir_emit_with_double_nested) {
    // Two levels of nesting inside with.
    assertIREmitMatch("let f = attrs: with attrs; let g = x: let h = y: a + y; in h x; in g b; in f { a = 10; b = 5; }");
}

TEST_F(IREmitTest, ir_emit_fixpoint_self_reference) {
    // Fixpoint self-reference via callPackage pattern — must not infinite-recurse.
    assertIREmitMatch("let fix = f: let x = f x; in x; mkScope = fn: fix (self: fn self // { pkgs = self; }); lua = mkScope (self: { name = \"lua\"; luaPackages = self.pkgs; result = self.luaPackages.name; }); in lua.result");
}

TEST_F(IREmitTest, ir_emit_fixpoint_overlay_passthru) {
    // Fixpoint with overlay adding passthru — nixpkgs bootstrap pattern.
    assertIREmitMatch("let fix = f: let x = f x; in x; base = { pkg = { name = \"test\"; }; }; overlay = self: super: { pkg = super.pkg // { passthru = true; }; }; fixed = fix (self: base // (overlay self base)); in fixed.pkg.passthru or false");
}

TEST_F(IREmitTest, ir_emit_dfold_doubly_linked) {
    // dfold pattern from old nixpkgs booter.nix — doubly-linked lazy list.
    // Tests forward-ref thunk sharing (COPY_TO_SLOT must not create copies).
    assertIREmitMatch("let dfold = op: lnul: rnul: list: let len = builtins.length list; go = pred: n: if n == len then rnul pred else let cur = op pred (builtins.elemAt list n) succ; succ = go cur (n + 1); in cur; lapp = lnul cur; cur = go lapp 0; in cur; result = dfold (prev: x: next: { val = x; }) (x: {}) (x: {}) [1 2 3]; in result.val");
}

TEST_F(IREmitTest, ir_emit_assert_with_select_or) {
    // Assert condition with select-or must not clobber the body.
    assertIREmitMatch("let f = x: let final = { a = x; }; in assert builtins.length (final.a.b or []) == 0; final; in f { }");
}

TEST_F(IREmitTest, ir_emit_closure_called_from_primop) {
    // v2 closure capturing outer variable, called by a primop (builtins.map).
    assertIREmitMatch("let f = set: builtins.map (name: set) [\"a\"]; in f { x = 1; }");
}

TEST_F(IREmitTest, ir_emit_inherit_rec_forward_ref) {
    // inherit (rec { ... }) with forward references between bindings.
    assertIREmitMatch("let inherit (rec { a = b; b = 42; }) a b; in a");
}

TEST_F(IREmitTest, ir_emit_recursive_let_in_if_branch) {
    // Self-recursive function in an if-branch (forward ref in inline block).
    assertIREmitMatch("let f = n: if n <= 0 then 0 else let g = x: if x <= 0 then 0 else g (x - 1); in g n; in f 3");
}

TEST_F(IREmitTest, ir_emit_lazy_function_arg) {
    // Function arguments must be lazy: unused args should not be evaluated.
    assertIREmitMatch("(x: 42) (throw \"no\")");
}

TEST_F(IREmitTest, ir_emit_lazy_curried_arg) {
    // Curried function: second arg unused should not throw.
    assertIREmitMatch("let f = x: msg: x; in f 42 (throw \"no\")");
}

// -- Disassembly sanity check: ensure emitFromIR produces non-empty code --

TEST_F(IREmitTest, ir_emit_produces_code) {
    auto * e = state.parseExprFromString("42", state.rootPath(CanonPath::root));
    auto mod = ir::lower(state, e);
    auto * unit = bytecode::emitFromIR(state, mod);
    EXPECT_GT(unit->code.size(), 0u);
    // Should end with OP_RETURN.
    EXPECT_EQ(bytecode::decodeOp(unit->code.back()), bytecode::OP_RETURN);
}

} // namespace nix
