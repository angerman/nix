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
    // OP_ADD (0x32) is not yet implemented in the skeleton VM.
    unit.emit(bytecode::OP_ADD);

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
// Dual-mode test template (for use once the compiler is implemented)
// ===========================================================================
// These are commented out until the bytecode compiler exists.
// They will use:
//
//   void assertDualMode(const std::string & expr) {
//       Value treeResult = evalTreeWalk(expr);
//       Value bcResult = evalBytecode(expr);
//       ASSERT_TRUE(valuesEqual(treeResult, bcResult))
//           << "Semantic divergence for: " << expr;
//   }
//
// Example:
//   TEST_F(BytecodeVMTest, dual_1plus1) { assertDualMode("1 + 1"); }
//   TEST_F(BytecodeVMTest, dual_let)    { assertDualMode("let x = 1; in x"); }
//   TEST_F(BytecodeVMTest, dual_if)     { assertDualMode("if true then 1 else 2"); }
//   TEST_F(BytecodeVMTest, dual_rec)    { assertDualMode("rec { a = b; b = 1; }.a"); }


} // namespace nix
