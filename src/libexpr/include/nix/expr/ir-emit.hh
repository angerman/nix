#pragma once
/// @file
/// IR -> Bytecode emitter (VM v2).
///
/// Translates an IRModule (produced by ir::lower()) into a CompilationUnit
/// using upvalue-based closures instead of env chains.
///
/// The emitter produces bytecode that the EXISTING vm.cc dispatch loop can
/// execute.  New opcodes (OP_GET_UPVALUE, OP_MAKE_CLOSURE_V2,
/// OP_MAKE_THUNK_V2, OP_GET_STACK_SLOT, OP_SET_STACK_SLOT) are additive --
/// all v1 opcodes continue to work for code compiled by the v1 compiler.
///
/// Key design decisions:
///   1. VarId -> stack slot mapping.  Each VarId gets a dedicated stack slot
///      within the frame.  The frame reserves `maxVarId` slots at entry.
///      OP_GET_STACK_SLOT/OP_SET_STACK_SLOT read/write by frame-relative index.
///
///   2. Upvalue capture.  When emitting an IRLambda or IRMkThunk creation,
///      the FreeVars list determines which enclosing variables to capture.
///      Each free variable is pushed onto the operand stack, then
///      OP_MAKE_CLOSURE_V2 / OP_MAKE_THUNK_V2 pops them into a flat
///      GC-traced Value** array stored on the closure/thunk.
///
///   3. Upvalue access.  Inside a lambda/thunk body, free variables are
///      accessed via OP_GET_UPVALUE(idx).  The idx corresponds to the
///      position in the FreeVars list (which is sorted, so deterministic).
///
///   4. Block layout.  Each IRBlock is emitted as a contiguous region of
///      instructions.  TermReturn emits OP_RETURN.  TermBranch is lowered
///      to OP_JUMP_IF_FALSE + OP_JUMP.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"
#include "nix/expr/ir.hh"

namespace nix {

class EvalState;

namespace bytecode {

/// Emit bytecode from an IRModule, returning a GC-allocated CompilationUnit.
///
/// The IRModule must have been through computeFreeVars() (lower() does this
/// automatically).  The resulting CompilationUnit uses the v2 opcodes
/// (OP_GET_UPVALUE, OP_MAKE_CLOSURE_V2, OP_MAKE_THUNK_V2) for closure
/// and thunk creation, and OP_GET_STACK_SLOT/OP_SET_STACK_SLOT for local
/// variable access within a block.
///
/// The CompilationUnit is fully compatible with the existing vmExec()
/// dispatch loop -- v2 opcodes are handled alongside v1 opcodes.
CompilationUnit * emitFromIR(EvalState & state, const ir::IRModule & module);

} // namespace bytecode
} // namespace nix
