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
#include "nix/expr/bytecode-thunk.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/print.hh"

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

        // Phase 0: infrastructure
        REGISTER_OP(OP_NOP,     op_nop);
        REGISTER_OP(OP_CONST,   op_const);
        REGISTER_OP(OP_TRUE,    op_true);
        REGISTER_OP(OP_FALSE,   op_false);
        REGISTER_OP(OP_NULL,    op_null);
        REGISTER_OP(OP_INT,     op_int);
        REGISTER_OP(OP_RETURN,  op_return);

        // Phase 1: variables, arithmetic, comparison, logic, control flow
        REGISTER_OP(OP_GET_LOCAL_0, op_get_local_0);
        REGISTER_OP(OP_GET_LOCAL_1, op_get_local_1);
        REGISTER_OP(OP_GET_LOCAL_2, op_get_local_2);
        REGISTER_OP(OP_GET_LOCAL_3, op_get_local_3);
        REGISTER_OP(OP_GET_LOCAL,   op_get_local);
        REGISTER_OP(OP_FORCE,       op_force);
        REGISTER_OP(OP_JUMP,        op_jump);
        REGISTER_OP(OP_JUMP_IF_FALSE, op_jump_if_false);
        REGISTER_OP(OP_JUMP_IF_TRUE,  op_jump_if_true);
        REGISTER_OP(OP_ADD,     op_add);
        REGISTER_OP(OP_SUB,     op_sub);
        REGISTER_OP(OP_MUL,     op_mul);
        REGISTER_OP(OP_DIV,     op_div);
        REGISTER_OP(OP_NEGATE,  op_negate);
        REGISTER_OP(OP_EQ,      op_eq);
        REGISTER_OP(OP_NEQ,     op_neq);
        REGISTER_OP(OP_LESS_THAN, op_less_than);
        REGISTER_OP(OP_NOT,     op_not);
        REGISTER_OP(OP_ASSERT,  op_assert);
        REGISTER_OP(OP_POP,     op_pop);
        REGISTER_OP(OP_DUP,     op_dup);

        // Fallback
        REGISTER_OP(OP_EVAL_EXPR,    op_eval_expr);

        // Phase 2: let-bindings, closures, calls, thunks
        REGISTER_OP(OP_ENTER_LET,    op_enter_let);
        REGISTER_OP(OP_LEAVE_SCOPE,  op_leave_scope);
        REGISTER_OP(OP_SET_ENV_SLOT, op_set_env_slot);
        REGISTER_OP(OP_MAKE_THUNK,   op_make_thunk);
        REGISTER_OP(OP_MAKE_CLOSURE, op_make_closure);
        REGISTER_OP(OP_CALL,         op_call);
        REGISTER_OP(OP_CALL_1,       op_call_1);

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

    // ==================================================================
    // Phase 1: Variables
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_get_local_0:
#else
    case OP_GET_LOCAL_0:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        vm.push(curEnv->values[displ]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_get_local_1:
#else
    case OP_GET_LOCAL_1:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        vm.push(curEnv->up->values[displ]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_get_local_2:
#else
    case OP_GET_LOCAL_2:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        vm.push(curEnv->up->up->values[displ]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_get_local_3:
#else
    case OP_GET_LOCAL_3:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        vm.push(curEnv->up->up->up->values[displ]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_get_local:
#else
    case OP_GET_LOCAL:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t level = unpackLevel(operand);
        uint16_t displ = unpackDispl(operand);
        Env * e = curEnv;
        for (uint8_t l = level; l > 0; --l)
            e = e->up;
        vm.push(e->values[displ]);
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Force
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_force:
#else
    case OP_FORCE:
#endif
    {
        Value * v = vm.top();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*v, pos);
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Control flow
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_jump:
#else
    case OP_JUMP:
#endif
    {
        int32_t offset = decodeSigned(CUR_INSTR);
        ip = static_cast<uint32_t>(static_cast<int32_t>(ip) + offset);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_jump_if_false:
#else
    case OP_JUMP_IF_FALSE:
#endif
    {
        int32_t offset = decodeSigned(CUR_INSTR);
        Value * v = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*v, pos);
        if (v->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%: %2%",
                showType(*v), ValuePrinter(state, *v, PrintOptions{}))
                .atPos(pos).debugThrow();
        if (!v->boolean())
            ip = static_cast<uint32_t>(static_cast<int32_t>(ip) + offset);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_jump_if_true:
#else
    case OP_JUMP_IF_TRUE:
#endif
    {
        int32_t offset = decodeSigned(CUR_INSTR);
        Value * v = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*v, pos);
        if (v->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%: %2%",
                showType(*v), ValuePrinter(state, *v, PrintOptions{}))
                .atPos(pos).debugThrow();
        if (v->boolean())
            ip = static_cast<uint32_t>(static_cast<int32_t>(ip) + offset);
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Arithmetic
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_add:
#else
    case OP_ADD:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        auto * result = state.allocValue();

        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl + fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto sum = lhs->integer() + rhs->integer();
            if (auto v = sum.valueChecked())
                result->mkInt(*v);
            else
                state.error<EvalError>("integer overflow in adding %1% + %2%",
                    lhs->integer(), rhs->integer()).atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot add %1% to %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        }

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_sub:
#else
    case OP_SUB:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        auto * result = state.allocValue();

        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl - fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto diff = lhs->integer() - rhs->integer();
            if (auto v = diff.valueChecked())
                result->mkInt(*v);
            else
                state.error<EvalError>("integer overflow in subtraction %1% - %2%",
                    lhs->integer(), rhs->integer()).atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot subtract %1% from %2%",
                showType(*rhs), showType(*lhs)).atPos(pos).debugThrow();
        }

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_mul:
#else
    case OP_MUL:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        auto * result = state.allocValue();

        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl * fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto prod = lhs->integer() * rhs->integer();
            if (auto v = prod.valueChecked())
                result->mkInt(*v);
            else
                state.error<EvalError>("integer overflow in multiplication %1% * %2%",
                    lhs->integer(), rhs->integer()).atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot multiply %1% and %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        }

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_div:
#else
    case OP_DIV:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        auto * result = state.allocValue();

        if (lhs->type() == nInt && rhs->type() == nInt) {
            if (rhs->integer().value == 0)
                state.error<EvalError>("division by zero").atPos(pos).debugThrow();
            auto quot = lhs->integer() / rhs->integer();
            if (auto v = quot.valueChecked())
                result->mkInt(*v);
            else
                state.error<EvalError>("integer overflow in division").atPos(pos).debugThrow();
        } else {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            if (fr == 0.0)
                state.error<EvalError>("division by zero").atPos(pos).debugThrow();
            result->mkFloat(fl / fr);
        }

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_negate:
#else
    case OP_NEGATE:
#endif
    {
        Value * v = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*v, pos);

        auto * result = state.allocValue();
        if (v->type() == nInt) {
            auto neg = NixInt(0) - v->integer();
            if (auto val = neg.valueChecked())
                result->mkInt(*val);
            else
                state.error<EvalError>("integer overflow in negation").atPos(pos).debugThrow();
        } else if (v->type() == nFloat)
            result->mkFloat(-v->fpoint());
        else
            state.error<EvalError>("cannot negate %1%", showType(*v)).atPos(pos).debugThrow();

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Comparison and logic
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_eq:
#else
    case OP_EQ:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        auto * result = state.allocValue();
        result->mkBool(state.eqValues(*lhs, *rhs, pos, "while comparing two values"));
        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_neq:
#else
    case OP_NEQ:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        auto * result = state.allocValue();
        result->mkBool(!state.eqValues(*lhs, *rhs, pos, "while comparing two values"));
        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_less_than:
#else
    case OP_LESS_THAN:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        bool cmpResult;
        if (lhs->type() == nFloat && rhs->type() == nInt)
            cmpResult = lhs->fpoint() < rhs->integer().value;
        else if (lhs->type() == nInt && rhs->type() == nFloat)
            cmpResult = lhs->integer().value < rhs->fpoint();
        else if (lhs->type() != rhs->type())
            state.error<EvalError>("cannot compare %1% with %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        else if (lhs->type() == nInt)
            cmpResult = lhs->integer() < rhs->integer();
        else if (lhs->type() == nFloat)
            cmpResult = lhs->fpoint() < rhs->fpoint();
        else if (lhs->type() == nString)
            cmpResult = lhs->string_view() < rhs->string_view();
        else if (lhs->type() == nPath)
            cmpResult = lhs->path() < rhs->path();
        else
            state.error<EvalError>("cannot compare %1% with %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();

        auto * result = state.allocValue();
        result->mkBool(cmpResult);
        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_not:
#else
    case OP_NOT:
#endif
    {
        Value * v = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*v, pos);
        if (v->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%: %2%",
                showType(*v), ValuePrinter(state, *v, PrintOptions{}))
                .atPos(pos).debugThrow();
        auto * result = state.allocValue();
        result->mkBool(!v->boolean());
        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Assert
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_assert:
#else
    case OP_ASSERT:
#endif
    {
        Value * cond = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*cond, pos);
        if (cond->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%: %2%",
                showType(*cond), ValuePrinter(state, *cond, PrintOptions{}))
                .atPos(pos).debugThrow();
        if (!cond->boolean())
            state.error<AssertionError>("assertion '%1%' failed", "bytecoded assertion")
                .atPos(pos).debugThrow();
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Stack management
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_pop:
#else
    case OP_POP:
#endif
    {
        vm.pop();
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_dup:
#else
    case OP_DUP:
#endif
    {
        vm.push(vm.top());
        DISPATCH();
    }

    // ==================================================================
    // Phase 2: Let-bindings and scope
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_enter_let:
#else
    case OP_ENTER_LET:
#endif
    {
        uint32_t envSize = decodeOperand(CUR_INSTR);
        Env & env2 = state.mem.allocEnv(envSize);
        env2.up = curEnv;
        curEnv = &env2;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_leave_scope:
#else
    case OP_LEAVE_SCOPE:
#endif
    {
        curEnv = curEnv->up;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_set_env_slot:
#else
    case OP_SET_ENV_SLOT:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        Value * v = vm.pop();
        // Heap-persist if needed: the value must outlive the stack frame.
        // Since our stack holds Value*, and the value is either from a
        // constant pool or already GC-allocated, we can store it directly.
        curEnv->values[displ] = v;
        DISPATCH();
    }

    // ==================================================================
    // Phase 2: Thunks
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_make_thunk:
#else
    case OP_MAKE_THUNK:
#endif
    {
        uint32_t thunkIdx = decodeOperand(CUR_INSTR);
        auto & desc = cu->thunks[thunkIdx];

        // Create an ExprBytecodeThunk in the BumpMemoryResource arena.
        // This is the bridge: forceValue() calls expr->eval() which
        // dispatches to vmExec.
        auto * thunkExpr = state.mem.exprs.add<ExprBytecodeThunk>(
            const_cast<CompilationUnit *>(cu), thunkIdx);

        // Create the thunk Value: (currentEnv, thunkExpr).
        auto * thunkVal = state.allocValue();
        thunkVal->mkThunk(curEnv, thunkExpr);

        vm.push(thunkVal);
        DISPATCH();
    }

    // ==================================================================
    // Phase 2: Closures
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_make_closure:
#else
    case OP_MAKE_CLOSURE:
#endif
    {
        uint32_t lambdaIdx = decodeOperand(CUR_INSTR);
        auto & desc = cu->lambdas[lambdaIdx];

        // Create an ExprLambdaBytecode proxy in BumpMemoryResource.
        // Its body will be an ExprBytecodeThunk for the lambda body.
        auto * proxy = state.mem.exprs.add<ExprLambdaBytecode>(
            const_cast<CompilationUnit *>(cu), lambdaIdx);

        // Create a thunk for the body so that callFunction's
        // lambda.body->eval() dispatches to the VM.
        // Register the body as a thunk descriptor.
        auto * bodyThunk = state.mem.exprs.add<ExprBytecodeThunk>(
            const_cast<CompilationUnit *>(cu),
            // We need a thunk descriptor for the body.  The lambda's
            // codeOffset IS the body, so we register it if not already done.
            // For simplicity, we add a thunk descriptor on the fly.
            static_cast<uint32_t>(const_cast<CompilationUnit *>(cu)->thunks.size()));
        const_cast<CompilationUnit *>(cu)->thunks.push_back(
            ThunkDescriptor{desc.codeOffset, desc.pos});
        proxy->body = bodyThunk;

        // Create the closure Value: (currentEnv, proxy).
        auto * closureVal = state.allocValue();
        closureVal->mkLambda(curEnv, proxy);

        vm.push(closureVal);
        DISPATCH();
    }

    // ==================================================================
    // Phase 2: Function calls
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_call_1:
#else
    case OP_CALL_1:
#endif
    {
        Value * arg = vm.pop();
        Value * fun = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);

        // Delegate to the existing callFunction which handles all
        // calling conventions: lambdas, primops, partial application,
        // functors, and profiling hooks.
        auto * result = state.allocValue();
        state.callFunction(*fun, *arg, *result, pos);

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_call:
#else
    case OP_CALL:
#endif
    {
        uint32_t nArgs = decodeOperand(CUR_INSTR);
        PosIdx pos = cu->posForOffset(ip - 1);

        // Collect arguments from the stack into a fixed-size array.
        // Max primop arity is 8; in practice Nix calls rarely exceed 3-4 args.
        // Use a stack-allocated array to avoid std::vector (which has a
        // non-trivial destructor that breaks computed-goto).
        assert(nArgs <= 16);
        Value * args[16];
        for (uint32_t i = nArgs; i > 0; --i)
            args[i - 1] = vm.pop();
        Value * fun = vm.pop();

        // Delegate to callFunction with the full argument span.
        auto * result = state.allocValue();
        state.callFunction(*fun, std::span<Value *>(args, nArgs), *result, pos);

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Fallback: delegate to tree-walking Expr::eval()
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_eval_expr:
#else
    case OP_EVAL_EXPR:
#endif
    {
        uint32_t exprIdx = decodeOperand(CUR_INSTR);
        Expr * expr = cu->exprPool[exprIdx];

        // Evaluate the expression via the tree-walking interpreter,
        // using the current bytecode env as context.
        auto * result = state.allocValue();
        expr->eval(state, *curEnv, *result);

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Unhandled opcode (must be last)
    // ==================================================================

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
