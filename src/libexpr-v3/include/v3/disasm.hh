#pragma once
/// @file
/// WC-32: Minimal v3 bytecode disassembler.  Used to investigate
/// eval-order divergences between v3 and tree-walker.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"

#include <cstdio>
#include <cstdint>

namespace nix::v3 {

/// Disassemble a window of instructions to `out`.
/// Prints `[ip] OP_NAME operand=N` per line.  Multi-word ops show
/// the extra words on the same line as `data=X,Y,...`.
///
/// Returns the next ip after the window (caller may advance).
uint32_t disassembleWindow(
    std::FILE * out,
    const CompilationUnit & cu,
    uint32_t startIp,
    uint32_t endIp);

/// Disassemble a single instruction at `ip`.  Returns the ip after
/// (advances past data words).  Useful for cycle-trace integration.
uint32_t disassembleOne(
    std::FILE * out,
    const CompilationUnit & cu,
    uint32_t ip);

/// Map an Op code to its `OP_*` mnemonic string.  Returns "OP_???"
/// for unknown codes.  Used by the per-opcode dispatch counter
/// reporter (NIX_VM_OPCOUNTS) and any future profiling tools.
const char * opName(Op op);

/// Number of extra (post-opcode) data words consumed by `op`,
/// possibly depending on the operand-encoded count.  `cu` and `ip`
/// are passed in case future opcodes need to inspect upcoming
/// data words for variable-length sections.  Returns 0 for fixed
/// single-word opcodes.  Exported for emit-time / post-emit
/// passes that walk the code linearly (#785 peephole).
uint32_t opExtraWords(Op op, uint32_t operand,
                      const CompilationUnit & cu, uint32_t ip);

} // namespace nix::v3
