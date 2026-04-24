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
#include "nix/util/environment-variables.hh"

#include <cassert>

namespace nix::bytecode {

// ---------------------------------------------------------------------------
// VM Tracing
// ---------------------------------------------------------------------------
//
// Controlled by environment variables:
//   NIX_VM_TRACE=1          -- trace every instruction
//   NIX_VM_TRACE_FROM=N     -- start tracing at step N (default 0)
//   NIX_VM_TRACE_TO=N       -- stop tracing at step N (default UINT64_MAX)
//
// Each traced step shows:
//   [step#] unit@offset opcode operands  ; source_file:line:col  | stack_depth
//
// The step counter is global (across all vmExec invocations) so you can
// pinpoint the exact moment something goes wrong in a long evaluation.

/// Print VM statistics at process exit when NIX_VM_STATS=1.
static void printVMStats(const VMState & vm) {
    if (getEnv("NIX_VM_STATS").value_or("") != "1") return;
    fprintf(stderr, "\n=== Bytecode VM Statistics ===\n");
    fprintf(stderr, "  Instructions executed: %llu\n", (unsigned long long)vm.nrInstructions);
    fprintf(stderr, "  OP_EVAL_EXPR fallbacks: %llu (%.1f%%)\n",
        (unsigned long long)vm.nrEvalExprFallbacks,
        vm.nrInstructions ? 100.0 * vm.nrEvalExprFallbacks / vm.nrInstructions : 0.0);
    fprintf(stderr, "  Bytecoded thunk forces: %llu\n", (unsigned long long)vm.nrBytecodeThunkForces);
    fprintf(stderr, "  Bytecoded call trampolines: %llu\n", (unsigned long long)vm.nrBytecodeCallTrampoline);
    fprintf(stderr, "  OP_FORCE → tree-walker: %llu\n", (unsigned long long)vm.nrForceFallbacks);
    fprintf(stderr, "  OP_CALL_1 → tree-walker: %llu\n", (unsigned long long)vm.nrCallFallbacks);
    fprintf(stderr, "  Peak stack depth: %llu\n", (unsigned long long)vm.peakStackDepth);
    fprintf(stderr, "  Peak frame depth: %llu\n", (unsigned long long)vm.peakFrameDepth);
    fprintf(stderr, "================================\n");
}

struct TraceConfig {
    bool enabled = false;
    uint64_t from = 0;
    uint64_t to = UINT64_MAX;

    TraceConfig() {
        auto t = getEnv("NIX_VM_TRACE");
        auto f = getEnv("NIX_VM_TRACE_FROM");
        auto tt = getEnv("NIX_VM_TRACE_TO");
        if (t.value_or("") == "1" || f || tt) {
            enabled = true;
            if (f) from = std::stoull(*f);
            if (tt) to = std::stoull(*tt);
        }
    }
};

static TraceConfig & traceConfig() {
    static TraceConfig cfg;
    return cfg;
}

static uint64_t & globalStepCounter() {
    static uint64_t counter = 0;
    return counter;
}

/// Print one trace line for the current instruction.
static void traceInstruction(
    EvalState & state,
    const CompilationUnit & cu,
    uint32_t ip,
    Instruction instr,
    uint64_t step,
    size_t stackDepth,
    size_t frameDepth)
{
    uint8_t op = decodeOp(instr);
    uint32_t operand = decodeOperand(instr);
    PosIdx posIdx = cu.posForOffset(ip);
    Pos pos = posIdx ? state.positions[posIdx] : Pos{};

    // Format: [step] frames:N stack:N  ip  opcode operand  ; source:line:col
    fprintf(stderr, "[%7llu] fr:%zu stk:%zu  %4u: %-18s",
        (unsigned long long)step, frameDepth, stackDepth, ip, opName(op));

    // Print operand details based on opcode type.
    switch (op) {
        case OP_CONST:
            fprintf(stderr, " const=%u", operand);
            if (operand < cu.constants.size() && cu.constants[operand]) {
                auto * v = cu.constants[operand];
                switch (v->type()) {
                    case nInt:    fprintf(stderr, " (int %lld)", (long long)v->integer().value); break;
                    case nFloat:  fprintf(stderr, " (float %g)", v->fpoint()); break;
                    case nString: fprintf(stderr, " (str \"%.*s\")",
                        (int)std::min((size_t)30, v->string_view().size()),
                        v->string_view().data()); break;
                    case nBool:   fprintf(stderr, " (%s)", v->boolean() ? "true" : "false"); break;
                    case nNull: case nAttrs: case nList: case nFunction:
                    case nExternal: case nPath: case nThunk: case nFailed:
                        fprintf(stderr, " (%s)", showType(*v).c_str()); break;
                }
            }
            break;
        case OP_INT:
            fprintf(stderr, " %u", operand);
            break;
        case OP_GET_LOCAL_0: case OP_GET_LOCAL_1:
        case OP_GET_LOCAL_2: case OP_GET_LOCAL_3:
        case OP_GET_LOCAL_0_FORCE:
            fprintf(stderr, " displ=%u", operand);
            break;
        case OP_GET_LOCAL:
            fprintf(stderr, " level=%u displ=%u", unpackLevel(operand), unpackDispl(operand));
            break;
        case OP_GET_WITH:
        case OP_EVAL_EXPR:
            fprintf(stderr, " idx=%u", operand);
            break;
        case OP_ATTR_SELECT: case OP_HAS_ATTR:
        case OP_SELECT_FORCE:
            if (operand < cu.symbols.size())
                fprintf(stderr, " sym=%s", state.symbols[cu.symbols[operand]].c_str());
            break;
        case OP_JUMP: case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE: case OP_JUMP_IF_NOT_ATTRS:
            fprintf(stderr, " %+d -> %u", decodeSigned(instr),
                static_cast<uint32_t>(static_cast<int32_t>(ip) + 1 + decodeSigned(instr)));
            break;
        case OP_MAKE_THUNK:
            if (operand < cu.thunks.size())
                fprintf(stderr, " thunk=%u -> offset %u", operand, cu.thunks[operand].codeOffset);
            break;
        case OP_MAKE_CLOSURE:
            if (operand < cu.lambdas.size())
                fprintf(stderr, " lambda=%u -> offset %u", operand, cu.lambdas[operand].codeOffset);
            break;
        case OP_CALL: case OP_TAIL_CALL:
            fprintf(stderr, " nArgs=%u", operand);
            break;
        case OP_ENTER_LET: case OP_INHERIT_FROM_INIT:
            fprintf(stderr, " envSize=%u", operand);
            break;
        case OP_SET_ENV_SLOT: case OP_INHERIT_FROM_SET: case OP_SET_ENV_SLOT_UP:
            fprintf(stderr, " displ=%u", operand);
            break;
        case OP_ATTRS_INIT:
            fprintf(stderr, " nAttrs=%u", operand);
            break;
        case OP_ATTRS_DYN_INIT:
            fprintf(stderr, " nStatic=%u nDynamic=%u", operand >> 12, operand & 0xFFF);
            break;
        case OP_LIST_INIT:
            fprintf(stderr, " size=%u", operand);
            break;
        case OP_STR_CONCAT_INIT:
            fprintf(stderr, " nParts=%u forceStr=%u",
                operand & ((1u<<23)-1), (operand >> 23) & 1);
            break;
        case OP_GET_UPVALUE:
            fprintf(stderr, " idx=%u", operand);
            break;
        case OP_MAKE_CLOSURE_V2:
            if (operand < cu.lambdas.size())
                fprintf(stderr, " lambda=%u -> offset %u nUpvalues(next)", operand, cu.lambdas[operand].codeOffset);
            break;
        case OP_MAKE_THUNK_V2:
            if (operand < cu.thunks.size())
                fprintf(stderr, " thunk=%u -> offset %u nUpvalues(next)", operand, cu.thunks[operand].codeOffset);
            break;
        case OP_GET_STACK_SLOT:
        case OP_SET_STACK_SLOT:
        case OP_COPY_TO_SLOT:
            fprintf(stderr, " slot=%u", operand);
            break;
        case OP_ALLOC_VALUE:
            break;
        default:
            if (operand) fprintf(stderr, " %u", operand);
            break;
    }

    // Source position.
    if (pos.line > 0) {
        fprintf(stderr, "  ; ");
        // Print just file:line:col, not the full Pos (which includes source).
        auto * path = std::get_if<SourcePath>(&pos.origin);
        if (path)
            fprintf(stderr, "%s:%u:%u", path->to_string().c_str(), pos.line, pos.column);
        else
            fprintf(stderr, "«string»:%u:%u", pos.line, pos.column);
    }

    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------------------
// VMState
// ---------------------------------------------------------------------------

VMState::~VMState()
{
    printVMStats(*this);
}

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

    // stackBaseOffset in CallFrames is relative to `stack`, so no fixup needed.
    sp       = newStack + used;
    stack    = newStack;
    stackEnd = newStack + newCap;
    // Old buffer is GC-managed; it will be collected when unreferenced.
}


// ---------------------------------------------------------------------------
// Helpers for opcode handlers (extracted to avoid non-trivial local
// destructors that break computed-goto dispatch)
// ---------------------------------------------------------------------------

/// Create an env for a lambda call and store the raw argument.
/// The lambda body's bytecoded prologue handles formal parameter
/// matching (unpacking the attrset, checking required args, defaults).
/// Returns nullptr if the lambda is not bytecoded.
[[gnu::noinline]]
static Env * vmBindLambdaArg(
    EvalState & state, Value & fun, Value * arg, PosIdx callPos)
{
    if (!fun.isLambda()) return nullptr;

    ExprLambda & lambda = *fun.lambda().fun;

    // Check if this lambda has a bytecoded body.
    auto it = state.lambdaBodyCache.find(&lambda);
    if (it == state.lambdaBodyCache.end())
        return nullptr;

    // Allocate env with the SAME layout as the tree-walker's callFunction.
    // This ensures the body code's variable displacements (from bindVars)
    // are correct.
    auto formals = lambda.getFormals();
    auto size = (!lambda.arg ? 0 : 1)
        + (formals ? formals->formals.size() : 0);
    if (!formals) size = 1; // simple lambda: 1 slot
    Env & env2 = state.mem.allocEnv(size);
    env2.up = fun.lambda().env;

    if (!formals) {
        // Simple lambda (x: body): store arg in slot 0. Body can run immediately.
        env2.values[0] = arg;
    } else {
        // Formals lambda ({ x, y }: body): store @-pattern if present.
        // The bytecoded prologue handles formal unpacking.
        // We push the raw arg onto the VALUE STACK so the prologue can pop it.
        if (lambda.arg)
            env2.values[0] = arg; // @-pattern goes in slot 0
    }

    return &env2;
}

/// Sorted merge of two attrsets with RHS-wins duplicate resolution.
[[gnu::noinline]]
static void vmAttrsUpdate(EvalState & state, Value & result, Value & lhs, Value & rhs)
{
    auto & bindings1 = *lhs.attrs();
    auto & bindings2 = *rhs.attrs();

    if (bindings1.empty()) { result = rhs; return; }
    if (bindings2.empty()) { result = lhs; return; }

    auto attrs = state.buildBindings(bindings1.size() + bindings2.size());
    auto i = bindings1.begin();
    auto j = bindings2.begin();

    while (i != bindings1.end() && j != bindings2.end()) {
        if (i->name == j->name) {
            attrs.insert(*j);
            ++i; ++j;
        } else if (i->name < j->name) {
            attrs.insert(*i); ++i;
        } else {
            attrs.insert(*j); ++j;
        }
    }
    while (i != bindings1.end()) { attrs.insert(*i); ++i; }
    while (j != bindings2.end()) { attrs.insert(*j); ++j; }

    result.mkAttrs(attrs.alreadySorted());
}

/// String concatenation handler (extracted for same reason).
[[gnu::noinline]]
static void vmStrConcat(
    EvalState & state, VMState & vm, const CompilationUnit & cu,
    uint32_t nParts, bool forceString, PosIdx pos, Value & result)
{
    NixStringContext context;
    std::vector<BackedStringView> strings;
    size_t sSize = 0;
    NixInt n{0};
    NixFloat nf = 0;
    bool first = !forceString;
    ValueType firstType = nString;

    constexpr uint32_t kStackPartsMax = 64;
    Value * stackParts[kStackPartsMax];
    Value ** parts = nParts <= kStackPartsMax ? stackParts : new Value*[nParts];
    for (uint32_t i = nParts; i > 0; --i)
        parts[i - 1] = vm.pop();

    for (uint32_t i = 0; i < nParts; i++) {
        Value & vTmp = *parts[i];
        state.forceValue(vTmp, pos);
        if (first) firstType = vTmp.type();

        if (firstType == nInt) {
            if (vTmp.type() == nInt) {
                auto newN = n + vTmp.integer();
                if (auto checked = newN.valueChecked())
                    n = NixInt(*checked);
                else
                    state.error<EvalError>("integer overflow in adding %1% + %2%", n, vTmp.integer())
                        .atPos(pos).debugThrow();
            } else if (vTmp.type() == nFloat) {
                firstType = nFloat; nf = n.value; nf += vTmp.fpoint();
            } else {
                state.error<EvalError>("cannot add %1% to an integer", showType(vTmp))
                    .atPos(pos).debugThrow();
            }
        } else if (firstType == nFloat) {
            if (vTmp.type() == nInt) nf += vTmp.integer().value;
            else if (vTmp.type() == nFloat) nf += vTmp.fpoint();
            else state.error<EvalError>("cannot add %1% to a float", showType(vTmp))
                .atPos(pos).debugThrow();
        } else {
            if (strings.empty()) strings.reserve(nParts);
            auto part = state.coerceToString(pos, vTmp, context,
                "while evaluating a path segment", false, firstType == nString, !first);
            sSize += part->size();
            strings.emplace_back(std::move(part));
        }
        first = false;
    }

    if (firstType == nInt) {
        result.mkInt(n);
    } else if (firstType == nFloat) {
        result.mkFloat(nf);
    } else if (firstType == nPath) {
        if (!context.empty())
            state.error<EvalError>("a string that refers to a store path cannot be appended to a path")
                .atPos(pos).debugThrow();
        std::string resultStr; resultStr.reserve(sSize);
        for (const auto & part : strings) resultStr += *part;
        result.mkPath(state.rootPath(CanonPath(resultStr)), state.mem);
    } else {
        auto & resultStr = StringData::alloc(state.mem, sSize);
        auto * tmp = resultStr.data();
        for (const auto & part : strings) {
            std::memcpy(tmp, part->data(), part->size());
            tmp += part->size();
        }
        *tmp = '\0';
        result.mkStringMove(resultStr, context, state.mem);
    }

    if (parts != stackParts) delete[] parts;
}

// ---------------------------------------------------------------------------
// vmExec -- main dispatch loop
// ---------------------------------------------------------------------------

void vmExec(
    EvalState & state,
    const CompilationUnit & unit,
    uint32_t startOffset,
    Env & env,
    Value & result,
    Value ** upvalues,
    Value * arg)
{
    // Ensure VMState is initialized.
    if (!state.vmState) [[unlikely]]
        state.vmState = std::make_unique<VMState>();

    auto & vm = *state.vmState;

    // Track the frame depth at entry so we know when OUR frames are
    // exhausted (as opposed to frames from an outer vmExec invocation).
    size_t entryFrameDepth = vm.frames.size();

    // Allocate a result slot that the OP_RETURN will write into.
    Value * resultSlot = &result;

    // Push the initial call frame.
    // For v2 thunks/closures, the caller may pass an upvalue array.
    vm.frames.push_back(CallFrame{
        .unit      = &unit,
        .ip        = startOffset,
        .env       = &env,
        .stackBaseOffset = vm.stackSize(),
        .resultSlot = resultSlot,
        .callPos   = unit.posForOffset(startOffset),
        .upvalues  = upvalues,
    });

    // If an argument was provided (e.g., from callFunction routing a
    // v2 closure call), push it as stack slot 0 so the body can read
    // it via OP_GET_STACK_SLOT(0).
    if (arg)
        vm.push(arg);

    // Frame-local aliases (updated when frames change).
    const CompilationUnit * cu = &unit;
    uint32_t ip   = startOffset;
    Env * curEnv = &env;

    // ------------------------------------------------------------------
    // Dispatch loop.
    // Use computed-goto where available (GCC/Clang), otherwise switch.
    // ------------------------------------------------------------------

// Re-enabled: the inline thunk trampoline that caused stack overflow
// has been removed. All thunk forcing goes through state.forceValue()
// -> tree-walker. The entryFrameDepth mechanism handles vmExec
// re-entrancy safely, with manageable stack frames.
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

        // Phase 3: select, attrs, lists, with
        REGISTER_OP(OP_GET_WITH,         op_get_with);
        REGISTER_OP(OP_SELECT_FORCE,     op_select_force);
        REGISTER_OP(OP_ATTR_SELECT,      op_attr_select);
        REGISTER_OP(OP_ATTR_SELECT_DYN,  op_attr_select_dyn);
        REGISTER_OP(OP_HAS_ATTR_DYN,    op_has_attr_dyn);
        REGISTER_OP(OP_ATTRS_DYN_INIT,  op_attrs_dyn_init);
        REGISTER_OP(OP_GET_LOCAL_0_FORCE, op_get_local_0_force);
        REGISTER_OP(OP_HAS_ATTR,         op_has_attr);
        REGISTER_OP(OP_ATTRS_UPDATE,     op_attrs_update);
        REGISTER_OP(OP_LIST_CONCAT,      op_list_concat);
        REGISTER_OP(OP_PUSH_WITH,        op_push_with);
        REGISTER_OP(OP_JUMP_IF_NOT_ATTRS, op_jump_if_not_attrs);

        // Phase 3b: list build, attrs build, string concat
        REGISTER_OP(OP_LIST_INIT,        op_list_init);
        REGISTER_OP(OP_ATTRS_INIT,       op_attrs_init);
        // OP_ATTR_INSERT and OP_ATTRS_FINISH are unused (compound OP_ATTRS_INIT handles everything).
        REGISTER_OP(OP_STR_CONCAT_INIT,  op_str_concat_init);
        REGISTER_OP(OP_POS,              op_pos);

        // Fallback
        REGISTER_OP(OP_EVAL_EXPR,    op_eval_expr);

        // Phase 2: let-bindings, closures, calls, thunks
        REGISTER_OP(OP_ENTER_LET,    op_enter_let);
        REGISTER_OP(OP_LEAVE_SCOPE,  op_leave_scope);
        REGISTER_OP(OP_SET_ENV_SLOT, op_set_env_slot);
        REGISTER_OP(OP_INHERIT_FROM_INIT, op_inherit_from_init);
        REGISTER_OP(OP_INHERIT_FROM_SET,  op_inherit_from_set);
        REGISTER_OP(OP_SET_ENV_SLOT_UP,   op_set_env_slot_up);
        REGISTER_OP(OP_MAKE_THUNK,   op_make_thunk);
        REGISTER_OP(OP_MAKE_CLOSURE, op_make_closure);
        REGISTER_OP(OP_CALL,         op_call);
        REGISTER_OP(OP_CALL_1,       op_call_1);

        // VM v2: upvalue-based closures (IR emitter)
        REGISTER_OP(OP_GET_UPVALUE,      op_get_upvalue);
        REGISTER_OP(OP_MAKE_CLOSURE_V2,  op_make_closure_v2);
        REGISTER_OP(OP_MAKE_THUNK_V2,    op_make_thunk_v2);
        REGISTER_OP(OP_GET_STACK_SLOT,   op_get_stack_slot);
        REGISTER_OP(OP_SET_STACK_SLOT,   op_set_stack_slot);
        REGISTER_OP(OP_ALLOC_VALUE,      op_alloc_value);
        REGISTER_OP(OP_COPY_TO_SLOT,     op_copy_to_slot);

#undef REGISTER_OP
        tableInitialized = true;
    }

    auto & tcfg = traceConfig();
    auto & stepCounter = globalStepCounter();

    // Profiling and tracing hook -- called before every instruction.
    // Compiled as a single branch test (tcfg.enabled) for zero-cost
    // when tracing is off.  Profiling counters are always incremented.
#define VM_HOOK() do {                                         \
        vm.nrInstructions++;                                   \
        if (tcfg.enabled) [[unlikely]] {                       \
            uint64_t step = stepCounter++;                     \
            if (step >= tcfg.from && step <= tcfg.to)          \
                traceInstruction(state, *cu, ip - 1,           \
                    cu->code[ip - 1], step,                    \
                    static_cast<size_t>(vm.sp - vm.stack),     \
                    vm.frames.size());                         \
        }                                                      \
    } while (0)

    // Computed-goto dispatch macro.
#define DISPATCH() do {                              \
        Instruction _instr = cu->code[ip++];         \
        VM_HOOK();                                   \
        goto *dispatchTable[decodeOp(_instr)];       \
    } while (0)

#define CUR_INSTR (cu->code[ip - 1])

    try { // exception cleanup: restore frame/stack on throw

    DISPATCH();

#else // switch-based fallback

#define DISPATCH() continue
#define CUR_INSTR (cu->code[ip - 1])

    auto & tcfg = traceConfig();
    auto & stepCounter = globalStepCounter();

    try { // exception cleanup: restore frame/stack on throw

    for (;;) {
        Instruction instr = cu->code[ip++];

        vm.nrInstructions++;
        if (tcfg.enabled) [[unlikely]] {
            uint64_t step = stepCounter++;
            if (step >= tcfg.from && step <= tcfg.to) {
                traceInstruction(state, *cu, ip - 1, instr, step,
                    static_cast<size_t>(vm.sp - vm.stack),
                    vm.frames.size());
            }
        }

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
        vm.push(&Value::vTrue);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_false:
#else
    case OP_FALSE:
#endif
    {
        vm.push(&Value::vFalse);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_null:
#else
    case OP_NULL:
#endif
    {
        vm.push(&Value::vNull);
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

        // Save frame state before popping (pop invalidates references).
        auto & frame = vm.frames.back();
        bool wasThunkForce = frame.isThunkForce;
        Value * resultSlot = frame.resultSlot;
        size_t stackBase = frame.stackBaseOffset;

        // Write the result into the caller's result slot.
        *resultSlot = *retVal;

        // Restore stack to frame entry point (offset-based, survives stack realloc).
        vm.sp = vm.stack + stackBase;
        vm.frames.pop_back();

        if (vm.frames.size() <= entryFrameDepth) {
            return;
        }

        // Resume the caller's frame.
        auto & caller = vm.frames.back();
        cu     = caller.unit;
        ip     = caller.ip;
        curEnv = caller.env;

        if (!wasThunkForce) {
            // Normal call return: push the resultSlot (an independent copy).
            // We must NOT push retVal directly — it may alias an env slot.
            // The caller's OP_CALL_1 allocated resultSlot as a fresh Value
            // and we've copied the result into it above.
            vm.push(resultSlot);
        }
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

    // Superinstruction: GET_LOCAL_0 + FORCE in one dispatch.
    // This is the most common two-instruction sequence (compileVar
    // emits GET_LOCAL_0 + FORCE for every variable access).
#ifdef NIX_VM_COMPUTED_GOTO
op_get_local_0_force:
#else
    case OP_GET_LOCAL_0_FORCE:
#endif
    {
        uint32_t displ = decodeOperand(CUR_INSTR);
        Value * v = curEnv->values[displ];
        vm.push(v);
        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline thunk trampoline (same as OP_FORCE).
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;

            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                uint32_t thunkOffset = thunkDesc.codeOffset;

                // Extract v2 upvalues from carrier env if present.
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0) {
                    frameUpvalues = reinterpret_cast<Value **>(
                        thunkEnv->values[1]);
                }

                v->mkBlackhole();
                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                vm.frames.push_back(CallFrame{
                    .unit = bcThunk->unit,
                    .ip = thunkOffset,
                    .env = thunkEnv,
                    .stackBaseOffset = vm.stackSize(),
                    .resultSlot = v,
                    .callPos = pos,
                    .isThunkForce = true,
                    .upvalues = frameUpvalues,
                });
                cu = bcThunk->unit;
                ip = thunkOffset;
                curEnv = thunkEnv;
                DISPATCH();
            }
        }

        // Fallback for non-bytecoded thunks, apps, non-thunks.
        if (v->isThunk() || v->isApp()) vm.nrForceFallbacks++;
        state.forceValue(*v, pos);
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
    // Phase 3+: With-scope variable lookup
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_get_with:
#else
    case OP_GET_WITH:
#endif
    {
        // The operand is an index into the expr pool, pointing to the
        // ExprVar that has fromWith, level, name, etc.
        uint32_t exprIdx = decodeOperand(CUR_INSTR);
        auto * var = static_cast<ExprVar *>(cu->exprPool[exprIdx]);

        // Walk up the env chain to the first with-scope.
        // Skip v2 carrier envs (sentinel values[0] == &Value::vNull)
        // as they don't correspond to scopes that bindVars() counted.
        Env * e = curEnv;
        for (auto l = var->level; l; --l) {
            e = e->up;
            while (e && e->values[0] == &Value::vNull)
                e = e->up;
        }

        // Walk the with-chain looking for the variable.
        auto * fromWith = var->fromWith;
        while (true) {
            // Skip v2 carrier envs.  Carrier envs (from
            // OP_MAKE_CLOSURE_V2 / OP_MAKE_THUNK_V2) have
            // values[0] = &Value::vNull as a sentinel.  They are
            // NOT with scopes and must be transparently skipped.
            while (e && e->values[0] == &Value::vNull)
                e = e->up;
            assert(e && "OP_GET_WITH: ran off end of env chain");

            PosIdx withPos = fromWith->pos;
            state.forceAttrs(*e->values[0], withPos,
                "while evaluating the first subexpression of a with expression");
            if (auto j = e->values[0]->attrs()->get(var->name)) {
                vm.push(j->value);
                break;
            }
            if (!fromWith->parentWith)
                state.error<UndefinedVarError>(
                    "undefined variable '%1%'", state.symbols[var->name])
                    .atPos(var->pos)
                    .debugThrow();
            for (size_t l = fromWith->prevWith; l; --l) {
                e = e->up;
                while (e && e->values[0] == &Value::vNull)
                    e = e->up;
            }
            fromWith = fromWith->parentWith;
        }
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

        // Inline trampoline for bytecoded thunks: force within the VM
        // loop by pushing a CallFrame, avoiding C-stack growth.
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;

            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                uint32_t thunkOffset = thunkDesc.codeOffset;

                // For v2 thunks (created by OP_MAKE_THUNK_V2), extract
                // the upvalue array from the carrier env's values[1].
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0) {
                    frameUpvalues = reinterpret_cast<Value **>(
                        thunkEnv->values[1]);
                }

                // Mark as blackhole before evaluating.
                v->mkBlackhole();

                // Save current frame state.
                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;

                // Push a new call frame for the thunk body.
                vm.frames.push_back(CallFrame{
                    .unit = bcThunk->unit,
                    .ip = thunkOffset,
                    .env = thunkEnv,
                    .stackBaseOffset = vm.stackSize(),
                    .resultSlot = v,  // Write result back into the thunk Value
                    .callPos = pos,
                    .isThunkForce = true,  // OP_RETURN doesn't push result
                    .upvalues = frameUpvalues,
                });

                // Switch to the thunk's code.
                cu = bcThunk->unit;
                ip = thunkOffset;
                curEnv = thunkEnv;
                DISPATCH();
            }
        }

        // App fast path: primops create App(f, arg) values via mkApp.
        // If f is a bytecoded lambda (in lambdaBodyCache), trampoline
        // into the VM instead of tree-walking through forceValue.
        // Safe now that forceValue marks App values as blackhole.
        if (v->isApp()) {
            Value * left = v->app().left;
            Value * right = v->app().right;
            state.forceValue(*left, pos);

            // v2 App path: the function is a v2 closure (ExprLambdaBytecode).
            if (left->isLambda() && left->lambda().fun->isBytecodeProxy) {
                v->mkBlackhole();
                vm.nrBytecodeCallTrampoline++;
                auto * bcLambda = static_cast<ExprLambdaBytecode *>(
                    left->lambda().fun);
                auto & bodyUnit = *bcLambda->unit;
                auto & desc = bodyUnit.lambdas[bcLambda->lambdaIdx];
                auto & thunkDesc = bodyUnit.thunks[desc.bodyThunkIdx];
                uint32_t startOffset = thunkDesc.codeOffset;

                Value ** frameUpvalues = nullptr;
                if (desc.nUpvalues > 0 && left->lambda().env) {
                    frameUpvalues = reinterpret_cast<Value **>(
                        left->lambda().env->values[1]);
                }

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                vm.frames.push_back(CallFrame{
                    .unit = &bodyUnit,
                    .ip = startOffset,
                    .env = left->lambda().env,
                    .stackBaseOffset = vm.stackSize(),
                    .resultSlot = v,  // update App in-place
                    .callPos = pos,
                    .isThunkForce = true,
                    .upvalues = frameUpvalues,
                });
                // Store arg as stack slot 0 (the parameter).
                vm.push(right);
                cu = &bodyUnit;
                ip = startOffset;
                curEnv = left->lambda().env;
                DISPATCH();
            }

            // v1 App path: env-chain closures in lambdaBodyCache.
            if (Env * env2 = vmBindLambdaArg(state, *left, right, pos)) {
                v->mkBlackhole();
                vm.nrBytecodeCallTrampoline++;
                auto & bodyInfo = state.lambdaBodyCache[left->lambda().fun];
                auto & bodyUnit = *bodyInfo.unit;
                bool hasFormals = left->lambda().fun->getFormals().has_value();
                uint32_t startOffset = hasFormals
                    ? bodyInfo.prologueOffset
                    : bodyUnit.thunks[bodyInfo.thunkIdx].codeOffset;

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                vm.frames.push_back(CallFrame{
                    .unit = &bodyUnit,
                    .ip = startOffset,
                    .env = env2,
                    .stackBaseOffset = vm.stackSize(),
                    .resultSlot = v,  // update App in-place
                    .callPos = pos,
                    .isThunkForce = true,
                });
                if (hasFormals) vm.push(right);
                cu = &bodyUnit;
                ip = startOffset;
                curEnv = env2;
                DISPATCH();
            }
        }

        // Fallback for non-bytecoded thunks, remaining apps, non-thunks.
        if (v->isThunk() || v->isApp()) vm.nrForceFallbacks++;
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
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        // Fast path: scalar equality without entering eqValues.
        bool eq;
        if (lhs == rhs) {
            eq = true;
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            eq = lhs->integer() == rhs->integer();
        } else if (lhs->type() == nString && rhs->type() == nString) {
            eq = lhs->string_view() == rhs->string_view();
        } else if (lhs->type() == nBool && rhs->type() == nBool) {
            eq = lhs->boolean() == rhs->boolean();
        } else if (lhs->type() == nNull && rhs->type() == nNull) {
            eq = true;
        } else {
            // Fall back to deep comparison for compound types.
            eq = state.eqValues(*lhs, *rhs, pos, "while comparing two values");
        }

        vm.push(eq ? &Value::vTrue : &Value::vFalse);
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
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);

        // Fast path: scalar inequality.
        bool eq;
        if (lhs == rhs) {
            eq = true;
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            eq = lhs->integer() == rhs->integer();
        } else if (lhs->type() == nString && rhs->type() == nString) {
            eq = lhs->string_view() == rhs->string_view();
        } else if (lhs->type() == nBool && rhs->type() == nBool) {
            eq = lhs->boolean() == rhs->boolean();
        } else if (lhs->type() == nNull && rhs->type() == nNull) {
            eq = true;
        } else {
            eq = state.eqValues(*lhs, *rhs, pos, "while comparing two values");
        }

        vm.push(eq ? &Value::vFalse : &Value::vTrue);
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

        vm.push(cmpResult ? &Value::vTrue : &Value::vFalse);
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
        vm.push(v->boolean() ? &Value::vFalse : &Value::vTrue);
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
    // Inherit-from env management
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_inherit_from_init:
#else
    case OP_INHERIT_FROM_INIT:
#endif
    {
        // Allocate a separate inherit-from env for `inherit (expr) ...` bindings.
        // This env is pushed as a scope (like OP_ENTER_LET) so that
        // ExprInheritFrom (level=0, displ=N) resolves to its slots.
        // The inherit-from expressions (stored via OP_INHERIT_FROM_SET) are
        // thunked in the OUTER env (inheritEnv.up), matching the tree-walker.
        uint32_t nExprs = decodeOperand(CUR_INSTR);
        Env & ienv = state.mem.allocEnv(nExprs);
        ienv.up = curEnv;
        curEnv = &ienv;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_inherit_from_set:
#else
    case OP_INHERIT_FROM_SET:
#endif
    {
        // Store a value into the inherit-from env.
        // The value was pushed by the preceding thunk/eager compilation.
        uint32_t displ = decodeOperand(CUR_INSTR);
        Value * v = vm.pop();
        curEnv->values[displ] = v;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_set_env_slot_up:
#else
    case OP_SET_ENV_SLOT_UP:
#endif
    {
        // Store a value into the PARENT env (curEnv->up).
        // Used by compileLet with inherit(expr): the inherit env is curEnv,
        // and the let env is curEnv->up. Binding values go into the let env.
        uint32_t displ = decodeOperand(CUR_INSTR);
        Value * v = vm.pop();
        curEnv->up->values[displ] = v;
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
        assert(thunkIdx < cu->thunks.size() && "OP_MAKE_THUNK: thunk index out of bounds");

        // Create an ExprBytecodeThunk that dispatches to the VM when forced.
        // Value::isTrivial() has been updated to unwrap ExprBytecodeThunk
        // and check the original sourceExpr type for flake compatibility.
        auto * thunkExpr = state.mem.exprs.add<ExprBytecodeThunk>(
            const_cast<CompilationUnit *>(cu), thunkIdx);

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
        assert(lambdaIdx < cu->lambdas.size() && "OP_MAKE_CLOSURE: lambda index out of bounds");
        auto & desc = cu->lambdas[lambdaIdx];

        // Use the original ExprLambda for closures.
        // Lambda bodies are still tree-walked when callFunction calls
        // lambda.body->eval(). Bytecoding lambda bodies requires
        // properly copying ExprLambda with its private formals fields,
        // which is complex. The bytecoded THUNKS (via ExprBytecodeThunk)
        // handle the majority of forced evaluations.
        ExprLambda * originalLambda = desc.sourceExpr;

        // Register this lambda's bytecoded body in the side-table,
        // keyed by ExprLambda* (unique per parse, stable in BumpMemoryResource).
        // The OP_CALL_1 trampoline checks this table to decide whether
        // to bytecode the body or fall back to callFunction.
        state.lambdaBodyCache[originalLambda] = {
            const_cast<CompilationUnit *>(cu), desc.bodyThunkIdx, desc.prologueOffset};

        auto * closureVal = state.allocValue();
        closureVal->mkLambda(curEnv, originalLambda);

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
        state.forceValue(*fun, pos);

        // ── v2 fast path: upvalue-based closures ──
        // v2 closures are created by OP_MAKE_CLOSURE_V2 and use
        // ExprLambdaBytecode as the lambda expression.  They use a
        // flat upvalue array instead of v1 Env chains.  Detect them
        // via the isBytecodeProxy flag (avoids dynamic_cast).
        if (fun->isLambda() && fun->lambda().fun->isBytecodeProxy) {
            vm.nrBytecodeCallTrampoline++;
            auto * bcLambda = static_cast<ExprLambdaBytecode *>(
                fun->lambda().fun);
            auto & bodyUnit = *bcLambda->unit;
            auto & desc = bodyUnit.lambdas[bcLambda->lambdaIdx];
            auto & thunkDesc = bodyUnit.thunks[desc.bodyThunkIdx];
            uint32_t startOffset = thunkDesc.codeOffset;

            assert(startOffset < bodyUnit.code.size()
                && "OP_CALL_1 v2: startOffset out of bounds");

            // Extract the upvalue array from the closure's carrier env.
            // OP_MAKE_CLOSURE_V2 stores it as closureEnv.values[1].
            Value ** frameUpvalues = nullptr;
            if (desc.nUpvalues > 0 && fun->lambda().env) {
                frameUpvalues = reinterpret_cast<Value **>(
                    fun->lambda().env->values[1]);
            }

            // Save current frame state.
            vm.frames.back().ip = ip;
            vm.frames.back().env = curEnv;

            auto * result = state.allocValue();

            // Push the call frame.  The body code uses OP_GET_STACK_SLOT(0)
            // to read the argument and OP_GET_UPVALUE(i) for captures.
            // OP_SET_STACK_SLOT auto-extends the stack, so no pre-allocation
            // of local slots is needed here.
            vm.frames.push_back(CallFrame{
                .unit = &bodyUnit,
                .ip = startOffset,
                .env = fun->lambda().env,
                .stackBaseOffset = vm.stackSize(),
                .resultSlot = result,
                .callPos = pos,
                .upvalues = frameUpvalues,
            });

            // Store the argument as stack slot 0 (the parameter).
            // The body's first parameter is always at slot 0.
            vm.push(arg);

            // Switch to the body code.
            cu = &bodyUnit;
            ip = startOffset;
            curEnv = fun->lambda().env;

            DISPATCH();
        }

        // ── v1 fast path: env-chain closures ──
        // Try the fast path: bytecoded lambda with inline argument binding.
        // This avoids going through callFunction and stays in the VM loop.
        if (Env * env2 = vmBindLambdaArg(state, *fun, arg, pos)) {
            vm.nrBytecodeCallTrampoline++;
            // Look up the bytecoded body info from the side-table.
            auto & bodyInfo = state.lambdaBodyCache[fun->lambda().fun];
            auto & bodyUnit = *bodyInfo.unit;

            // For formals lambdas, jump to the PROLOGUE (which unpacks
            // the attrset arg into env slots, then falls through to
            // the body).  For simple lambdas, prologue == body offset.
            bool hasFormals = fun->lambda().fun->getFormals().has_value();
            uint32_t startOffset = hasFormals
                ? bodyInfo.prologueOffset
                : bodyUnit.thunks[bodyInfo.thunkIdx].codeOffset;

            assert(startOffset < bodyUnit.code.size()
                && "OP_CALL_1: startOffset out of bounds");

            // Save current frame state.
            vm.frames.back().ip = ip;
            vm.frames.back().env = curEnv;

            // Push a new call frame for the lambda body.
            auto * result = state.allocValue();

            vm.frames.push_back(CallFrame{
                .unit = &bodyUnit,
                .ip = startOffset,
                .env = env2,
                .stackBaseOffset = vm.stackSize(),
                .resultSlot = result,
                .callPos = pos,
            });

            // For formals lambdas, push the raw arg onto the stack
            // so the bytecoded prologue can pop it for unpacking.
            if (hasFormals) {
                vm.push(arg);
            }

            // Switch to the prologue/body code.
            cu = &bodyUnit;
            ip = startOffset;
            curEnv = env2;

            DISPATCH();
        }

        // ── Direct primop dispatch ──
        // Avoids callFunction overhead (profiler hooks, call depth, loop).
        if (fun->isPrimOp()) {
            auto * fn = fun->primOp();
            // state.nrPrimOpCalls is private; skip for now.
            if (fn->arity == 1) {
                // Saturated single-arg primop (head, length, typeOf, etc.)
                auto * result = state.allocValue();
                Value * argPtr = arg;
                fn->impl(state, pos, &argPtr, *result);
                vm.push(result);
                DISPATCH();
            } else {
                // Unsaturated (arity > 1): create PrimOpApp.
                auto * funCopy = state.allocValue();
                *funCopy = *fun;
                auto * result = state.allocValue();
                result->mkPrimOpApp(funCopy, arg);
                vm.push(result);
                DISPATCH();
            }
        }

        if (fun->isPrimOpApp()) {
            // Walk the chain to find root PrimOp and count captured args.
            size_t argsDone = 0;
            Value * root = fun;
            while (root->isPrimOpApp()) {
                argsDone++;
                root = root->primOpApp().left;
            }
            assert(root->isPrimOp());
            auto * fn = root->primOp();
            auto argsLeft = fn->arity - argsDone;

            if (argsLeft == 1) {
                // Saturated: collect all args and call.
                // state.nrPrimOpCalls is private; skip for now.
                Value * vArgs[maxPrimOpArity];
                auto n = argsDone;
                for (Value * v = fun; v->isPrimOpApp(); v = v->primOpApp().left)
                    vArgs[--n] = v->primOpApp().right;
                vArgs[argsDone] = arg;

                auto * result = state.allocValue();
                fn->impl(state, pos, vArgs, *result);
                vm.push(result);
                DISPATCH();
            } else {
                // Still unsaturated: extend the PrimOpApp chain.
                auto * funCopy = state.allocValue();
                *funCopy = *fun;
                auto * result = state.allocValue();
                result->mkPrimOpApp(funCopy, arg);
                vm.push(result);
                DISPATCH();
            }
        }

        // ── Functor dispatch (__functor attrset) ──
        // { __functor = self: arg: body; } arg
        // → (__functor self) arg (two sequential calls)
        // Step 1 uses callFunction (the __functor value is arbitrary).
        // Step 2 tries VM fast paths for the result.
        if (fun->type() == nAttrs) {
            if (auto * functorAttr = fun->attrs()->get(state.s.functor)) {
                Value * self = state.allocValue();
                *self = *fun;

                // Step 1: __functor(self) → partial.
                Value * partial = state.allocValue();
                state.callFunction(*functorAttr->value, *self, *partial, functorAttr->pos);

                // Step 2: partial(arg) — try VM fast paths.
                state.forceValue(*partial, pos);

                if (Env * env2 = vmBindLambdaArg(state, *partial, arg, pos)) {
                    vm.nrBytecodeCallTrampoline++;
                    auto & bodyInfo = state.lambdaBodyCache[partial->lambda().fun];
                    auto & bodyUnit = *bodyInfo.unit;
                    bool hasFormals = partial->lambda().fun->getFormals().has_value();
                    uint32_t startOffset = hasFormals
                        ? bodyInfo.prologueOffset
                        : bodyUnit.thunks[bodyInfo.thunkIdx].codeOffset;

                    vm.frames.back().ip = ip;
                    vm.frames.back().env = curEnv;
                    auto * result = state.allocValue();
                    vm.frames.push_back(CallFrame{
                        .unit = &bodyUnit,
                        .ip = startOffset,
                        .env = env2,
                        .stackBaseOffset = vm.stackSize(),
                        .resultSlot = result,
                        .callPos = pos,
                    });
                    if (hasFormals) vm.push(arg);
                    cu = &bodyUnit;
                    ip = startOffset;
                    curEnv = env2;
                    DISPATCH();
                }

                if (partial->isPrimOp()) {
                    auto * fn = partial->primOp();
                    if (fn->arity == 1) {
                        auto * result = state.allocValue();
                        Value * argPtr = arg;
                        fn->impl(state, pos, &argPtr, *result);
                        vm.push(result);
                        DISPATCH();
                    }
                }

                // Fallback for step 2.
                auto * result = state.allocValue();
                state.callFunction(*partial, *arg, *result, pos);
                vm.push(result);
                DISPATCH();
            }
        }

        // Fallback: non-bytecoded lambdas, exotic cases.
        vm.nrCallFallbacks++;
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
    // Phase 3: Attribute selection
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_select_force:
#else
    case OP_SELECT_FORCE:
#endif
    {
        uint32_t symIdx = decodeOperand(CUR_INSTR);
        Value * attrs = vm.top();
        PosIdx pos = cu->posForOffset(ip - 1);
        Symbol name = cu->symbols[symIdx];
        state.forceAttrs(*attrs, pos, "while selecting an attribute");
        if (auto j = attrs->attrs()->get(name)) {
            // TODO: state.nrLookups++ (private, needs friend decl)
            state.forceValue(*j->value, pos);
            // Replace top of stack with the selected value.
            *(vm.sp - 1) = j->value;
        } else {
            state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                .atPos(pos).debugThrow();
        }
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_attr_select:
#else
    case OP_ATTR_SELECT:
#endif
    {
        uint32_t symIdx = decodeOperand(CUR_INSTR);
        Value * attrs = vm.top();
        PosIdx pos = cu->posForOffset(ip - 1);
        Symbol name = cu->symbols[symIdx];
        state.forceAttrs(*attrs, pos, "while selecting an attribute");
        if (auto j = attrs->attrs()->get(name)) {
            // TODO: state.nrLookups++ (private, needs friend decl)
            *(vm.sp - 1) = j->value;
        } else {
            state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                .atPos(pos).debugThrow();
        }
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_attr_select_dyn:
#else
    case OP_ATTR_SELECT_DYN:
#endif
    {
        // Dynamic attribute selection: pop nameVal, pop attrs.
        // Coerce name to string, create Symbol, lookup in attrs.
        Value * nameVal = vm.pop();
        Value * attrs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceStringNoCtx(*nameVal, pos,
            "while evaluating an attribute name");
        Symbol name = state.symbols.create(nameVal->string_view());
        state.forceAttrs(*attrs, pos, "while selecting an attribute");
        if (auto j = attrs->attrs()->get(name)) {
            // Push the POINTER (not a copy) — same as OP_ATTR_SELECT.
            // Thunk memoization requires pointer identity: forceValue
            // updates the Value in-place, and all holders must see it.
            vm.push(j->value);
        } else {
            state.error<EvalError>("attribute '%1%' missing",
                state.symbols[name]).atPos(pos).debugThrow();
        }
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_has_attr:
#else
    case OP_HAS_ATTR:
#endif
    {
        uint32_t symIdx = decodeOperand(CUR_INSTR);
        Value * attrs = vm.top();
        Symbol name = cu->symbols[symIdx];
        bool has = attrs->type() == nAttrs && attrs->attrs()->get(name);
        *(vm.sp - 1) = has ? &Value::vTrue : &Value::vFalse;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_has_attr_dyn:
#else
    case OP_HAS_ATTR_DYN:
#endif
    {
        // Dynamic has-attr: pop nameVal, peek attrs, push bool.
        // Stack: [..., attrs, nameVal] → [..., attrs, bool]
        Value * nameVal = vm.pop();
        Value * attrs = vm.top();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceStringNoCtx(*nameVal, pos,
            "while evaluating an attribute name");
        Symbol name = state.symbols.create(nameVal->string_view());
        bool has = attrs->type() == nAttrs && attrs->attrs()->get(name);
        *(vm.sp - 1) = has ? &Value::vTrue : &Value::vFalse;
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_jump_if_not_attrs:
#else
    case OP_JUMP_IF_NOT_ATTRS:
#endif
    {
        int32_t offset = decodeSigned(CUR_INSTR);
        Value * v = vm.top();
        // v is already forced.
        if (v->type() != nAttrs)
            ip = static_cast<uint32_t>(static_cast<int32_t>(ip) + offset);
        DISPATCH();
    }

    // ==================================================================
    // Phase 3: Attrset update (//)
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_attrs_update:
#else
    case OP_ATTRS_UPDATE:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceAttrs(*lhs, pos, "in the left operand of the update (//) operator");
        state.forceAttrs(*rhs, pos, "in the right operand of the update (//) operator");

        auto * result = state.allocValue();
        // Delegate to a non-inline helper to avoid non-trivial local
        // destructors that break computed-goto dispatch.
        vmAttrsUpdate(state, *result, *lhs, *rhs);
        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Phase 3: List concatenation (++)
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_list_concat:
#else
    case OP_LIST_CONCAT:
#endif
    {
        Value * rhs = vm.pop();
        Value * lhs = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceList(*lhs, pos, "while evaluating the left operand of ++");
        state.forceList(*rhs, pos, "while evaluating the right operand of ++");

        auto lSize = lhs->listSize();
        auto rSize = rhs->listSize();

        auto * result = state.allocValue();
        if (lSize == 0) { *result = *rhs; }
        else if (rSize == 0) { *result = *lhs; }
        else {
            auto list = state.buildList(lSize + rSize);
            auto * out = list.elems;
            auto lView = lhs->listView();
            auto rView = rhs->listView();
            if (lSize) memcpy(out, lView.data(), lSize * sizeof(Value *));
            if (rSize) memcpy(out + lSize, rView.data(), rSize * sizeof(Value *));
            result->mkList(list);
        }

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Phase 3: With scope
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_push_with:
#else
    case OP_PUSH_WITH:
#endif
    {
        Value * attrsVal = vm.pop();
        // Allocate a 1-slot env for the with-scope.
        Env & env2 = state.mem.allocEnv(1);
        env2.up = curEnv;
        env2.values[0] = attrsVal;
        curEnv = &env2;
        DISPATCH();
    }

    // ==================================================================
    // Phase 3b: List construction
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_list_init:
#else
    case OP_LIST_INIT:
#endif
    {
        uint32_t size = decodeOperand(CUR_INSTR);

        if (size == 0) {
            auto * result = state.allocValue();
            result->mkList(state.mem.buildList(0));
            vm.push(result);
        } else {
            auto list = state.mem.buildList(size);
            // Pop values in reverse order (last pushed = last element).
            for (uint32_t i = size; i > 0; --i)
                list[i - 1] = vm.pop();
            auto * result = state.allocValue();
            result->mkList(list);
            vm.push(result);
        }
        DISPATCH();
    }

    // ==================================================================
    // Phase 3b: Attrset construction
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_attrs_init:
#else
    case OP_ATTRS_INIT:
#endif
    {
        uint32_t nAttrs = decodeOperand(CUR_INSTR);

        auto bindings = state.buildBindings(nAttrs);

        // Read nAttrs symbol indices from the following data words.
        // Pop nAttrs values from the stack (in reverse, since last
        // pushed = last attr in sorted order).
        // We need to pair them: the data words are in forward order
        // (matching the sorted attr iteration), and the stack has
        // values in the same order (first pushed = first attr).
        // So we collect values first, then pair.
        // Use heap allocation for large attrsets.
        // Stack allocation for small ones (common case).
        constexpr uint32_t kStackMax = 64;
        Value * stackValues[kStackMax];
        Value ** values = nAttrs <= kStackMax
            ? stackValues
            : new Value*[nAttrs];
        for (uint32_t i = nAttrs; i > 0; --i)
            values[i - 1] = vm.pop();

        for (uint32_t i = 0; i < nAttrs; i++) {
            // Read the (symbol index, position index) pair from data words.
            assert(ip < cu->code.size() && "OP_ATTRS_INIT: code buffer overrun (symbol)");
            uint32_t symIdx = decodeOperand(cu->code[ip++]);
            assert(ip < cu->code.size() && "OP_ATTRS_INIT: code buffer overrun (position)");
            uint32_t posIdx = decodeOperand(cu->code[ip++]);
            assert(symIdx < cu->symbols.size() && "OP_ATTRS_INIT: symbol index out of bounds");
            Symbol name = cu->symbols[symIdx];
            PosIdx attrPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
            bindings.insert(name, values[i], attrPos);
        }

        auto * result = state.allocValue();
        result->mkAttrs(bindings.alreadySorted());

        if (values != stackValues)
            delete[] values;

        vm.push(result);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_attrs_dyn_init:
#else
    case OP_ATTRS_DYN_INIT:
#endif
    {
        // Mixed static+dynamic attrset builder.
        // Operand: [nStatic:12 | nDynamic:12]
        // Stack: [static_vals...] [dyn_name, dyn_val] pairs...
        // Data words: (symIdx, posIdx) pairs for static attrs,
        //             then posIdx for each dynamic attr.
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t nStatic  = operand >> 12;
        uint32_t nDynamic = operand & 0xFFF;
        PosIdx pos = cu->posForOffset(ip - 1);

        // Pop dynamic name+value pairs (reverse stack order).
        constexpr uint32_t kMaxDyn = 32;
        struct DynPair { Value * name; Value * val; };
        DynPair dynStack[kMaxDyn];
        DynPair * dynPairs = nDynamic <= kMaxDyn ? dynStack : new DynPair[nDynamic];
        for (uint32_t i = nDynamic; i > 0; --i) {
            dynPairs[i-1].val  = vm.pop();
            dynPairs[i-1].name = vm.pop();
        }

        // Pop static values (reverse stack order).
        constexpr uint32_t kMaxStatic = 64;
        Value * staticStack[kMaxStatic];
        Value ** staticVals = nStatic <= kMaxStatic ? staticStack : new Value*[nStatic];
        for (uint32_t i = nStatic; i > 0; --i)
            staticVals[i-1] = vm.pop();

        // Build bindings with max capacity.
        auto bindings = state.buildBindings(nStatic + nDynamic);

        // Insert static attrs (already sorted from compiler).
        for (uint32_t i = 0; i < nStatic; i++) {
            uint32_t symIdx = decodeOperand(cu->code[ip++]);
            uint32_t posIdx = decodeOperand(cu->code[ip++]);
            Symbol name = cu->symbols[symIdx];
            PosIdx attrPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
            bindings.insert(name, staticVals[i], attrPos);
        }

        // Process dynamic attrs.
        bool needsSort = false;
        for (uint32_t i = 0; i < nDynamic; i++) {
            uint32_t posIdx = decodeOperand(cu->code[ip++]);
            PosIdx dynPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;

            state.forceValue(*dynPairs[i].name, dynPos);

            // Null name → skip this attribute.
            if (dynPairs[i].name->type() == nNull)
                continue;

            state.forceStringNoCtx(*dynPairs[i].name, dynPos,
                "while evaluating the name of a dynamic attribute");
            auto nameSym = state.symbols.create(dynPairs[i].name->string_view());

            bindings.insert(nameSym, dynPairs[i].val, dynPos);
            needsSort = true;
        }

        auto * result = state.allocValue();
        result->mkAttrs(needsSort ? bindings.finish() : bindings.alreadySorted());

        if (staticVals != staticStack) delete[] staticVals;
        if (dynPairs != dynStack) delete[] dynPairs;

        vm.push(result);
        DISPATCH();
    }

    // OP_ATTR_INSERT and OP_ATTRS_FINISH are not needed with the
    // compound OP_ATTRS_INIT approach -- left as unhandled.

    // ==================================================================
    // Phase 3b: String concatenation / addition
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_str_concat_init:
#else
    case OP_STR_CONCAT_INIT:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t nParts = operand & ((1u << 23) - 1);
        bool forceString = (operand >> 23) & 1;
        PosIdx pos = cu->posForOffset(ip - 1);

        auto * result = state.allocValue();
        vmStrConcat(state, vm, *cu, nParts, forceString, pos, *result);
        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // __curPos
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_pos:
#else
    case OP_POS:
#endif
    {
        uint32_t posIdx = decodeOperand(CUR_INSTR);
        PosIdx pos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
        auto * result = state.allocValue();
        state.mkPos(*result, pos);
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
        vm.nrEvalExprFallbacks++;
        if (getEnv("NIX_VM_TRACE_FALLBACK").value_or("") == "1") {
            fprintf(stderr, "[FALLBACK #%llu] %s cu=%p exprIdx=%u\n",
                (unsigned long long)vm.nrEvalExprFallbacks,
                typeid(*expr).name(), (void*)cu, exprIdx);
        }
        if (getEnv("NIX_VM_TRACE_FALLBACK").value_or("") == "1") {
            std::ostringstream oss;
            expr->show(state.symbols, oss);
            auto s = oss.str();
            fprintf(stderr, "[FALLBACK #%llu] %s: %.200s\n",
                (unsigned long long)vm.nrEvalExprFallbacks,
                typeid(*expr).name(), s.c_str());
        }

        auto * result = state.allocValue();
        expr->eval(state, *curEnv, *result);

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Upvalue access
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_get_upvalue:
#else
    case OP_GET_UPVALUE:
#endif
    {
        uint32_t idx = decodeOperand(CUR_INSTR);
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_GET_UPVALUE: no upvalue array in current frame");
        vm.push(upvalues[idx]);
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Stack slot access (frame-relative)
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_get_stack_slot:
#else
    case OP_GET_STACK_SLOT:
#endif
    {
        uint32_t slot = decodeOperand(CUR_INSTR);
        size_t base = vm.frames.back().stackBaseOffset;
        vm.push(vm.stack[base + slot]);
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_set_stack_slot:
#else
    case OP_SET_STACK_SLOT:
#endif
    {
        uint32_t slot = decodeOperand(CUR_INSTR);
        Value * v = vm.pop();
        size_t base = vm.frames.back().stackBaseOffset;
        // Ensure the slot exists in the stack.  If needed, push nulls
        // to extend up to the slot index.
        size_t targetIdx = base + slot;
        while (vm.stackSize() <= targetIdx) {
            vm.push(&Value::vNull);
        }
        vm.stack[targetIdx] = v;
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Allocate a fresh Value* for recursive binding pre-allocation
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_alloc_value:
#else
    case OP_ALLOC_VALUE:
#endif
    {
        // Allocate a fresh Value* via the GC-traced allocator.
        // This is used to pre-allocate stack slot values for recursive
        // let bindings so that thunks can capture a stable pointer.
        vm.push(state.allocValue());
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Copy Value data into a pre-allocated stack slot
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_copy_to_slot:
#else
    case OP_COPY_TO_SLOT:
#endif
    {
        uint32_t slot = decodeOperand(CUR_INSTR);
        Value * src = vm.pop();
        size_t base = vm.frames.back().stackBaseOffset;
        Value * dst = vm.stack[base + slot];
        // Copy the Value data in-place, preserving the destination pointer.
        // Any upvalues that captured this Value* will see the updated data.
        *dst = *src;
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Closure creation with upvalue capture
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_make_closure_v2:
#else
    case OP_MAKE_CLOSURE_V2:
#endif
    {
        uint32_t lambdaIdx = decodeOperand(CUR_INSTR);
        assert(lambdaIdx < cu->lambdas.size()
            && "OP_MAKE_CLOSURE_V2: lambda index out of bounds");

        // Read the upvalue count from the next data word.
        uint32_t nUpvalues = decodeOperand(cu->code[ip++]);

        // Allocate a flat GC-traced array for captured upvalues.
        Value ** upvalues = nullptr;
        if (nUpvalues > 0) {
            upvalues = static_cast<Value **>(
                GC_MALLOC(nUpvalues * sizeof(Value *)));
            // Pop upvalues from the stack.
            // They were pushed in forward order (upvalue 0 first),
            // so pop in reverse to get the correct mapping.
            for (uint32_t i = nUpvalues; i > 0; --i)
                upvalues[i - 1] = vm.pop();
        }

        auto & desc = cu->lambdas[lambdaIdx];

        // Register this lambda's bytecoded body in the side-table.
        // v2 closures also need this for OP_CALL_1 trampoline dispatch.
        if (desc.sourceExpr) {
            state.lambdaBodyCache[desc.sourceExpr] = {
                const_cast<CompilationUnit *>(cu),
                desc.bodyThunkIdx,
                desc.prologueOffset,
            };
        }

        // Create the closure Value.
        // For v2 closures, we store the upvalue array on a side-allocated
        // 2-slot Env whose values[1] is a pointer to the upvalue array.
        // The actual upvalue array is the GC-allocated flat array.
        //
        // We still need an Env to satisfy the Value::lambda().env field.
        // The env is minimal (2 slots) and acts as a carrier for the upvalues.
        // values[0] is set to vNull so that OP_GET_WITH (which reads
        // env.values[0]) sees a safe sentinel instead of a stale pointer.
        Env & closureEnv = state.mem.allocEnv(2);
        closureEnv.up = curEnv; // Parent env for with-chain walking.
        closureEnv.values[0] = const_cast<Value *>(&Value::vNull);
        // Store the upvalue array pointer in values[1].
        // The OP_CALL_1 v2 path will extract it from here.
        closureEnv.values[1] = reinterpret_cast<Value *>(upvalues);

        // For v2, we need to create a lambda expression wrapper.
        // Use ExprLambdaBytecode which stores the compilation unit + index.
        auto * lambdaExpr = state.mem.exprs.add<ExprLambdaBytecode>(
            const_cast<CompilationUnit *>(cu), lambdaIdx);

        auto * closureVal = state.allocValue();
        closureVal->mkLambda(&closureEnv, lambdaExpr);

        vm.push(closureVal);
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Thunk creation with upvalue capture
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_make_thunk_v2:
#else
    case OP_MAKE_THUNK_V2:
#endif
    {
        uint32_t thunkIdx = decodeOperand(CUR_INSTR);
        assert(thunkIdx < cu->thunks.size()
            && "OP_MAKE_THUNK_V2: thunk index out of bounds");

        // Read the upvalue count from the next data word.
        uint32_t nUpvalues = decodeOperand(cu->code[ip++]);

        // Allocate and populate the upvalue array.
        Value ** upvalues = nullptr;
        if (nUpvalues > 0) {
            upvalues = static_cast<Value **>(
                GC_MALLOC(nUpvalues * sizeof(Value *)));
            for (uint32_t i = nUpvalues; i > 0; --i)
                upvalues[i - 1] = vm.pop();
        }

        // Create ExprBytecodeThunk for the body.
        auto * thunkExpr = state.mem.exprs.add<ExprBytecodeThunk>(
            const_cast<CompilationUnit *>(cu), thunkIdx);

        // Create a carrier Env for the upvalue array.
        // Allocate size=2: values[0] is a safe null Value (so
        // OP_GET_WITH won't crash if the env is in a with-chain),
        // values[1] holds the reinterpret_cast'd upvalue pointer.
        Env & thunkEnv = state.mem.allocEnv(2);
        thunkEnv.up = curEnv;
        thunkEnv.values[0] = &Value::vNull;
        thunkEnv.values[1] = reinterpret_cast<Value *>(upvalues);

        auto * thunkVal = state.allocValue();
        thunkVal->mkThunk(&thunkEnv, thunkExpr);

        vm.push(thunkVal);
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
        // Dump disassembly around the crash point for debugging.
        std::string disasm = disassemble(*cu, &state);
        throw Error("bytecode VM: unhandled opcode 0x%02x at offset %d\n\nDisassembly:\n%s",
            op, ip - 1, disasm);
    }

#ifndef NIX_VM_COMPUTED_GOTO
        } // switch
    } // for(;;)
#endif

    } catch (...) {
        // Exception thrown during VM execution (e.g., from forceValue,
        // callFunction, or an OP_EVAL_EXPR fallback).  Clean up the
        // frame and stack state so the caller sees a consistent VMState.
        while (vm.frames.size() > entryFrameDepth) {
            vm.sp = vm.stack + vm.frames.back().stackBaseOffset;
            vm.frames.pop_back();
        }
        throw;
    }
}

} // namespace nix::bytecode
