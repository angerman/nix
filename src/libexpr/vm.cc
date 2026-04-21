/// @file
/// Bytecode VM execution loop.
///
/// The dispatch loop uses computed-goto on GCC/Clang for minimal dispatch
/// overhead (~1 indirect branch per instruction vs ~2 for switch).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/vm.hh"
#include "nix/expr/bytecode.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-error.hh"

#include <cassert>

namespace nix::bytecode {

// ---------------------------------------------------------------------------
// VMState
// ---------------------------------------------------------------------------

VMState::VMState()
{
    // GC-allocated so Boehm traces all Value* pointers on the stack.
    stack = static_cast<Value **>(GC_MALLOC(kInitialStackCapacity * sizeof(Value *)));
    if (!stack)
        throw std::bad_alloc();
    sp = stack;
    stackEnd = stack + kInitialStackCapacity;
    frames.reserve(256);
}

void VMState::grow()
{
    size_t oldCap = static_cast<size_t>(stackEnd - stack);
    size_t used   = static_cast<size_t>(sp - stack);
    size_t newCap = oldCap * 2;

    auto * newStack = static_cast<Value **>(GC_MALLOC(newCap * sizeof(Value *)));
    if (!newStack)
        throw std::bad_alloc();

    std::memcpy(newStack, stack, used * sizeof(Value *));

    // Fix up all CallFrame stackBase pointers to reference the new buffer.
    ptrdiff_t delta = newStack - stack;
    for (auto & frame : frames)
        frame.stackBase += delta;

    sp       = newStack + used;
    stack    = newStack;
    stackEnd = newStack + newCap;
    // Old buffer is GC-managed; it will be collected when unreferenced.
}


// ---------------------------------------------------------------------------
// vmExec -- main dispatch loop
// ---------------------------------------------------------------------------

void vmExec(
    EvalState & state,
    const CompilationUnit & unit,
    uint32_t startOffset,
    Env & env,
    Value & result)
{
    // Ensure VMState is initialized.
    if (!state.vmState) [[unlikely]]
        state.vmState = std::make_unique<VMState>();

    auto & vm = *state.vmState;

    // Allocate a result slot that the OP_RETURN will write into.
    Value * resultSlot = &result;

    // Push the initial call frame.
    vm.frames.push_back(CallFrame{
        .unit      = &unit,
        .ip        = startOffset,
        .env       = &env,
        .stackBase = vm.sp,
        .resultSlot = resultSlot,
        .callPos   = unit.posForOffset(startOffset),
    });

    // Frame-local aliases (updated when frames change).
    const CompilationUnit * cu = &unit;
    uint32_t ip   = startOffset;
    [[maybe_unused]] Env * curEnv = &env;

    // ------------------------------------------------------------------
    // Dispatch loop.
    // Use computed-goto where available (GCC/Clang), otherwise switch.
    // ------------------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define NIX_VM_COMPUTED_GOTO 1
#endif

#ifdef NIX_VM_COMPUTED_GOTO
    // Build the dispatch table.  We fill all 256 entries; unused opcodes
    // jump to the `unhandled` label.
    static const void * dispatchTable[256] = {
        // Fill with unhandled first, then patch known opcodes.
        // (C++ doesn't allow designated array init with goto labels,
        //  so we initialize in a static block below.)
    };

    // Static initialization of the dispatch table.
    // This is a bit ugly but GCC/Clang handle it correctly.
    static bool tableInitialized = false;
    if (!tableInitialized) [[unlikely]] {
        for (int i = 0; i < 256; i++)
            const_cast<const void *&>(dispatchTable[i]) = &&op_unhandled;

#define REGISTER_OP(op, label) \
        const_cast<const void *&>(dispatchTable[op]) = &&label

        REGISTER_OP(OP_NOP,     op_nop);
        REGISTER_OP(OP_CONST,   op_const);
        REGISTER_OP(OP_TRUE,    op_true);
        REGISTER_OP(OP_FALSE,   op_false);
        REGISTER_OP(OP_NULL,    op_null);
        REGISTER_OP(OP_INT,     op_int);
        REGISTER_OP(OP_RETURN,  op_return);

        // Phase 1+ opcodes will be registered here as they are implemented.

#undef REGISTER_OP
        tableInitialized = true;
    }

    // Computed-goto dispatch macro.
#define DISPATCH() do {                              \
        Instruction _instr = cu->code[ip++];         \
        goto *dispatchTable[decodeOp(_instr)];       \
    } while (0)

    // We need the current instruction available in each handler.
    // Re-read it (the compiler will CSE this with the dispatch).
#define CUR_INSTR (cu->code[ip - 1])

    DISPATCH();

#else // switch-based fallback

#define DISPATCH() continue
#define CUR_INSTR (cu->code[ip - 1])

    for (;;) {
        Instruction instr = cu->code[ip++];
        switch (decodeOp(instr)) {

#endif // NIX_VM_COMPUTED_GOTO

    // ==================================================================
    // Opcode handlers
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_nop:
#else
    case OP_NOP:
#endif
    {
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_const:
#else
    case OP_CONST:
#endif
    {
        uint32_t idx = decodeOperand(CUR_INSTR);
        vm.push(cu->constants[idx]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_true:
#else
    case OP_TRUE:
#endif
    {
        // Push the global `true` singleton.
        // We need a pointer to a Value that is `true`.
        // EvalState has vTrue; we use state.vTrue here.
        // TODO: use a global true Value* constant.
        auto * v = state.allocValue();
        v->mkBool(true);
        vm.push(v);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_false:
#else
    case OP_FALSE:
#endif
    {
        auto * v = state.allocValue();
        v->mkBool(false);
        vm.push(v);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_null:
#else
    case OP_NULL:
#endif
    {
        auto * v = state.allocValue();
        v->mkNull();
        vm.push(v);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_int:
#else
    case OP_INT:
#endif
    {
        uint32_t imm = decodeOperand(CUR_INSTR);
        auto * v = state.allocValue();
        v->mkInt(static_cast<NixInt::Inner>(imm));
        vm.push(v);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_return:
#else
    case OP_RETURN:
#endif
    {
        Value * retVal = vm.pop();

        // Write the result into the caller's result slot.
        auto & frame = vm.frames.back();
        *frame.resultSlot = *retVal;

        // Restore stack to frame entry point.
        vm.sp = frame.stackBase;
        vm.frames.pop_back();

        if (vm.frames.empty()) {
            // Outermost frame -- we're done.
            return;
        }

        // Resume the caller's frame.
        auto & caller = vm.frames.back();
        cu     = caller.unit;
        ip     = caller.ip;
        curEnv = caller.env;

        // The caller expects the return value on the stack.
        vm.push(retVal);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_unhandled:
#else
    default:
#endif
    {
        uint8_t op = decodeOp(CUR_INSTR);
        throw Error("bytecode VM: unhandled opcode 0x%02x at offset %d", op, ip - 1);
    }

#ifndef NIX_VM_COMPUTED_GOTO
        } // switch
    } // for(;;)
#endif
}

} // namespace nix::bytecode
