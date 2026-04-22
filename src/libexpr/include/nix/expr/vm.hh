#pragma once
/// @file
/// Bytecode VM state and execution interface.
///
/// The VM executes bytecoded Nix expressions using an explicit value stack
/// (of Value* pointers) and a CallFrame stack for trampolining.  This avoids
/// C-stack recursion for bytecoded-to-bytecoded calls.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/eval-gc.hh"
#include "nix/expr/bytecode.hh"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nix {

class EvalState;
struct Env;
struct Value;

namespace bytecode {

/// A saved execution context for a bytecoded function call.
/// The VM maintains an explicit stack of these to implement trampolining:
/// bytecoded-to-bytecoded calls push a CallFrame and jump rather than
/// recursing on the C stack.
struct CallFrame
{
    const CompilationUnit * unit; ///< The compilation unit being executed.
    uint32_t ip;                  ///< Instruction pointer (index into unit->code).
    Env * env;                    ///< Current environment for this frame.
    Value ** stackBase;           ///< Base of this frame's portion of the value stack.
    Value * resultSlot;           ///< Where to write the return value (GC-allocated).
    PosIdx callPos;               ///< Source position of the call site (for stack traces).
    bool isThunkForce = false;    ///< If true, OP_RETURN doesn't push result (thunk was updated in-place).
};

/// Initial capacity of the value stack (in Value* slots).
static constexpr size_t kInitialStackCapacity = 4096;

/// Per-EvalState VM state.  Lazily initialized on first bytecode execution.
/// The value stack and call frame stack are GC-allocated so that Boehm
/// traces all Value*/Env* pointers stored in them.
struct VMState
{
    /// Value stack: array of Value* pointers.
    /// GC-allocated so Boehm conservatively scans it for roots.
    Value ** stack = nullptr;
    Value ** sp    = nullptr;   ///< Stack pointer: next free slot.
    Value ** stackEnd = nullptr;

    /// Call frame stack.
    /// Uses traceable_allocator so GC sees Env*/Value* pointers in frames.
    std::vector<CallFrame, traceable_allocator<CallFrame>> frames;

    VMState();
    ~VMState();

    // -- Stack operations (inline for hot-path performance) --

    [[gnu::always_inline]]
    void push(Value * v)
    {
        if (sp >= stackEnd) [[unlikely]]
            grow();
        *sp++ = v;
    }

    [[gnu::always_inline]]
    Value * pop()
    {
        assert(sp > stack);
        return *--sp;
    }

    [[gnu::always_inline]]
    Value * top() const
    {
        assert(sp > stack);
        return *(sp - 1);
    }

    [[gnu::always_inline]]
    Value * peek(size_t depth) const
    {
        assert(sp - depth > stack);
        return *(sp - 1 - depth);
    }

    /// Number of Value* entries currently on the stack.
    size_t stackSize() const { return static_cast<size_t>(sp - stack); }

    // -- Profiling counters --
    uint64_t nrInstructions = 0;       ///< Total bytecoded instructions executed
    uint64_t nrEvalExprFallbacks = 0;  ///< OP_EVAL_EXPR fallbacks to tree-walker
    uint64_t nrForceOps = 0;           ///< OP_FORCE invocations
    uint64_t nrCallOps = 0;            ///< OP_CALL + OP_CALL_1 invocations
    uint64_t peakStackDepth = 0;       ///< Maximum stack depth observed
    uint64_t peakFrameDepth = 0;       ///< Maximum call frame depth observed
    uint64_t nrBytecodeThunkForces = 0; ///< Thunks forced via ExprBytecodeThunk → vmExec
    uint64_t nrBytecodeCallTrampoline = 0; ///< Lambda calls via OP_CALL_1 trampoline

private:
    void grow();
};


/// Execute bytecode starting at the given offset within a CompilationUnit.
///
/// This is the main VM entry point.  Called from:
/// - ExprBytecodeThunk::eval()  (thunk forcing)
/// - The bytecode compiler's top-level evaluation path
///
/// On return, `result` holds the evaluated value.
///
/// The function uses the VMState attached to `state` for its value stack
/// and call frame stack.  It handles trampolining internally: bytecoded
/// calls to other bytecoded functions push CallFrames rather than
/// recursing on the C stack.
void vmExec(
    EvalState & state,
    const CompilationUnit & unit,
    uint32_t startOffset,
    Env & env,
    Value & result);

} // namespace bytecode
} // namespace nix
