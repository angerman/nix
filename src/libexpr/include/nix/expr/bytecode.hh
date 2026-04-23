#pragma once
/// @file
/// Bytecode VM instruction encoding, opcode definitions, and compilation
/// unit structure for the Nix expression evaluator.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/eval-gc.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/pos-idx.hh"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace nix {

// Forward declarations for types used by pointer only.
struct Expr;
struct Formals;
struct ExprLambda;
struct SourcePath;
class EvalState;

} // namespace nix

namespace nix::bytecode {

// ---------------------------------------------------------------------------
// Instruction encoding
// ---------------------------------------------------------------------------
//
// Fixed-width 32-bit instructions:
//
//   31       24 23                    0
//  +----------+------------------------+
//  |  opcode  |       operand          |
//  | (8 bits) |       (24 bits)        |
//  +----------+------------------------+
//
// The operand is unsigned by default. For jump offsets, we sign-extend
// from 24 bits (range: -8388608 .. +8388607 instruction words).
//
// Two-operand instructions pack level:8 | displacement:16 into the
// 24-bit operand field.

using Instruction = uint32_t;

static constexpr uint32_t kOperandBits    = 24;
static constexpr uint32_t kOperandMask    = (1u << kOperandBits) - 1;
static constexpr int32_t  kSignedMax      = (1 << (kOperandBits - 1)) - 1;
static constexpr int32_t  kSignedMin      = -(1 << (kOperandBits - 1));

/// Encode an instruction from opcode + unsigned operand.
[[gnu::always_inline]]
inline constexpr Instruction encode(uint8_t op, uint32_t operand = 0) noexcept
{
    return (static_cast<uint32_t>(op) << kOperandBits) | (operand & kOperandMask);
}

/// Decode the 8-bit opcode from an instruction word.
[[gnu::always_inline]]
inline constexpr uint8_t decodeOp(Instruction instr) noexcept
{
    return static_cast<uint8_t>(instr >> kOperandBits);
}

/// Decode the 24-bit unsigned operand.
[[gnu::always_inline]]
inline constexpr uint32_t decodeOperand(Instruction instr) noexcept
{
    return instr & kOperandMask;
}

/// Decode the operand as a signed 24-bit value (for jump offsets).
[[gnu::always_inline]]
inline constexpr int32_t decodeSigned(Instruction instr) noexcept
{
    uint32_t raw = instr & kOperandMask;
    if (raw & (1u << (kOperandBits - 1)))
        raw |= ~kOperandMask; // sign-extend
    return static_cast<int32_t>(raw);
}

/// Pack (level:8, displacement:16) into a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint32_t packLevelDispl(uint8_t level, uint16_t displ) noexcept
{
    return (static_cast<uint32_t>(level) << 16) | displ;
}

/// Unpack level (high 8 bits) from a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint8_t unpackLevel(uint32_t operand) noexcept
{
    return static_cast<uint8_t>(operand >> 16);
}

/// Unpack displacement (low 16 bits) from a 24-bit operand.
[[gnu::always_inline]]
inline constexpr uint16_t unpackDispl(uint32_t operand) noexcept
{
    return static_cast<uint16_t>(operand & 0xFFFF);
}


// ---------------------------------------------------------------------------
// Opcodes
// ---------------------------------------------------------------------------

enum Op : uint8_t {
    // -- Literals & constants --
    OP_NOP              = 0x00, // no-op (padding)
    OP_CONST            = 0x01, // [constIdx]     push constants[idx]
    OP_TRUE             = 0x02, //                push true
    OP_FALSE            = 0x03, //                push false
    OP_NULL             = 0x04, //                push null
    OP_INT              = 0x05, // [imm24]        push mkInt(imm24)

    // -- Variable access --
    OP_GET_LOCAL_0      = 0x06, // [displ:24]     env->values[displ]
    OP_GET_LOCAL_1      = 0x07, // [displ:24]     env->up->values[displ]
    OP_GET_LOCAL_2      = 0x08, // [displ:24]     env->up->up->values[displ]
    OP_GET_LOCAL_3      = 0x09, // [displ:24]     env->up->up->up->values[displ]
    OP_GET_LOCAL        = 0x0A, // [lvl:8|dsp:16] general (level, displacement)
    OP_GET_WITH         = 0x0B, // [symIdx:24]    dynamic with-chain lookup

    // -- Attribute operations --
    OP_ATTR_SELECT      = 0x0C, // [symIdx:24]    pop attrs, push attrs.sym
    OP_ATTR_SELECT_OR   = 0x0D, // [symIdx:24]    select with or-default (see OP_JUMP)
    OP_HAS_ATTR         = 0x0E, // [symIdx:24]    pop v, push bool (v ? sym)
    OP_ATTR_INSERT      = 0x0F, // [symIdx:24]    pop v, insert into builder
    OP_ATTR_INSERT_DYN  = 0x10, //                pop name+v, insert into builder
    OP_ATTRS_INIT       = 0x11, // [capacity:24]  begin non-rec attrset
    OP_ATTRS_FINISH     = 0x12, //                finalize attrset, push Value
    OP_ATTRS_UPDATE     = 0x13, //                pop lhs+rhs, push lhs // rhs
    OP_REC_ATTRS_INIT   = 0x14, // [envSize:24]   begin rec attrset
    OP_REC_ATTRS_FINISH = 0x15, // [hasOverrides]  finalize rec attrset

    // -- List operations --
    OP_LIST_INIT        = 0x16, // [size:24]      begin list
    OP_LIST_ELEM        = 0x17, // [idx:24]       pop v, set list[idx]
    OP_LIST_FINISH      = 0x18, //                finalize list, push Value
    OP_LIST_CONCAT      = 0x19, //                pop l1+l2, push l1 ++ l2

    // -- String operations --
    OP_STR_CONCAT_INIT  = 0x1A, // [nParts:24]    begin string interpolation
    OP_STR_CONCAT_PART  = 0x1B, //                pop v, add to concat state
    OP_STR_CONCAT_FINISH= 0x1C, //                finalize, push result string
    OP_COERCE_TO_STRING = 0x1D, // [flags:24]     pop v, push coerced string

    // -- Control flow --
    OP_JUMP             = 0x1E, // [offset:s24]   unconditional jump
    OP_JUMP_IF_FALSE    = 0x1F, // [offset:s24]   pop, force bool, jump if false
    OP_JUMP_IF_TRUE     = 0x20, // [offset:s24]   pop, force bool, jump if true
    OP_JUMP_IF_NOT_ATTRS= 0x21, // [offset:s24]   force top; jump if not attrset (no pop)
    OP_JUMP_IF_NO_ATTR  = 0x22, // [symIdx:24]    jump if top attrs lacks sym (next word=offset)

    // -- Thunks & closures --
    OP_MAKE_THUNK       = 0x23, // [thunkIdx:24]  push thunk(currentEnv, unit.thunks[idx])
    OP_MAKE_CLOSURE     = 0x24, // [lambdaIdx:24] push closure(currentEnv, unit.lambdas[idx])

    // -- Calling --
    OP_CALL             = 0x25, // [nArgs:24]     pop fun+args, call, push result
    OP_CALL_1           = 0x26, //                pop fun+arg, call, push result
    OP_TAIL_CALL        = 0x27, // [nArgs:24]     tail-call (reuses frame)

    // -- Force & return --
    OP_FORCE            = 0x28, //                force TOS in-place
    OP_RETURN           = 0x29, //                pop result, return to caller

    // -- Environment --
    OP_ENTER_LET        = 0x2A, // [envSize:24]   alloc Env, enter scope
    OP_LEAVE_SCOPE      = 0x2B, //                restore previous env
    OP_SET_ENV_SLOT     = 0x2C, // [displ:24]     pop v, store in currentEnv->values[displ]
    OP_PUSH_WITH        = 0x2D, //                pop attrs, enter with-scope

    // -- Comparison & logic --
    OP_EQ               = 0x2E, //                pop a+b, push deep equality
    OP_NEQ              = 0x2F, //                pop a+b, push !eq
    OP_NOT              = 0x30, //                pop v, push !v (force, assert bool)
    OP_IMPL             = 0x31, //                pop a+b, push !a || b

    // -- Arithmetic --
    OP_ADD              = 0x32, //                pop a+b, push a + b
    OP_SUB              = 0x33, //                pop a+b, push a - b
    OP_MUL              = 0x34, //                pop a+b, push a * b
    OP_DIV              = 0x35, //                pop a+b, push a / b
    OP_NEGATE           = 0x36, //                pop v, push -v
    OP_LESS_THAN        = 0x37, //                pop a+b, push a < b

    // -- Assertion --
    OP_ASSERT           = 0x38, //                pop cond, throw if false

    // -- Scope helpers --
    OP_INHERIT_FROM_INIT= 0x39, // [nExprs:24]    alloc inherit-from Env
    OP_INHERIT_FROM_SET = 0x3A, // [displ:24]     pop v, set in inheritEnv
    OP_SET_ENV_SLOT_UP  = 0x41, // [displ:24]     pop v, store in curEnv->up->values[displ]
    OP_ATTR_SELECT_DYN  = 0x42, //                pop nameVal+attrs, select attr by string name
    OP_HAS_ATTR_DYN     = 0x43, //                pop nameVal, peek attrs, push bool (dynamic has-attr)
    OP_ATTRS_DYN_INIT   = 0x44, // [nS:12|nD:12] build attrset with nS static + nD dynamic attrs

    // -- Miscellaneous --
    OP_POS              = 0x3B, // [posIdx:24]    push __curPos attrset
    OP_DUP              = 0x3C, //                duplicate TOS
    OP_POP              = 0x3D, //                discard TOS
    OP_SWAP             = 0x3E, //                swap top two stack entries

    // -- Fallback --
    OP_EVAL_EXPR        = 0x3F, // [exprIdx:24]   fallback: eval exprPool[idx] in current env

    // -- Fused hot-path instructions --
    OP_SELECT_FORCE     = 0x40, // [symIdx:24]    select attr + force result
};


// ---------------------------------------------------------------------------
// Source position table entry (sparse)
// ---------------------------------------------------------------------------

/// Maps an instruction offset to its source position.
/// The position table is sorted by instrOffset for binary search.
struct PosEntry
{
    uint32_t instrOffset;
    PosIdx pos;
};


// ---------------------------------------------------------------------------
// Thunk and Lambda descriptors
// ---------------------------------------------------------------------------

/// Identifies a lazy sub-expression (thunk body) within a CompilationUnit.
/// The VM creates ExprBytecodeThunk objects pointing at these.
struct ThunkDescriptor
{
    uint32_t codeOffset; // Instruction index into CompilationUnit::code
    PosIdx   pos;        // Source position for error messages
    Expr *   sourceExpr = nullptr; // Original AST expression (for isTrivial() compat)
};

/// Identifies a function body within a CompilationUnit.
struct LambdaDescriptor
{
    uint32_t codeOffset; // Instruction index for the lambda body
    PosIdx   pos;        // Source position of the lambda definition
    Symbol   name;       // Lambda name (may be empty for anonymous)
    Symbol   arg;        // Simple parameter name (empty if formals-only)

    /// Formals metadata. nullptr if the lambda takes a single positional
    /// argument (x: ...) rather than a pattern ({ a, b, ... }: ...).
    /// Points into BumpMemoryResource (shared with AST Formals).
    Formals * formals = nullptr;

    /// Number of slots required in the Env for this lambda's body.
    uint16_t envSize = 0;

    /// Back-pointer to the source ExprLambda for profiling compatibility.
    ExprLambda * sourceExpr = nullptr;

    /// Index into CompilationUnit::thunks for the lambda body's
    /// ThunkDescriptor. Pre-allocated at compile time to avoid
    /// runtime vector modifications.  Points to bodyStart (after
    /// the formals-binding prologue), used by ExprBytecodeThunk
    /// when callFunction dispatches via lambda.body->eval().
    uint32_t bodyThunkIdx = 0;

    /// Code offset for the formals-binding prologue.
    /// OP_CALL_1 trampoline jumps here so it runs the bytecoded
    /// formal parameter unpacking before the body.  For simple
    /// lambdas (no formals), prologueOffset == body code offset.
    uint32_t prologueOffset = 0;
};


// ---------------------------------------------------------------------------
// CompilationUnit
// ---------------------------------------------------------------------------

/// One per parsed file or top-level eval. Owns a single flat bytecode
/// buffer plus metadata arrays. Thunks and lambdas are byte ranges
/// within this buffer -- no per-thunk heap allocations.
///
/// GC-allocated so Boehm traces all Value* pointers in the constants pool.
struct CompilationUnit : gc
{
    // -- Bytecode --
    // Flat array of 32-bit instructions.
    std::vector<Instruction, traceable_allocator<Instruction>> code;

    // -- Constants pool --
    // GC-traced array of Value* pointers. Indexed by OP_CONST operand.
    std::vector<Value *, traceable_allocator<Value *>> constants;

    // -- Symbol pool --
    // Attribute names used by OP_ATTR_SELECT, OP_ATTR_INSERT, etc.
    // References into the global EvalState::symbols table.
    std::vector<Symbol> symbols;

    // -- Thunk descriptors --
    // Indexed by OP_MAKE_THUNK operand.
    std::vector<ThunkDescriptor> thunks;

    // -- Lambda descriptors --
    // Indexed by OP_MAKE_CLOSURE operand.
    std::vector<LambdaDescriptor> lambdas;

    // -- PosIdx pool --
    // Source positions used by OP_ATTRS_INIT for attribute positions.
    // Indexed by operand of data words following OP_ATTRS_INIT.
    std::vector<PosIdx> posPool;

    // -- Expr fallback pool --
    // Expr* pointers for expressions that are not yet compiled to bytecode.
    // Indexed by OP_EVAL_EXPR operand.  The VM calls expr->eval() on these.
    std::vector<Expr *> exprPool;

    // -- Position table (sparse, sorted by instrOffset) --
    std::vector<PosEntry> positions;

    // -- Source path for cache keying and error messages --
    // May be nullptr for string-evaluated expressions.
    const SourcePath * sourcePath = nullptr;

    /// Look up the source position for a given instruction offset.
    /// Uses binary search over the sparse position table.
    PosIdx posForOffset(uint32_t instrOffset) const noexcept
    {
        auto it = std::upper_bound(
            positions.begin(), positions.end(), instrOffset,
            [](uint32_t offset, const PosEntry & entry) {
                return offset < entry.instrOffset;
            });
        if (it == positions.begin())
            return noPos;
        --it;
        return it->pos;
    }

    /// Append an instruction, returning its index (for patching jumps).
    uint32_t emit(uint8_t op, uint32_t operand = 0)
    {
        uint32_t idx = static_cast<uint32_t>(code.size());
        code.push_back(encode(op, operand));
        return idx;
    }

    /// Patch a previously emitted jump instruction to target the current
    /// code offset. The offset is relative: target - source - 1.
    void patchJump(uint32_t jumpInstrIdx)
    {
        uint32_t target = static_cast<uint32_t>(code.size());
        int32_t offset = static_cast<int32_t>(target) - static_cast<int32_t>(jumpInstrIdx) - 1;
        uint8_t op = decodeOp(code[jumpInstrIdx]);
        code[jumpInstrIdx] = encode(op, static_cast<uint32_t>(offset) & kOperandMask);
    }

    /// Add a Value* to the constants pool, returning its index.
    uint32_t addConstant(Value * v)
    {
        uint32_t idx = static_cast<uint32_t>(constants.size());
        constants.push_back(v);
        return idx;
    }

    /// Add a PosIdx to the pos pool, returning its index.
    uint32_t addPos(PosIdx pos)
    {
        uint32_t idx = static_cast<uint32_t>(posPool.size());
        posPool.push_back(pos);
        return idx;
    }

    /// Add an Expr* to the expr fallback pool, returning its index.
    /// Used for expressions not yet compiled to bytecode (OP_EVAL_EXPR).
    uint32_t addExpr(Expr * e)
    {
        uint32_t idx = static_cast<uint32_t>(exprPool.size());
        exprPool.push_back(e);
        return idx;
    }

    /// Add a Symbol to the symbol pool, returning its index.
    /// Deduplicates: returns existing index if already present.
    /// Defined in bytecode.cc (requires complete Symbol type).
    uint32_t addSymbol(Symbol sym);

    /// Record a source position for the current instruction offset.
    /// Only emits if the position differs from the previous entry.
    void emitPos(PosIdx pos)
    {
        uint32_t offset = static_cast<uint32_t>(code.size());
        if (!positions.empty() && positions.back().pos == pos)
            return; // deduplicate consecutive identical positions
        positions.push_back({offset, pos});
    }
};

/// Return the mnemonic string for an opcode.
const char * opName(uint8_t op);

/// Disassemble a CompilationUnit into a human-readable string.
/// If `state` is non-null, prints constant values and symbol names.
std::string disassemble(const CompilationUnit & unit, const EvalState * state = nullptr);

} // namespace nix::bytecode
