/// @file
/// v3 VM dispatch loop (switch-based for now; computed-goto comes later
/// once the opcode set is stable).
///
/// Frame model:
///   - One large valueStack of Values shared across all frames.
///   - Each CallFrame has a stackBaseOffset; frame-local slots are
///     valueStack[stackBaseOffset .. stackBaseOffset + nLocals).
///   - Operand stack scratch grows beyond locals; the next frame is laid
///     out on top of it.
///
/// OP_FORCE walks: if the value is a Suspended thunk, we push a CFF_THUNK_RETURN
/// frame that runs the thunk's bytecode; on OP_RETURN, the result is written
/// into the thunk (state -> Evaluated, evaluated = result), the thunk is
/// dropped from the side-table, and the result is left on the operand stack.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/alloc.hh"
#include "v3/primop.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

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
inline Value & top(VMState & vm) { return vm.valueStack.back(); }

[[gnu::always_inline]]
inline void push(VMState & vm, Value v)
{
    vm.valueStack.push_back(v);
}

inline bool valueEqual(const Value & a, const Value & b)
{
    if (a.tag() != b.tag()) {
        if (a.isInt() && b.isFloat()) return static_cast<double>(a.payload.i) == b.payload.f;
        if (a.isFloat() && b.isInt()) return a.payload.f == static_cast<double>(b.payload.i);
        return false;
    }
    switch (a.tag()) {
    case Tag::Int:    return a.payload.i == b.payload.i;
    case Tag::Float:  return a.payload.f == b.payload.f;
    case Tag::Bool:   return a.payload.i == b.payload.i; // 0 or 1
    case Tag::Null:   return true;
    case Tag::String: return std::string_view(a.payload.str) == std::string_view(b.payload.str);
    case Tag::Path:   return std::string_view(a.payload.path) == std::string_view(b.payload.path);
    case Tag::Uninitialized:
    case Tag::Attrs:
    case Tag::List:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:          return a.payload.raw == b.payload.raw;
    }
}

inline bool valueLess(const Value & a, const Value & b)
{
    if (a.isInt() && b.isInt())     return a.payload.i < b.payload.i;
    if (a.isFloat() && b.isFloat()) return a.payload.f < b.payload.f;
    if (a.isInt() && b.isFloat())   return static_cast<double>(a.payload.i) < b.payload.f;
    if (a.isFloat() && b.isInt())   return a.payload.f < static_cast<double>(b.payload.i);
    if (a.isString() && b.isString())
        return std::string_view(a.payload.str) < std::string_view(b.payload.str);
    throw std::runtime_error("v3 OP_LESS: unsupported operand types");
}

inline bool isTrueValue(const Value & v)
{
    if (!v.isBool()) throw std::runtime_error("v3: expected bool");
    return v.payload.i == 1;
}

/// Coerce a Value to its string representation for OP_STR_CONCAT.
/// Bring-up subset: int / float / bool / string / path / null.  Lists,
/// attrsets, and lambdas trigger an error here for now (the AST → IR pass
/// is responsible for inserting `toString` primop calls where needed).
inline std::string coerceToString(const Value & v, bool forceString)
{
    switch (v.tag()) {
    case Tag::String: return std::string(v.payload.str);
    case Tag::Path:   return std::string(v.payload.path);
    case Tag::Int:    return std::to_string(v.payload.i);
    case Tag::Float:  return std::to_string(v.payload.f);
    case Tag::Bool:   return v.payload.i == 1 ? "1" : "";
    case Tag::Null:   return "";
    case Tag::Uninitialized:
    case Tag::Attrs:
    case Tag::List:
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::External:
    default:
        throw std::runtime_error("v3 STR_CONCAT: cannot coerce value of this type to string");
    }
    (void)forceString;
}

inline Bindings * mergeBindings(const Bindings * a, const Bindings * b)
{
    // Sorted-merge two attrsets (b wins on duplicate keys).
    const uint32_t na = a->size, nb = b->size;
    Bindings * out = Alloc::allocBindings(na + nb);
    uint32_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a->entries[i].name < b->entries[j].name) {
            out->entries[k++] = a->entries[i++];
        } else if (a->entries[i].name > b->entries[j].name) {
            out->entries[k++] = b->entries[j++];
        } else {
            out->entries[k++] = b->entries[j++]; // duplicate; b wins
            i++;
        }
    }
    while (i < na) out->entries[k++] = a->entries[i++];
    while (j < nb) out->entries[k++] = b->entries[j++];
    out->size = k;
    return out;
}

/// Look up `name` in the with-stack, walking from top (innermost) outward.
/// Returns the Value (or throws if not found).  `depth` skips that many
/// innermost entries (currently unused — the static analysis hint isn't
/// trusted yet).
inline Value withLookup(VMState & vm, SymbolId name, uint32_t depth)
{
    if (depth >= vm.withStack.size())
        throw std::runtime_error("v3 OP_WITH_LOOKUP: depth exceeds with-stack size");
    for (size_t i = vm.withStack.size(); i-- > depth; ) {
        const Value & w = vm.withStack[i];
        if (!w.isAttrs()) continue;
        if (auto * v = w.payload.bindings->lookup(name))
            return *v;
    }
    throw std::runtime_error("v3 OP_WITH_LOOKUP: name not found in with-scope");
}

} // namespace

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {

/// Run the dispatch loop on `vm` until either:
///   - OP_HALT is reached (top-level exit), or
///   - The frame stack is popped down to `exitDepth` (used by inner
///     re-entries from callback primops to return to the caller).
/// Returns the final value (whatever was on the operand stack at exit).
Value dispatchLoop(VMState & vm, size_t exitDepth)
{
    const CallFrame & topFrame = vm.frames.back();
    const CompilationUnit * cu = topFrame.cu;
    uint32_t ip = topFrame.ip;
    const Closure * closure = topFrame.closure;
    size_t stackBase = topFrame.stackBaseOffset;

    Value finalResult{};
    finalResult.mkNull();

    bool running = true;
    while (running) {
        Instruction instr = cu->code[ip++];
        vm.nrInstructions++;
        Op op = decodeOp(instr);
        uint32_t operand = decodeOperand(instr);

        switch (op) {

        case OP_NOP: break;

        // --- Literals ---
        case OP_LIT_INT: {
            int32_t imm = decodeSignedOperand(instr);
            Value v; v.mkInt(imm); push(vm, v);
            break;
        }
        case OP_LIT_INT_BIG: { Value v; v.mkInt(cu->intConstants[operand]); push(vm, v); break; }
        case OP_LIT_FLOAT: {
            Value v; v.mkFloat(cu->floatConstants[operand]); push(vm, v); break;
        }
        case OP_LIT_STR: {
            Value v;
            v.mkString(cu->stringConstants[operand].c_str());
            push(vm, v);
            break;
        }
        case OP_LIT_PATH: {
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Path);
            v.payload.path = cu->stringConstants[operand].c_str();
            push(vm, v);
            break;
        }
        case OP_LIT_TRUE:  push(vm, Value::vTrue);  break;
        case OP_LIT_FALSE: push(vm, Value::vFalse); break;
        case OP_LIT_NULL:  push(vm, Value::vNull);  break;

        // --- Locals / upvalues ---
        case OP_GET_LOCAL: {
            if (stackBase + operand >= vm.valueStack.size())
                throw std::runtime_error("v3 OP_GET_LOCAL: slot out of range");
            push(vm, vm.valueStack[stackBase + operand]);
            break;
        }
        case OP_SET_LOCAL: {
            Value v = pop(vm);
            while (stackBase + operand >= vm.valueStack.size())
                vm.valueStack.push_back(Value{});
            vm.valueStack[stackBase + operand] = v;
            break;
        }
        case OP_GET_UPVALUE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE: no closure context");
            if (operand >= closure->nUpvalues)
                throw std::runtime_error("v3 OP_GET_UPVALUE: index out of range");
            push(vm, closure->upvalues[operand]);
            break;
        }
        case OP_DUP:  push(vm, top(vm)); break;
        case OP_POP:  vm.valueStack.pop_back(); break;
        case OP_SWAP: {
            size_t n = vm.valueStack.size();
            std::swap(vm.valueStack[n - 1], vm.valueStack[n - 2]);
            break;
        }

        // --- Arithmetic ---
        case OP_ADD: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt())          r.mkInt(lhs.payload.i + rhs.payload.i);
            else if (lhs.isFloat() && rhs.isFloat()) r.mkFloat(lhs.payload.f + rhs.payload.f);
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) + rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f + static_cast<double>(rhs.payload.i));
            else throw std::runtime_error("v3 OP_ADD: type mismatch");
            push(vm, r);
            break;
        }
        case OP_SUB: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt())          r.mkInt(lhs.payload.i - rhs.payload.i);
            else if (lhs.isFloat() && rhs.isFloat()) r.mkFloat(lhs.payload.f - rhs.payload.f);
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) - rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f - static_cast<double>(rhs.payload.i));
            else throw std::runtime_error("v3 OP_SUB: type mismatch");
            push(vm, r);
            break;
        }
        case OP_MUL: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt())          r.mkInt(lhs.payload.i * rhs.payload.i);
            else if (lhs.isFloat() && rhs.isFloat()) r.mkFloat(lhs.payload.f * rhs.payload.f);
            else throw std::runtime_error("v3 OP_MUL: unsupported types");
            push(vm, r);
            break;
        }
        case OP_DIV: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt()) {
                if (rhs.payload.i == 0) throw std::runtime_error("v3 OP_DIV: division by zero");
                r.mkInt(lhs.payload.i / rhs.payload.i);
            } else if (lhs.isFloat() && rhs.isFloat()) {
                r.mkFloat(lhs.payload.f / rhs.payload.f);
            } else throw std::runtime_error("v3 OP_DIV: unsupported types");
            push(vm, r);
            break;
        }
        case OP_NEGATE: {
            Value v = pop(vm), r;
            if      (v.isInt())   r.mkInt(-v.payload.i);
            else if (v.isFloat()) r.mkFloat(-v.payload.f);
            else throw std::runtime_error("v3 OP_NEGATE: unsupported type");
            push(vm, r);
            break;
        }

        // --- Comparison ---
        case OP_EQ:  { Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break; }
        case OP_NEQ: { Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(a, b) ? Value::vFalse : Value::vTrue; push(vm, r); break; }
        case OP_LESS:{ Value b = pop(vm), a = pop(vm); Value r; r = valueLess(a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break; }

        // --- Boolean / branches ---
        case OP_NOT: { Value v = pop(vm); push(vm, isTrueValue(v) ? Value::vFalse : Value::vTrue); break; }

        case OP_AND_BRANCH: {
            // peek; if false -> jump (keep false); if true -> pop and fall through
            const Value & v = top(vm);
            if (v.isBool() && v.payload.i == 0) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_OR_BRANCH: {
            const Value & v = top(vm);
            if (v.isBool() && v.payload.i == 1) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_IMPL_BRANCH: {
            // If lhs false -> result is true; jump.  If lhs true -> pop, fall through.
            Value v = pop(vm);
            if (v.isBool() && v.payload.i == 0) { push(vm, Value::vTrue); ip = operand; }
            break;
        }

        case OP_JUMP: ip = operand; break;
        case OP_BRANCH_FALSE: { Value v = pop(vm); if (v.isBool() && v.payload.i == 0) ip = operand; break; }
        case OP_BRANCH_TRUE:  { Value v = pop(vm); if (v.isBool() && v.payload.i == 1) ip = operand; break; }

        // --- Closure / call / thunk ---
        case OP_MAKE_CLOSURE: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            Closure * c = Alloc::allocClosure(nUp);
            allocStats().closuresAllocated++;
            c->desc = &cu->lambdas[funcIdx];
            c->nUpvalues = nUp;
            for (uint16_t i = nUp; i > 0; --i) c->upvalues[i - 1] = pop(vm);
            Value v; v.mkClosure(c); push(vm, v);
            break;
        }
        case OP_MAKE_THUNK: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            Thunk * t = Alloc::allocThunkSuspended(nUp);
            allocStats().thunksAllocated++;
            // The "descriptor" we use is the LambdaDescriptor for the
            // referenced function (treated as 0-arg for thunks).
            // Reuse the LambdaDescriptor pointer through suspended.desc.
            t->suspended.desc = reinterpret_cast<const ThunkDescriptor *>(&cu->lambdas[funcIdx]);
            t->suspended.withEnv = nullptr;
            for (uint16_t i = nUp; i > 0; --i) t->tail[i - 1] = pop(vm);
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Thunk);
            v.payload.thunk = t;
            push(vm, v);
            break;
        }
        case OP_CALL: {
            Value arg = pop(vm), fun = pop(vm);

            // PrimOp / PrimOpApp partial application.
            if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
                // Walk the PrimOpApp chain to find the root PrimOp and
                // collect the previously-applied args.
                Value cur = fun;
                size_t depth = 0;
                while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.payload.pair->left; }
                if (!cur.isPrimOp())
                    throw std::runtime_error("v3 OP_CALL: PrimOpApp chain doesn't terminate in a PrimOp");
                const PrimOp * po = cur.payload.primop;
                size_t totalArgs = depth + 1;
                if (totalArgs < po->arity) {
                    // Build a new PrimOpApp wrapping (fun, arg).
                    ValuePair * vp = static_cast<ValuePair *>(std::malloc(sizeof(ValuePair)));
                    vp->left = fun;
                    vp->right = arg;
                    Value v;
                    v.tag_payload = static_cast<uint64_t>(Tag::PrimOpApp);
                    v.payload.pair = vp;
                    push(vm, v);
                    break;
                }
                if (totalArgs > po->arity)
                    throw std::runtime_error("v3 OP_CALL: too many args for primop");
                // Collect args in [arg_0, arg_1, ..., arg_{N-1}, arg] order.
                Value buf[8];
                if (po->arity > 8) throw std::runtime_error("v3 OP_CALL: primop arity > 8");
                buf[totalArgs - 1] = arg;
                Value chain = fun;
                for (size_t i = totalArgs - 1; i > 0; --i) {
                    buf[i - 1] = chain.payload.pair->right;
                    chain = chain.payload.pair->left;
                }
                vm.frames.back().ip = ip;
                EvalState state; state.vm = &vm;
                Value out;
                po->fn(state, buf, out);
                push(vm, out);
                break;
            }

            if (!fun.isClosure())
                throw std::runtime_error("v3 OP_CALL: callee is not a closure");
            const Closure * callee = fun.payload.closure;
            const LambdaDescriptor * desc = callee->desc;

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
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
                .thunk = nullptr,
            });

            ip = desc->codeOffset;
            closure = callee;
            stackBase = newBase;
            break;
        }
        case OP_RETURN: {
            Value retVal = pop(vm);
            // Pop callee's locals.
            CallFrame fr = vm.frames.back();
            vm.valueStack.resize(fr.stackBaseOffset);
            vm.frames.pop_back();
            // If this was a thunk-return frame, write the result back into the thunk.
            if (fr.flags & CFF_THUNK_RETURN) {
                fr.thunk->state = ThunkState::Evaluated;
                fr.thunk->evaluated = retVal;
            }
            // Inner-loop exit: when called from a primop callback, exit
            // back to the C++ caller with the return value.
            if (vm.frames.size() == exitDepth) {
                finalResult = retVal;
                running = false;
                break;
            }
            const auto & caller = vm.frames.back();
            cu = caller.cu;
            ip = caller.ip;
            closure = caller.closure;
            stackBase = caller.stackBaseOffset;
            push(vm, retVal);
            break;
        }
        case OP_FORCE: {
            Value v = pop(vm);
            if (!v.isThunk()) { push(vm, v); break; }
            Thunk * t = v.payload.thunk;
            if (t->state == ThunkState::Evaluated) { push(vm, t->evaluated); break; }
            if (t->state == ThunkState::Blackhole) throw std::runtime_error("v3 OP_FORCE: infinite recursion (blackhole)");
            // Suspended: blackhole and run.
            // We treat suspended.desc as a LambdaDescriptor* (see OP_MAKE_THUNK).
            const LambdaDescriptor * desc = reinterpret_cast<const LambdaDescriptor *>(t->suspended.desc);
            // Synthesize a closure-like view for OP_GET_UPVALUE: we set
            // `closure` to a fake Closure pointer crafted from the thunk
            // tail.  Instead of allocating a temporary Closure, we build
            // one on the heap (cheap; thunk forcing is uncommon enough).
            Closure * fakeClo = Alloc::allocClosure(t->nUpvalues);
            fakeClo->desc = desc;
            fakeClo->nUpvalues = t->nUpvalues;
            for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];

            t->state = ThunkState::Blackhole;

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);

            vm.frames.push_back(CallFrame{
                .cu = cu,
                .ip = desc->codeOffset,
                .resultSlot = 0,
                .flags = CFF_THUNK_RETURN,
                ._pad0 = 0,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .closure = fakeClo,
                .resultPtr = nullptr,
                .thunk = t,
            });

            ip = desc->codeOffset;
            closure = fakeClo;
            stackBase = newBase;
            break;
        }

        // --- Lists ---
        case OP_LIST_INIT: {
            uint32_t n = operand;
            ListVec * l = Alloc::allocList(n);
            allocStats().listsAllocated++;
            for (uint32_t i = n; i > 0; --i) l->elems[i - 1] = pop(vm);
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::List);
            v.payload.list = l;
            push(vm, v);
            break;
        }
        case OP_LIST_CONCAT: {
            Value rhs = pop(vm), lhs = pop(vm);
            if (!lhs.isList() || !rhs.isList())
                throw std::runtime_error("v3 OP_LIST_CONCAT: not lists");
            uint32_t n = lhs.payload.list->size + rhs.payload.list->size;
            ListVec * out = Alloc::allocList(n);
            allocStats().listsAllocated++;
            uint32_t k = 0;
            for (uint32_t i = 0; i < lhs.payload.list->size; ++i) out->elems[k++] = lhs.payload.list->elems[i];
            for (uint32_t i = 0; i < rhs.payload.list->size; ++i) out->elems[k++] = rhs.payload.list->elems[i];
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::List);
            v.payload.list = out;
            push(vm, v);
            break;
        }

        // --- Attrsets ---
        case OP_ATTRS_INIT: {
            uint32_t n = operand;
            // Read n SymbolIds inline (each is a 32-bit code word).
            std::vector<SymbolId> names(n);
            for (uint32_t i = 0; i < n; ++i) names[i] = static_cast<SymbolId>(cu->code[ip + i]);
            ip += n;
            // Pop n values (in reverse order).
            std::vector<Value> values(n);
            for (uint32_t i = n; i > 0; --i) values[i - 1] = pop(vm);
            // Build sorted entries.
            std::vector<std::pair<SymbolId, Value>> entries(n);
            for (uint32_t i = 0; i < n; ++i) entries[i] = {names[i], values[i]};
            std::sort(entries.begin(), entries.end(),
                      [](auto & a, auto & b) { return a.first < b.first; });
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                b->entries[i].name = entries[i].first;
                b->entries[i].value = entries[i].second;
            }
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            push(vm, v);
            break;
        }
        case OP_ATTRS_INIT_DYN: {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    = operand & 0xFFFu;
            // Stack layout (bottom-up): [static values...][dyn name+value pairs...]
            uint32_t totalDynVals = nDyn * 2;
            std::vector<Value> dynPairs(totalDynVals);
            for (uint32_t i = totalDynVals; i > 0; --i) dynPairs[i - 1] = pop(vm);
            std::vector<Value> staticVals(nStatic);
            for (uint32_t i = nStatic; i > 0; --i) staticVals[i - 1] = pop(vm);
            // Static SymbolIds inline.
            std::vector<SymbolId> staticNames(nStatic);
            for (uint32_t i = 0; i < nStatic; ++i)
                staticNames[i] = static_cast<SymbolId>(cu->code[ip + i]);
            ip += nStatic;

            std::vector<std::pair<SymbolId, Value>> entries;
            entries.reserve(nStatic + nDyn);
            for (uint32_t i = 0; i < nStatic; ++i)
                entries.emplace_back(staticNames[i], staticVals[i]);
            for (uint32_t i = 0; i < nDyn; ++i) {
                Value & nameV = dynPairs[i * 2];
                Value & valV  = dynPairs[i * 2 + 1];
                if (!nameV.isString())
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: dynamic name must be a string");
                // For now: do an O(n) intern by checking the symbolTable.
                // The CompilationUnit owns the symbolTable; we look up or
                // append.  This is correct but slow; an interner side-table
                // can speed it up.
                std::string nm(nameV.payload.str);
                SymbolId id = kInvalidSymbol;
                for (size_t s = 0; s < cu->symbolTable.size(); ++s)
                    if (cu->symbolTable[s] == nm) { id = static_cast<SymbolId>(s); break; }
                if (id == kInvalidSymbol) {
                    // Add to a thread-local extension table?  For bring-up,
                    // mutate cu's symbolTable.  cu is const here; cast away.
                    auto & st = const_cast<std::vector<std::string> &>(cu->symbolTable);
                    id = static_cast<SymbolId>(st.size());
                    st.push_back(nm);
                }
                entries.emplace_back(id, valV);
            }
            std::sort(entries.begin(), entries.end(),
                      [](auto & a, auto & b) { return a.first < b.first; });
            Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
            allocStats().attrsetsAllocated++;
            for (size_t i = 0; i < entries.size(); ++i) {
                b->entries[i].name = entries[i].first;
                b->entries[i].value = entries[i].second;
            }
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            push(vm, v);
            break;
        }
        case OP_ATTRS_REC_INIT: {
            // Allocate a Bindings(n) with placeholder values; values
            // are written later by OP_ATTRS_REC_SET[slot].  Names come
            // pre-sorted from emit (LetRec emit sorts entries by
            // SymbolId before writing the data words and rewrites the
            // REC_SET operand to the sorted slot).
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                b->entries[i].name = static_cast<SymbolId>(cu->code[ip + i]);
                b->entries[i].value.mkNull();
            }
            ip += n;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            push(vm, v);
            break;
        }
        case OP_ATTRS_SELECT: {
            Value attrs = pop(vm);
            if (!attrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_SELECT: not an attrset");
            const Value * found = attrs.payload.bindings->lookup(operand);
            if (!found)
                throw std::runtime_error("v3 OP_ATTRS_SELECT: attribute not found");
            push(vm, *found);
            break;
        }
        case OP_ATTRS_SELECT_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            if (!name.isString() || !attrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: type error");
            std::string_view nm(name.payload.str);
            // Look up the symbol id; if not present, attribute is absent.
            SymbolId id = kInvalidSymbol;
            for (size_t s = 0; s < cu->symbolTable.size(); ++s)
                if (cu->symbolTable[s] == nm) { id = static_cast<SymbolId>(s); break; }
            if (id == kInvalidSymbol)
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: attribute not found");
            const Value * found = attrs.payload.bindings->lookup(id);
            if (!found)
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: attribute not found");
            push(vm, *found);
            break;
        }
        case OP_ATTRS_HAS: {
            Value attrs = pop(vm);
            push(vm, (attrs.isAttrs() && attrs.payload.bindings->has(operand))
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_HAS_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            if (!name.isString() || !attrs.isAttrs()) { push(vm, Value::vFalse); break; }
            std::string_view nm(name.payload.str);
            SymbolId id = kInvalidSymbol;
            for (size_t s = 0; s < cu->symbolTable.size(); ++s)
                if (cu->symbolTable[s] == nm) { id = static_cast<SymbolId>(s); break; }
            push(vm, (id != kInvalidSymbol && attrs.payload.bindings->has(id))
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_UPDATE: {
            Value rhs = pop(vm), lhs = pop(vm);
            if (!lhs.isAttrs() || !rhs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_UPDATE: not attrsets");
            Bindings * out = mergeBindings(lhs.payload.bindings, rhs.payload.bindings);
            allocStats().attrsetsAllocated++;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = out;
            push(vm, v);
            break;
        }

        // --- With ---
        case OP_WITH_PUSH: vm.withStack.push_back(pop(vm)); break;
        case OP_WITH_POP:  vm.withStack.pop_back(); break;
        case OP_WITH_LOOKUP: {
            uint32_t depth = cu->code[ip++];
            push(vm, withLookup(vm, static_cast<SymbolId>(operand), depth));
            break;
        }

        // --- Strings / pos / assert ---
        case OP_STR_CONCAT: {
            uint32_t n = operand >> 1;
            bool forceStr = (operand & 1u) != 0;
            std::vector<Value> parts(n);
            for (uint32_t i = n; i > 0; --i) parts[i - 1] = pop(vm);

            // nix `+` semantics: if forceString=false and the first operand
            // is numeric (Int/Float), perform arithmetic addition; otherwise
            // do string concatenation.  forceString=true (e.g. "${foo}")
            // always coerces to string.
            if (!forceStr && n > 0 && (parts[0].isInt() || parts[0].isFloat())) {
                bool allInt = true;
                for (auto & p : parts) if (!p.isInt()) { allInt = false; break; }
                Value r;
                if (allInt) {
                    int64_t sum = 0;
                    for (auto & p : parts) sum += p.payload.i;
                    r.mkInt(sum);
                } else {
                    double sum = 0.0;
                    for (auto & p : parts) {
                        if (p.isInt())   sum += static_cast<double>(p.payload.i);
                        else if (p.isFloat()) sum += p.payload.f;
                        else throw std::runtime_error("v3 OP_STR_CONCAT: mixed numeric and non-numeric");
                    }
                    r.mkFloat(sum);
                }
                push(vm, r);
                break;
            }

            std::string out;
            for (auto & p : parts) out.append(coerceToString(p, forceStr));
            char * buf = static_cast<char *>(std::malloc(out.size() + 1));
            std::memcpy(buf, out.data(), out.size());
            buf[out.size()] = '\0';
            Value v; v.mkString(buf); push(vm, v);
            break;
        }
        case OP_ASSERT: {
            Value c = pop(vm);
            if (!isTrueValue(c)) throw std::runtime_error("v3 OP_ASSERT: assertion failed");
            break;
        }
        case OP_POS: {
            // Stub: emit an empty attrset.
            Bindings * b = Alloc::allocBindings(0);
            Value v; v.tag_payload = static_cast<uint64_t>(Tag::Attrs); v.payload.bindings = b;
            push(vm, v);
            break;
        }

        case OP_LIT_PRIMOP: {
            const PrimOp * po = cu->primops[operand];
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::PrimOp);
            v.payload.primop = po;
            push(vm, v);
            break;
        }

        case OP_CALL_PRIMOP: {
            uint32_t nArgs = operand;
            uint32_t poIdx = cu->code[ip++];
            const PrimOp * po = cu->primops[poIdx];
            Value args[8];
            if (nArgs > 8) throw std::runtime_error("v3 OP_CALL_PRIMOP: arity > 8 not supported");
            for (uint32_t i = nArgs; i > 0; --i) args[i - 1] = pop(vm);
            // Save current frame state in case the primop calls back
            // into the VM via callClosure().
            vm.frames.back().ip = ip;
            // Wire the EvalState to this VM so callback primops can
            // re-enter the dispatcher.
            EvalState state;
            state.vm = &vm;
            Value out;
            po->fn(state, args, out);
            push(vm, out);
            break;
        }

        case OP_ATTRS_REC_SET: {
            uint32_t i = operand;
            Value v = pop(vm);
            // Peek at the rec bindings (top of stack now) and write into entry i.
            Value & recAttrs = top(vm);
            if (!recAttrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: top is not an attrset");
            if (!recAttrs.payload.bindings || i >= recAttrs.payload.bindings->size)
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: index out of range");
            recAttrs.payload.bindings->entries[i].value = v;
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

Value run(const CompilationUnit & rootCu)
{
    VMState vm;
    vm.valueStack.reserve(1024);
    vm.frames.reserve(64);
    vm.withStack.reserve(16);

    vm.frames.push_back(CallFrame{
        .cu = &rootCu,
        .ip = rootCu.entryOffset,
        .resultSlot = 0,
        .flags = 0,
        ._pad0 = 0,
        .stackBaseOffset = 0,
        .closure = nullptr,
        .resultPtr = nullptr,
        .thunk = nullptr,
    });

    if (!rootCu.lambdas.empty())
        vm.valueStack.resize(rootCu.lambdas[0].nLocals);

    return dispatchLoop(vm, /*exitDepth=*/0);
}

Value forceValue(VMState & vm, Value v)
{
    if (!v.isThunk()) return v;
    Thunk * t = v.payload.thunk;
    if (t->state == ThunkState::Evaluated) return t->evaluated;
    if (t->state == ThunkState::Blackhole)
        throw std::runtime_error("v3 forceValue: infinite recursion (blackhole)");

    const LambdaDescriptor * desc = reinterpret_cast<const LambdaDescriptor *>(t->suspended.desc);
    Closure * fakeClo = Alloc::allocClosure(t->nUpvalues);
    fakeClo->desc = desc;
    fakeClo->nUpvalues = t->nUpvalues;
    for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];
    t->state = ThunkState::Blackhole;

    size_t exitDepth = vm.frames.size();
    size_t newBase = vm.valueStack.size();
    vm.valueStack.resize(newBase + desc->nLocals);

    vm.frames.push_back(CallFrame{
        .cu = vm.frames.back().cu,
        .ip = desc->codeOffset,
        .resultSlot = 0,
        .flags = CFF_THUNK_RETURN,
        ._pad0 = 0,
        .stackBaseOffset = static_cast<uint32_t>(newBase),
        .closure = fakeClo,
        .resultPtr = nullptr,
        .thunk = t,
    });

    return dispatchLoop(vm, exitDepth);
}

Value callClosure(VMState & vm, Value fun, Value arg)
{
    // Single-arg primop fast path (no VM re-entry).
    if (fun.isPrimOp()) {
        const PrimOp * po = fun.payload.primop;
        if (po->arity == 1) {
            Value buf[1] = {arg};
            EvalState state; state.vm = &vm;
            Value out;
            po->fn(state, buf, out);
            return out;
        }
        throw std::runtime_error("v3 callClosure: multi-arg primop callbacks not supported yet");
    }
    if (!fun.isClosure())
        throw std::runtime_error("v3 callClosure: not callable");

    const Closure * callee = fun.payload.closure;
    const LambdaDescriptor * desc = callee->desc;
    const CompilationUnit * cu = vm.frames.back().cu;

    // Push a CALL frame for the callee — mirrors OP_CALL.
    size_t exitDepth = vm.frames.size();
    size_t newBase = vm.valueStack.size();
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
        .thunk = nullptr,
    });

    return dispatchLoop(vm, exitDepth);
}

} // namespace nix::v3
