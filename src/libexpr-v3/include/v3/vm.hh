#pragma once
/// @file
/// v3 VM: dispatch loop + slim CallFrame.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/value.hh"
#include "v3/closure.hh"

#include <vector>
#include <cstdint>

namespace nix::v3 {

/// Slim 32-byte CallFrame.  No continuation state (rare; goes to side-table
/// when added).  No register-form result-store fields.
struct CallFrame
{
    const CompilationUnit * cu;        // 8
    uint32_t  ip;                       // 4
    uint16_t  resultSlot;               // 2: relative to caller's stackBase
    uint8_t   flags;                    // 1
    uint8_t   _pad0;                    // 1
    uint32_t  stackBaseOffset;          // 4
    const Closure * closure;            // 8
    Value *   resultPtr;                // 8: where return value is written
};
static_assert(sizeof(CallFrame) <= 40, "CallFrame should be small");

/// Per-EvalState VM state.
struct VMState
{
    std::vector<Value>     valueStack;   // operand + locals
    std::vector<CallFrame> frames;
    uint64_t nrInstructions = 0;
};

/// Bytecode IR → CompilationUnit pipeline.
namespace ir { struct Module; }
CompilationUnit compile(const ir::Module & m);

/// Run the top-level CU's entry until OP_HALT, returning the final value.
Value run(const CompilationUnit & cu);

} // namespace nix::v3
