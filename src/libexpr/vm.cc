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
#include "nix/expr/ir-emit.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-error.hh"
#include "nix/expr/nix-word.hh"
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

/// Push a thunk-force CallFrame and mkBlackhole the value being forced.
/// Captures origExpr/origEnv so vmExec's catch block can revert the
/// blackhole to mkFailed if the body throws.
///
/// The maxSlot parameter is currently UNUSED — an attempt at frame-entry
/// stack pre-extension caused subtle test failures (some sub-block
/// pattern reads a slot before the body writes it; pre-fill with vNull
/// trips the read).  The per-instruction ensureCapacity in the
/// register-form ops is cheap enough that the optimization isn't
/// worth the risk.  Kept the parameter to avoid touching every caller.
[[gnu::always_inline]]
static inline void pushThunkFrame(VMState & vm,
    const CompilationUnit * unit, uint32_t ip, Env * env,
    Value * v, PosIdx pos, Value ** upvalues,
    Expr * origExpr, uint16_t /*maxSlot*/ = 0)
{
    v->mkBlackhole();
    CallFrame f{};
    f.unit = unit;
    f.ip = ip;
    f.env = env;
    f.stackBaseOffset = vm.stackSize();
    f.resultSlot = v;
    f.callPos = pos;
    f.isThunkForce = true;
    f.upvalues = upvalues;
    f.origExpr = origExpr;
    f.origEnv = env;
    vm.frames.push_back(f);
}

/// Materialize a tagged immediate into a heap-allocated Value.
/// If `w` is already a real pointer (low bit clear), returns it unchanged.
/// If `w` is a tagged immediate, allocates a Value and decodes the tag.
[[gnu::always_inline]]
static inline Value * materializeWord(EvalState & state, Value * w)
{
    if (!nanbox::isTagged(w)) [[likely]]
        return w;
    // Only tagged ints exist; bool/null are static singletons.
    assert(nanbox::isTaggedInt(w));
    Value * v = state.allocValue();
    v->mkInt(static_cast<NixInt::Inner>(nanbox::decodeInt(w)));
    return v;
}

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
    uint64_t totalAttr = vm.nrAttrCacheHits + vm.nrAttrCacheMisses;
    fprintf(stderr, "  AttrCache hits: %llu, misses: %llu (hit rate: %.1f%%)\n",
        (unsigned long long)vm.nrAttrCacheHits,
        (unsigned long long)vm.nrAttrCacheMisses,
        totalAttr ? 100.0 * vm.nrAttrCacheHits / totalAttr : 0.0);
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
        case OP_CELL_GET:
            fprintf(stderr, " cell_uv=%u formal=%u", operand >> 16, operand & 0xFFFF);
            break;
        case OP_CELL_SET:
            fprintf(stderr, " cell_slot=%u formal=%u", operand >> 16, operand & 0xFFFF);
            break;
        case OP_CALL_PRIMOP:
            fprintf(stderr, " arity=%u constIdx=%u", operand >> 16, operand & 0xFFFF);
            break;
        case OP_SLOT_SLOT_CALL1:
            fprintf(stderr, " func=%u arg=%u", operand >> 12, operand & 0xFFF);
            break;
        case OP_GET_SLOT_FORCE:
        case OP_GET_SLOT_RETURN:
        case OP_GET_UV_FORCE:
            fprintf(stderr, " idx=%u", operand);
            break;
        case OP_MOV_SLOTS:
            fprintf(stderr, " src=%u dst=%u", operand >> 12, operand & 0xFFF);
            break;
        case OP_RFORCE_FROM:
        case OP_RGET_UV_TO:
        case OP_RUVF_TO:
            fprintf(stderr, " dst=%u src=%u", operand >> 16, operand & 0xFFFF);
            break;
        case OP_RADD_R:
        case OP_RSUB_R:
        case OP_RMUL_R:
        case OP_RLESS_R:
        case OP_REQ_R:
            fprintf(stderr, " dst=%u lhs=%u rhs=%u",
                bytecode::unpackDst(operand),
                bytecode::unpackA(operand),
                bytecode::unpackB(operand));
            break;
        case OP_RATTR_SELF_R:
            fprintf(stderr, " dst=%u attrs=%u cacheIdx=%u",
                bytecode::unpackDst(operand),
                bytecode::unpackA(operand),
                bytecode::unpackB(operand));
            break;
        case OP_RCALL1_R:
            fprintf(stderr, " dst=%u func=%u arg=%u",
                bytecode::unpackDst(operand),
                bytecode::unpackA(operand),
                bytecode::unpackB(operand));
            break;
        case OP_ATTR_SELECT_CACHED:
        case OP_ATTR_SELECT_FORCE_CACHED:
            fprintf(stderr, " cacheIdx=%u", operand);
            break;
        case OP_ALLOC_CELL:
            fprintf(stderr, " size=%u", operand);
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

    // grow() only fires when sp passes the previous stackEnd, so this
    // is also a convenient sample point for peakStackDepth and
    // peakFrameDepth.  Misses peaks that don't trigger a stack grow
    // (we'd need to instrument every push() / push_back()), but
    // captures enough for stat purposes.
    if (used > peakStackDepth)
        peakStackDepth = used;
    if (frames.size() > peakFrameDepth)
        peakFrameDepth = frames.size();
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
///
/// M2b: when LHS is non-empty, RHS is small, and LHS's layer chain is not
/// already at maxLayers, build a layered Bindings (RHS as top layer over
/// LHS) instead of materialising a flat copy.  Mirrors the tree-walker's
/// optimisation in ExprOpUpdate::eval (eval.cc:~2261).  All callers of
/// (*b)[N] in vm.cc are already guarded with !b->isLayered() and the
/// libexpr-c API collapses layers before raw access, so layered results
/// are safe to propagate.
[[gnu::noinline]]
static void vmAttrsUpdate(EvalState & state, Value & result, Value & lhs, Value & rhs)
{
    auto & bindings1 = *lhs.attrs();
    auto & bindings2 = *rhs.attrs();

    if (bindings1.empty()) { result = rhs; return; }
    if (bindings2.empty()) { result = lhs; return; }

    /* Layer when the RHS is small enough that copying it on top of an
       already-allocated LHS Bindings is cheaper than materialising the
       union as a fresh flat Bindings.  Use the same setting the tree-
       walker honours, so behaviour is identical across both paths. */
    const bool shouldLayer =
        !bindings1.isLayerListFull()
        && bindings2.size() <= state.settings.bindingsUpdateLayerRhsSizeThreshold;

    if (shouldLayer) {
        auto attrs = state.buildBindings(bindings2.size());
        attrs.layerOnTopOf(bindings1);
        std::ranges::copy(bindings2, std::back_inserter(attrs));
        result.mkAttrs(attrs.alreadySorted());
        return;
    }

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
    for (uint32_t i = nParts; i > 0; --i) {
        Value * v = vm.pop();
        parts[i - 1] = materializeWord(state, v);
    }

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
// Out-of-line helpers for opcodes with large stack-local arrays.
// Extracting these prevents the compiler from allocating their arrays
// in vmExec's frame, keeping vmExec's C stack footprint small (~100-200
// bytes) for deep recursive re-entry chains.
// ---------------------------------------------------------------------------

/// OP_ATTRS_INIT: build a Bindings from N values on the VM stack.
[[gnu::noinline]]
static Value * vmAttrsInit(
    EvalState & state, VMState & vm,
    const CompilationUnit * cu, uint32_t & ip, uint32_t nAttrs)
{
    auto bindings = state.buildBindings(nAttrs);
    constexpr uint32_t kStackMax = 64;
    Value * stackValues[kStackMax];
    Value ** values = nAttrs <= kStackMax
        ? stackValues : new Value*[nAttrs];
    for (uint32_t i = nAttrs; i > 0; --i) {
        Value * v = vm.pop();
        values[i - 1] = materializeWord(state, v);
    }
    for (uint32_t i = 0; i < nAttrs; i++) {
        uint32_t symIdx = decodeOperand(cu->code[ip++]);
        uint32_t posIdx = decodeOperand(cu->code[ip++]);
        Symbol name = cu->symbols[symIdx];
        PosIdx attrPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
        bindings.insert(name, values[i], attrPos);
    }
    auto * result = state.allocValue();
    result->mkAttrs(bindings.alreadySorted());
    if (values != stackValues) delete[] values;
    return result;
}

/// OP_ATTRS_DYN_INIT: build a Bindings with static + dynamic attrs.
[[gnu::noinline]]
static Value * vmAttrsDynInit(
    EvalState & state, VMState & vm,
    const CompilationUnit * cu, uint32_t & ip,
    uint32_t nStatic, uint32_t nDynamic)
{
    // Pop dynamic name/value pairs.
    struct DynPair { Value * name; Value * val; };
    constexpr uint32_t kMaxDyn = 32;
    DynPair dynStack[kMaxDyn];
    DynPair * dynPairs = nDynamic <= kMaxDyn ? dynStack : new DynPair[nDynamic];
    for (uint32_t i = nDynamic; i > 0; --i) {
        Value * val = vm.pop();
        Value * name = vm.pop();
        dynPairs[i-1].val  = materializeWord(state, val);
        dynPairs[i-1].name = materializeWord(state, name);
    }
    // Pop static values.
    constexpr uint32_t kMaxStatic = 64;
    Value * staticStack[kMaxStatic];
    Value ** staticVals = nStatic <= kMaxStatic ? staticStack : new Value*[nStatic];
    for (uint32_t i = nStatic; i > 0; --i) {
        Value * v = vm.pop();
        staticVals[i-1] = materializeWord(state, v);
    }

    auto bindings = state.buildBindings(nStatic + nDynamic);
    for (uint32_t i = 0; i < nStatic; i++) {
        uint32_t symIdx = decodeOperand(cu->code[ip++]);
        uint32_t posIdx = decodeOperand(cu->code[ip++]);
        Symbol name = cu->symbols[symIdx];
        PosIdx attrPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
        bindings.insert(name, staticVals[i], attrPos);
    }
    bool needsSort = false;
    for (uint32_t i = 0; i < nDynamic; i++) {
        uint32_t posIdx = decodeOperand(cu->code[ip++]);
        PosIdx dynPos = posIdx < cu->posPool.size() ? cu->posPool[posIdx] : noPos;
        state.forceValue(*dynPairs[i].name, dynPos);
        if (dynPairs[i].name->type() == nNull) continue;
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
    return result;
}

/// OP_CALL_1 saturated primop: collect args and call.
[[gnu::noinline]]
static void vmCallSaturatedPrimOp(
    EvalState & state, VMState & vm, const PrimOp * fn,
    Value * fun, Value * arg, uint32_t argsDone, PosIdx pos)
{
    Value * vArgs[maxPrimOpArity];
    auto n = argsDone;
    for (Value * v = fun; v->isPrimOpApp(); v = v->primOpApp().left)
        vArgs[--n] = v->primOpApp().right;
    vArgs[argsDone] = arg;
    auto * result = state.allocValue();
    const_cast<PrimOp *>(fn)->impl(state, pos, vArgs, *result);
    vm.push(result);
}

/// OP_CALL multi-arg: collect args and call callFunction.
[[gnu::noinline]]
static void vmCallMultiArg(
    EvalState & state, VMState & vm, uint32_t nArgs, PosIdx pos)
{
    assert(nArgs <= 16);
    Value * args[16];
    for (uint32_t i = nArgs; i > 0; --i) {
        Value * v = vm.pop();
        args[i - 1] = materializeWord(state, v);
    }
    Value * fun = vm.pop();
    fun = materializeWord(state, fun);
    auto * result = state.allocValue();
    state.callFunction(*fun, std::span<Value *>(args, nArgs), *result, pos);
    vm.push(result);
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

    // Recursion guard: detect runaway vmExec re-entry.
    static thread_local uint32_t vmExecDepth = 0;
    if (++vmExecDepth > 10000) {
        fprintf(stderr, "FATAL: vmExec depth %u — infinite recursion\n", vmExecDepth);
        abort();
    }
    struct DepthGuard { ~DepthGuard() { vmExecDepth--; } } depthGuard;

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
    // stackBase is hoisted from vm.frames.back().stackBaseOffset so that
    // hot opcodes (GET_STACK_SLOT, SET_STACK_SLOT, register-form ops)
    // can index off a register-cached local instead of paying for a
    // vector-back access on every read.  Must be refreshed at every
    // frame transition (push, pop, in-place replace).
    const CompilationUnit * cu = &unit;
    uint32_t ip   = startOffset;
    Env * curEnv = &env;
    size_t stackBase = vm.frames.back().stackBaseOffset;

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
        REGISTER_OP(OP_CALL_PRIMOP,  op_call_primop);

        // VM v2: upvalue-based closures (IR emitter)
        REGISTER_OP(OP_GET_UPVALUE,      op_get_upvalue);
        REGISTER_OP(OP_MAKE_CLOSURE_V2,  op_make_closure_v2);
        REGISTER_OP(OP_MAKE_THUNK_V2,    op_make_thunk_v2);
        REGISTER_OP(OP_GET_STACK_SLOT,   op_get_stack_slot);
        REGISTER_OP(OP_SET_STACK_SLOT,   op_set_stack_slot);
        REGISTER_OP(OP_ALLOC_VALUE,      op_alloc_value);
        REGISTER_OP(OP_COPY_TO_SLOT,     op_copy_to_slot);
        REGISTER_OP(OP_CELL_GET,         op_cell_get);
        REGISTER_OP(OP_CELL_SET,         op_cell_set);
        REGISTER_OP(OP_ALLOC_CELL,       op_alloc_cell);

        // Superinstructions (fused common patterns)
        REGISTER_OP(OP_GET_SLOT_FORCE,   op_get_slot_force);
        REGISTER_OP(OP_GET_SLOT_RETURN,  op_get_slot_return);
        REGISTER_OP(OP_GET_UV_FORCE,     op_get_uv_force);
        REGISTER_OP(OP_SLOT_SLOT_CALL1,  op_slot_slot_call1);
        REGISTER_OP(OP_ATTR_SELECT_CACHED, op_attr_select_cached);
        REGISTER_OP(OP_ATTR_SELECT_FORCE_CACHED, op_attr_select_force_cached);
        REGISTER_OP(OP_MOV_SLOTS, op_mov_slots);
        REGISTER_OP(OP_RFORCE_FROM, op_rforce_from);
        REGISTER_OP(OP_RGET_UV_TO, op_rget_uv_to);
        REGISTER_OP(OP_RUVF_TO, op_ruvf_to);
        REGISTER_OP(OP_RADD_R, op_radd_r);
        REGISTER_OP(OP_RSUB_R, op_rsub_r);
        REGISTER_OP(OP_RMUL_R, op_rmul_r);
        REGISTER_OP(OP_RLESS_R, op_rless_r);
        REGISTER_OP(OP_REQ_R, op_req_r);
        REGISTER_OP(OP_RATTR_SELF_R, op_rattr_self_r);
        REGISTER_OP(OP_RCALL1_R, op_rcall1_r);
        REGISTER_OP(OP_TAIL_CALL_1, op_tail_call_1);
        REGISTER_OP(OP_RLIT_INT, op_rlit_int);
        REGISTER_OP(OP_RCONST, op_rconst);
        REGISTER_OP(OP_RUPDATE_R, op_rupdate_r);
        REGISTER_OP(OP_RCONCATLIST_R, op_rconcatlist_r);
        REGISTER_OP(OP_RNOT_R, op_rnot_r);
        REGISTER_OP(OP_RNEG_R, op_rneg_r);
        REGISTER_OP(OP_RMAKE_THUNK_V2, op_rmake_thunk_v2);
        REGISTER_OP(OP_RMAKE_CLOSURE_V2, op_rmake_closure_v2);
        REGISTER_OP(OP_RATTRS_INIT, op_rattrs_init);
        REGISTER_OP(OP_RLIST_INIT, op_rlist_init);
        REGISTER_OP(OP_GET_UV2, op_get_uv2);
        REGISTER_OP(OP_GET_SLOT2, op_get_slot2);
        REGISTER_OP(OP_GET_UV_SLOT, op_get_uv_slot);
        REGISTER_OP(OP_GET_SLOT_UV, op_get_slot_uv);

#undef REGISTER_OP
        tableInitialized = true;
    }

    auto & tcfg = traceConfig();
    auto & stepCounter = globalStepCounter();

    // Profiling and tracing hook -- called before every instruction.
    // When tracing is disabled (the common case), this is just a counter
    // increment with ZERO branches.  The branch on tcfg.enabled is
    // hoisted out of the dispatch loop entirely.
    bool tracingEnabled = tcfg.enabled;
#define VM_HOOK() do {                                         \
        vm.nrInstructions++;                                   \
    } while (0)
#define VM_HOOK_TRACE() do {                                   \
        vm.nrInstructions++;                                   \
        uint64_t step = stepCounter++;                         \
        if (step >= tcfg.from && step <= tcfg.to)              \
            traceInstruction(state, *cu, ip - 1,               \
                cu->code[ip - 1], step,                        \
                static_cast<size_t>(vm.sp - vm.stack),         \
                vm.frames.size());                             \
    } while (0)

    // Computed-goto dispatch macro.
    // When tracing is enabled, use the trace-aware hook.
    // When disabled, just increment the counter (zero branches).
#define DISPATCH() do {                              \
        Instruction _instr = cu->code[ip++];         \
        if (tracingEnabled) [[unlikely]]             \
            VM_HOOK_TRACE();                         \
        else                                         \
            VM_HOOK();                               \
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
    bool tracingEnabled = tcfg.enabled;

    try { // exception cleanup: restore frame/stack on throw

    for (;;) {
        Instruction instr = cu->code[ip++];

        vm.nrInstructions++;
        if (tracingEnabled) [[unlikely]] {
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
        // OP_NOP is normally a data word for OP_MAKE_*_V2 / OP_ATTRS_INIT
        // (consumed via `cu->code[ip++]` in those handlers).  In rare
        // cases (e.g. unit tests) it appears as a real instruction at
        // a code offset; treat it as a true no-op.
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
        // 24-bit immediates always fit in 60-bit tagged int (positive).
        // No allocation, no cache lookup — just encode directly.
        vm.push(nanbox::encodeInt(static_cast<int64_t>(imm)));
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_return:
#else
    case OP_RETURN:
#endif
    {
        Value * retVal = vm.pop();

        // Snapshot frame fields BEFORE pop_back / push_back below — both
        // can invalidate the reference (push_back may reallocate the
        // frames vector storage; pop_back leaves frame past-the-end).
        // Reading `frame.callPos` after pop_back is UB in standard C++
        // (the bytes survive in practice but only by coincidence).
        bool wasThunkForce;
        Value * resultSlot;
        size_t outgoingBase;
        uint32_t resultStoreSlot;
        size_t resultStoreParentBase;
        PosIdx callPos;
        {
            auto & frame = vm.frames.back();
            wasThunkForce = frame.isThunkForce;
            resultSlot = frame.resultSlot;
            outgoingBase = frame.stackBaseOffset;
            resultStoreSlot = frame.resultStoreSlot;
            resultStoreParentBase = frame.resultStoreParentBase;
            callPos = frame.callPos;
        }

        // Materialize tagged immediates before writing to resultSlot
        // (which is a real heap-allocated Value) or storing into a
        // register-form parent slot.
        retVal = materializeWord(state, retVal);

        // Write the result into the caller's result slot.  When
        // resultSlot is nullptr (set by OP_RCALL1_R's fast path which
        // skips the per-call Value alloc), the result is delivered
        // directly via resultStoreSlot below — no copy needed.
        if (resultSlot)
            *resultSlot = *retVal;

        // Restore stack to frame entry point (offset-based, survives stack realloc).
        vm.sp = vm.stack + outgoingBase;
        vm.frames.pop_back();

        // Iterative thunk chain resolution for thunk-force returns.
        // If the result is STILL a thunk (thunk chain), push a new
        // trampoline frame at the SAME frame depth (the previous frame
        // was just popped).  Each chain link runs as: push → body →
        // OP_RETURN → pop → check → push next.  Frame depth stays at
        // original + 1, matching the tree-walker's iterative forceValue.
        if (wasThunkForce && (resultSlot->isThunkOrApp())) {
            if (resultSlot->isThunk()) {
                Env * chainEnv = resultSlot->thunk().env;
                Expr * chainExpr = resultSlot->thunk().expr;
                if (chainEnv && chainExpr && chainExpr->isBytecodeThunk) {
                    auto * bcThunk = static_cast<ExprBytecodeThunk *>(chainExpr);
                    auto & td = bcThunk->unit->thunks[bcThunk->thunkIdx];
                    Value ** uv = nullptr;
                    if (td.nUpvalues > 0)
                        uv = &chainEnv->values[1];

                    // Frame-depth guard: chain hops can recurse unbounded
                    // for pathological aliases like `let a=b; b=c; ...`.
                    if (vm.frames.size() > 65536) [[unlikely]]
                        state.error<EvalError>("infinite recursion encountered")
                            .atPos(callPos).debugThrow();

                    resultSlot->mkBlackhole();
                    CallFrame chainFrame{};
                    chainFrame.unit = bcThunk->unit;
                    chainFrame.ip = td.codeOffset;
                    chainFrame.env = chainEnv;
                    chainFrame.stackBaseOffset = vm.stackSize();
                    chainFrame.resultSlot = resultSlot;
                    chainFrame.callPos = callPos;
                    chainFrame.isThunkForce = true;
                    chainFrame.upvalues = uv;
                    chainFrame.origExpr = chainExpr;
                    chainFrame.origEnv = chainEnv;
                    stackBase = chainFrame.stackBaseOffset;
                    vm.frames.push_back(chainFrame);
                    cu = bcThunk->unit;
                    ip = td.codeOffset;
                    curEnv = chainEnv;
                    DISPATCH();
                }
            }
            // Non-bytecode thunk or App chain: delegate to forceValue.
            state.forceValue(*resultSlot, callPos);
        }

        if (vm.frames.size() <= entryFrameDepth) {
            // Final sample for stats (misses interim peaks but captures
            // total stack/frame extent before unwind).
            size_t curStack = vm.stackSize();
            if (curStack > vm.peakStackDepth)
                vm.peakStackDepth = curStack;
            if (vm.frames.size() > vm.peakFrameDepth)
                vm.peakFrameDepth = vm.frames.size();
            return;
        }

        // Resume the caller's frame.
        auto & caller = vm.frames.back();
        cu     = caller.unit;
        ip     = caller.ip;
        curEnv = caller.env;
        stackBase = caller.stackBaseOffset;

        // ── VM-native primop continuations ──
        // Caller frame may carry a 1-based contIdx into vm.contStack
        // when participating in a VM-native primop loop.
        if (caller.contIdx != 0) [[unlikely]] {
            auto & cont = vm.cont(caller.contIdx);
            if (cont.kind == ContKind::Map) {
                // Store this iteration's result.
                Value * elemResult = state.allocValue();
                *elemResult = *resultSlot;
                cont.results[cont.index] = elemResult;
                cont.index++;

                if (cont.index < cont.count) {
                    // More elements: trigger next call f(list[index]).
                    vm.push(cont.func);
                    vm.push(cont.list->listView()[cont.index]);
                    goto op_call_1;
                }

                // Done: build the final list Value.
                auto * listVal = state.allocValue();
                auto listBuilder = state.buildList(cont.count);
                for (uint32_t i = 0; i < cont.count; i++)
                    listBuilder[i] = cont.results[i];
                listVal->mkList(listBuilder);
                // Clear cont state for hygiene (no stale GC roots).
                cont.kind = ContKind::None;
                cont.func = nullptr;
                cont.list = nullptr;
                cont.results = nullptr;
                cont.inputElems = nullptr;
                caller.contIdx = 0;
                vm.push(listVal);
                DISPATCH();
            }
        }

        // Register-form caller (OP_RCALL1_R): write the result POINTER
        // directly to the parent frame's stack slot, skipping the
        // operand-stack push.  When resultSlot is nullptr, the fast
        // path didn't pre-allocate — deliver retVal directly (saves
        // one Value allocation per fast-path call).
        if (resultStoreSlot > 0) {
            vm.stack[resultStoreParentBase + (resultStoreSlot - 1)] =
                resultSlot ? resultSlot : retVal;
        } else if (!wasThunkForce) {
            vm.push(resultSlot ? resultSlot : retVal);
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
        // Fast path: tagged scalars or already-forced values.
        if (nanbox::isTagged(v)) [[likely]]
            DISPATCH();
        if (!v->isThunkOrApp()) [[likely]]
            DISPATCH();
        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline thunk trampoline (same as OP_FORCE).
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;

            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];

                // Extract v2 upvalues from carrier env if present.
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];

                // Frame depth guard.
                if (vm.frames.size() > 65536) [[unlikely]] {
                    std::ostringstream oss;
                    oss << state.positions[pos];
                    fprintf(stderr, "Frame guard at %s (depth=%zu)\n",
                        oss.str().c_str(), vm.frames.size());
                    state.error<EvalError>("infinite recursion encountered")
                        .atPos(pos).debugThrow();
                }

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);
                cu = bcThunk->unit;
                ip = thunkOffset;
                curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
                DISPATCH();
            }
        }

        // Fallback for non-bytecoded thunks, apps, non-thunks.
        if (v->isThunkOrApp()) vm.nrForceFallbacks++;
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
        // Skip non-with envs: v2 carrier envs (values[0] == &Value::vNull),
        // tree-walker envs with NULL values[0], and any other env that
        // doesn't have a valid with-scope attrs pointer.
        auto isNonWithEnv = [](Env * env) {
            return env && (!env->values[0] || env->values[0] == &Value::vNull);
        };
        Env * e = curEnv;
        for (auto l = var->level; l; --l) {
            e = e->up;
            while (isNonWithEnv(e))
                e = e->up;
        }

        // Walk the with-chain looking for the variable.
        auto * fromWith = var->fromWith;
        while (true) {
            while (isNonWithEnv(e))
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
                while (isNonWithEnv(e))
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

        // Fast path: tagged scalars (int/bool/null) are always forced.
        if (nanbox::isTagged(v)) [[likely]]
            DISPATCH();

        // Fast path: value is already forced (most common case).
        // Skip all branch checks for ints, strings, attrsets, lists, etc.
        if (!v->isThunkOrApp()) [[likely]]
            DISPATCH();

        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline trampoline for bytecoded thunks: force within the VM
        // loop by pushing a CallFrame, avoiding C-stack growth.
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;

            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];

                // v2 thunks store upvalues inline starting at env.values[1].
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];

                // Save current frame state.
                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;

                // Push a new call frame for the thunk body.
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);

                // Switch to the thunk's code.
                cu = bcThunk->unit;
                ip = thunkOffset;
                curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
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
                vm.nrBytecodeCallTrampoline++;
                auto * bcLambda = static_cast<ExprLambdaBytecode *>(
                    left->lambda().fun);
                auto & bodyUnit = *bcLambda->unit;
                auto & desc = bodyUnit.lambdas[bcLambda->lambdaIdx];
                uint32_t startOffset = bytecode::realizeThunkCodeOffset(
                    state, bodyUnit, desc.bodyThunkIdx);
                auto & thunkDesc = bodyUnit.thunks[desc.bodyThunkIdx];

                Value ** frameUpvalues = nullptr;
                if (desc.nUpvalues > 0 && left->lambda().env)
                    frameUpvalues = &left->lambda().env->values[1];

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                pushThunkFrame(vm, &bodyUnit, startOffset,
                    left->lambda().env, v, pos, frameUpvalues,
                    /*origExpr=*/nullptr, thunkDesc.maxSlot);
                stackBase = vm.frames.back().stackBaseOffset;
                // Store arg as stack slot 0 (the parameter).
                vm.push(right);
                cu = &bodyUnit;
                ip = startOffset;
                curEnv = left->lambda().env;
                DISPATCH();
            }

            // v1 App path: env-chain closures in lambdaBodyCache.
            if (Env * env2 = vmBindLambdaArg(state, *left, right, pos)) {
                vm.nrBytecodeCallTrampoline++;
                auto & bodyInfo = state.lambdaBodyCache[left->lambda().fun];
                auto & bodyUnit = *bodyInfo.unit;
                bool hasFormals = left->lambda().fun->getFormals().has_value();
                uint32_t startOffset = hasFormals
                    ? bodyInfo.prologueOffset
                    : bodyUnit.thunks[bodyInfo.thunkIdx].codeOffset;

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                pushThunkFrame(vm, &bodyUnit, startOffset, env2,
                    v, pos, /*upvalues=*/nullptr, /*origExpr=*/nullptr,
                    bodyUnit.thunks[bodyInfo.thunkIdx].maxSlot);
                stackBase = vm.frames.back().stackBaseOffset;
                if (hasFormals) vm.push(right);
                cu = &bodyUnit;
                ip = startOffset;
                curEnv = env2;
                DISPATCH();
            }
        }

        // Fallback for non-bytecoded thunks, remaining apps, non-thunks.
        if (v->isThunkOrApp()) vm.nrForceFallbacks++;
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
        v = materializeWord(state, v);
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
        v = materializeWord(state, v);
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

        // Tagged-int + tagged-int fast path — no allocation, no force.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t sum;
            if (!__builtin_add_overflow(a, b, &sum)
                && nanbox::intFitsTagged(sum)) {
                vm.push(nanbox::encodeInt(sum));
                DISPATCH();
            }
            // Overflow or doesn't fit in 60 bits — fall through to heap.
        }

        // Materialize tagged operands so the rest of the code can treat
        // them as real Value*s.
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t diff;
            if (!__builtin_sub_overflow(a, b, &diff)
                && nanbox::intFitsTagged(diff)) {
                vm.push(nanbox::encodeInt(diff));
                DISPATCH();
            }
        }

        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t prod;
            if (!__builtin_mul_overflow(a, b, &prod)
                && nanbox::intFitsTagged(prod)) {
                vm.push(nanbox::encodeInt(prod));
                DISPATCH();
            }
        }

        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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

        // Tagged-int fast path (integer division).
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t b = nanbox::decodeInt(rhs);
            if (b != 0) {
                int64_t a = nanbox::decodeInt(lhs);
                // Watch for INT_MIN / -1 overflow.
                if (!(a == INT64_MIN && b == -1)) {
                    int64_t q = a / b;
                    if (nanbox::intFitsTagged(q)) {
                        vm.push(nanbox::encodeInt(q));
                        DISPATCH();
                    }
                }
            }
        }

        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(v)) [[likely]] {
            int64_t a = nanbox::decodeInt(v);
            if (a != INT64_MIN) {
                int64_t neg = -a;
                if (nanbox::intFitsTagged(neg)) {
                    vm.push(nanbox::encodeInt(neg));
                    DISPATCH();
                }
            }
        }

        v = materializeWord(state, v);
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

        // Tagged scalar fast paths.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            bool eq = nanbox::decodeInt(lhs) == nanbox::decodeInt(rhs);
            vm.push(eq ? &Value::vTrue : &Value::vFalse);
            DISPATCH();
        }
        if (nanbox::isTagged(lhs) || nanbox::isTagged(rhs)) {
            // Mixed: pointer equality (cheap) or materialize for full check.
            if (lhs == rhs) {
                vm.push(&Value::vTrue);
                DISPATCH();
            }
            lhs = materializeWord(state, lhs);
            rhs = materializeWord(state, rhs);
        }

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

        // Tagged scalar fast paths.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            bool neq = nanbox::decodeInt(lhs) != nanbox::decodeInt(rhs);
            vm.push(neq ? &Value::vTrue : &Value::vFalse);
            DISPATCH();
        }
        if (nanbox::isTagged(lhs) || nanbox::isTagged(rhs)) {
            lhs = materializeWord(state, lhs);
            rhs = materializeWord(state, rhs);
        }

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

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            bool lt = nanbox::decodeInt(lhs) < nanbox::decodeInt(rhs);
            vm.push(lt ? &Value::vTrue : &Value::vFalse);
            DISPATCH();
        }

        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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

        v = materializeWord(state, v);
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

        cond = materializeWord(state, cond);
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
        // Materialize before storing in heap env.
        v = materializeWord(state, v);
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
        v = materializeWord(state, v);
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
        v = materializeWord(state, v);
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
        // Materialize tagged immediates — fun and arg need to be real
        // Values for callFunction / forceValue / closure dispatch.
        fun = materializeWord(state, fun);
        arg = materializeWord(state, arg);
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
            uint32_t startOffset = bytecode::realizeThunkCodeOffset(
                state, bodyUnit, desc.bodyThunkIdx);

            assert(startOffset < bodyUnit.code.size()
                && "OP_CALL_1 v2: startOffset out of bounds");

            // v2 closures store upvalues inline starting at env.values[1].
            Value ** frameUpvalues = nullptr;
            if (desc.nUpvalues > 0 && fun->lambda().env)
                frameUpvalues = &fun->lambda().env->values[1];

            // Save current frame state.
            vm.frames.back().ip = ip;
            vm.frames.back().env = curEnv;

            auto * result = state.allocValue();

            // Push the call frame.  The body code uses OP_GET_STACK_SLOT(0)
            // to read the argument and OP_GET_UPVALUE(i) for captures.
            // OP_SET_STACK_SLOT auto-extends the stack, so no pre-allocation
            // of local slots is needed here.
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
            stackBase = vm.stackSize();
            vm.frames.push_back(CallFrame{
                .unit = &bodyUnit,
                .ip = startOffset,
                .env = fun->lambda().env,
                .stackBaseOffset = stackBase,
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

                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
            stackBase = vm.stackSize();
            vm.frames.push_back(CallFrame{
                .unit = &bodyUnit,
                .ip = startOffset,
                .env = env2,
                .stackBaseOffset = stackBase,
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
                // ── VM-native continuation for builtins.map ──
                // DISABLED by default: eager evaluation of map elements
                // breaks `take 1 (map throw xs)` and similar lazy-take
                // patterns common in nixpkgs' module system.  The proper
                // fix is a thunk-creating continuation that defers each
                // f(list[i]) until the consumer forces it.  Until then,
                // gate behind NIX_VM_NATIVE_MAP=1 for benchmarking.
                static const bool nativeMap =
                    getenv("NIX_VM_NATIVE_MAP") != nullptr;
                if (nativeMap && fn->arity == 2 && fn->name == "map") {
                    Value * f = fun->primOpApp().right;
                    state.forceList(*arg, pos, "while evaluating the second argument of builtins.map");
                    auto listView = arg->listView();
                    auto listSize = listView.size();

                    if (listSize == 0) {
                        // Empty list → empty result list.
                        auto * result = state.allocValue();
                        result->mkList(state.buildList(0));
                        vm.push(result);
                        DISPATCH();
                    }

                    // Index listView[i] directly — no need to copy.
                    auto * results = static_cast<Value **>(
                        GC_MALLOC(listSize * sizeof(Value *)));
                    auto & frame = vm.frames.back();
                    if (frame.contIdx == 0)
                        frame.contIdx = vm.allocCont();
                    auto & cont = vm.cont(frame.contIdx);
                    cont.kind = ContKind::Map;
                    cont.index = 0;
                    cont.count = static_cast<uint32_t>(listSize);
                    cont.func = f;
                    cont.results = results;
                    cont.list = arg;

                    // Trigger the first call: f(list[0]).
                    vm.push(f);
                    vm.push(listView[0]);
                    goto op_call_1;
                }

                // ── VM-native continuation for builtins.foldl' ──
                // foldl' op nul list — strict by spec, no laziness
                // gotcha.  Drives module merging, lib.recursiveUpdate,
                // attrset folds.  Avoids prim_foldlStrict's intermediate
                // Value alloc per element for the running accumulator.
                if (fn->arity == 3 && fn->name == "__foldl'") {
                    // fun = primOpApp(primOpApp(primop, op), nul)
                    // → op = fun.left.right, nul = fun.right
                    Value * op = fun->primOpApp().left->primOpApp().right;
                    Value * nul = fun->primOpApp().right;
                    state.forceFunction(*op, pos,
                        "while evaluating the first argument passed to builtins.foldl'");
                    state.forceList(*arg, pos,
                        "while evaluating the third argument passed to builtins.foldl'");
                    auto listView = arg->listView();
                    auto listSize = listView.size();

                    if (listSize == 0) {
                        // Empty list → return nul forced.
                        state.forceValue(*nul, pos);
                        vm.push(nul);
                        DISPATCH();
                    }

                    auto & frame = vm.frames.back();
                    if (frame.contIdx == 0)
                        frame.contIdx = vm.allocCont();
                    auto & cont = vm.cont(frame.contIdx);
                    cont.kind = ContKind::FoldlStrict;
                    cont.index = 0;
                    cont.count = static_cast<uint32_t>(listSize);
                    cont.func = op;
                    cont.list = arg;
                    cont.accumulator = nul;

                    // Trigger the first call: op(nul, list[0]).  We
                    // need to apply op to two args; OP_CALL_1 takes
                    // one.  Strategy: emit `(op nul) list[0]` — push
                    // op, push nul, call (returns partial), then push
                    // list[0] and call again.  Simpler: build a
                    // primOpApp-equivalent closure invocation by hand.
                    // We use the curried op approach: callFunction(op,
                    // [nul, list[0]]) — but to match the continuation
                    // resumption pattern, we use OP_CALL_1 with nul
                    // first to produce `op nul`, then dispatch the
                    // continuation which pushes list[0].
                    //
                    // Easier: make the FIRST call a normal call to
                    // produce the curried `op nul`, then push list[0]
                    // and op_call_1 again.  We track this via index=0
                    // meaning "next call delivers element 0".
                    Value * args2[2] = {nul, listView[0]};
                    auto * result = state.allocValue();
                    state.callFunction(*op, args2, *result, pos);
                    // After this synchronous call we have the new
                    // accumulator.  Loop in a tight C while because
                    // continuations require a v2 closure to reach
                    // OP_RETURN; safer to drive the fold here.
                    cont.accumulator = result;
                    cont.index = 1;
                    while (cont.index < cont.count) {
                        Value * args3[2] = {cont.accumulator,
                            listView[cont.index]};
                        auto * next = state.allocValue();
                        state.callFunction(*op, args3, *next, pos);
                        cont.accumulator = next;
                        cont.index++;
                    }
                    state.forceValue(*cont.accumulator, pos);
                    Value * finalAcc = cont.accumulator;
                    cont.kind = ContKind::None;
                    cont.func = nullptr;
                    cont.list = nullptr;
                    cont.accumulator = nullptr;
                    frame.contIdx = 0;
                    vm.push(finalAcc);
                    DISPATCH();
                }

                // ── VM-native continuation for builtins.filter ──
                // filter pred list — semantically equivalent to scan
                // through list, applying pred, collecting elements
                // where pred returns true.  Predicate result is
                // BOOL — strict — and the elements themselves are
                // not transformed, so the resulting list preserves
                // identity of element thunks.  Safe to drive eagerly.
                if (fn->arity == 2 && fn->name == "filter") {
                    Value * pred = fun->primOpApp().right;
                    state.forceFunction(*pred, pos,
                        "while evaluating the first argument passed to builtins.filter");
                    state.forceList(*arg, pos,
                        "while evaluating the second argument passed to builtins.filter");
                    auto listView = arg->listView();
                    auto listSize = listView.size();

                    if (listSize == 0) {
                        auto * result = state.allocValue();
                        result->mkList(state.buildList(0));
                        vm.push(result);
                        DISPATCH();
                    }

                    // Synchronous filter loop.  For each element, call
                    // pred(elem); if true, keep the original element.
                    auto * matched = static_cast<Value **>(
                        GC_MALLOC(listSize * sizeof(Value *)));
                    size_t nMatched = 0;
                    bool anyMissed = false;
                    for (size_t i = 0; i < listSize; i++) {
                        Value * elem = listView[i];
                        auto * predResult = state.allocValue();
                        Value * predArgs[1] = {elem};
                        state.callFunction(*pred, predArgs, *predResult, pos);
                        state.forceValue(*predResult, pos);
                        if (predResult->type() != nBool)
                            state.error<TypeError>(
                                "expected a Boolean but found %1%: %2%",
                                showType(*predResult),
                                ValuePrinter(state, *predResult, PrintOptions{}))
                                .atPos(pos).debugThrow();
                        if (predResult->boolean()) {
                            matched[nMatched++] = elem;
                        } else {
                            anyMissed = true;
                        }
                    }

                    auto * result = state.allocValue();
                    if (!anyMissed) {
                        // Optimization: pred kept everything — return
                        // the input list unchanged, preserving thunks.
                        *result = *arg;
                    } else {
                        auto listBuilder = state.buildList(nMatched);
                        for (size_t i = 0; i < nMatched; i++)
                            listBuilder[i] = matched[i];
                        result->mkList(listBuilder);
                    }
                    vm.push(result);
                    DISPATCH();
                }

                // ── VM-native continuations for builtins.all / any ──
                // Strict, short-circuiting: stop at first counter-
                // example.  Like filter, drives the predicate inline.
                if (fn->arity == 2
                    && (fn->name == "all" || fn->name == "any"))
                {
                    bool isAll = (fn->name == "all");
                    Value * pred = fun->primOpApp().right;
                    state.forceFunction(*pred, pos,
                        isAll
                            ? "while evaluating the first argument passed to builtins.all"
                            : "while evaluating the first argument passed to builtins.any");
                    state.forceList(*arg, pos,
                        isAll
                            ? "while evaluating the second argument passed to builtins.all"
                            : "while evaluating the second argument passed to builtins.any");
                    auto listView = arg->listView();
                    auto listSize = listView.size();

                    bool result = isAll;  // all-of-empty=true, any-of-empty=false
                    for (size_t i = 0; i < listSize; i++) {
                        auto * predResult = state.allocValue();
                        Value * predArgs[1] = {listView[i]};
                        state.callFunction(*pred, predArgs, *predResult, pos);
                        state.forceValue(*predResult, pos);
                        if (predResult->type() != nBool)
                            state.error<TypeError>(
                                "expected a Boolean but found %1%: %2%",
                                showType(*predResult),
                                ValuePrinter(state, *predResult, PrintOptions{}))
                                .atPos(pos).debugThrow();
                        bool b = predResult->boolean();
                        if (isAll && !b) { result = false; break; }
                        if (!isAll && b) { result = true; break; }
                    }
                    vm.push(result ? &Value::vTrue : &Value::vFalse);
                    DISPATCH();
                }

                // genList continuation deferred — eager genList breaks
                // lazy fixpoint patterns in stage.nix (old nixpkgs).
                // Need a thunk-creating variant that trampolines within
                // the existing vmExec to fix the frame depth issue
                // without changing lazy semantics.

                if (fn->arity == 2) {
                    Value * vArgs[2] = {fun->primOpApp().right, arg};
                    auto * result = state.allocValue();
                    fn->impl(state, pos, vArgs, *result);
                    vm.push(result);
                } else {
                    vmCallSaturatedPrimOp(state, vm, fn, fun, arg, argsDone, pos);
                }
                DISPATCH();
            } else {
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
                // Frame depth guard
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                    stackBase = vm.stackSize();
                    vm.frames.push_back(CallFrame{
                        .unit = &bodyUnit,
                        .ip = startOffset,
                        .env = env2,
                        .stackBaseOffset = stackBase,
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
        // Delegate to noinline helper (keeps 16-element array off vmExec's frame).
        uint32_t nArgs = decodeOperand(CUR_INSTR);
        PosIdx pos = cu->posForOffset(ip - 1);
        vmCallMultiArg(state, vm, nArgs, pos);
        DISPATCH();
    }

    // OP_TAIL_CALL_1: pop arg + fun, replace the CURRENT call frame
    // with the callee's body in place (no frames.push_back).  The
    // caller's resultSlot / resultStoreSlot survive so OP_RETURN
    // delivers the eventual result to the original consumer.  This
    // converts linear recursion (foldr, imap1, recursiveUpdate) from
    // stacking N frames to running in O(1) frame depth.
    //
    // Only fires when fun is a v2 bytecode-proxy lambda.  Anything
    // else (primop, primopApp, attrset functor, env-chain v1 lambda)
    // falls through to op_call_1 followed by op_return — same
    // behavior as before, just two dispatches instead of one.
#ifdef NIX_VM_COMPUTED_GOTO
op_tail_call_1:
#else
    case OP_TAIL_CALL_1:
#endif
    {
        Value * arg = vm.pop();
        Value * fun = vm.pop();
        PosIdx pos = cu->posForOffset(ip - 1);
        fun = materializeWord(state, fun);
        arg = materializeWord(state, arg);
        if (fun->isThunkOrApp()) [[unlikely]]
            state.forceValue(*fun, pos);

        if (fun->isLambda() && fun->lambda().fun->isBytecodeProxy) {
            vm.nrBytecodeCallTrampoline++;
            auto * bcLambda = static_cast<ExprLambdaBytecode *>(
                fun->lambda().fun);
            auto & bodyUnit = *bcLambda->unit;
            auto & desc = bodyUnit.lambdas[bcLambda->lambdaIdx];
            uint32_t startOffset = bytecode::realizeThunkCodeOffset(
                state, bodyUnit, desc.bodyThunkIdx);
            Value ** frameUpvalues = nullptr;
            if (desc.nUpvalues > 0 && fun->lambda().env)
                frameUpvalues = &fun->lambda().env->values[1];

            // Replace the current frame in place.  Truncate stack to
            // the caller's stackBaseOffset, then push arg as slot 0.
            auto & frame = vm.frames.back();
            vm.sp = vm.stack + frame.stackBaseOffset;
            frame.unit = &bodyUnit;
            frame.ip = startOffset;
            frame.env = fun->lambda().env;
            frame.upvalues = frameUpvalues;
            // Preserve: resultSlot, resultStoreSlot,
            // resultStoreParentBase, callPos, isThunkForce, contIdx —
            // these belong to the caller's caller, not us.

            vm.push(arg);
            cu = &bodyUnit;
            ip = startOffset;
            curEnv = fun->lambda().env;
            DISPATCH();
        }

        // Slow path: not a v2 closure.  Fall back to a regular call
        // followed by return.  Push fun and arg back, jump to op_call_1
        // — the next dispatch lands on the OP_RETURN that ir-emit
        // emits after every tail call.
        vm.push(fun);
        vm.push(arg);
        goto op_call_1;
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
            *(vm.sp - 1) = j->value;
        } else {
            state.error<EvalError>("attribute '%1%' missing", state.symbols[name])
                .atPos(pos).debugThrow();
        }
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_attr_select_cached:
#else
    case OP_ATTR_SELECT_CACHED:
#endif
    {
        // 4-way polymorphic inline cache for attribute select.
        // Hit promotes the matched entry to slot 0 (LRU-on-hit) so the
        // dominant Bindings* is always the first compare.  Miss inserts
        // at nextEvict; entries beyond slot 0 are FIFO.
        uint32_t cacheIdx = decodeOperand(CUR_INSTR);
        AttrCache & cache = cu->attrCaches[cacheIdx];
        Value * attrs = vm.top();
        if (nanbox::isTagged(attrs)) [[unlikely]] {
            attrs = materializeWord(state, attrs);
            *(vm.sp - 1) = attrs;
        }
        // Fast path: if attrs is already a forced attrset, skip
        // forceAttrs (avoids the virtual call + posForOffset for the
        // common case where the value was already evaluated).
        if (attrs->isThunkOrApp()) [[unlikely]]
            state.forceAttrs(*attrs, cu->posForOffset(ip - 1),
                "while selecting an attribute");
        else if (attrs->type() != nAttrs) [[unlikely]]
            state.forceAttrs(*attrs, cu->posForOffset(ip - 1),
                "while selecting an attribute");
        const Bindings * b = attrs->attrs();

        if (cache.entries[0].bindings == b) [[likely]] {
            vm.nrAttrCacheHits++;
            *(vm.sp - 1) = cache.entries[0].value;
            DISPATCH();
        }
        for (int i = 1; i < AttrCache::kEntries; i++) {
            if (cache.entries[i].bindings == b) {
                vm.nrAttrCacheHits++;
                Value * v = cache.entries[i].value;
                // Promote to slot 0.
                cache.entries[i] = cache.entries[0];
                cache.entries[0] = {b, v};
                *(vm.sp - 1) = v;
                DISPATCH();
            }
        }

        // B6: type-shape fallback before binary search.  For non-layered
        // Bindings whose first symbol and size match the cached shape,
        // probe the cached offset directly and verify name equality.
        if (cache.shape.size != 0
            && !b->isLayered()
            && b->size() == cache.shape.size
            && (*b)[0].name == cache.shape.firstSym
            && cache.shape.offset < cache.shape.size
            && (*b)[cache.shape.offset].name == cache.name) {
            vm.nrAttrCacheHits++;
            Value * v = (*b)[cache.shape.offset].value;
            // Insert into identity cache so the next pointer-equal
            // lookup hits the fast path immediately.
            uint8_t evict = cache.nextEvict;
            cache.entries[evict] = {b, v};
            cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
            *(vm.sp - 1) = v;
            DISPATCH();
        }

        // Slow path: cache miss.
        vm.nrAttrCacheMisses++;
        if (auto j = b->get(cache.name)) {
            uint8_t evict = cache.nextEvict;
            cache.entries[evict] = {b, j->value};
            cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
            // Update the shape entry too — non-layered Bindings only,
            // since the offset semantics rely on a single-layer FAM.
            if (!b->isLayered()) {
                cache.shape.firstSym = (*b)[0].name;
                cache.shape.size = b->size();
                cache.shape.offset = static_cast<uint32_t>(j - &(*b)[0]);
            }
            *(vm.sp - 1) = j->value;
        } else {
            state.error<EvalError>("attribute '%1%' missing", state.symbols[cache.name])
                .atPos(cu->posForOffset(ip - 1)).debugThrow();
        }
        DISPATCH();
    }

    // Fused: cached attr select + force.  Same as OP_ATTR_SELECT_CACHED
    // followed by OP_FORCE, but in a single dispatch.
#ifdef NIX_VM_COMPUTED_GOTO
op_attr_select_force_cached:
#else
    case OP_ATTR_SELECT_FORCE_CACHED:
#endif
    {
        uint32_t cacheIdx = decodeOperand(CUR_INSTR);
        AttrCache & cache = cu->attrCaches[cacheIdx];
        Value * attrs = vm.top();
        if (nanbox::isTagged(attrs)) [[unlikely]] {
            attrs = materializeWord(state, attrs);
            *(vm.sp - 1) = attrs;
        }
        // Skip forceAttrs for the common case where attrs is already
        // a forced attrset.  Saves the virtual call + posForOffset
        // binary search in the hot path.
        if (attrs->isThunkOrApp()
            || attrs->type() != nAttrs) [[unlikely]]
            state.forceAttrs(*attrs, cu->posForOffset(ip - 1),
                "while selecting an attribute");
        const Bindings * b = attrs->attrs();

        Value * selected = nullptr;

        if (cache.entries[0].bindings == b) [[likely]] {
            vm.nrAttrCacheHits++;
            selected = cache.entries[0].value;
        } else {
            bool hit = false;
            for (int i = 1; i < AttrCache::kEntries; i++) {
                if (cache.entries[i].bindings == b) {
                    vm.nrAttrCacheHits++;
                    Value * v = cache.entries[i].value;
                    cache.entries[i] = cache.entries[0];
                    cache.entries[0] = {b, v};
                    selected = v;
                    hit = true;
                    break;
                }
            }
            if (!hit
                && cache.shape.size != 0
                && !b->isLayered()
                && b->size() == cache.shape.size
                && (*b)[0].name == cache.shape.firstSym
                && cache.shape.offset < cache.shape.size
                && (*b)[cache.shape.offset].name == cache.name) {
                // B6: type-shape fallback hit — see OP_ATTR_SELECT_CACHED
                // for the rationale.
                vm.nrAttrCacheHits++;
                Value * v = (*b)[cache.shape.offset].value;
                uint8_t evict = cache.nextEvict;
                cache.entries[evict] = {b, v};
                cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
                selected = v;
                hit = true;
            }
            if (!hit) {
                vm.nrAttrCacheMisses++;
                if (auto j = b->get(cache.name)) {
                    uint8_t evict = cache.nextEvict;
                    cache.entries[evict] = {b, j->value};
                    cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
                    if (!b->isLayered()) {
                        cache.shape.firstSym = (*b)[0].name;
                        cache.shape.size = b->size();
                        cache.shape.offset =
                            static_cast<uint32_t>(j - &(*b)[0]);
                    }
                    selected = j->value;
                } else {
                    state.error<EvalError>("attribute '%1%' missing", state.symbols[cache.name])
                        .atPos(cu->posForOffset(ip - 1)).debugThrow();
                }
            }
        }

        // Replace top of stack and force the result inline.
        *(vm.sp - 1) = selected;
        if (!selected->isThunkOrApp()) [[likely]]
            DISPATCH();
        // Slow path: force the value.
        state.forceValue(*selected, cu->posForOffset(ip - 1));
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
        Value * nameVal = materializeWord(state, vm.pop());
        Value * attrs = materializeWord(state, vm.pop());
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
        if (nanbox::isTagged(attrs)) [[unlikely]] {
            attrs = materializeWord(state, attrs);
            *(vm.sp - 1) = attrs;
        }
        // Force the value if it's still a thunk (belt-and-suspenders:
        // the IR should emit OP_FORCE before OP_HAS_ATTR, but the
        // tree-walker's ExprOpHasAttr::eval always forces).
        state.forceValue(*attrs, cu->posForOffset(ip - 1));
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
        Value * nameVal = materializeWord(state, vm.pop());
        Value * attrs = vm.top();
        if (nanbox::isTagged(attrs)) [[unlikely]] {
            attrs = materializeWord(state, attrs);
            *(vm.sp - 1) = attrs;
        }
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceStringNoCtx(*nameVal, pos,
            "while evaluating an attribute name");
        state.forceValue(*attrs, pos);
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
        Value * rhs = materializeWord(state, vm.pop());
        Value * lhs = materializeWord(state, vm.pop());
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
        Value * rhs = materializeWord(state, vm.pop());
        Value * lhs = materializeWord(state, vm.pop());
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
        Value * attrsVal = materializeWord(state, vm.pop());
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
            for (uint32_t i = size; i > 0; --i) {
                Value * v = vm.pop();
                list[i - 1] = materializeWord(state, v);
            }
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

        // Delegate to noinline helper (keeps large arrays off vmExec's frame).
        vm.push(vmAttrsInit(state, vm, cu, ip, nAttrs));
        DISPATCH();
    }

#ifdef NIX_VM_COMPUTED_GOTO
op_attrs_dyn_init:
#else
    case OP_ATTRS_DYN_INIT:
#endif
    {
        // Delegate to noinline helper (keeps large arrays off vmExec's frame).
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t nStatic  = operand >> 12;
        uint32_t nDynamic = operand & 0xFFF;
        vm.push(vmAttrsDynInit(state, vm, cu, ip, nStatic, nDynamic));
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

    // B5 superinstruction: dual GET_UPVALUE (~7.5% of dispatches per
    // profiler).  Encoding: [uv1:12 | uv2:12].  Pushes upvalues[uv1]
    // then upvalues[uv2] in one dispatch.
#ifdef NIX_VM_COMPUTED_GOTO
op_get_uv2:
#else
    case OP_GET_UV2:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t uv1 = operand >> 12;
        uint32_t uv2 = operand & 0xFFF;
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_GET_UV2: no upvalue array in current frame");
        vm.push(upvalues[uv1]);
        vm.push(upvalues[uv2]);
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
        size_t base = stackBase;
        Value * v = vm.stack[base + slot];
        if (!v) [[unlikely]] {
            fprintf(stderr, "FATAL: OP_GET_STACK_SLOT(%u) is NULL at base=%zu\n",
                slot, base);
            abort();
        }
        vm.push(v);
        DISPATCH();
    }

    // B5 superinstruction: dual GET_STACK_SLOT (~6.3% of dispatches).
    // Encoding: [slot1:12 | slot2:12].
#ifdef NIX_VM_COMPUTED_GOTO
op_get_slot2:
#else
    case OP_GET_SLOT2:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t s1 = operand >> 12;
        uint32_t s2 = operand & 0xFFF;
        size_t base = stackBase;
        vm.push(vm.stack[base + s1]);
        vm.push(vm.stack[base + s2]);
        DISPATCH();
    }

    // B5 superinstruction: GET_UPVALUE + GET_STACK_SLOT (~3.9%).
    // Encoding: [uv:12 | slot:12].
#ifdef NIX_VM_COMPUTED_GOTO
op_get_uv_slot:
#else
    case OP_GET_UV_SLOT:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t uv   = operand >> 12;
        uint32_t slot = operand & 0xFFF;
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_GET_UV_SLOT: no upvalue array");
        vm.push(upvalues[uv]);
        size_t base = stackBase;
        vm.push(vm.stack[base + slot]);
        DISPATCH();
    }

    // B5 superinstruction: GET_STACK_SLOT + GET_UPVALUE.
    // Encoding: [slot:12 | uv:12].
#ifdef NIX_VM_COMPUTED_GOTO
op_get_slot_uv:
#else
    case OP_GET_SLOT_UV:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t slot = operand >> 12;
        uint32_t uv   = operand & 0xFFF;
        size_t base = stackBase;
        vm.push(vm.stack[base + slot]);
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_GET_SLOT_UV: no upvalue array");
        vm.push(upvalues[uv]);
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
        // Materialize tagged immediate before storing to slot.
        // This ensures slots always contain real Value* pointers,
        // simplifying readers throughout the VM.
        v = materializeWord(state, v);
        size_t base = stackBase;
        size_t targetIdx = base + slot;
        // Bulk-grow the stack instead of per-element push() loop:
        // ensureCapacity() runs grow() at most once and fills the gap
        // with vNull in a tight loop, vs. the previous N separate
        // bound-check+push iterations.  Identified as a hot
        // SET_STACK_SLOT cost by the bottleneck profiler (B1).
        vm.ensureCapacity(targetIdx + 1, const_cast<Value *>(&Value::vNull));
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
        size_t base = stackBase;
        Value * dst = vm.stack[base + slot];
        // Copy the Value data in-place, preserving the destination pointer.
        // Any upvalues that captured this Value* will see the updated data.
        *dst = *src;
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Formals cell dereference
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_cell_get:
#else
    case OP_CELL_GET:
#endif
    {
        // Operand packs (cell_upvalue_idx:8, formal_index:16).
        // Reads cell[formal_index] where cell is a GC-traced Value*[]
        // array stored as upvalues[cell_upvalue_idx] (reinterpret_cast).
        uint32_t packed = decodeOperand(CUR_INSTR);
        uint32_t cellUvIdx = packed >> 16;
        uint32_t formalIdx = packed & 0xFFFF;
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_CELL_GET: no upvalue array");
        Value ** cell = reinterpret_cast<Value **>(upvalues[cellUvIdx]);
        assert(cell && "OP_CELL_GET: NULL cell pointer");
        vm.push(cell[formalIdx]);
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Formals cell write
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_cell_set:
#else
    case OP_CELL_SET:
#endif
    {
        // Operand packs (cell_stack_slot:8, formal_index:16).
        // Pops a Value* from TOS and writes it into cell[formal_index].
        // The cell pointer is read from stack[base + cell_stack_slot].
        uint32_t packed = decodeOperand(CUR_INSTR);
        uint32_t cellStackSlot = packed >> 16;
        uint32_t formalIdx = packed & 0xFFFF;
        size_t base = stackBase;
        Value ** cell = reinterpret_cast<Value **>(vm.stack[base + cellStackSlot]);
        Value * val = vm.pop();
        cell[formalIdx] = val;
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Allocate formals cell
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_alloc_cell:
#else
    case OP_ALLOC_CELL:
#endif
    {
        // Allocate a GC-traced Value*[] array of the given size.
        // Initialize all entries to &Value::vNull.
        // Push the array pointer (reinterpret_cast'd to Value*).
        uint32_t size = decodeOperand(CUR_INSTR);
        auto * cell = static_cast<Value **>(
            GC_MALLOC(size * sizeof(Value *)));
        for (uint32_t i = 0; i < size; ++i)
            cell[i] = const_cast<Value *>(&Value::vNull);
        vm.push(reinterpret_cast<Value *>(cell));
        DISPATCH();
    }

    // ==================================================================
    // VM v2: Direct saturated primop call
    // ==================================================================

#ifdef NIX_VM_COMPUTED_GOTO
op_call_primop:
#else
    case OP_CALL_PRIMOP:
#endif
    {
        // Operand packs (arity:8, constIdx:16).
        // Calls the primop's impl function directly with all arguments.
        // No intermediate PrimOpApp values, no callFunction dispatch.
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t arity = static_cast<uint8_t>(operand >> 16);
        uint16_t constIdx = static_cast<uint16_t>(operand & 0xFFFF);
        PosIdx pos = cu->posForOffset(ip - 1);

        Value * primVal = cu->constants[constIdx];
        assert(primVal->isPrimOp());
        auto * fn = primVal->primOp();

        // Collect arguments from the operand stack.
        // Args were pushed left-to-right; pop in reverse to fill array.
        // Materialize tagged immediates so primops can deref the Value*.
        Value * vArgs[maxPrimOpArity];
        for (uint8_t i = arity; i > 0; --i)
            vArgs[i - 1] = materializeWord(state, vm.pop());

        // Allocate result and call the primop implementation directly.
        auto * result = state.allocValue();
        fn->impl(state, pos, vArgs, *result);

        vm.push(result);
        DISPATCH();
    }

    // ==================================================================
    // Superinstructions: fused common instruction patterns
    // ==================================================================

    // S1: GET_STACK_SLOT + FORCE → single dispatch.
    // Eliminates push+force round-trip for the most common pattern:
    // reading a local variable and forcing it.
#ifdef NIX_VM_COMPUTED_GOTO
op_get_slot_force:
#else
    case OP_GET_SLOT_FORCE:
#endif
    {
        uint32_t slot = decodeOperand(CUR_INSTR);
        size_t base = stackBase;
        Value * v = vm.stack[base + slot];
        vm.push(v);
        // Fast path: tagged scalar or already-forced.
        if (nanbox::isTagged(v)) [[likely]]
            DISPATCH();
        if (!v->isThunkOrApp()) [[likely]]
            DISPATCH();
        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline force trampoline (same as OP_FORCE).
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;
            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);
                cu = bcThunk->unit; ip = thunkOffset; curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
                DISPATCH();
            }
        }
        // Fallback for non-bytecoded thunks/apps or already-forced values.
        state.forceValue(*v, pos);
        DISPATCH();
    }

    // S2: GET_STACK_SLOT + RETURN → single dispatch.
    // Pushes the slot value then falls through to OP_RETURN,
    // reusing the complex frame-pop logic exactly.
#ifdef NIX_VM_COMPUTED_GOTO
op_get_slot_return:
#else
    case OP_GET_SLOT_RETURN:
#endif
    {
        uint32_t slot = decodeOperand(CUR_INSTR);
        size_t base = stackBase;
        vm.push(vm.stack[base + slot]);
        goto op_return;  // reuse OP_RETURN's frame-pop logic
    }

    // S3: GET_UPVALUE + FORCE → single dispatch.
    // Same as S1 but for captured variables.
#ifdef NIX_VM_COMPUTED_GOTO
op_get_uv_force:
#else
    case OP_GET_UV_FORCE:
#endif
    {
        uint32_t idx = decodeOperand(CUR_INSTR);
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_GET_UV_FORCE: no upvalue array");
        Value * v = upvalues[idx];
        vm.push(v);
        // Fast path: already forced.
        if (!v->isThunkOrApp()) [[likely]]
            DISPATCH();
        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline force trampoline (same as OP_FORCE).
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;
            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];
                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();
                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);
                cu = bcThunk->unit; ip = thunkOffset; curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
                DISPATCH();
            }
        }
        state.forceValue(*v, pos);
        DISPATCH();
    }

    // S4: GET_STACK_SLOT(func) + GET_STACK_SLOT(arg) + CALL_1 → single dispatch.
    // Operand: funcSlot:12 | argSlot:12.  Loads both from local slots,
    // then falls into the OP_CALL_1 handler.  Saves 2 dispatch cycles.
#ifdef NIX_VM_COMPUTED_GOTO
op_slot_slot_call1:
#else
    case OP_SLOT_SLOT_CALL1:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t funcSlot = operand >> 12;
        uint32_t argSlot = operand & 0xFFF;
        size_t base = stackBase;
        // Push fun and arg onto operand stack, then let OP_CALL_1 handle
        // all the dispatch logic (v2 closures, v1 closures, primops, etc.)
        // This saves 2 dispatches (the two GET_STACK_SLOT) while reusing
        // the complex, well-tested CALL_1 handler.
        vm.push(vm.stack[base + funcSlot]);
        vm.push(vm.stack[base + argSlot]);
        goto op_call_1;  // fall through to CALL_1 handler
    }

    // Slot-to-slot copy (mini register-based op).
    // Equivalent to GET_STACK_SLOT(src) + SET_STACK_SLOT(dst) in one
    // dispatch.  Skips the operand stack round-trip entirely.
#ifdef NIX_VM_COMPUTED_GOTO
op_mov_slots:
#else
    case OP_MOV_SLOTS:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t srcSlot = operand >> 12;
        uint32_t dstSlot = operand & 0xFFF;
        size_t base = stackBase;
        // Auto-extend stack if dstSlot is beyond current end.
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = vm.stack[base + srcSlot];
        DISPATCH();
    }

    // ==================================================================
    // Phase 1: Register-form ops (read src/uv, write to dst slot)
    // No operand stack round-trip — direct slot-to-slot.
    // Encoding: [dst:8|src:16]
    // ==================================================================

    // OP_RFORCE_FROM: force value at slot src, write to slot dst.
    // Replaces: GET_SLOT_FORCE + SET_STACK_SLOT (2 → 1 dispatch).
#ifdef NIX_VM_COMPUTED_GOTO
op_rforce_from:
#else
    case OP_RFORCE_FROM:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t srcSlot = operand & 0xFFFF;
        size_t base = stackBase;
        Value * v = vm.stack[base + srcSlot];

        // Auto-extend stack if dstSlot beyond current end.
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));

        // Fast path: already forced.
        if (!v->isThunkOrApp()) [[likely]] {
            vm.stack[base + dstSlot] = v;
            DISPATCH();
        }

        // Need to force.  Push the value, force it (via OP_FORCE inline
        // trampoline path), then store to dst.  We can't trivially
        // trampoline here because the result needs to land in a slot,
        // not on the operand stack.  Use forceValue for now (fallback
        // tree-walker path).  Future optimization: inline trampoline
        // with resultSlot pointing at vm.stack[base + dstSlot].
        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline bytecoded thunk trampoline with resultSlot = the dest slot.
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;
            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];

                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                // Write v's pointer to dst before forcing.  After the
                // force, both src and dst slots point to the same
                // forced Value.
                vm.stack[base + dstSlot] = v;
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);
                cu = bcThunk->unit; ip = thunkOffset; curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
                DISPATCH();
            }
        }

        // Fallback: tree-walker forceValue, then store.
        state.forceValue(*v, pos);
        vm.stack[base + dstSlot] = v;
        DISPATCH();
    }

    // OP_RGET_UV_TO: read upvalue at idx, write to slot dst.
    // Replaces: GET_UPVALUE + SET_STACK_SLOT.
#ifdef NIX_VM_COMPUTED_GOTO
op_rget_uv_to:
#else
    case OP_RGET_UV_TO:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t uvIdx = operand & 0xFFFF;
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_RGET_UV_TO: no upvalue array");
        size_t base = stackBase;
        // Auto-extend stack.
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = upvalues[uvIdx];
        DISPATCH();
    }

    // OP_RUVF_TO: read upvalue at idx, force it, write to slot dst.
    // Replaces: GET_UV_FORCE + SET_STACK_SLOT.
#ifdef NIX_VM_COMPUTED_GOTO
op_ruvf_to:
#else
    case OP_RUVF_TO:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t uvIdx = operand & 0xFFFF;
        Value ** upvalues = vm.frames.back().upvalues;
        assert(upvalues && "OP_RUVF_TO: no upvalue array");
        size_t base = stackBase;
        Value * v = upvalues[uvIdx];

        // Auto-extend stack.
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));

        // Fast path: already forced.
        if (!v->isThunkOrApp()) [[likely]] {
            vm.stack[base + dstSlot] = v;
            DISPATCH();
        }

        PosIdx pos = cu->posForOffset(ip - 1);

        // Inline thunk trampoline (same pattern as RFORCE_FROM).
        if (v->isThunk()) {
            Env * thunkEnv = v->thunk().env;
            Expr * thunkExpr = v->thunk().expr;
            if (thunkEnv && thunkExpr->isBytecodeThunk) {
                auto * bcThunk = static_cast<ExprBytecodeThunk *>(thunkExpr);
                uint32_t thunkOffset = bytecode::realizeThunkCodeOffset(
                    state, *bcThunk->unit, bcThunk->thunkIdx);
                auto & thunkDesc = bcThunk->unit->thunks[bcThunk->thunkIdx];
                Value ** frameUpvalues = nullptr;
                if (thunkDesc.nUpvalues > 0)
                    frameUpvalues = &thunkEnv->values[1];

                if (vm.frames.size() > 65536) [[unlikely]]
                    state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();

                vm.frames.back().ip = ip;
                vm.frames.back().env = curEnv;
                vm.stack[base + dstSlot] = v;
                pushThunkFrame(vm, bcThunk->unit, thunkOffset,
                    thunkEnv, v, pos, frameUpvalues, thunkExpr,
                    thunkDesc.maxSlot);
                cu = bcThunk->unit; ip = thunkOffset; curEnv = thunkEnv;
                stackBase = vm.frames.back().stackBaseOffset;
                DISPATCH();
            }
        }

        state.forceValue(*v, pos);
        vm.stack[base + dstSlot] = v;
        DISPATCH();
    }

    // ==================================================================
    // Phase 2: Three-address register-form arithmetic and comparison.
    // Encoding: [dst:8|a:8|b:8].  Read from slots, write to slot.
    // No operand stack push/pop.
    // ==================================================================

    // dst = *lhs + *rhs, with tagged-int fast path.
#ifdef NIX_VM_COMPUTED_GOTO
op_radd_r:
#else
    case OP_RADD_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        // Auto-extend stack for dst.
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        Value * lhs = vm.stack[base + lhsSlot];
        Value * rhs = vm.stack[base + rhsSlot];

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t sum;
            if (!__builtin_add_overflow(a, b, &sum)
                && nanbox::intFitsTagged(sum)) {
                vm.stack[base + dstSlot] = nanbox::encodeInt(sum);
                DISPATCH();
            }
        }

        PosIdx pos = cu->posForOffset(ip - 1);
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);
        auto * result = state.allocValue();
        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl + fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto sum = lhs->integer() + rhs->integer();
            if (auto v = sum.valueChecked()) result->mkInt(*v);
            else state.error<EvalError>("integer overflow").atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot add %1% to %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        }
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // dst = *lhs - *rhs
#ifdef NIX_VM_COMPUTED_GOTO
op_rsub_r:
#else
    case OP_RSUB_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        Value * lhs = vm.stack[base + lhsSlot];
        Value * rhs = vm.stack[base + rhsSlot];

        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t diff;
            if (!__builtin_sub_overflow(a, b, &diff)
                && nanbox::intFitsTagged(diff)) {
                vm.stack[base + dstSlot] = nanbox::encodeInt(diff);
                DISPATCH();
            }
        }

        PosIdx pos = cu->posForOffset(ip - 1);
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);
        auto * result = state.allocValue();
        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl - fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto diff = lhs->integer() - rhs->integer();
            if (auto v = diff.valueChecked()) result->mkInt(*v);
            else state.error<EvalError>("integer overflow").atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot subtract %1% from %2%",
                showType(*rhs), showType(*lhs)).atPos(pos).debugThrow();
        }
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // dst = *lhs * *rhs
#ifdef NIX_VM_COMPUTED_GOTO
op_rmul_r:
#else
    case OP_RMUL_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        Value * lhs = vm.stack[base + lhsSlot];
        Value * rhs = vm.stack[base + rhsSlot];

        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            int64_t a = nanbox::decodeInt(lhs);
            int64_t b = nanbox::decodeInt(rhs);
            int64_t prod;
            if (!__builtin_mul_overflow(a, b, &prod)
                && nanbox::intFitsTagged(prod)) {
                vm.stack[base + dstSlot] = nanbox::encodeInt(prod);
                DISPATCH();
            }
        }

        PosIdx pos = cu->posForOffset(ip - 1);
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);
        auto * result = state.allocValue();
        if (lhs->type() == nFloat || rhs->type() == nFloat) {
            NixFloat fl = lhs->type() == nFloat ? lhs->fpoint() : static_cast<NixFloat>(lhs->integer().value);
            NixFloat fr = rhs->type() == nFloat ? rhs->fpoint() : static_cast<NixFloat>(rhs->integer().value);
            result->mkFloat(fl * fr);
        } else if (lhs->type() == nInt && rhs->type() == nInt) {
            auto prod = lhs->integer() * rhs->integer();
            if (auto v = prod.valueChecked()) result->mkInt(*v);
            else state.error<EvalError>("integer overflow").atPos(pos).debugThrow();
        } else {
            state.error<EvalError>("cannot multiply %1% and %2%",
                showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        }
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // dst = *lhs < *rhs
#ifdef NIX_VM_COMPUTED_GOTO
op_rless_r:
#else
    case OP_RLESS_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        Value * lhs = vm.stack[base + lhsSlot];
        Value * rhs = vm.stack[base + rhsSlot];

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            bool lt = nanbox::decodeInt(lhs) < nanbox::decodeInt(rhs);
            vm.stack[base + dstSlot] = lt ? &Value::vTrue : &Value::vFalse;
            DISPATCH();
        }

        PosIdx pos = cu->posForOffset(ip - 1);
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
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
        else if (lhs->type() == nInt) cmpResult = lhs->integer() < rhs->integer();
        else if (lhs->type() == nFloat) cmpResult = lhs->fpoint() < rhs->fpoint();
        else if (lhs->type() == nString) cmpResult = lhs->string_view() < rhs->string_view();
        else if (lhs->type() == nPath) cmpResult = lhs->path() < rhs->path();
        else state.error<EvalError>("cannot compare %1% with %2%",
            showType(*lhs), showType(*rhs)).atPos(pos).debugThrow();
        vm.stack[base + dstSlot] = cmpResult ? &Value::vTrue : &Value::vFalse;
        DISPATCH();
    }

    // dst = *lhs == *rhs
#ifdef NIX_VM_COMPUTED_GOTO
op_req_r:
#else
    case OP_REQ_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
        Value * lhs = vm.stack[base + lhsSlot];
        Value * rhs = vm.stack[base + rhsSlot];

        // Tagged-int fast path.
        if (nanbox::isTaggedInt(lhs) && nanbox::isTaggedInt(rhs)) [[likely]] {
            bool eq = nanbox::decodeInt(lhs) == nanbox::decodeInt(rhs);
            vm.stack[base + dstSlot] = eq ? &Value::vTrue : &Value::vFalse;
            DISPATCH();
        }
        if (lhs == rhs) {
            vm.stack[base + dstSlot] = &Value::vTrue;
            DISPATCH();
        }

        PosIdx pos = cu->posForOffset(ip - 1);
        lhs = materializeWord(state, lhs);
        rhs = materializeWord(state, rhs);
        state.forceValue(*lhs, pos);
        state.forceValue(*rhs, pos);
        bool eq;
        if (lhs == rhs) eq = true;
        else if (lhs->type() == nInt && rhs->type() == nInt)
            eq = lhs->integer() == rhs->integer();
        else if (lhs->type() == nString && rhs->type() == nString)
            eq = lhs->string_view() == rhs->string_view();
        else if (lhs->type() == nBool && rhs->type() == nBool)
            eq = lhs->boolean() == rhs->boolean();
        else if (lhs->type() == nNull && rhs->type() == nNull) eq = true;
        else eq = state.eqValues(*lhs, *rhs, pos, "while comparing two values");
        vm.stack[base + dstSlot] = eq ? &Value::vTrue : &Value::vFalse;
        DISPATCH();
    }

    // OP_RCALL1_R: dst = call(*funcSlot, *argSlot).
    // Encoding: [dst:8|funcSlot:8|argSlot:8].  Always emitted by ir-emit
    // followed by OP_SET_STACK_SLOT(dstSlot).
    //
    // Fast path (v2 closure): set up call frame with resultStoreSlot, the
    // body's OP_RETURN writes the result POINTER directly to the dst slot
    // and skips the trailing SET_STACK_SLOT (parent.ip = ip + 1).
    //
    // Slow path: push fun and arg onto operand stack and fall through to
    // op_call_1.  op_call_1's full dispatch (functor, primop, env-chain
    // lambda) handles all callable kinds and pushes the final result onto
    // the operand stack.  The trailing OP_SET_STACK_SLOT then pops and
    // stores normally.
#ifdef NIX_VM_COMPUTED_GOTO
op_rcall1_r:
#else
    case OP_RCALL1_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t funcSlot = bytecode::unpackA(operand);
        uint8_t argSlot = bytecode::unpackB(operand);
        size_t base = stackBase;

        Value * fun = vm.stack[base + funcSlot];
        Value * arg = vm.stack[base + argSlot];
        PosIdx pos = cu->posForOffset(ip - 1);

        fun = materializeWord(state, fun);
        if (fun->isThunkOrApp()) [[unlikely]]
            state.forceValue(*fun, pos);

        // ── Fast path: v2 closure ──
        if (fun->isLambda() && fun->lambda().fun->isBytecodeProxy) {
            arg = materializeWord(state, arg);
            vm.nrBytecodeCallTrampoline++;
            auto * bcLambda = static_cast<ExprLambdaBytecode *>(fun->lambda().fun);
            auto & bodyUnit = *bcLambda->unit;
            auto & desc = bodyUnit.lambdas[bcLambda->lambdaIdx];
            uint32_t startOffset = bytecode::realizeThunkCodeOffset(
                state, bodyUnit, desc.bodyThunkIdx);
            Value ** frameUpvalues = nullptr;
            if (desc.nUpvalues > 0 && fun->lambda().env)
                frameUpvalues = &fun->lambda().env->values[1];

            // Pre-extend parent stack so dst slot exists when OP_RETURN
            // writes into it.
            size_t needed = base + dstSlot + 1;
            vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));

            // Save parent frame; advance IP past the trailing
            // OP_SET_STACK_SLOT (already-handled by resultStoreSlot).
            vm.frames.back().ip = ip + 1;
            vm.frames.back().env = curEnv;
            if (vm.frames.size() > 65536) [[unlikely]]
                state.error<EvalError>("infinite recursion encountered").atPos(pos).debugThrow();

            CallFrame newFrame{};
            newFrame.unit = &bodyUnit;
            newFrame.ip = startOffset;
            newFrame.env = fun->lambda().env;
            stackBase = vm.stackSize();
            newFrame.stackBaseOffset = stackBase;
            // Skip allocValue: when resultStoreSlot is set, OP_RETURN
            // delivers retVal pointer directly to the parent's slot.
            // Saves one Value alloc per fast-path call.
            newFrame.resultSlot = nullptr;
            newFrame.callPos = pos;
            newFrame.upvalues = frameUpvalues;
            newFrame.resultStoreSlot = static_cast<uint32_t>(dstSlot) + 1;
            newFrame.resultStoreParentBase = base;
            vm.frames.push_back(newFrame);

            vm.push(arg);
            cu = &bodyUnit;
            ip = startOffset;
            curEnv = fun->lambda().env;
            DISPATCH();
        }

        // ── Primop fast path: write result directly to dst slot ──
        // For saturated primops, dispatch into the impl directly and
        // store the result into the dst stack slot, then skip past the
        // trailing OP_SET_STACK_SLOT.  Avoids the push/pop dance and
        // op_call_1's full dispatch table.  Continuation-bearing
        // primops (foldl', filter, all/any) are NOT handled here —
        // they require op_call_1's contIdx machinery, so we fall
        // through to the slow path for those.
        if (fun->isPrimOp()) {
            auto * fn = fun->primOp();
            if (fn->arity == 1) {
                arg = materializeWord(state, arg);
                size_t needed = base + dstSlot + 1;
                vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
                auto * result = state.allocValue();
                Value * argPtr = arg;
                fn->impl(state, pos, &argPtr, *result);
                vm.stack[base + dstSlot] = result;
                ip++;  // skip trailing OP_SET_STACK_SLOT
                DISPATCH();
            }
            // Unsaturated 1-of-N primop: build PrimOpApp directly into slot.
            if (fn->arity > 1) {
                arg = materializeWord(state, arg);
                size_t needed = base + dstSlot + 1;
                vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
                auto * funCopy = state.allocValue();
                *funCopy = *fun;
                auto * result = state.allocValue();
                result->mkPrimOpApp(funCopy, arg);
                vm.stack[base + dstSlot] = result;
                ip++;
                DISPATCH();
            }
        }
        if (fun->isPrimOpApp()) {
            size_t argsDone = 0;
            Value * root = fun;
            while (root->isPrimOpApp()) {
                argsDone++;
                root = root->primOpApp().left;
            }
            assert(root->isPrimOp());
            auto * fn = root->primOp();
            auto argsLeft = fn->arity - argsDone;
            // Skip continuation-bearing primops; let op_call_1 handle them.
            bool isContPrimOp = (fn->arity == 2
                && (fn->name == "filter" || fn->name == "all"
                    || fn->name == "any" || fn->name == "map"))
                || (fn->arity == 3 && fn->name == "__foldl'");
            if (argsLeft == 1 && !isContPrimOp) {
                arg = materializeWord(state, arg);
                Value * vArgs[16];
                auto n = argsDone;
                for (Value * v = fun; v->isPrimOpApp(); v = v->primOpApp().left)
                    vArgs[--n] = v->primOpApp().right;
                vArgs[argsDone] = arg;
                size_t needed = base + dstSlot + 1;
                vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
                auto * result = state.allocValue();
                const_cast<PrimOp *>(fn)->impl(state, pos, vArgs, *result);
                vm.stack[base + dstSlot] = result;
                ip++;
                DISPATCH();
            }
            if (argsLeft > 1) {
                arg = materializeWord(state, arg);
                size_t needed = base + dstSlot + 1;
                vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));
                auto * funCopy = state.allocValue();
                *funCopy = *fun;
                auto * result = state.allocValue();
                result->mkPrimOpApp(funCopy, arg);
                vm.stack[base + dstSlot] = result;
                ip++;
                DISPATCH();
            }
        }

        // ── Slow path: delegate to op_call_1 ──
        // Push fun and arg onto the operand stack and let op_call_1
        // perform the full dispatch (primop, primopApp, functor,
        // env-chain lambda).  The trailing OP_SET_STACK_SLOT(dstSlot)
        // emitted by ir-emit will pop the result and store it.
        vm.push(fun);
        vm.push(arg);
        goto op_call_1;
    }

    // OP_RATTR_SELF_R: dst = (*attrs).<attr>, then force.
    // Operand: [dst:8|attrsSlot:8|cacheIdxLow:8].  Limited to cache
    // indices < 256.  For larger indices, fall back to stack form.
#ifdef NIX_VM_COMPUTED_GOTO
op_rattr_self_r:
#else
    case OP_RATTR_SELF_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t attrsSlot = bytecode::unpackA(operand);
        uint8_t cacheIdx = bytecode::unpackB(operand);
        size_t base = stackBase;
        size_t needed = base + dstSlot + 1;
        vm.ensureCapacity(needed, const_cast<Value *>(&Value::vNull));

        AttrCache & cache = cu->attrCaches[cacheIdx];
        Value * attrs = vm.stack[base + attrsSlot];
        PosIdx pos = cu->posForOffset(ip - 1);

        // Force attrs if needed.
        if (!nanbox::isTagged(attrs)
            && (attrs->isThunkOrApp())) {
            state.forceValue(*attrs, pos);
            // Re-read since forceValue may have updated.
            attrs = vm.stack[base + attrsSlot];
        }

        if (nanbox::isTagged(attrs)) {
            attrs = materializeWord(state, attrs);
        }
        if (attrs->type() != nAttrs)
            state.error<EvalError>("expected an attrset").atPos(pos).debugThrow();

        const Bindings * b = attrs->attrs();

        Value * selected = nullptr;
        if (cache.entries[0].bindings == b) [[likely]] {
            vm.nrAttrCacheHits++;
            selected = cache.entries[0].value;
        } else {
            bool hit = false;
            for (int i = 1; i < AttrCache::kEntries; i++) {
                if (cache.entries[i].bindings == b) {
                    vm.nrAttrCacheHits++;
                    Value * v = cache.entries[i].value;
                    cache.entries[i] = cache.entries[0];
                    cache.entries[0] = {b, v};
                    selected = v;
                    hit = true;
                    break;
                }
            }
            if (!hit
                && cache.shape.size != 0
                && !b->isLayered()
                && b->size() == cache.shape.size
                && (*b)[0].name == cache.shape.firstSym
                && cache.shape.offset < cache.shape.size
                && (*b)[cache.shape.offset].name == cache.name) {
                // B6: type-shape fallback hit.
                vm.nrAttrCacheHits++;
                Value * v = (*b)[cache.shape.offset].value;
                uint8_t evict = cache.nextEvict;
                cache.entries[evict] = {b, v};
                cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
                selected = v;
                hit = true;
            }
            if (!hit) {
                vm.nrAttrCacheMisses++;
                if (auto j = b->get(cache.name)) {
                    uint8_t evict = cache.nextEvict;
                    cache.entries[evict] = {b, j->value};
                    cache.nextEvict = (evict + 1) & AttrCache::kEvictMask;
                    if (!b->isLayered()) {
                        cache.shape.firstSym = (*b)[0].name;
                        cache.shape.size = b->size();
                        cache.shape.offset =
                            static_cast<uint32_t>(j - &(*b)[0]);
                    }
                    selected = j->value;
                } else {
                    state.error<EvalError>("attribute '%1%' missing",
                        state.symbols[cache.name]).atPos(pos).debugThrow();
                }
            }
        }

        // Force the selected value if it's a thunk.
        if (!nanbox::isTagged(selected)
            && (selected->isThunkOrApp())) {
            state.forceValue(*selected, pos);
        }

        vm.stack[base + dstSlot] = selected;
        DISPATCH();
    }

    // ==================================================================
    // B4-impl: register-form literal/constant emission
    // ==================================================================

    // OP_RLIT_INT: write small int constant directly to slot.
    // Encoding: [dst:8 | imm:16].  Both are unsigned in the encoding
    // bits; ir-emit only emits this for non-negative imm <= 0xFFFF.
    //
    // Slots must contain real Value* pointers (downstream readers
    // assume that — see OP_GET_STACK_SLOT / register-form opcodes
    // which dereference slot[N] without materializing).  We allocate
    // a fresh Value here rather than nan-box-tag the slot.  Cost: 1
    // allocValue per RLIT_INT.  Still cheaper than the OP_INT +
    // SET_STACK_SLOT pair (which also materializes).
#ifdef NIX_VM_COMPUTED_GOTO
op_rlit_int:
#else
    case OP_RLIT_INT:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t imm     = operand & 0xFFFF;
        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        Value * v = state.allocValue();
        v->mkInt(static_cast<NixInt::Inner>(imm));
        vm.stack[base + dstSlot] = v;
        DISPATCH();
    }

    // OP_RCONST: write a constants-pool entry directly to slot.
    // Encoding: [dst:8 | constIdx:16].
#ifdef NIX_VM_COMPUTED_GOTO
op_rconst:
#else
    case OP_RCONST:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot  = operand >> 16;
        uint32_t constIdx = operand & 0xFFFF;
        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = cu->constants[constIdx];
        DISPATCH();
    }

    // OP_RUPDATE_R: dst = (slot lhs) // (slot rhs).
    // Encoding: [dst:8 | lhs:8 | rhs:8].
#ifdef NIX_VM_COMPUTED_GOTO
op_rupdate_r:
#else
    case OP_RUPDATE_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        Value * lhs = materializeWord(state, vm.stack[base + lhsSlot]);
        Value * rhs = materializeWord(state, vm.stack[base + rhsSlot]);
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceAttrs(*lhs, pos, "in the left operand of the update (//) operator");
        state.forceAttrs(*rhs, pos, "in the right operand of the update (//) operator");
        auto * result = state.allocValue();
        vmAttrsUpdate(state, *result, *lhs, *rhs);
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // OP_RCONCATLIST_R: dst = (slot lhs) ++ (slot rhs).
    // Encoding: [dst:8 | lhs:8 | rhs:8].
#ifdef NIX_VM_COMPUTED_GOTO
op_rconcatlist_r:
#else
    case OP_RCONCATLIST_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint8_t dstSlot = bytecode::unpackDst(operand);
        uint8_t lhsSlot = bytecode::unpackA(operand);
        uint8_t rhsSlot = bytecode::unpackB(operand);
        size_t base = stackBase;
        Value * lhs = materializeWord(state, vm.stack[base + lhsSlot]);
        Value * rhs = materializeWord(state, vm.stack[base + rhsSlot]);
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
            memcpy(out, lView.data(), lSize * sizeof(Value *));
            memcpy(out + lSize, rView.data(), rSize * sizeof(Value *));
            result->mkList(list);
        }
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // OP_RNOT_R: dst = !(slot src).
    // Encoding: [dst:8 | srcSlot:16].
#ifdef NIX_VM_COMPUTED_GOTO
op_rnot_r:
#else
    case OP_RNOT_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t srcSlot = operand & 0xFFFF;
        size_t base = stackBase;
        Value * src = materializeWord(state, vm.stack[base + srcSlot]);
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*src, pos);
        if (src->type() != nBool)
            state.error<TypeError>("expected a Boolean but found %1%: %2%",
                showType(*src), ValuePrinter(state, *src, PrintOptions{}))
                .atPos(pos).debugThrow();
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = src->boolean()
            ? &Value::vFalse : &Value::vTrue;
        DISPATCH();
    }

    // OP_RNEG_R: dst = -(slot src).  Numeric negation, both int and
    // float.  Encoding: [dst:8 | srcSlot:16].
#ifdef NIX_VM_COMPUTED_GOTO
op_rneg_r:
#else
    case OP_RNEG_R:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t srcSlot = operand & 0xFFFF;
        size_t base = stackBase;
        Value * src = materializeWord(state, vm.stack[base + srcSlot]);
        PosIdx pos = cu->posForOffset(ip - 1);
        state.forceValue(*src, pos);
        auto * result = state.allocValue();
        if (src->type() == nInt) {
            auto neg = NixInt(0) - src->integer();
            if (auto val = neg.valueChecked())
                result->mkInt(*val);
            else
                state.error<EvalError>("integer overflow in negation")
                    .atPos(pos).debugThrow();
        } else if (src->type() == nFloat) {
            result->mkFloat(-src->fpoint());
        } else {
            state.error<EvalError>("cannot negate %1%", showType(*src))
                .atPos(pos).debugThrow();
        }
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = result;
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
        // For v2 closures, allocate a single Env(1 + nUpvalues) and store
        // upvalues INLINE in values[1..1+nUpvalues].  values[0] is vNull
        // so OP_GET_WITH (which reads env.values[0]) sees a safe
        // sentinel.  Readers in callFunction / fast-paths set
        // frame.upvalues = &env->values[1] so OP_GET_UPVALUE(idx)'s
        // offsets stay unchanged.
        //
        // For closures with NO upvalues (very common — every nullary
        // helper), allocate a 1-slot Env, which hits the thread-local
        // size-1 fast pool (eval-inline.hh:70-86).
        Env & closureEnv = state.mem.allocEnv(1 + nUpvalues);
        closureEnv.up = curEnv; // Parent env for with-chain walking.
        closureEnv.values[0] = const_cast<Value *>(&Value::vNull);
        if (nUpvalues > 0) {
            // Pop upvalues from the stack into values[1..1+nUpvalues].
            // They were pushed in forward order (upvalue 0 first),
            // so pop in reverse to get the correct mapping.
            // Materialize tagged immediates so the captured slots are
            // proper Value* pointers — OP_GET_UPVALUE / OP_RUVF_TO read
            // these directly without re-materialization.
            for (uint32_t i = nUpvalues; i > 0; --i)
                closureEnv.values[i] = materializeWord(state, vm.pop());
        }

        // Use the pre-allocated ExprLambdaBytecode from compilation.
        // Lazy ExprLambdaBytecode allocation (Phase 3.1 lite): emit no
        // longer pre-allocates, so the first OP_MAKE_CLOSURE_V2 for this
        // descriptor allocates and caches.  Subsequent creations reuse
        // the cached pointer.
        Expr * lambdaExpr = desc.cachedExpr;
        if (!lambdaExpr) [[unlikely]] {
            lambdaExpr = state.mem.exprs.add<ExprLambdaBytecode>(
                const_cast<CompilationUnit *>(cu), lambdaIdx);
            const_cast<bytecode::LambdaDescriptor &>(desc).cachedExpr = lambdaExpr;
        }

        auto * closureVal = state.allocValue();
        closureVal->mkLambda(&closureEnv, static_cast<ExprLambda *>(lambdaExpr));

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

        // Lazy ExprBytecodeThunk allocation (Phase 3.1 lite): emit no
        // longer pre-allocates, so the first OP_MAKE_THUNK_V2 for this
        // descriptor allocates and caches.  Cached pointer is reused on
        // subsequent thunk creations from the same descriptor.  Saves
        // descriptor-count allocations at compile time, deferring them
        // to the first force.
        auto & desc = cu->thunks[thunkIdx];
        Expr * thunkExpr = desc.cachedExpr;
        if (!thunkExpr) [[unlikely]] {
            thunkExpr = state.mem.exprs.add<ExprBytecodeThunk>(
                const_cast<CompilationUnit *>(cu), thunkIdx);
            const_cast<bytecode::ThunkDescriptor &>(desc).cachedExpr = thunkExpr;
        }

        // Allocate Env(1 + nUpvalues) and store upvalues INLINE in
        // values[1..1+nUpvalues] — eliminates the separate GC_MALLOC.
        // For thunks with no upvalues, allocEnv(1) hits the size-1
        // fast pool (Phase 1.2a).
        Env & thunkEnv = state.mem.allocEnv(1 + nUpvalues);
        thunkEnv.up = curEnv;
        thunkEnv.values[0] = &Value::vNull;
        for (uint32_t i = nUpvalues; i > 0; --i)
            thunkEnv.values[i] = vm.pop();

        auto * thunkVal = state.allocValue();
        thunkVal->mkThunk(&thunkEnv, thunkExpr);

        vm.push(thunkVal);
        DISPATCH();
    }

    // ==================================================================
    // B4-impl batch 3: register-form variable-arity ops
    // Same semantics as OP_MAKE_THUNK_V2 / OP_MAKE_CLOSURE_V2 /
    // OP_ATTRS_INIT / OP_LIST_INIT but writing the result directly
    // to a stack slot instead of pushing onto the operand stack.
    // ==================================================================

    // OP_RMAKE_THUNK_V2: dst = makeThunk(thunkIdx, popped upvalues)
    // Encoding: [dst:8 | thunkIdx:16] + data word [nUpvalues:24]
#ifdef NIX_VM_COMPUTED_GOTO
op_rmake_thunk_v2:
#else
    case OP_RMAKE_THUNK_V2:
#endif
    {
        uint32_t operand   = decodeOperand(CUR_INSTR);
        uint32_t dstSlot   = operand >> 16;
        uint32_t thunkIdx  = operand & 0xFFFF;
        uint32_t nUpvalues = decodeOperand(cu->code[ip++]);

        auto & desc = cu->thunks[thunkIdx];
        Expr * thunkExpr = desc.cachedExpr;
        if (!thunkExpr) [[unlikely]] {
            thunkExpr = state.mem.exprs.add<ExprBytecodeThunk>(
                const_cast<CompilationUnit *>(cu), thunkIdx);
            const_cast<bytecode::ThunkDescriptor &>(desc).cachedExpr = thunkExpr;
        }

        Env & thunkEnv = state.mem.allocEnv(1 + nUpvalues);
        thunkEnv.up = curEnv;
        thunkEnv.values[0] = &Value::vNull;
        for (uint32_t i = nUpvalues; i > 0; --i)
            thunkEnv.values[i] = vm.pop();

        auto * thunkVal = state.allocValue();
        thunkVal->mkThunk(&thunkEnv, thunkExpr);

        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = thunkVal;
        DISPATCH();
    }

    // OP_RMAKE_CLOSURE_V2: dst = makeClosure(lambdaIdx, popped upvalues)
    // Encoding: [dst:8 | lambdaIdx:16] + data word [nUpvalues:24]
#ifdef NIX_VM_COMPUTED_GOTO
op_rmake_closure_v2:
#else
    case OP_RMAKE_CLOSURE_V2:
#endif
    {
        uint32_t operand   = decodeOperand(CUR_INSTR);
        uint32_t dstSlot   = operand >> 16;
        uint32_t lambdaIdx = operand & 0xFFFF;
        uint32_t nUpvalues = decodeOperand(cu->code[ip++]);

        auto & desc = cu->lambdas[lambdaIdx];

        if (desc.sourceExpr) {
            state.lambdaBodyCache[desc.sourceExpr] = {
                const_cast<CompilationUnit *>(cu),
                desc.bodyThunkIdx,
                desc.prologueOffset,
            };
        }

        Env & closureEnv = state.mem.allocEnv(1 + nUpvalues);
        closureEnv.up = curEnv;
        closureEnv.values[0] = const_cast<Value *>(&Value::vNull);
        if (nUpvalues > 0) {
            for (uint32_t i = nUpvalues; i > 0; --i)
                closureEnv.values[i] = materializeWord(state, vm.pop());
        }

        Expr * lambdaExpr = desc.cachedExpr;
        if (!lambdaExpr) [[unlikely]] {
            lambdaExpr = state.mem.exprs.add<ExprLambdaBytecode>(
                const_cast<CompilationUnit *>(cu), lambdaIdx);
            const_cast<bytecode::LambdaDescriptor &>(desc).cachedExpr = lambdaExpr;
        }

        auto * closureVal = state.allocValue();
        closureVal->mkLambda(&closureEnv, static_cast<ExprLambda *>(lambdaExpr));

        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = closureVal;
        DISPATCH();
    }

    // OP_RATTRS_INIT: dst = build attrset from popped values + data words
    // Encoding: [dst:8 | nAttrs:16] + nAttrs data words [sym, pos] pairs.
#ifdef NIX_VM_COMPUTED_GOTO
op_rattrs_init:
#else
    case OP_RATTRS_INIT:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t nAttrs  = operand & 0xFFFF;
        Value * result = vmAttrsInit(state, vm, cu, ip, nAttrs);
        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = result;
        DISPATCH();
    }

    // OP_RLIST_INIT: dst = build list from popped values
    // Encoding: [dst:8 | nElems:16].
#ifdef NIX_VM_COMPUTED_GOTO
op_rlist_init:
#else
    case OP_RLIST_INIT:
#endif
    {
        uint32_t operand = decodeOperand(CUR_INSTR);
        uint32_t dstSlot = operand >> 16;
        uint32_t nElems  = operand & 0xFFFF;
        auto list = state.buildList(nElems);
        for (uint32_t i = nElems; i > 0; --i) {
            list[i - 1] = materializeWord(state, vm.pop());
        }
        auto * result = state.allocValue();
        result->mkList(list);
        size_t base = stackBase;
        vm.ensureCapacity(base + dstSlot + 1, const_cast<Value *>(&Value::vNull));
        vm.stack[base + dstSlot] = result;
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
        //
        // Critical: revert any mkBlackhole'd thunks back to a recoverable
        // state via state.handleEvalExceptionForThunk.  Without this, the
        // tree-walker's tryEval and similar mechanisms break — every
        // future force of the same Value would throw "infinite recursion
        // encountered" because the blackhole tag persists.  The tree
        // walker uses RAII Finally to do this; the bytecode VM walks the
        // popped frames here in the catch block.
        while (vm.frames.size() > entryFrameDepth) {
            auto & frame = vm.frames.back();
            if (frame.isThunkForce && frame.resultSlot
                && frame.resultSlot->isBlackhole()
                && frame.origExpr)
            {
                state.handleEvalExceptionForThunk(
                    frame.origEnv, frame.origExpr,
                    *frame.resultSlot, frame.callPos);
            }
            vm.sp = vm.stack + frame.stackBaseOffset;
            vm.frames.pop_back();
        }
        throw;
    }
}

} // namespace nix::bytecode
