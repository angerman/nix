/// @file
/// v3 VM dispatch loop.
///
/// Computed-goto on supported compilers; switch fallback otherwise.
/// Slim CallFrame; locals stored on a single value-stack with frame-relative
/// indexing.
///
/// Stack layout per frame:
///   [params...]
///   [locals...]   nLocals slots, indexed 0..nLocals-1 from stackBaseOffset
///   [operand stack scratch]
///
/// OP_CALL: pop fun, pop arg, push CallFrame, switch cu/ip; on OP_RETURN
/// the popped result is written to the caller's resultPtr (set when the
/// caller emitted OP_CALL).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/alloc.hh"

#include <cassert>
#include <cstdio>
#include <stdexcept>

namespace nix::v3 {

namespace {

[[gnu::always_inline]]
inline Value pop(VMState & vm)
{
    Value v = vm.valueStack.back();
    vm.valueStack.pop_back();
    return v;
}

[[gnu::always_inline]]
inline void push(VMState & vm, Value v)
{
    vm.valueStack.push_back(v);
}

} // namespace

Value run(const CompilationUnit & rootCu)
{
    VMState vm;
    vm.valueStack.reserve(1024);
    vm.frames.reserve(64);

    const CompilationUnit * cu = &rootCu;
    uint32_t ip = cu->entryOffset;
    const Closure * closure = nullptr; // top-level has no closure
    size_t stackBase = 0;

    // Top-level entry frame.
    vm.frames.push_back(CallFrame{
        .cu = cu,
        .ip = ip,
        .resultSlot = 0,
        .flags = 0,
        ._pad0 = 0,
        .stackBaseOffset = 0,
        .closure = nullptr,
        .resultPtr = nullptr,
    });

    // Reserve locals up front.  Top-level lambda is functions[0] which has
    // its descriptor at lambdas[0] — its nLocals tells us how many.
    if (!cu->lambdas.empty()) {
        vm.valueStack.resize(cu->lambdas[0].nLocals);
    }

    Value finalResult{};
    finalResult.mkNull();

    bool running = true;
    while (running) {
        Instruction instr = cu->code[ip++];
        vm.nrInstructions++;
        Op op = decodeOp(instr);
        uint32_t operand = decodeOperand(instr);

        switch (op) {

        case OP_NOP:
            break;

        case OP_LIT_INT: {
            int32_t imm = decodeSignedOperand(instr);
            Value v;
            v.mkInt(imm);
            push(vm, v);
            break;
        }

        case OP_LIT_INT_BIG: {
            Value v;
            v.mkInt(cu->intConstants[operand]);
            push(vm, v);
            break;
        }

        case OP_GET_LOCAL: {
            assert(stackBase + operand < vm.valueStack.size());
            push(vm, vm.valueStack[stackBase + operand]);
            break;
        }

        case OP_SET_LOCAL: {
            Value v = pop(vm);
            // Auto-extend in case the producer pushed beyond the initial
            // resize (function-call paths grow).
            while (stackBase + operand >= vm.valueStack.size())
                vm.valueStack.push_back(Value{});
            vm.valueStack[stackBase + operand] = v;
            break;
        }

        case OP_GET_UPVALUE: {
            assert(closure && operand < closure->nUpvalues);
            push(vm, closure->upvalues[operand]);
            break;
        }

        case OP_ADD: {
            Value rhs = pop(vm);
            Value lhs = pop(vm);
            assert(lhs.isInt() && rhs.isInt());
            Value r;
            r.mkInt(lhs.payload.i + rhs.payload.i);
            push(vm, r);
            break;
        }
        case OP_SUB: {
            Value rhs = pop(vm);
            Value lhs = pop(vm);
            assert(lhs.isInt() && rhs.isInt());
            Value r;
            r.mkInt(lhs.payload.i - rhs.payload.i);
            push(vm, r);
            break;
        }
        case OP_MUL: {
            Value rhs = pop(vm);
            Value lhs = pop(vm);
            assert(lhs.isInt() && rhs.isInt());
            Value r;
            r.mkInt(lhs.payload.i * rhs.payload.i);
            push(vm, r);
            break;
        }

        case OP_MAKE_CLOSURE: {
            uint32_t funcIdx = operand;
            Instruction nUpW = cu->code[ip++];
            uint16_t nUpvalues = static_cast<uint16_t>(nUpW);

            Closure * c = Alloc::allocClosure(nUpvalues);
            allocStats().closuresAllocated++;
            c->desc = &cu->lambdas[funcIdx];
            c->nUpvalues = nUpvalues;
            // Pop captures in reverse order.
            for (uint16_t i = nUpvalues; i > 0; --i)
                c->upvalues[i - 1] = pop(vm);

            Value v;
            v.mkClosure(c);
            push(vm, v);
            break;
        }

        case OP_CALL: {
            Value arg = pop(vm);
            Value fun = pop(vm);
            assert(fun.isClosure() && "v3 OP_CALL: only closures supported in bring-up");
            const Closure * callee = fun.payload.closure;
            const LambdaDescriptor * desc = callee->desc;

            // Save current IP.
            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            // The callee's locals start at newBase.  The arg goes into slot 0.
            // Reserve locals for the callee.
            vm.valueStack.resize(newBase + desc->nLocals);
            vm.valueStack[newBase + 0] = arg;

            vm.frames.push_back(CallFrame{
                .cu = cu,
                .ip = desc->codeOffset,
                .resultSlot = 0,
                .flags = 0,
                ._pad0 = 0,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .closure = callee,
                .resultPtr = nullptr,
            });

            // Switch context.
            ip = desc->codeOffset;
            closure = callee;
            stackBase = newBase;
            break;
        }

        case OP_RETURN: {
            Value retVal = pop(vm);
            // Pop callee's locals.
            vm.valueStack.resize(stackBase);
            vm.frames.pop_back();
            // Restore caller context.
            const auto & caller = vm.frames.back();
            cu = caller.cu;
            ip = caller.ip;
            closure = caller.closure;
            stackBase = caller.stackBaseOffset;
            // Push return value into caller's operand stack.
            push(vm, retVal);
            break;
        }

        case OP_HALT: {
            finalResult = pop(vm);
            running = false;
            break;
        }

        default:
            std::fprintf(stderr, "v3 VM: unhandled opcode 0x%02x at ip=%u\n",
                static_cast<int>(op), ip - 1);
            std::abort();
        }
    }

    return finalResult;
}

} // namespace nix::v3
