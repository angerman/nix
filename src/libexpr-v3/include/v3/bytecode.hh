#pragma once
/// @file
/// v3 bytecode — minimal opcode set for the bring-up subset.
///
/// Encoding: a single 32-bit Instruction.  Top 8 bits = opcode; low 24 bits
/// = operand (or two packed 12-bit operands; opcode-specific).  Multi-word
/// instructions (e.g. CALL with N args) follow with extra data words.
///
/// Designed to be expanded as we cover more of Nix.  The bring-up subset
/// uses ~10 opcodes; full Nix coverage will add ~40 more.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/closure.hh"

#include <cstdint>
#include <vector>

namespace nix::v3 {

using Instruction = uint32_t;

enum Op : uint8_t
{
    OP_NOP            = 0x00,

    // Bring-up subset.
    OP_LIT_INT        = 0x01,  // [imm:24]   push tagged int (24-bit signed)
    OP_LIT_INT_BIG    = 0x02,  // [const:24] push int from constants pool
    OP_GET_LOCAL      = 0x03,  // [slot:24]  push frame.locals[slot]
    OP_GET_UPVALUE    = 0x04,  // [idx:24]   push closure->upvalues[idx]
    OP_SET_LOCAL      = 0x05,  // [slot:24]  pop into frame.locals[slot]

    OP_ADD            = 0x10,
    OP_SUB            = 0x11,
    OP_MUL            = 0x12,

    OP_MAKE_CLOSURE   = 0x20,  // [funcIdx:24]; nUpvalues data word follows; pops nUpvalues from stack
    OP_CALL           = 0x21,  // pop arg, pop fun, push result (single-arg call)
    OP_RETURN         = 0x22,  // pop result, return to caller

    OP_HALT           = 0xFF,  // top-level "stop dispatch loop"
};

constexpr inline Op decodeOp(Instruction i) noexcept
{
    return static_cast<Op>((i >> 24) & 0xFF);
}

constexpr inline uint32_t decodeOperand(Instruction i) noexcept
{
    return i & 0x00FFFFFF;
}

constexpr inline int32_t decodeSignedOperand(Instruction i) noexcept
{
    int32_t op = static_cast<int32_t>(i & 0x00FFFFFF);
    if (op & 0x00800000) op |= 0xFF000000; // sign extend from 24-bit
    return op;
}

constexpr inline Instruction encode(Op op, uint32_t operand = 0) noexcept
{
    return (static_cast<uint32_t>(op) << 24) | (operand & 0x00FFFFFF);
}

// ---------------------------------------------------------------------------
// CompilationUnit
// ---------------------------------------------------------------------------

struct CompilationUnit
{
    /// Flat instruction stream.
    std::vector<Instruction> code;

    /// Constants pool (large ints, future strings).
    std::vector<int64_t> intConstants;

    /// Lambda descriptors, indexed by IRLambda::funcIdx.
    std::vector<LambdaDescriptor> lambdas;

    /// Per-lambda starting offset within `code`.  Same length as `lambdas`.
    std::vector<uint32_t> lambdaCodeOffsets;

    /// Top-level entry offset.
    uint32_t entryOffset = 0;
};

} // namespace nix::v3
