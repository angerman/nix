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
#include "v3/ir.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

/// Equality with WHNF forcing — handles lazy list/attr entries.
/// Recurses on List / Attrs after forcing each element.
///
/// `insideContainer` is true when called recursively from list/attr
/// comparison: in that case Nix's "value identity optimization" allows
/// two closures to compare equal if they share the same underlying
/// Closure pointer (matches tree-walker's `if (&v1 == &v2) return true`
/// short-circuit when sibling list/attr entries point to the same
/// in-memory Value).  Top-level `f == f` always returns false because
/// the OP_EQ stack-pop holds two distinct Value structs even when their
/// payload pointer is identical.
inline bool valueEqual(VMState & vm, Value a, Value b, bool insideContainer = false)
{
    a = forceValue(vm, a);
    b = forceValue(vm, b);
    if (a.tag() != b.tag()) {
        if (a.isInt() && b.isFloat()) return static_cast<double>(a.payload.i) == b.payload.f;
        if (a.isFloat() && b.isInt()) return a.payload.f == static_cast<double>(b.payload.i);
        return false;
    }
    switch (a.tag()) {
    case Tag::Int:    return a.payload.i == b.payload.i;
    case Tag::Float:  return a.payload.f == b.payload.f;
    case Tag::Bool:   return a.payload.i == b.payload.i;
    case Tag::Null:   return true;
    case Tag::String: return std::string_view(a.payload.str) == std::string_view(b.payload.str);
    case Tag::Path:   return std::string_view(a.payload.path) == std::string_view(b.payload.path);
    case Tag::List: {
        auto * la = a.payload.list;
        auto * lb = b.payload.list;
        if (la == lb) return true;
        uint32_t na = la ? la->size : 0;
        uint32_t nb = lb ? lb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i)
            if (!valueEqual(vm, la->elems[i], lb->elems[i], /*insideContainer=*/true)) return false;
        return true;
    }
    case Tag::Attrs: {
        auto * aa = a.payload.bindings;
        auto * bb = b.payload.bindings;
        if (aa == bb) return true;
        // Special-case derivations: if both attrsets are derivations
        // (have `type = "derivation"`), compare their `outPath` fields
        // and ignore the rest.  Matches tree-walker semantics — required
        // by `eval-okay-eq-derivations` (where `drv // { dummy = 1; }`
        // still compares equal to the bare `drv`).
        static const SymbolId tyId = ir::globalInternSymbol("type");
        static const SymbolId opId = ir::globalInternSymbol("outPath");
        auto isDrv = [&](const Bindings * b) {
            if (!b) return false;
            const Value * t = b->lookup(tyId);
            if (!t) return false;
            Value tf = forceValue(vm, *t);
            return tf.isString() && std::string_view(tf.payload.str) == "derivation";
        };
        if (isDrv(aa) && isDrv(bb)) {
            const Value * pa = aa->lookup(opId);
            const Value * pb = bb->lookup(opId);
            if (pa && pb) return valueEqual(vm, *pa, *pb, /*insideContainer=*/true);
        }
        uint32_t na = aa ? aa->size : 0;
        uint32_t nb = bb ? bb->size : 0;
        if (na != nb) return false;
        for (uint32_t i = 0; i < na; ++i) {
            if (aa->entries[i].name != bb->entries[i].name) return false;
            if (!valueEqual(vm, aa->entries[i].value, bb->entries[i].value, /*insideContainer=*/true)) return false;
        }
        return true;
    }
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
        // Direct comparison: never equal.  Inside a container: equal iff
        // the underlying pointer matches (matches Nix's value-identity
        // optimization for sibling list/attrset entries).
        if (!insideContainer) return false;
        return a.payload.closure == b.payload.closure;
    case Tag::Uninitialized:
    case Tag::Thunk:
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
    if (a.isList() && b.isList()) {
        // Lexicographic compare; matches tree-walker.
        uint32_t na = a.payload.list ? a.payload.list->size : 0;
        uint32_t nb = b.payload.list ? b.payload.list->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            const Value & ai = a.payload.list->elems[i];
            const Value & bi = b.payload.list->elems[i];
            if (valueLess(ai, bi)) return true;
            if (valueLess(bi, ai)) return false;
        }
        return na < nb;
    }
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
/// Bounded below by the current frame's `withStackBase`: a closure must
/// not see its caller's `with` scopes.  `depth` is currently unused.
///
/// Each with-stack entry is forced lazily on first access — this is the
/// "delayed-with" rule.  `with pkgs; ...` inside a recursive group that
/// also defines pkgs would blackhole if we forced eagerly at
/// OP_WITH_PUSH; instead we keep the thunk on the stack and only force
/// when an unbound name actually triggers a lookup.  The forced value
/// is written back so subsequent lookups skip the force.
inline Value withLookup(VMState & vm, SymbolId name, uint32_t /*depth*/)
{
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    for (size_t i = vm.withStack.size(); i-- > base; ) {
        Value & w = vm.withStack[i];
        if (w.isThunk() || w.tag() == Tag::App) {
            try {
                w = forceValue(vm, w);
            } catch (const std::exception & ex) {
                // Blackhole here is the delayed-with corner case: the
                // with-stack entry references something that's still
                // being forced from a deeper frame.  Skip it so outer
                // scopes still get a chance to define `name`.  Real
                // errors propagate as usual.
                std::string what(ex.what());
                if (what.find("blackhole") != std::string::npos)
                    continue;
                throw;
            }
        }
        if (!w.isAttrs()) continue;
        if (auto * v = w.payload.bindings->lookup(name))
            return *v;
    }
    throw std::runtime_error("v3 OP_WITH_LOOKUP: name not found in with-scope");
}

/// Snapshot the current frame's visible with-stack (entries from
/// `withStackBase` to top) into a fresh ListVec.  Returns nullptr when
/// no withs are currently in scope (cheap fast-path for the common case
/// of no enclosing `with`).
inline ListVec * snapshotCurrentWiths(VMState & vm)
{
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    size_t top  = vm.withStack.size();
    if (top <= base) return nullptr;
    uint32_t n = static_cast<uint32_t>(top - base);
    ListVec * out = Alloc::allocList(n);
    for (uint32_t i = 0; i < n; ++i)
        out->elems[i] = vm.withStack[base + i];
    return out;
}

/// Push a closure/thunk's captured with-stack onto vm.withStack so it
/// becomes visible to the body's OP_WITH_LOOKUPs.  The caller must have
/// already set the new frame's withStackBase to vm.withStack.size()
/// BEFORE calling this so the floor is correct.
inline void pushCapturedWiths(VMState & vm, ListVec * capturedWiths)
{
    if (!capturedWiths) return;
    for (uint32_t i = 0; i < capturedWiths->size; ++i)
        vm.withStack.push_back(capturedWiths->elems[i]);
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
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.payload.i) * rhs.payload.f);
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.payload.f * static_cast<double>(rhs.payload.i));
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
            } else if (lhs.isInt() && rhs.isFloat()) {
                r.mkFloat(static_cast<double>(lhs.payload.i) / rhs.payload.f);
            } else if (lhs.isFloat() && rhs.isInt()) {
                r.mkFloat(lhs.payload.f / static_cast<double>(rhs.payload.i));
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
        case OP_EQ:  { Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break; }
        case OP_NEQ: { Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vFalse : Value::vTrue; push(vm, r); break; }
        case OP_LESS:{ Value b = pop(vm), a = pop(vm); Value r; r = valueLess(a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break; }

        // --- Boolean / branches ---
        // All boolean opcodes force their operand: a function arg may be
        // a thunk whose evaluated value is the bool we need to branch on.
        // Without a force, `arg || y` would peek the thunk, fail the
        // isBool check, and incorrectly fall through into the rhs block.
        case OP_NOT: {
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            push(vm, isTrueValue(v) ? Value::vFalse : Value::vTrue);
            break;
        }

        case OP_AND_BRANCH: {
            // peek; if false -> jump (keep false); if true -> pop and fall through
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_OR_BRANCH: {
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 1) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_IMPL_BRANCH: {
            // If lhs false -> result is true; jump.  If lhs true -> pop, fall through.
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) { push(vm, Value::vTrue); ip = operand; }
            break;
        }

        case OP_JUMP: ip = operand; break;
        case OP_BRANCH_FALSE: {
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 0) ip = operand;
            break;
        }
        case OP_BRANCH_TRUE:  {
            Value v = pop(vm);
            if (v.isThunk() || v.tag() == Tag::App) v = forceValue(vm, v);
            if (v.isBool() && v.payload.i == 1) ip = operand;
            break;
        }

        // --- Closure / call / thunk ---
        case OP_MAKE_CLOSURE: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            Closure * c = Alloc::allocClosure(nUp);
            allocStats().closuresAllocated++;
            c->desc = &cu->lambdas[funcIdx];
            c->cu   = cu;
            c->nUpvalues = nUp;
            c->capturedWiths = snapshotCurrentWiths(vm);
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
            t->suspended.capturedWiths = snapshotCurrentWiths(vm);
            t->suspended.cu = cu;
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
                // PrimOpApp accumulates args lazily — primops expect
                // WHNF, so force each here before invoking.  Mirrors
                // the eager-force lowerCall does for the
                // direct-primop-call path.
                for (uint32_t i = 0; i < po->arity; ++i)
                    buf[i] = forceValue(vm, buf[i]);
                EvalState state; state.vm = &vm;
                Value out;
                po->fn(state, buf, out);
                push(vm, out);
                break;
            }

            // __functor: applying an attrset that has a `__functor`
            // attribute calls `__functor self arg` per the standard
            // Nix protocol.  Push (functor, attrset, arg) and re-enter
            // OP_CALL twice to match the curried call sequence.
            if (fun.isAttrs()) {
                static const SymbolId functorId = ir::globalInternSymbol("__functor");
                if (!fun.payload.bindings)
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                const Value * fn = fun.payload.bindings->lookup(functorId);
                if (!fn)
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                Value forced = forceValue(vm, *fn);
                // First apply functor to self (= the attrset).
                Value firstStep = callClosure(vm, forced, fun);
                // Then apply that result to the original arg.
                Value out = callClosure(vm, firstStep, arg);
                push(vm, out);
                break;
            }

            if (!fun.isClosure())
                throw std::runtime_error("v3 OP_CALL: callee is not a closure");
            const Closure * callee = fun.payload.closure;
            const LambdaDescriptor * desc = callee->desc;
            // Closures from imported files own their own CompilationUnit;
            // when callee->cu differs, switch the dispatch loop to the
            // callee's bytecode/constant pools.  Falls back to the caller's
            // cu when the closure was made before cu-tracking landed.
            const CompilationUnit * calleeCu = callee->cu ? callee->cu : cu;

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            vm.valueStack[newBase + 0] = arg;

            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            // Direct field-by-field setup avoids constructing a temp
            // CallFrame and then move-copying it into the vector.
            vm.frames.emplace_back();
            CallFrame & nf = vm.frames.back();
            nf.cu              = calleeCu;
            nf.closure         = callee;
            nf.thunk           = nullptr;
            nf.ip              = desc->codeOffset;
            nf.stackBaseOffset = static_cast<uint32_t>(newBase);
            nf.withStackBase   = newWithBase;
            nf.flags           = 0;
            pushCapturedWiths(vm, callee->capturedWiths);

            ip = desc->codeOffset;
            cu  = calleeCu;
            closure = callee;
            stackBase = newBase;
            break;
        }
        case OP_RETURN: {
            Value retVal = pop(vm);
            // Capture only the fields we need across the pop_back —
            // copying the whole CallFrame is the per-recursion-call
            // hot path on fib/ack benchmarks.
            const CallFrame & frRef = vm.frames.back();
            const uint32_t fStackBase    = frRef.stackBaseOffset;
            const uint32_t fWithBase     = frRef.withStackBase;
            const uint8_t  fFlags        = frRef.flags;
            Thunk *        fThunk        = frRef.thunk;
            vm.valueStack.resize(fStackBase);
            vm.withStack.resize(fWithBase);
            vm.frames.pop_back();
            CallFrame fr;  // referenced by name later — only thunk + flags matter.
            fr.flags = fFlags;
            fr.thunk = fThunk;
            if (fFlags & CFF_THUNK_RETURN) {
                // Chase Evaluated chains so the thunk caches the
                // ultimate WHNF and not an intermediate thunk.
                while (retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Evaluated)
                    retVal = retVal.payload.thunk->evaluated;
                fr.thunk->state = ThunkState::Evaluated;
                fr.thunk->evaluated = retVal;

                // If the body returned a Suspended thunk (e.g., the
                // common pattern where an `inherit` binding's body is
                // just AttrSelect on the parent's rec attrs), force
                // that next by setting up another thunk-return frame.
                // This implements transitive force for the OP_FORCE
                // bytecode op without C++ recursion.
                if (retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Suspended) {
                    Thunk * next = retVal.payload.thunk;
                    const LambdaDescriptor * desc =
                        reinterpret_cast<const LambdaDescriptor *>(next->suspended.desc);
                    Closure * fakeClo = Alloc::allocClosure(next->nUpvalues);
                    fakeClo->desc = desc;
                    fakeClo->nUpvalues = next->nUpvalues;
                    fakeClo->capturedWiths = next->suspended.capturedWiths;
                    fakeClo->cu = next->suspended.cu;
                    for (uint16_t i = 0; i < next->nUpvalues; ++i)
                        fakeClo->upvalues[i] = next->tail[i];
                    next->state = ThunkState::Blackhole;
                    const CompilationUnit * thunkCu = next->suspended.cu ? next->suspended.cu : cu;

                    size_t newBase = vm.valueStack.size();
                    vm.valueStack.resize(newBase + desc->nLocals);
                    uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
                    vm.frames.push_back(CallFrame{
                        .cu = thunkCu,
                        .closure = fakeClo,
                        .thunk = next,
                        .ip = desc->codeOffset,
                        .stackBaseOffset = static_cast<uint32_t>(newBase),
                        .withStackBase = newWithBase,
                        .flags = CFF_THUNK_RETURN,
                    });
                    pushCapturedWiths(vm, next->suspended.capturedWiths);
                    ip = desc->codeOffset;
                    cu = thunkCu;
                    closure = fakeClo;
                    stackBase = newBase;
                    // Also write the deeper resolution back to the
                    // outer thunk we just popped: when the next thunk
                    // resolves, it'll re-loop and update the original.
                    // Actually we already set fr.thunk->evaluated to
                    // retVal (which is the next thunk).  When next
                    // resolves and the chase loop runs above, it'll
                    // walk back through fr.thunk.  But fr.thunk is no
                    // longer in `next->...` — so we need a chain of
                    // references.  Simpler: leave fr.thunk pointing at
                    // retVal; when next becomes Evaluated, the next
                    // chase step (in any subsequent OP_RETURN's loop or
                    // forceValue helper) follows from fr.thunk → next →
                    // its eval.
                    break;
                }
            }
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
            // Chase Evaluated chains and resolve Tag::App deferred
            // calls (used by mapAttrs et al. for lazy entries).
            while (true) {
                if (v.tag() == Tag::App) {
                    Value left = v.payload.pair->left;
                    Value right = v.payload.pair->right;
                    vm.frames.back().ip = ip;
                    // left may itself need forcing (App spines).
                    left = forceValue(vm, left);
                    v = callClosure(vm, left, right);
                    continue;
                }
                if (!v.isThunk()) break;
                if (v.payload.thunk->state == ThunkState::Evaluated) {
                    v = v.payload.thunk->evaluated;
                    continue;
                }
                break;
            }
            if (!v.isThunk()) { push(vm, v); break; }
            Thunk * t = v.payload.thunk;
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
            fakeClo->capturedWiths = t->suspended.capturedWiths;
            fakeClo->cu = t->suspended.cu;
            for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];

            ListVec * thunkWiths = t->suspended.capturedWiths;
            const CompilationUnit * thunkCu = t->suspended.cu ? t->suspended.cu : cu;
            t->state = ThunkState::Blackhole;

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

            vm.frames.push_back(CallFrame{
                .cu = thunkCu,
                .closure = fakeClo,
                .thunk = t,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = CFF_THUNK_RETURN,
            });
            pushCapturedWiths(vm, thunkWiths);
            cu = thunkCu;

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
            // Each entry is a (SymbolId, PosIdx) pair inlined as 2 code
            // words.  PosIdx feeds the per-attr position side-table that
            // backs `builtins.unsafeGetAttrPos`.
            std::vector<SymbolId> names(n);
            std::vector<uint32_t> poses(n);
            for (uint32_t i = 0; i < n; ++i) {
                names[i] = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                poses[i] = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * n;
            // Pop n values (in reverse order).
            std::vector<Value> values(n);
            for (uint32_t i = n; i > 0; --i) values[i - 1] = pop(vm);
            // Build sorted entries; carry pos alongside.
            std::vector<std::tuple<SymbolId, Value, uint32_t>> entries(n);
            for (uint32_t i = 0; i < n; ++i)
                entries[i] = {names[i], values[i], poses[i]};
            std::sort(entries.begin(), entries.end(),
                      [](auto & a, auto & b) { return std::get<0>(a) < std::get<0>(b); });
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                b->entries[i].name  = std::get<0>(entries[i]);
                b->entries[i].value = std::get<1>(entries[i]);
                recordAttrPos(b, std::get<0>(entries[i]), std::get<2>(entries[i]));
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
            // Inline layout: nStatic*(name, pos) pairs followed by nDyn
            // pos words for the dynamic entries.
            std::vector<SymbolId> staticNames(nStatic);
            std::vector<uint32_t> staticPoses(nStatic);
            for (uint32_t i = 0; i < nStatic; ++i) {
                staticNames[i] = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                staticPoses[i] = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * nStatic;
            std::vector<uint32_t> dynPoses(nDyn);
            for (uint32_t i = 0; i < nDyn; ++i)
                dynPoses[i] = cu->code[ip + i];
            ip += nDyn;

            std::vector<std::tuple<SymbolId, Value, uint32_t>> entries;
            entries.reserve(nStatic + nDyn);
            for (uint32_t i = 0; i < nStatic; ++i)
                entries.emplace_back(staticNames[i], staticVals[i], staticPoses[i]);
            for (uint32_t i = 0; i < nDyn; ++i) {
                Value & nameV = dynPairs[i * 2];
                Value & valV  = dynPairs[i * 2 + 1];
                // null-named dynamic attrs are silently dropped — Nix
                // semantics so things like `{ ${if cond then "k" else null}
                // = v; }` work as a conditional add.
                if (nameV.isNull()) continue;
                if (!nameV.isString())
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: dynamic name must be a string");
                // Use the global symbol table — IDs from any CU stay
                // consistent so attrset lookups across CUs work.
                SymbolId id = ir::globalInternSymbol(nameV.payload.str);
                entries.emplace_back(id, valV, dynPoses[i]);
            }
            std::sort(entries.begin(), entries.end(),
                      [](auto & a, auto & b) { return std::get<0>(a) < std::get<0>(b); });
            Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(entries.size()));
            allocStats().attrsetsAllocated++;
            for (size_t i = 0; i < entries.size(); ++i) {
                b->entries[i].name  = std::get<0>(entries[i]);
                b->entries[i].value = std::get<1>(entries[i]);
                recordAttrPos(b, std::get<0>(entries[i]), std::get<2>(entries[i]));
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
            // REC_SET operand to the sorted slot).  Each entry is
            // (SymbolId, PosIdx) — the PosIdx feeds the per-attr
            // position side-table.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            allocStats().attrsetsAllocated++;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].value.mkNull();
                recordAttrPos(b, nm, ps);
            }
            ip += 2 * n;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = b;
            push(vm, v);
            break;
        }
        case OP_APPLY_OVERRIDES: {
            // Peek the attrset on top of stack.  If it has __overrides,
            // force it and merge each (name, value) into the rec attrs:
            //   - Names already present are overwritten in place (so
            //     OP_ATTRS_SELECT inside the rec body sees the new value).
            //   - New names cause a Bindings grow + re-sort so the result
            //     attrset visible to the outer scope contains them.
            //   This matches tree-walker semantics.
            Value & top = vm.valueStack.back();
            if (!top.isAttrs() || !top.payload.bindings) break;
            static const SymbolId ovId = ir::globalInternSymbol("__overrides");
            const Value * ovRaw = top.payload.bindings->lookup(ovId);
            if (!ovRaw) break;
            Value ov = forceValue(vm, *ovRaw);
            if (!ov.isAttrs() || !ov.payload.bindings) break;
            auto * dst = top.payload.bindings;
            const auto * src = ov.payload.bindings;
            // First pass: overwrite existing entries; collect names to add.
            std::vector<std::pair<SymbolId, Value>> toAdd;
            for (uint32_t i = 0; i < src->size; ++i) {
                SymbolId k = src->entries[i].name;
                const Value * existing = dst->lookup(k);
                if (existing) {
                    // Mutate in place via const_cast — `lookup` returns a
                    // pointer to the actual storage and we own this Bindings.
                    const_cast<Value &>(*existing) = src->entries[i].value;
                } else {
                    toAdd.emplace_back(k, src->entries[i].value);
                }
            }
            if (!toAdd.empty()) {
                Bindings * grown = Alloc::allocBindings(dst->size + toAdd.size());
                allocStats().attrsetsAllocated++;
                std::vector<std::pair<SymbolId, Value>> all;
                all.reserve(dst->size + toAdd.size());
                for (uint32_t i = 0; i < dst->size; ++i)
                    all.emplace_back(dst->entries[i].name, dst->entries[i].value);
                for (auto & e : toAdd) all.push_back(e);
                std::sort(all.begin(), all.end(),
                    [](auto & a, auto & b) { return a.first < b.first; });
                for (size_t i = 0; i < all.size(); ++i) {
                    grown->entries[i].name  = all[i].first;
                    grown->entries[i].value = all[i].second;
                }
                top.payload.bindings = grown;
            }
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
            // Intern via the global table so the SymbolId matches the
            // ones the attrset's bindings were built with.
            SymbolId id = ir::globalInternSymbol(name.payload.str);
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
            SymbolId id = ir::globalInternSymbol(name.payload.str);
            push(vm, attrs.payload.bindings->has(id)
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
            // Hot path on every Nix-level `a + b` (which the parser
            // lowers to ConcatStrings).  Avoid allocating a heap
            // vector for the common 2-part case — most ConcatStrings
            // expressions are exactly two operands.
            constexpr uint32_t kSmall = 8;
            Value small[kSmall];
            std::vector<Value> overflow;
            Value * parts = small;
            if (n > kSmall) {
                overflow.resize(n);
                parts = overflow.data();
            }
            for (uint32_t i = n; i > 0; --i) parts[i - 1] = pop(vm);

            // nix `+` semantics: if forceString=false and the first operand
            // is numeric (Int/Float), perform arithmetic addition; otherwise
            // do string concatenation.  forceString=true (e.g. "${foo}")
            // always coerces to string.
            if (!forceStr && n > 0 && (parts[0].isInt() || parts[0].isFloat())) {
                bool allInt = true;
                for (uint32_t i = 0; i < n; ++i) if (!parts[i].isInt()) { allInt = false; break; }
                Value r;
                if (allInt) {
                    int64_t sum = 0;
                    for (uint32_t i = 0; i < n; ++i) sum += parts[i].payload.i;
                    r.mkInt(sum);
                } else {
                    double sum = 0.0;
                    for (uint32_t i = 0; i < n; ++i) {
                        const Value & p = parts[i];
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
            for (uint32_t i = 0; i < n; ++i) {
                const Value & p = parts[i];
                // Attrset coercion: __toString self  or  outPath.
                // Matches tree-walker's coerceToString behaviour for
                // attrsets (used to interpolate derivation values).
                if (p.isAttrs() && p.payload.bindings) {
                    static const SymbolId tsId  = ir::globalInternSymbol("__toString");
                    static const SymbolId outId = ir::globalInternSymbol("outPath");
                    if (auto * fn = p.payload.bindings->lookup(tsId)) {
                        Value forced = forceValue(vm, *fn);
                        Value s = callClosure(vm, forced, p);
                        s = forceValue(vm, s);
                        if (s.isString()) { out.append(s.payload.str); continue; }
                    }
                    if (auto * op = p.payload.bindings->lookup(outId)) {
                        Value forced = forceValue(vm, *op);
                        if (forced.isString()) { out.append(forced.payload.str); continue; }
                        if (forced.isPath())   { out.append(forced.payload.path); continue; }
                    }
                }
                out.append(coerceToString(p, forceStr));
            }
            // Path + string semantics: when the first operand is a Path
            // and we're in plain `+` mode (not interpolation), the result
            // is a Path (lexically normalized), not a String.  Required
            // by `dirOf p + ""` and by string-test concat patterns like
            // `/foo/bar + "/../xyzzy/."` which must collapse to /foo/xyzzy.
            bool resultIsPath = !forceStr && n > 0 && parts[0].isPath();
            if (resultIsPath) {
                std::string normalized =
                    std::filesystem::path(out).lexically_normal().string();
                // lexically_normal leaves a trailing "/." for inputs
                // like "/a/b/." — strip it so output matches Nix.
                while (normalized.size() > 1 && normalized.back() == '/')
                    normalized.pop_back();
                out = std::move(normalized);
            }
            char * buf = static_cast<char *>(std::malloc(out.size() + 1));
            std::memcpy(buf, out.data(), out.size());
            buf[out.size()] = '\0';
            Value v;
            if (resultIsPath) {
                v.tag_payload = static_cast<uint64_t>(Tag::Path);
                v.payload.path = buf;
            } else {
                v.mkString(buf);
            }
            push(vm, v);
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
            // re-enter the dispatcher; also propagate the (optional)
            // nix EvalState so primops like `import` can parse files.
            EvalState state;
            state.vm = &vm;
            state.nixEvalState = getNixEvalState();
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
    // Generous initial reservations: deep-recursive workloads (fib,
    // ackermann, large fold chains) churn the value/frame stacks
    // many times.  Avoiding reallocation through the hot path is a
    // measurable win.
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    vm.frames.push_back(CallFrame{
        .cu = &rootCu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = rootCu.entryOffset,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    if (!rootCu.lambdas.empty())
        vm.valueStack.resize(rootCu.lambdas[0].nLocals);

    return dispatchLoop(vm, /*exitDepth=*/0);
}

Value forceValue(VMState & vm, Value v)
{
    // Loop until WHNF: a thunk's body might itself yield a thunk
    // (e.g., `let inherit outer; in outer` returns the outer thunk),
    // and we want to chase the chain until we land on a real value.
    while (true) {
        // Tag::App is a deferred application — force it by actually
        // applying.  Used by primops like mapAttrs that build lazy
        // entries: each entry is `App(fn, arg)` and we materialize on
        // demand.  `left` may itself be an App / Thunk (e.g. mapAttrs
        // builds App(App(fn, name), value)) — force the spine first.
        if (v.tag() == Tag::App) {
            Value left  = v.payload.pair->left;
            Value right = v.payload.pair->right;
            left = forceValue(vm, left);
            v = callClosure(vm, left, right);
            continue;
        }
        if (!v.isThunk()) break;
        Thunk * t = v.payload.thunk;
        if (t->state == ThunkState::Evaluated) { v = t->evaluated; continue; }
        if (t->state == ThunkState::Blackhole)
            throw std::runtime_error("v3 forceValue: infinite recursion (blackhole)");

        const LambdaDescriptor * desc = reinterpret_cast<const LambdaDescriptor *>(t->suspended.desc);
        Closure * fakeClo = Alloc::allocClosure(t->nUpvalues);
        fakeClo->desc = desc;
        fakeClo->nUpvalues = t->nUpvalues;
        fakeClo->capturedWiths = t->suspended.capturedWiths;
        fakeClo->cu = t->suspended.cu;
        for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];
        ListVec * thunkWiths = t->suspended.capturedWiths;
        const CompilationUnit * thunkCu = t->suspended.cu
            ? t->suspended.cu
            : vm.frames.back().cu;
        t->state = ThunkState::Blackhole;

        size_t exitDepth = vm.frames.size();
        size_t newBase = vm.valueStack.size();
        vm.valueStack.resize(newBase + desc->nLocals);
        uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

        vm.frames.push_back(CallFrame{
            .cu = thunkCu,
            .closure = fakeClo,
            .thunk = t,
            .ip = desc->codeOffset,
            .stackBaseOffset = static_cast<uint32_t>(newBase),
            .withStackBase = newWithBase,
            .flags = CFF_THUNK_RETURN,
        });
        pushCapturedWiths(vm, thunkWiths);

        v = dispatchLoop(vm, exitDepth);
    }
    return v;
}

Value callClosure(VMState & vm, Value fun, Value arg)
{
    // PrimOp / PrimOpApp: build a partial application or invoke once
    // we have all the args.  Mirrors the OP_CALL primop branch.
    if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
        Value cur = fun;
        size_t depth = 0;
        while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.payload.pair->left; }
        if (!cur.isPrimOp())
            throw std::runtime_error("v3 callClosure: PrimOpApp chain doesn't terminate in a PrimOp");
        const PrimOp * po = cur.payload.primop;
        size_t totalArgs = depth + 1;
        if (totalArgs < po->arity) {
            ValuePair * vp = static_cast<ValuePair *>(std::malloc(sizeof(ValuePair)));
            vp->left = fun;
            vp->right = arg;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::PrimOpApp);
            v.payload.pair = vp;
            return v;
        }
        if (totalArgs > po->arity)
            throw std::runtime_error("v3 callClosure: too many args for primop");
        Value buf[8];
        if (po->arity > 8) throw std::runtime_error("v3 callClosure: primop arity > 8");
        buf[totalArgs - 1] = arg;
        Value chain = fun;
        for (size_t i = totalArgs - 1; i > 0; --i) {
            buf[i - 1] = chain.payload.pair->right;
            chain = chain.payload.pair->left;
        }
        for (uint32_t i = 0; i < po->arity; ++i) buf[i] = forceValue(vm, buf[i]);
        EvalState state; state.vm = &vm; state.nixEvalState = getNixEvalState();
        Value out;
        po->fn(state, buf, out);
        return out;
    }

    // Attrset with __functor: apply functor self arg.
    if (fun.isAttrs() && fun.payload.bindings) {
        static const SymbolId functorId = ir::globalInternSymbol("__functor");
        if (auto * fn = fun.payload.bindings->lookup(functorId)) {
            Value forced = forceValue(vm, *fn);
            Value firstStep = callClosure(vm, forced, fun);
            return callClosure(vm, firstStep, arg);
        }
    }

    if (!fun.isClosure())
        throw std::runtime_error("v3 callClosure: not callable");

    const Closure * callee = fun.payload.closure;
    const LambdaDescriptor * desc = callee->desc;
    // Cross-CU calls (e.g., calling a closure returned from
    // builtins.import): use the closure's own CU when available.
    const CompilationUnit * cu = callee->cu ? callee->cu : vm.frames.back().cu;

    // Push a CALL frame for the callee — mirrors OP_CALL.
    size_t exitDepth = vm.frames.size();
    size_t newBase = vm.valueStack.size();
    vm.valueStack.resize(newBase + desc->nLocals);
    vm.valueStack[newBase + 0] = arg;
    uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

    vm.frames.push_back(CallFrame{
        .cu = cu,
        .closure = callee,
        .thunk = nullptr,
        .ip = desc->codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(newBase),
        .withStackBase = newWithBase,
        .flags = 0,
    });
    pushCapturedWiths(vm, callee->capturedWiths);

    return dispatchLoop(vm, exitDepth);
}

} // namespace nix::v3
