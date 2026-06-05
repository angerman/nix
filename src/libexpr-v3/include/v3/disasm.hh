#pragma once
/// @file
/// v3 bytecode disassembler.  Originally "WC-32 minimal" (eval-order
/// divergence debugging); upgraded 2026-06-05 to high-quality:
/// per-function framing + operand resolution + symbolic jump labels.
/// See disasm.cc for the full format.
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
/// (advances past data words).  Self-contained: resolves the operand
/// against `cu` (literals→values, primops→names, MAKE_*→func names,
/// attr ops→symbols, branches→`-> Lk`, OP_POS→file:line:col).  Useful
/// for cycle-trace + crash-context integration.
uint32_t disassembleOne(
    std::FILE * out,
    const CompilationUnit & cu,
    uint32_t ip);

/// Disassemble a WHOLE compilation unit, framed per function:
///   ; module functions=N code=M entry=E
///   ; func K "name" arity=A nUp=U nLocals=L entry=E
///   L<off>:                       (branch-target leader labels)
///     [ip] OP_... operand=N  ; <resolved>
/// Functions are segmented via `lambdaCodeOffsets`; a module-wide
/// pre-pass labels branch targets (absolute offsets).  Readable +
/// `v3-check` CHECK-LABEL-friendly; used by `NIX_V3_EMIT_BYTECODE`
/// and `v3-eval --emit-bytecode`.
void disassembleModule(
    std::FILE * out,
    const CompilationUnit & cu);

/// Map an Op code to its `OP_*` mnemonic string.  Returns "OP_???"
/// for unknown codes.  Used by the per-opcode dispatch counter
/// reporter (NIX_VM_OPCOUNTS) and any future profiling tools.
const char * opName(Op op);

} // namespace nix::v3
