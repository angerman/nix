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
#include "v3/disasm.hh"

#include "nix/expr/eval.hh"
#include "nix/store/store-api.hh"
#include "nix/util/canon-path.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace nix::v3 {

// WC-10: forward declaration at namespace scope so the `extern` use sites
// inside the anonymous namespaces below resolve to nix::v3::forceBridgeThunk
// (defined in primops.cc) rather than to a phantom anonymous-namespace symbol.
Value forceBridgeThunk(Thunk * t);

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
    case Tag::Slot:
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
///
/// In interpolation context (`forceString = true`) we route Path values
/// through tree-walker's `copyPathToStore` (DryRun under
/// settings.readOnlyMode = true) so `${./foo}` produces the proper
/// `/nix/store/<32-hash>-name` representation, not the absolute file
/// path.  Required by tests like `eval-okay-context` that count on the
/// store-path prefix length.
inline std::string coerceToString(const Value & v, bool forceString)
{
    switch (v.tag()) {
    case Tag::String: return std::string(v.payload.str);
    case Tag::Path: {
        std::string p(v.payload.path ? v.payload.path : "");
        if (forceString) {
            if (auto * ns = getNixEvalState()) {
                // Let copyPathToStore exceptions propagate — tree-walker
                // raises on missing paths during interpolation, and v3
                // should match.  Note for the caller: this string carries
                // an Opaque context entry for `storePath`; the caller is
                // responsible for recording it (see OP_STR_CONCAT below).
                nix::NixStringContext ctx;
                nix::SourcePath sp(ns->rootFS, nix::CanonPath(p));
                auto storePath = ns->copyPathToStore(ctx, sp);
                return ns->store->printStorePath(storePath);
            }
        }
        return p;
    }
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
    case Tag::Slot:
    default:
        {
            char buf[96];
            std::snprintf(buf, sizeof buf,
                "v3 STR_CONCAT: cannot coerce type to string (tag=%u)",
                (unsigned)v.tag());
            static const bool dbg = std::getenv("V3_DBG_STRCONCAT") != nullptr;
            if (dbg) {
                std::fprintf(stderr, "%s\n", buf);
                if (v.tag() == Tag::Closure && v.payload.closure && v.payload.closure->desc) {
                    auto * d = v.payload.closure->desc;
                    std::fprintf(stderr, "  closure: %s code=[%u..) nUp=%u\n",
                        !d->name.empty() ? d->name.c_str() : "<anon>",
                        d->codeOffset, d->nUpvalues);
                }
            }
            throw std::runtime_error(buf);
        }
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
        // Tag::Slot: deref the slot pointer.  Slots are mutated when
        // their backing let-rec body publishes a result, so reading
        // through the slot here gives us the LATEST value (matches
        // tree-walker's `state.forceValue(*v2)` on slot pointers).
        if (w.tag() == Tag::Slot) {
            Value * p = w.payload.slot;
            if (!p) continue;
            // Don't write *p back into w — leave the slot pointer in
            // place for subsequent lookups (the slot may mutate again,
            // e.g. OP_APPLY_OVERRIDES grows the bindings).  Force a
            // local copy and use it for this iteration.
            Value derefed = *p;
            if (derefed.isThunk() || derefed.tag() == Tag::App) {
                try {
                    derefed = forceValue(vm, derefed);
                } catch (const std::exception & ex) {
                    std::string what(ex.what());
                    if (what.find("blackhole") != std::string::npos)
                        continue;
                    throw;
                }
            }
            if (!derefed.isAttrs()) continue;
            if (auto * v = derefed.payload.bindings->lookup(name))
                return *v;
            continue;
        }
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
    // V3_DBG_WITH: print the missing name + the with-stack contents to
    // help diagnose pure-VM nixpkgs failures where eval-order divergence
    // causes a name to be looked up before its `with` scope is visible.
    static const bool s_dbg_with = std::getenv("V3_DBG_WITH") != nullptr;
    const auto & symTab = ir::globalSymbolTable();
    std::string nm = name < symTab.size() ? symTab[name] : "<?>";
    if (s_dbg_with) {
        std::fprintf(stderr,
            "v3 OP_WITH_LOOKUP miss: name='%s' (sid=%u) base=%zu top=%zu\n",
            nm.c_str(), (unsigned)name, base, vm.withStack.size());
        for (size_t i = vm.withStack.size(); i-- > base; ) {
            Value w = vm.withStack[i];   // copy so we can chase
            void * orig_thunk = w.isThunk() ? (void *)w.payload.thunk : nullptr;
            std::fprintf(stderr, "  with[%zu] tag=%u thunk_ptr=%p",
                i, (unsigned)w.tag(), orig_thunk);
            // Chase Evaluated thunk chains to find the underlying attrset.
            int chase_lim = 5;
            while (chase_lim-- > 0 && w.isThunk()
                   && w.payload.thunk->state == ThunkState::Evaluated) {
                w = w.payload.thunk->evaluated;
                std::fprintf(stderr, " -> tag=%u", (unsigned)w.tag());
                if (w.isThunk())
                    std::fprintf(stderr, "(ptr=%p)", (void *)w.payload.thunk);
            }
            if (w.isAttrs() && w.payload.bindings) {
                auto * b = w.payload.bindings;
                std::fprintf(stderr, " attrs size=%u {", b->size);
                for (uint32_t k = 0; k < b->size && k < 30; ++k) {
                    SymbolId s = b->entries[k].name;
                    std::fprintf(stderr, "%s%s",
                        k ? "," : "",
                        s < symTab.size() ? symTab[s].c_str() : "?");
                }
                if (b->size > 30) std::fprintf(stderr, ",...");
                std::fprintf(stderr, "}");
                // Also check if name IS in this attrset (via binary search) —
                // if it is, we have a real bug (lookup failed but it's there).
                if (auto * v = b->lookup(name)) {
                    std::fprintf(stderr, " [name-IS-here-but-missed!]");
                    (void)v;
                }
            } else if (w.isThunk()) {
                Thunk * t = w.payload.thunk;
                std::fprintf(stderr, " thunk state=%d nUp=%u",
                    (int)t->state, (unsigned)t->nUpvalues);
                if (t->state == ThunkState::Suspended) {
                    auto * d = reinterpret_cast<const LambdaDescriptor *>(t->suspended.desc);
                    if (d)
                        std::fprintf(stderr, " %s [%u..)",
                            !d->name.empty() ? d->name.c_str() : "<anon>",
                            d->codeOffset);
                }
            } else if (w.isClosure() && w.payload.closure
                       && w.payload.closure->desc) {
                auto * d = w.payload.closure->desc;
                std::fprintf(stderr, " closure=%s nUp=%u",
                    !d->name.empty() ? d->name.c_str() : "<anon>",
                    w.payload.closure->nUpvalues);
            }
            std::fprintf(stderr, "\n");
        }
        // Dump frame stack — the failing `with` lookup happens during a
        // specific frame's body; identifying it helps localize the source.
        size_t lim = vm.frames.size();
        std::fprintf(stderr, "  frames=%zu (showing all):\n", lim);
        for (size_t i = lim; i > 0; --i) {
            const auto & fr = vm.frames[i - 1];
            const LambdaDescriptor * d = nullptr;
            if (fr.thunk) d = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
            else if (fr.closure) d = fr.closure->desc;
            std::fprintf(stderr,
                "    frame[%zu]: %s code=[%u..) ip=%u flags=%u thunk=%p withBase=%u\n",
                i - 1,
                d && !d->name.empty() ? d->name.c_str()
                    : (d ? "<anon>" : "<root>"),
                d ? d->codeOffset : 0, fr.ip,
                (unsigned)fr.flags, (void *)fr.thunk, fr.withStackBase);
        }
    }
    throw std::runtime_error(
        "v3 OP_WITH_LOOKUP: name '" + nm + "' not found in with-scope");
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

/// WC-38 architectural fix attempt: walk the frame stack from BOTTOM
/// up and publish `v` to the OUTERMOST CFF_THUNK_RETURN frame whose
/// `thunk` is in Blackhole state.  Mirrors tree-walker's
/// `v.mkAttrs(...)` semantics writing into the slot of the currently-
/// forcing thunk DURING `ExprAttrs::eval`.  Sub-thunks captured-with
/// the slot will then see the (possibly intermediate) attrset rather
/// than blackholing.
///
/// We target the OUTERMOST (deepest in the stack) Black thunk, not
/// the innermost.  Reason: the inner Black thunks (e.g., a sub-thunk
/// firing INSIDE x's body) have a different final value than x.
/// Publishing to inner thunks would corrupt their evaluated.  The
/// outermost Black thunk is the one whose `with self;` capture is in
/// scope for failing sub-thunks (e.g., x in `let x = f x; in x`).
///
/// Idempotent: the eventual `OP_RETURN` of the outer thunk frame
/// will overwrite `evaluated` with the final retVal.  Risk:
/// intermediate values may be partial.  For the lib.fix bootstrap
/// pattern, the final value IS one of the intermediate attrsets,
/// so a sub-thunk reading the intermediate is reading a valid
/// snapshot.
///
/// Gated behind `NIX_V3_EARLY_PUBLISH=1`.  Default off until validated.
inline void publishToNearestBlackThunkFrame(VMState & vm, const Value & v)
{
    static const bool s_enabled =
        std::getenv("NIX_V3_EARLY_PUBLISH") != nullptr;
    if (!s_enabled) return;
    // Only publish concrete values, not thunks/apps/blackholes.
    Tag t = v.tag();
    if (t == Tag::Thunk || t == Tag::App || t == Tag::Blackhole) return;
    static const bool s_dbg =
        std::getenv("NIX_V3_EARLY_PUBLISH_DBG") != nullptr;
    // Walk from bottom up.  Find the OUTERMOST CFF_THUNK_RETURN frame
    // whose thunk is Black.  This is the frame whose captured-with
    // entries (`with self;`) sub-thunks may be racing against.
    for (size_t i = 0; i < vm.frames.size(); ++i) {
        CallFrame & fr = vm.frames[i];
        if (!(fr.flags & CFF_THUNK_RETURN)) continue;
        if (!fr.thunk) continue;
        if (fr.thunk->state != ThunkState::Blackhole) continue;
        // Publish to outermost Black thunk.  Idempotent — OP_RETURN
        // of this thunk frame will overwrite with the final retVal.
        if (s_dbg) {
            const char * tagName = "?";
            uint32_t nKeys = 0;
            if (t == Tag::Attrs) {
                tagName = "Attrs";
                if (v.payload.bindings) nKeys = v.payload.bindings->size;
            } else if (t == Tag::List) {
                tagName = "List";
                if (v.payload.list) nKeys = v.payload.list->size;
            } else {
                tagName = "Other";
            }
            std::fprintf(stderr,
                "v3 EARLY_PUBLISH: frame[%zu] thunk=%p tag=%s n=%u "
                "(at frames=%zu)\n",
                i, (void*)fr.thunk, tagName, nKeys, vm.frames.size());
        }
        fr.thunk->state = ThunkState::Evaluated;
        fr.thunk->evaluated = v;
        return;
    }
}

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
    // Gate the per-instruction counter behind an env var: it adds a
    // memory write to every instruction and is only useful for
    // profiling.  Overhead on fib32 was ~3% on first-run timings.
    static const bool kCountInstructions = std::getenv("NIX_VM_STATS") != nullptr;
    // V3_DBG_TRACE_THUNK_BODY: per-instruction trace gated on the
    // currently-running thunk frame having a specific (codeOffset, nUp).
    // Used to nail down WC-37 frame/thunk mismatch. Format:
    //   V3_DBG_TRACE_THUNK_BODY=1346,5  ← trace any thunk with codeOffset
    //                                     1346 and nUp=5
    static const char * s_trace_env = std::getenv("V3_DBG_TRACE_THUNK_BODY");
    static const uint32_t s_trace_codeoff =
        s_trace_env ? static_cast<uint32_t>(std::strtoul(s_trace_env, nullptr, 10)) : 0;
    static const uint16_t s_trace_nup = []() -> uint16_t {
        const char * e = std::getenv("V3_DBG_TRACE_THUNK_BODY");
        if (!e) return 0;
        const char * comma = std::strchr(e, ',');
        if (!comma) return 0;
        return static_cast<uint16_t>(std::strtoul(comma + 1, nullptr, 10));
    }();
    while (running) {
        // V3_DBG_TRACE_THUNK_BODY: print this instruction if the current
        // frame is a thunk frame matching the configured codeOffset/nUp.
        if (s_trace_env && !vm.frames.empty()) {
            const auto & cur = vm.frames.back();
            if (cur.thunk && (cur.flags & CFF_THUNK_RETURN)
                && cur.thunk->nUpvalues == s_trace_nup)
            {
                const auto * d = reinterpret_cast<const LambdaDescriptor *>(
                    cur.thunk->suspended.desc);
                if (d && d->codeOffset == s_trace_codeoff) {
                    Instruction peek = cu->code[ip];
                    Op pop_o = decodeOp(peek);
                    uint32_t pop_n = decodeOperand(peek);
                    // Fingerprint: pointer values of the first 3 upvalues
                    // — distinguishes different thunks that happen to share
                    // pointer (Boehm GC reuse) by their upvalue contents.
                    uint64_t fp0 = cur.thunk->tail[0].tag_payload;
                    uint64_t fp1 = cur.thunk->nUpvalues > 1 ? cur.thunk->tail[1].tag_payload : 0;
                    std::fprintf(stderr,
                        "  TRACE thunk=%p state=%d ip=%u op=0x%02x operand=%u (cu=%p) fp=[%016llx,%016llx]\n",
                        (void*)cur.thunk, (int)cur.thunk->state, ip, (unsigned)pop_o, pop_n,
                        (void*)cu, (unsigned long long)fp0, (unsigned long long)fp1);
                }
            }
        }
        Instruction instr = cu->code[ip++];
        if (kCountInstructions) vm.nrInstructions++;
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
        // Note: bounds checking on GET_LOCAL is omitted — the emit pass
        // + LambdaDescriptor::nLocals + the OP_CALL resize guarantee
        // every slot a function references has been pre-allocated.  A
        // bounds violation means the bytecode is corrupt; accept the
        // UB rather than pay for the check on every read.
        case OP_GET_LOCAL: {
            push(vm, vm.valueStack[stackBase + operand]);
            break;
        }
        case OP_GET_LOCAL_FORCE: {
            // Superinstruction: GET_LOCAL + FORCE.  Push the slot value
            // and apply the FORCE fast path inline.
            const Value & v = vm.valueStack[stackBase + operand];
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App, 1)) {
                push(vm, v);
                break;
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_SET_LOCAL: {
            // SET keeps an auto-grow loop because some lower paths
            // (notably tryEval / inherit-from temp slots) write to a
            // slot that wasn't reserved by the function's nLocals
            // count — see eval-okay-tryeval-failed-thunk-reeval.
            // Fast path: slot is already in range — just pop+store, no grow.
            const size_t idx = stackBase + operand;
            if (__builtin_expect(idx < vm.valueStack.size() - 1, 1)) {
                vm.valueStack[idx] = vm.valueStack.back();
                vm.valueStack.pop_back();
            } else {
                Value v = pop(vm);
                while (idx >= vm.valueStack.size())
                    vm.valueStack.push_back(Value{});
                vm.valueStack[idx] = v;
            }
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
        case OP_GET_UPVALUE_FORCE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE_FORCE: no closure context");
            const Value & v = closure->upvalues[operand];
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App, 1)) {
                push(vm, v);
                break;
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_DUP:  push(vm, top(vm)); break;
        case OP_POP:  vm.valueStack.pop_back(); break;
        case OP_SWAP: {
            size_t n = vm.valueStack.size();
            std::swap(vm.valueStack[n - 1], vm.valueStack[n - 2]);
            break;
        }

        // --- Arithmetic ---
        // Int operations check for overflow via __builtin_*_overflow:
        // tree-walker raises an integer-overflow error, and v3 should
        // match.  Float operations have no such check (NaN/Inf semantics
        // mirror IEEE-754, same as tree-walker).
        case OP_ADD: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isInt() && rhs.isInt()) {
                int64_t sum;
                if (__builtin_add_overflow(lhs.payload.i, rhs.payload.i, &sum))
                    throw std::runtime_error("v3 OP_ADD: integer overflow");
                r.mkInt(sum);
            }
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
            if (lhs.isInt() && rhs.isInt()) {
                int64_t diff;
                if (__builtin_sub_overflow(lhs.payload.i, rhs.payload.i, &diff))
                    throw std::runtime_error("v3 OP_SUB: integer overflow");
                r.mkInt(diff);
            }
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
            if (lhs.isInt() && rhs.isInt()) {
                int64_t prod;
                if (__builtin_mul_overflow(lhs.payload.i, rhs.payload.i, &prod))
                    throw std::runtime_error("v3 OP_MUL: integer overflow");
                r.mkInt(prod);
            }
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
                // INT64_MIN / -1 wraps around (mathematical result is
                // INT64_MAX + 1).  Match tree-walker by raising.
                if (lhs.payload.i == std::numeric_limits<int64_t>::min() && rhs.payload.i == -1)
                    throw std::runtime_error("v3 OP_DIV: integer overflow");
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
        // Inline fast-path for the int-int case (common: `n == 0`,
        // `n < 2` etc.).  In-place mutate the deeper slot to the bool
        // result and pop the top — no helper call, no Value temporaries.
        case OP_EQ:  {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool eq = top0.payload.i == top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = eq ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }
        case OP_NEQ: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool ne = top0.payload.i != top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = ne ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vFalse : Value::vTrue; push(vm, r); break;
        }
        case OP_LESS:{
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool lt = top0.payload.i < top1.payload.i;
                vm.valueStack.pop_back();
                vm.valueStack.back() = lt ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueLess(a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }

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
            op_call_dispatch:
            // A non-tail call resets the tail-iteration counter — any
            // subsequent runaway recursion is bounded against the
            // 5000-frame stack guard, not the tail-call counter.
            vm.tailCallCount = 0;
            Value arg = pop(vm), fun = pop(vm);

            // Force `fun` if it's a deferred shape (Tag::App from lazy
            // primops like mapAttrs, or a Thunk that lazy attr access
            // produced).  Tree-walker's `callFunction` does the same up
            // front; mirroring it here keeps the rest of the dispatch
            // simple and avoids the OP_RETURN-chase cycle problem (where
            // chasing while the outer thunk is still Black trips
            // infinite-recursion).  Cheap on the hot path: one tag
            // check on already-WHNF callables.
            if (fun.tag() == Tag::App || fun.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                fun = forceValue(vm, fun);
            }

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
                // WHNF, so force each here before invoking, EXCEPT for
                // args the primop has explicitly opted out of via its
                // `lazyArgs` bitmask (e.g., addErrorContext's value arg
                // — see lib/modules.nix's
                // `config = addErrorContext "..." config` cycle).
                for (uint32_t i = 0; i < po->arity; ++i) {
                    if (po->lazyArgs & (1u << i)) continue;
                    buf[i] = forceValue(vm, buf[i]);
                }
                bumpPrimOpCallCount(po);
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

            if (!fun.isClosure()) {
                static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
                if (dbg) {
                    std::fprintf(stderr,
                        "v3 OP_CALL: callee is not a closure tag=%u "
                        "frames=%zu callerIp=%u\n",
                        (unsigned)fun.tag(), vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        std::fprintf(stderr,
                            "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                            i - 1,
                            desc && !desc->name.empty() ? desc->name.c_str()
                                : (desc ? "<anon>" : "<closure-body>"),
                            desc ? desc->codeOffset : 0,
                            fr.ip, (unsigned)fr.flags);
                    }
                    // Dump 32 instructions before/after the failing OP_CALL.
                    if (cu) {
                        uint32_t fip = ip > 0 ? ip - 1 : 0;
                        uint32_t lo = fip > 64 ? fip - 64 : 0;
                        uint32_t hi = fip + 16;
                        std::fprintf(stderr, "  current frame disasm [%u..%u):\n", lo, hi);
                        disassembleWindow(stderr, *cu, lo, hi);
                    }
                }
                throw std::runtime_error("v3 OP_CALL: callee is not a closure");
            }
            const Closure * callee = fun.payload.closure;
            const LambdaDescriptor * desc = callee->desc;
            // Closures from imported files own their own CompilationUnit;
            // when callee->cu differs, switch the dispatch loop to the
            // callee's bytecode/constant pools.  Falls back to the caller's
            // cu when the closure was made before cu-tracking landed.
            const CompilationUnit * calleeCu = callee->cu ? callee->cu : cu;

            // Formals validation: when a lambda has formals and no
            // ellipsis, every key in the param attrset must match a
            // declared formal name.  Tree-walker raises with the offending
            // attribute name; we mirror that message format.
            if (desc->hasFormals && !desc->ellipsis) {
                Value forcedArg = forceValue(vm, arg);
                if (forcedArg.isAttrs() && forcedArg.payload.bindings) {
                    const Bindings * b = forcedArg.payload.bindings;
                    for (uint32_t i = 0; i < b->size; ++i) {
                        SymbolId name = b->entries[i].name;
                        bool found = false;
                        for (auto & f : desc->formals)
                            if (f.name == name) { found = true; break; }
                        if (!found) {
                            const auto & tbl = ir::globalSymbolTable();
                            std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                            throw std::runtime_error("v3 OP_CALL: function "
                                "called with unexpected argument '" + nm + "'");
                        }
                    }
                }
                arg = forcedArg;
            }

            // Max call-depth check — guards `(x: x x) (x: x x)` and
            // similar non-thunk-mediated infinite recursion.  Tree-walker
            // defaults to 5000; we match that.  Cheap O(1) check.
            constexpr size_t kMaxCallDepth = 5000;
            if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                throw std::runtime_error("v3 OP_CALL: stack overflow; call depth exceeded "
                                          + std::to_string(kMaxCallDepth));

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            vm.valueStack[newBase + 0] = arg;

            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            // Push the new frame in a single move-construct: lets the
            // compiler initialize the trailing 40 bytes inline at the
            // back of the vector rather than emplace_back + 7 separate
            // field stores.  Frames are pre-reserved so push_back never
            // reallocates on the hot path.
            vm.frames.push_back(CallFrame{
                .cu = calleeCu,
                .closure = callee,
                .thunk = nullptr,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = 0,
            });
            pushCapturedWiths(vm, callee->capturedWiths);

            ip = desc->codeOffset;
            cu  = calleeCu;
            closure = callee;
            stackBase = newBase;
            break;
        }
        case OP_TAIL_CALL: {
            // Tail call: same semantics as OP_CALL but reuses the
            // current frame — no frame push.  Lets long recursive
            // chains run in O(1) frame stack space.
            //
            // Tail-iteration guard: catch infinite tail recursion
            // (`(x: x x) (x: x x)`) which the frame-stack limit
            // can't see because we don't grow the stack.  Tree-walker
            // catches it via C-stack overflow.  We bound at 10^7
            // iterations between frame-stack changes; ~99% headroom
            // over any real-world deep tail recursion.
            constexpr size_t kMaxTailCalls = 10'000'000;
            if (__builtin_expect(++vm.tailCallCount >= kMaxTailCalls, 0)) {
                vm.tailCallCount = 0;
                throw std::runtime_error("v3 OP_TAIL_CALL: tail-call iteration limit exceeded "
                                          + std::to_string(kMaxTailCalls)
                                          + " (likely infinite recursion)");
            }

            // Falls back to OP_CALL behaviour for non-closure callees
            // (primops, __functor, partial application) since those
            // need the full OP_CALL machinery.  We jump back into the
            // OP_CALL case via goto.
            Value arg = pop(vm), fun = pop(vm);
            if (!fun.isClosure()) {
                // Push back and replay through OP_CALL.
                push(vm, fun);
                push(vm, arg);
                goto op_call_dispatch;
            }
            const Closure * tcCallee = fun.payload.closure;
            const LambdaDescriptor * tcDesc = tcCallee->desc;
            const CompilationUnit * tcCalleeCu = tcCallee->cu ? tcCallee->cu : cu;

            // Same formals validation OP_CALL does.
            if (tcDesc->hasFormals && !tcDesc->ellipsis) {
                Value forcedArg = forceValue(vm, arg);
                if (forcedArg.isAttrs() && forcedArg.payload.bindings) {
                    const Bindings * b = forcedArg.payload.bindings;
                    for (uint32_t i = 0; i < b->size; ++i) {
                        SymbolId name = b->entries[i].name;
                        bool found = false;
                        for (auto & f : tcDesc->formals)
                            if (f.name == name) { found = true; break; }
                        if (!found) {
                            const auto & tbl = ir::globalSymbolTable();
                            std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                            throw std::runtime_error("v3 OP_TAIL_CALL: function "
                                "called with unexpected argument '" + nm + "'");
                        }
                    }
                }
                arg = forcedArg;
            }

            // Reuse the current frame: shrink valueStack down to our
            // stackBase, then resize for the callee's locals.  The
            // outer-frame's stackBaseOffset and CallFrame stay put;
            // we just retarget cu/closure/ip and overwrite locals.
            vm.valueStack.resize(stackBase + tcDesc->nLocals);
            vm.valueStack[stackBase + 0] = arg;

            // Update the existing frame in place (don't push a new one).
            CallFrame & cur = vm.frames.back();
            // V3_DBG_STORE_PREVSTAGE: trace when OP_TAIL_CALL retargets
            // a frame that has CFF_THUNK_RETURN — this is the path that
            // can corrupt thunk evaluated values (WC-37 hypothesis).
            {
                static const bool s_dbg_tc =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_tc && (cur.flags & CFF_THUNK_RETURN) && cur.thunk) {
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL on thunk-frame: thunk %p nUp=%u "
                        "old_cu=%p old_ip=%u -> new_cu=%p new_ip=%u "
                        "callee=%s nUp=%u\n",
                        (void*)cur.thunk,
                        (unsigned)cur.thunk->nUpvalues,
                        (void*)cur.cu, cur.ip,
                        (void*)tcCalleeCu, tcDesc->codeOffset,
                        !tcDesc->name.empty() ? tcDesc->name.c_str() : "<anon>",
                        tcDesc->nUpvalues);
                }
            }
            cur.cu = tcCalleeCu;
            cur.closure = tcCallee;
            // thunk stays whatever it was — if we're inside a thunk
            // re-entry frame, the thunk should still be set when
            // we eventually OP_RETURN.
            cur.ip = tcDesc->codeOffset;
            // stackBaseOffset and withStackBase are unchanged: we
            // reuse the same operand-stack window and keep any
            // captured-with entries the outer frame already pushed.

            // Push the callee's captured-withs on top of whatever
            // the outer frame had — they get popped together at
            // OP_RETURN since withStackBase is the outer's floor.
            pushCapturedWiths(vm, tcCallee->capturedWiths);

            ip = tcDesc->codeOffset;
            cu = tcCalleeCu;
            closure = tcCallee;
            // stackBase unchanged.
            break;
        }
        case OP_RETURN: {
            // Reset the tail-call counter — once we return out of a
            // tail-recursive burst, subsequent tail calls in a
            // different chain start fresh.
            vm.tailCallCount = 0;
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
                //
                // Tag::App is intentionally NOT chased here: chasing
                // would call callClosure while `fr.thunk` is still
                // Blackhole, and any transitive force of fr.thunk in
                // the App's body would trip "infinite recursion".  The
                // App is left in `evaluated`; downstream consumers
                // (OP_CALL, OP_FORCE, callClosure) all force-on-receive
                // and chase Apps through forceValue's own loop, by
                // which time `fr.thunk->state` is Evaluated and any
                // re-entry just reads the cached App and chases it
                // again (idempotent — the App's left/right don't
                // change).
                while (retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Evaluated)
                    retVal = retVal.payload.thunk->evaluated;
                // Self-reference detection: `let x = x; in x` makes the
                // thunk's body return the thunk itself (the chase above
                // can't catch this since we hit a Blackhole-state thunk
                // which isn't ThunkState::Evaluated until we're about
                // to assign).  Storing self into evaluated would make
                // subsequent forceValue calls spin forever in the
                // chase loop above.  Match tree-walker by raising.
                if (retVal.isThunk() && retVal.payload.thunk == fr.thunk)
                    throw std::runtime_error("v3 OP_RETURN: infinite recursion (thunk evaluates to itself)");
                // V3_DBG_STORE_PREVSTAGE: trace any thunk that gets
                // evaluated to a Closure whose desc is "prevStage" and
                // codeOffset 3099, nUp=0 — used to isolate WC-37.
                {
                    static const bool s_dbg_pv =
                        std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                    if (s_dbg_pv && retVal.tag() == Tag::Closure
                        && retVal.payload.closure
                        && retVal.payload.closure->desc
                        && retVal.payload.closure->nUpvalues == 0
                        && retVal.payload.closure->desc->name == "prevStage")
                    {
                        const auto * d = reinterpret_cast<const LambdaDescriptor *>(
                            fr.thunk->suspended.desc);
                        std::fprintf(stderr,
                            "v3 OP_RETURN: storing prevStage(nUp=0) into thunk "
                            "%p desc=%s codeOffset=%u nUp=%u; cu=%p ip=%u\n",
                            (void*)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str()
                                : (d ? "<anon>" : "<no-desc>"),
                            d ? d->codeOffset : 0,
                            (unsigned)fr.thunk->nUpvalues,
                            (void*)cu,
                            ip - 1);
                        // Dump frame stack to help locate caller.
                        size_t lim2 = vm.frames.size();
                        for (size_t i = lim2; i > 0 && i + 6 > lim2; --i) {
                            const auto & fr2 = vm.frames[i - 1];
                            const LambdaDescriptor * d2 = nullptr;
                            if (fr2.thunk) d2 = reinterpret_cast<const LambdaDescriptor *>(fr2.thunk->suspended.desc);
                            else if (fr2.closure) d2 = fr2.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u cu=%p\n",
                                i - 1,
                                d2 && !d2->name.empty() ? d2->name.c_str()
                                    : (d2 ? "<anon>" : "<closure-body>"),
                                d2 ? d2->codeOffset : 0, fr2.ip,
                                (unsigned)fr2.flags, (void*)fr2.cu);
                        }
                        // Dump bytecode around ip-1 in the popped frame's cu
                        // — verifies that ip-1 is actually OP_RETURN.
                        if (cu && ip > 1) {
                            uint32_t lo = ip > 6 ? ip - 6 : 0;
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr, "  popped frame cu=%p disasm [%u..%u):\n",
                                (void*)cu, lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm the descriptor's codeOffset region
                            // — this is what the body SHOULD have started at.
                            if (d && d->codeOffset != ip - 1) {
                                std::fprintf(stderr,
                                    "  desc.codeOffset=%u disasm [%u..%u):\n",
                                    d->codeOffset, d->codeOffset, d->codeOffset + 200);
                                disassembleWindow(stderr, *cu,
                                    d->codeOffset, d->codeOffset + 200);
                            }
                            // Find the lambda whose codeOffset is closest
                            // to (ip-1), going backwards.  Tells us which
                            // function we actually returned from.
                            uint32_t target_off = ip - 1;
                            uint32_t best_idx = ~0u;
                            uint32_t best_off = 0;
                            for (uint32_t li = 0; li < cu->lambdas.size(); ++li) {
                                uint32_t lo2 = cu->lambdas[li].codeOffset;
                                if (lo2 <= target_off && lo2 > best_off) {
                                    best_off = lo2;
                                    best_idx = li;
                                }
                            }
                            if (best_idx != ~0u) {
                                const auto & ld = cu->lambdas[best_idx];
                                std::fprintf(stderr,
                                    "  ip-1=%u falls inside lambdas[%u]"
                                    " (name=%s codeOffset=%u nUp=%u nLocals=%u)\n",
                                    target_off, best_idx,
                                    !ld.name.empty() ? ld.name.c_str() : "<anon>",
                                    ld.codeOffset, ld.nUpvalues, ld.nLocals);
                            }
                            // Also dump the thunk pointer's `tail` (its
                            // upvalues) so we can identify which specific
                            // thunk instance.
                            std::fprintf(stderr,
                                "  fr.thunk->tail upvalues (nUp=%u):\n",
                                (unsigned)fr.thunk->nUpvalues);
                            for (uint16_t ui = 0; ui < fr.thunk->nUpvalues && ui < 8; ++ui) {
                                const Value & uv = fr.thunk->tail[ui];
                                std::fprintf(stderr,
                                    "    [%u] tag=%u\n", ui, (unsigned)uv.tag());
                            }
                        }
                    }
                }
                fr.thunk->state = ThunkState::Evaluated;
                fr.thunk->evaluated = retVal;

                // If the body returned a Suspended thunk (e.g., the
                // common pattern where an `inherit` binding's body is
                // just AttrSelect on the parent's rec attrs), force
                // that next by setting up another thunk-return frame.
                // This implements transitive force for the OP_FORCE
                // bytecode op without C++ recursion.
                //
                // WC-38: this chain push is OPTIONAL.  When disabled,
                // the outer thunk's evaluated remains a Suspended thunk;
                // forceValue's own chase loop handles the chain at the
                // consumer's request (matches tree-walker semantics).
                // The chain push is over-eager: it forces the inner
                // thunk's body to start running immediately as part of
                // the outer's RETURN, even if no one needs it yet.  In
                // nixpkgs, this causes deep bootstrap-chain evaluation
                // INSIDE the lib.fix's `let x = f x; in x` body, while
                // x is in Blackhole — sub-thunks then can't look up
                // names via `with x;`.
                // WC-38: chain push is now OFF by default.  When OP_FORCE
                // pushed the popped frame, it set CFF_FORCE_RETRY on the
                // CALLER frame (one level up).  After this OP_RETURN
                // pops the popped frame, the caller-resume path below
                // will check CFF_FORCE_RETRY and re-enter op_force_slow
                // if retVal is still a Thunk/App — implementing
                // GHC STG-style indirection (the consumer drives the
                // chain, not the producer).
                //
                // Set NIX_V3_RETURN_CHAIN=1 to re-enable the legacy
                // chain push for A/B testing.  Default is OFF — this is
                // the WC-38 fix.
                static const bool s_chain_enabled =
                    std::getenv("NIX_V3_RETURN_CHAIN") != nullptr;
                if (s_chain_enabled && retVal.isThunk() && retVal.payload.thunk->state == ThunkState::Suspended) {
                    Thunk * next = retVal.payload.thunk;
                    // Same call-depth guard — chained let-rec recursion
                    // (`let x = y; y = x; in x`) re-enters the next thunk
                    // here without going through OP_CALL or OP_FORCE.
                    if (__builtin_expect(vm.frames.size() >= 5000, 0))
                        throw std::runtime_error("v3 OP_RETURN: stack overflow; call depth exceeded 5000");
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
                    {
                        static const bool s_dbg_force =
                            std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                        if (s_dbg_force && next->nUpvalues == 5) {
                            std::fprintf(stderr,
                                "v3 OP_RETURN-chain: pushing thunk %p desc=%s "
                                "codeOffset=%u nUp=%u cu=%p\n",
                                (void*)next,
                                !desc->name.empty() ? desc->name.c_str() : "<anon>",
                                desc->codeOffset, (unsigned)next->nUpvalues,
                                (void*)thunkCu);
                        }
                    }
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
            {
                // WC-38: GHC STG-style force-retry.  If the caller frame
                // was marked CFF_FORCE_RETRY (set by OP_FORCE /
                // OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE before
                // pushing the now-popped thunk frame) AND retVal is
                // still a Thunk/App (= the body returned a forwarding
                // pointer to another unforced value), re-enter
                // op_force_slow to drive the chain.
                //
                // ALSO (WC-38 part 2 / "early publish"): if the popped
                // frame was a CLOSURE call frame (NOT CFF_THUNK_RETURN)
                // and the caller frame is a thunk frame in Black state,
                // EARLY-PUBLISH retVal to the caller's thunk.evaluated.
                // This mirrors tree-walker's `v.mkAttrs(...)` writing
                // to the slot DURING expr->eval (not at body return),
                // so sub-thunks captured-with that fire DURING the
                // outer's body see the published value rather than
                // hitting the Black state and throwing.
                CallFrame & caller = vm.frames.back();
                cu = caller.cu;
                ip = caller.ip;
                closure = caller.closure;
                stackBase = caller.stackBaseOffset;

                // Early publish: only fires when popped frame was a
                // closure call (not a thunk frame), retVal is fully
                // resolved (non-thunk/app), and caller is a Black
                // thunk frame.  Idempotent — the eventual OP_RETURN
                // of the caller's thunk frame will overwrite with the
                // FINAL retVal.
                //
                // Disabled by default — set NIX_V3_EARLY_PUBLISH=1
                // to enable.  Currently doesn't fully fix WC-38 but
                // is kept for experimentation.
                static const bool s_early_publish =
                    std::getenv("NIX_V3_EARLY_PUBLISH") != nullptr;
                if (s_early_publish
                    && !(fFlags & CFF_THUNK_RETURN)
                    && (caller.flags & CFF_THUNK_RETURN)
                    && caller.thunk
                    && caller.thunk->state == ThunkState::Blackhole
                    && retVal.tag() != Tag::Thunk
                    && retVal.tag() != Tag::App
                    && retVal.tag() != Tag::Blackhole)
                {
                    caller.thunk->state = ThunkState::Evaluated;
                    caller.thunk->evaluated = retVal;
                }

                bool retry = (caller.flags & CFF_FORCE_RETRY)
                    && (retVal.tag() == Tag::Thunk
                        || retVal.tag() == Tag::App);
                // Clear the retry flag — it's a one-shot per
                // OP_FORCE.  The next OP_FORCE will re-set it.
                caller.flags &= ~CFF_FORCE_RETRY;
                push(vm, retVal);
                if (retry)
                    goto op_force_slow;
            }
            break;
        }
        case OP_FORCE: {
            // Fast path: peek at the top of the stack.  The vast majority
            // of OP_FORCE calls hit values already in WHNF (Int / Bool /
            // String / Attrs / List / Closure / Path / Null / Float /
            // PrimOp / PrimOpApp).  Skip the pop+push for those.
            {
                Value & topRef = vm.valueStack.back();
                Tag t = topRef.tag();
                if (t != Tag::Thunk && t != Tag::App && t != Tag::Slot) break;
            }
            // Slow path: shared with OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
            // which push the value first and then jump here.
            op_force_slow:
            Value v = pop(vm);
            // Chase Evaluated chains, deref Tag::Slot, and resolve
            // Tag::App deferred calls (used by mapAttrs et al. for
            // lazy entries).
            while (true) {
                if (v.tag() == Tag::Slot) {
                    Value * p = v.payload.slot;
                    if (!p) throw std::runtime_error(
                        "v3 OP_FORCE: null slot pointer");
                    v = *p;
                    continue;
                }
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
            if (t->state == ThunkState::Blackhole) {
                // WC-17.1 diagnostic: dump the v3 frame stack with
                // function names + IP deltas when V3_DBG_OPCYCLE=1.
                // The `name` field on LambdaDescriptor (populated by
                // emit() from ir::Function::name) lets us correlate
                // cycle frames back to source-level rec-attrset attr
                // names — invaluable for diagnosing the closure-bridge
                // cycle without a full bytecode disassembler.
                static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
                if (s_dbg) {
                    auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t fip) -> std::string {
                        const LambdaDescriptor * desc = nullptr;
                        if (th) desc = reinterpret_cast<const LambdaDescriptor *>(th->suspended.desc);
                        else if (cl) desc = cl->desc;
                        if (!desc) return "<closure-body>";
                        char buf[256];
                        std::snprintf(buf, sizeof buf,
                            "%s code=[%u..) nUp=%u nLocals=%u",
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            desc->codeOffset, desc->nUpvalues, desc->nLocals);
                        return buf;
                    };
                    std::fprintf(stderr,
                        "v3 OP_FORCE Black thunk=%p frames=%zu callerIp=%u\n",
                        (void*)t, vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        std::fprintf(stderr,
                            "  frame[%zu]: %s flags=%u ip=%u thunk=%p\n",
                            i - 1, frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                            (unsigned)fr.flags, fr.ip, (void*)fr.thunk);
                    }
                    // WC-32 disassembler: when V3_DBG_OPCYCLE_DISASM=1,
                    // also dump 8 instructions surrounding each frame's ip.
                    static const bool s_dbg_disasm =
                        std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                    if (s_dbg_disasm) {
                        // Expanded: dump top 12 frames (was 8) for WC-38
                        // investigation.
                        for (size_t i = lim; i > 0 && i + 12 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            uint32_t fip = fr.ip;
                            uint32_t lo = fip > 16 ? fip - 16 : 0;
                            uint32_t hi = fip + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] disasm [%u..%u):\n",
                                i - 1, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                        // Also dump the prologue of each frame's lambda
                        // (where the body STARTS) for context.
                        std::fprintf(stderr, "  --- frame prologues ---\n");
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            const LambdaDescriptor * desc = nullptr;
                            if (fr.thunk)
                                desc = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                            else if (fr.closure)
                                desc = fr.closure->desc;
                            if (!desc) continue;
                            uint32_t prologueStart = desc->codeOffset;
                            uint32_t prologueEnd = prologueStart + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] prologue [%u..%u):\n",
                                i - 1, prologueStart, prologueEnd);
                            disassembleWindow(stderr, *fr.cu, prologueStart, prologueEnd);
                        }
                    }
                }
                throw std::runtime_error("v3 OP_FORCE: infinite recursion (blackhole)");
            }
            // WC-10: Bridge thunk — call into tree-walker for the
            // single nix::Value*, then bridge the already-forced
            // result.  Defined in primops.cc so vm.cc stays free of
            // nix:: includes.
            if (t->state == ThunkState::Bridge) {
                Value resolved = forceBridgeThunk(t);
                t->state = ThunkState::Evaluated;
                t->evaluated = resolved;
                push(vm, resolved);
                break;
            }
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

            // Same call-depth guard as OP_CALL — catches blackhole-style
            // recursion that doesn't go through OP_CALL (e.g. `let x = x;
            // in x`, where every reference to x re-enters via OP_FORCE).
            if (__builtin_expect(vm.frames.size() >= 5000, 0))
                throw std::runtime_error("v3 OP_FORCE: stack overflow; call depth exceeded 5000");

            t->state = ThunkState::Blackhole;

            vm.frames.back().ip = ip;
            // WC-38: mark the caller frame for force-retry. When the
            // pushed thunk's body returns, OP_RETURN's caller-resume
            // path will re-enter op_force_slow if retVal is still a
            // Thunk/App.  This replaces the over-eager OP_RETURN
            // chain push with GHC STG-style consumer-driven chase.
            vm.frames.back().flags |= CFF_FORCE_RETRY;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

            // V3_DBG_STORE_PREVSTAGE: trace OP_FORCE pushes for thunks
            // with nUp=5 to verify their codeOffset before body runs.
            {
                static const bool s_dbg_force =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_force && t->nUpvalues == 5) {
                    std::fprintf(stderr,
                        "v3 OP_FORCE: pushing thunk %p desc=%s codeOffset=%u "
                        "nUp=%u cu=%p\n",
                        (void*)t,
                        !desc->name.empty() ? desc->name.c_str() : "<anon>",
                        desc->codeOffset, (unsigned)t->nUpvalues,
                        (void*)thunkCu);
                }
            }

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
            // Empty list: skip the alloc, push the singleton.  Common
            // for default formals (`xs ? []`) and branch results.
            if (n == 0) {
                push(vm, Value::vEmptyList);
                break;
            }
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
            // Force-on-receive: lazy values (Tag::App from mapAttrs/
            // map/zipAttrsWith, Tag::Thunk from chained AttrSelects)
            // must be forced before shape-checking.  See WC-35.
            if (lhs.tag() == Tag::App || lhs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                lhs = forceValue(vm, lhs);
            }
            if (rhs.tag() == Tag::App || rhs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                rhs = forceValue(vm, rhs);
            }
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
            // Empty attrset: skip the alloc entirely, push the singleton.
            // Real-world Nix code creates many empty attrsets (default
            // formal `... ? {}`, branch results etc.) — not allocating
            // them is cheap and reduces GC pressure.
            if (n == 0) {
                push(vm, Value::vEmptyAttrs);
                break;
            }
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
            // After sort, duplicate names are adjacent — match
            // tree-walker by raising on dup-static-attr.
            for (uint32_t i = 1; i < n; ++i) {
                if (std::get<0>(entries[i]) == std::get<0>(entries[i - 1])) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = std::get<0>(entries[i]);
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT: attribute '" + s +
                                              "' already defined");
                }
            }
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
            publishToNearestBlackThunkFrame(vm, v);
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
            // Dup-attr detection: after sort, duplicates are adjacent.
            for (size_t i = 1; i < entries.size(); ++i) {
                if (std::get<0>(entries[i]) == std::get<0>(entries[i - 1])) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = std::get<0>(entries[i]);
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: attribute '" + s +
                                              "' already defined");
                }
            }
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
            publishToNearestBlackThunkFrame(vm, v);
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
            publishToNearestBlackThunkFrame(vm, v);
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
            // Tree-walker raises if __overrides is present but not an
            // attrset; v3 silently ignored.
            if (!ov.isAttrs())
                throw std::runtime_error("v3 OP_APPLY_OVERRIDES: __overrides must be an attrset");
            if (!ov.payload.bindings) break;
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
            // Force lazy shapes (Tag::App from mapAttrs entries, Thunks
            // from chained AttrSelects).  Same rationale as OP_CALL —
            // tree-walker forces target before AttrSelect; v3's lower
            // emits an explicit OP_FORCE most of the time, but App/Thunk
            // values can sneak through via OP_RETURN's no-chase
            // semantics.  Cheap on already-forced values.
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!attrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_SELECT: not an attrset");
            uint32_t icIdx = cu->code[ip++];
            auto & ic = cu->attrSelectCache[icIdx];
            const auto * b = attrs.payload.bindings;
            // V3_DBG_PREHOOK diagnostic: log every ATTRS_SELECT preHook
            // attempt with what value it returns.  Used to localize WC-37.
            static const bool s_dbg_prehook = std::getenv("V3_DBG_PREHOOK") != nullptr;
            if (s_dbg_prehook) {
                static const SymbolId preHookSym = ir::globalInternSymbol("preHook");
                if (operand == preHookSym) {
                    static int call_n = 0;
                    ++call_n;
                    std::fprintf(stderr,
                        "v3 ATTRS_SELECT preHook (#%d): bindings=%p size=%u attrs:\n",
                        call_n, (void*)b, b ? b->size : 0);
                    if (b) {
                        const auto & st = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 30; ++i) {
                            SymbolId nm = b->entries[i].name;
                            const Value & vv = b->entries[i].value;
                            Tag vtag = vv.tag();
                            std::fprintf(stderr, "  [%u] %s tag=%u",
                                i, nm < st.size() ? st[nm].c_str() : "?",
                                (unsigned)vtag);
                            if (vtag == Tag::Thunk && vv.payload.thunk) {
                                Thunk * t = vv.payload.thunk;
                                const LambdaDescriptor * d = nullptr;
                                if (t->state == ThunkState::Suspended)
                                    d = reinterpret_cast<const LambdaDescriptor *>(t->suspended.desc);
                                std::fprintf(stderr, " state=%d nUp=%u",
                                    (int)t->state, (unsigned)t->nUpvalues);
                                if (d)
                                    std::fprintf(stderr, " %s [%u..)",
                                        !d->name.empty() ? d->name.c_str() : "<anon>",
                                        d->codeOffset);
                                // For preHook entry [0], also dump the
                                // thunk's body bytecode + upvalue tags.
                                if (i == 0 && t->state == ThunkState::Suspended && d) {
                                    std::fprintf(stderr, "\n    body [%u..%u):\n",
                                        d->codeOffset, d->codeOffset + 200);
                                    if (t->suspended.cu)
                                        disassembleWindow(stderr, *t->suspended.cu,
                                            d->codeOffset, d->codeOffset + 200);
                                    // Dump the FUNCTION DESCRIPTORS of every
                                    // MAKE_THUNK target in this body — names
                                    // like "recref-X" tell us what each
                                    // upvalue resolves to in source.
                                    if (t->suspended.cu) {
                                        const auto & cu2 = *t->suspended.cu;
                                        std::fprintf(stderr, "    referenced functions:\n");
                                        for (uint32_t cur = d->codeOffset;
                                             cur < d->codeOffset + 80 && cur < cu2.code.size(); ) {
                                            Op op = decodeOp(cu2.code[cur]);
                                            uint32_t operand = decodeOperand(cu2.code[cur]);
                                            if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                                if (operand < cu2.lambdas.size()) {
                                                    const auto & d3 = cu2.lambdas[operand];
                                                    std::fprintf(stderr,
                                                        "      [%u] -> fn[%u] (%s, nUp=%u, code=[%u..))\n",
                                                        cur, operand,
                                                        !d3.name.empty() ? d3.name.c_str() : "<anon>",
                                                        d3.nUpvalues, d3.codeOffset);
                                                    // Also dump the body of recref- thunks
                                                    if (d3.name.find("recref-") == 0) {
                                                        std::fprintf(stderr, "        body:\n");
                                                        disassembleWindow(stderr, cu2,
                                                            d3.codeOffset, d3.codeOffset + 6);
                                                    }
                                                }
                                                cur += 2;
                                            } else {
                                                cur++;
                                            }
                                        }
                                    }
                                    std::fprintf(stderr, "    upvalues:\n");
                                    for (uint16_t u = 0; u < t->nUpvalues && u < 8; ++u) {
                                        const Value & uv = t->tail[u];
                                        Tag ut = uv.tag();
                                        std::fprintf(stderr, "      [%u] tag=%u", u, (unsigned)ut);
                                        if (ut == Tag::Closure && uv.payload.closure
                                            && uv.payload.closure->desc) {
                                            auto * cd = uv.payload.closure->desc;
                                            std::fprintf(stderr, " closure=%s [%u..) nUp=%u",
                                                !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                cd->codeOffset, uv.payload.closure->nUpvalues);
                                        } else if (ut == Tag::Thunk && uv.payload.thunk) {
                                            Thunk * ut2 = uv.payload.thunk;
                                            std::fprintf(stderr, " state=%d nUp=%u",
                                                (int)ut2->state, (unsigned)ut2->nUpvalues);
                                            if (ut2->state == ThunkState::Suspended) {
                                                auto * d2 = reinterpret_cast<const LambdaDescriptor *>(ut2->suspended.desc);
                                                if (d2)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d2->name.empty() ? d2->name.c_str() : "<anon>",
                                                        d2->codeOffset);
                                            } else if (ut2->state == ThunkState::Evaluated) {
                                                Tag et = ut2->evaluated.tag();
                                                std::fprintf(stderr, " EVAL=tag%u", (unsigned)et);
                                                if (et == Tag::Closure && ut2->evaluated.payload.closure
                                                    && ut2->evaluated.payload.closure->desc) {
                                                    auto * cd = ut2->evaluated.payload.closure->desc;
                                                    std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                        !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                        cd->codeOffset, ut2->evaluated.payload.closure->nUpvalues);
                                                }
                                            }
                                        } else if (ut == Tag::Attrs && uv.payload.bindings) {
                                            auto * b2 = uv.payload.bindings;
                                            std::fprintf(stderr, " attrs size=%u {", b2->size);
                                            const auto & st2 = ir::globalSymbolTable();
                                            for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                                SymbolId nm = b2->entries[k].name;
                                                std::fprintf(stderr, "%s%s",
                                                    k ? "," : "",
                                                    nm < st2.size() ? st2[nm].c_str() : "?");
                                            }
                                            std::fprintf(stderr, "}");
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                }
                                if (t->state == ThunkState::Evaluated) {
                                    Tag etag = t->evaluated.tag();
                                    std::fprintf(stderr, " EVAL=tag%u", (unsigned)etag);
                                    if (etag == Tag::Closure && t->evaluated.payload.closure
                                        && t->evaluated.payload.closure->desc) {
                                        auto * ed = t->evaluated.payload.closure->desc;
                                        std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                            !ed->name.empty() ? ed->name.c_str() : "<anon>",
                                            ed->codeOffset,
                                            t->evaluated.payload.closure->nUpvalues);
                                    } else if (etag == Tag::String && t->evaluated.payload.str) {
                                        std::fprintf(stderr, "(\"%.40s\")",
                                            t->evaluated.payload.str);
                                    }
                                }
                            } else if (vtag == Tag::Closure && vv.payload.closure
                                && vv.payload.closure->desc) {
                                auto * d = vv.payload.closure->desc;
                                std::fprintf(stderr, " %s [%u..) nUp=%u",
                                    !d->name.empty() ? d->name.c_str() : "<anon>",
                                    d->codeOffset, vv.payload.closure->nUpvalues);
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            // Inline-cache fast path: if the same Bindings* is hit
            // again, skip the binary search and read entries[lastSlot]
            // directly.  Cache miss falls back to lookup() and updates
            // the slot.
            //
            // V3_DBG_NO_IC disables the IC fast-path — useful for
            // bisecting whether IC corruption causes wrong-value bugs.
            static const bool s_no_ic = std::getenv("V3_DBG_NO_IC") != nullptr;
            if (!s_no_ic && ic.lastBindings == b
                && ic.lastSlot < b->size
                && b->entries[ic.lastSlot].name == static_cast<SymbolId>(operand))
            {
                push(vm, b->entries[ic.lastSlot].value);
            } else {
                // Manual binary search inlined to also recover the
                // matched slot index, so we can update the cache.
                uint32_t lo = 0, hi = b->size;
                while (lo < hi) {
                    uint32_t mid = (lo + hi) >> 1;
                    SymbolId midName = b->entries[mid].name;
                    if (midName == static_cast<SymbolId>(operand)) { lo = mid; break; }
                    if (midName < static_cast<SymbolId>(operand)) lo = mid + 1; else hi = mid;
                }
                if (lo >= b->size || b->entries[lo].name != static_cast<SymbolId>(operand)) {
                    // WC-21 diagnostic: dump requested attr + present
                    // attr names to help root-cause closure-bridge
                    // attr-shape divergences.  Off by default.
                    static const bool dbg = std::getenv("V3_DBG_ATTRS_SELECT") != nullptr;
                    if (dbg) {
                        auto & symTab = ir::globalSymbolTable();
                        SymbolId want = static_cast<SymbolId>(operand);
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_SELECT miss: want sid=%u name=\"%s\" "
                            "bindings=%p size=%u present=[",
                            (unsigned)want,
                            want < symTab.size() ? symTab[want].c_str() : "?",
                            (void*)b, (unsigned)b->size);
                        for (uint32_t i = 0; i < b->size && i < 20; ++i) {
                            SymbolId nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s",
                                i ? "," : "",
                                nm < symTab.size() ? symTab[nm].c_str() : "?");
                        }
                        if (b->size > 20) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "]\n");
                    }
                    throw std::runtime_error("v3 OP_ATTRS_SELECT: attribute not found");
                }
                ic.lastBindings = b;
                ic.lastSlot     = lo;
                push(vm, b->entries[lo].value);
            }
            break;
        }
        case OP_ATTRS_SELECT_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            // Force lazy `name` too — attrs.${dynKey} where dynKey is
            // `formal.cpu` (now lazy via mapAttrs Tag::App entries) was
            // landing in OP_ATTRS_SELECT_DYN with name still in App form
            // and tripping `not a string`.
            if (name.tag() == Tag::App || name.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
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
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            push(vm, (attrs.isAttrs() && attrs.payload.bindings->has(operand))
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_HAS_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            if (name.tag() == Tag::App || name.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            if (attrs.tag() == Tag::App || attrs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!name.isString() || !attrs.isAttrs()) { push(vm, Value::vFalse); break; }
            SymbolId id = ir::globalInternSymbol(name.payload.str);
            push(vm, attrs.payload.bindings->has(id)
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_UPDATE: {
            Value rhs = pop(vm), lhs = pop(vm);
            if (lhs.tag() == Tag::App || lhs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                lhs = forceValue(vm, lhs);
            }
            if (rhs.tag() == Tag::App || rhs.tag() == Tag::Thunk) {
                vm.frames.back().ip = ip;
                rhs = forceValue(vm, rhs);
            }
            if (!lhs.isAttrs() || !rhs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_UPDATE: not attrsets");
            Bindings * out = mergeBindings(lhs.payload.bindings, rhs.payload.bindings);
            allocStats().attrsetsAllocated++;
            Value v;
            v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
            v.payload.bindings = out;
            publishToNearestBlackThunkFrame(vm, v);
            push(vm, v);
            break;
        }

        // --- With ---
        case OP_WITH_PUSH: vm.withStack.push_back(pop(vm)); break;
        case OP_WITH_POP:  vm.withStack.pop_back(); break;
        case OP_LOAD_SLOT_REF: {
            // Push a Tag::Slot Value pointing at vm.valueStack[stackBase + operand].
            // The pointer remains valid as long as vm.valueStack does
            // not reallocate.  Phase 4 will address slot-stability for
            // pathological depth; for now we rely on the pre-reserved
            // capacity (vm.valueStack.reserve(64*1024)).
            Value v;
            v.mkSlot(&vm.valueStack[stackBase + operand]);
            push(vm, v);
            break;
        }
        case OP_WITH_LOOKUP: {
            uint32_t depth = cu->code[ip++];
            push(vm, withLookup(vm, static_cast<SymbolId>(operand), depth));
            break;
        }

        // --- Strings / pos / assert ---
        case OP_STR_CONCAT: {
            uint32_t n = operand >> 1;
            bool forceStr = (operand & 1u) != 0;
            // Ultra-fast path: 2 ints with no forceStr — covers every
            // arithmetic `a + b` over ints, which is the dominant case
            // on compute-bound benchmarks like fib.  Skip the small[]
            // setup, the loop, and the per-part type checks.  Match
            // tree-walker by raising on overflow.
            if (!forceStr && n == 2) {
                Value & top1 = vm.valueStack.back();
                Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
                if (top0.isInt() && top1.isInt()) {
                    int64_t sum;
                    if (__builtin_add_overflow(top0.payload.i, top1.payload.i, &sum))
                        throw std::runtime_error("v3 OP_STR_CONCAT: integer overflow");
                    vm.valueStack.pop_back();
                    vm.valueStack.back().mkInt(sum);
                    break;
                }
            }
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

            // Force lazy parts (Tag::App from mapAttrs/zipAttrsWith,
            // Tag::Thunk from lazy attr values).  Without this, a
            // string interpolation like `"${(map f xs)[0]}"` blows up
            // because map's entries are now Tag::App after the WC-35
            // fix.  Cheap on already-WHNF values.
            for (uint32_t i = 0; i < n; ++i) {
                Tag t = parts[i].tag();
                if (t == Tag::App || t == Tag::Thunk) {
                    vm.frames.back().ip = ip;
                    parts[i] = forceValue(vm, parts[i]);
                }
            }
            // V3_DBG_STRCONCAT: when a Closure leaks into STR_CONCAT
            // (which happens when v3's eval-order divergence forces a
            // function value where tree-walker keeps it lazy), dump
            // the call stack to localise the source.
            {
                static const bool s_dbg = std::getenv("V3_DBG_STRCONCAT") != nullptr;
                if (s_dbg) {
                    bool hasUncoercible = false;
                    for (uint32_t i = 0; i < n; ++i) {
                        Tag t = parts[i].tag();
                        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp || t == Tag::List)
                            { hasUncoercible = true; break; }
                    }
                    if (hasUncoercible) {
                        std::fprintf(stderr,
                            "v3 OP_STR_CONCAT pre-trace tags=[");
                        for (uint32_t i = 0; i < n; ++i)
                            std::fprintf(stderr, "%s%u", i ? "," : "", (unsigned)parts[i].tag());
                        std::fprintf(stderr, "] forceStr=%d frames=%zu callerIp=%u\n",
                            forceStr ? 1 : 0, vm.frames.size(), ip - 1);
                        // Dump current frame's upvalues / closure context.
                        if (!vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const Closure * cl = fr.closure;
                            const Thunk * th = fr.thunk;
                            uint16_t nUp = 0;
                            const Value * uvs = nullptr;
                            if (cl) { nUp = cl->nUpvalues; uvs = cl->upvalues; }
                            else if (th) { nUp = th->nUpvalues; uvs = th->tail; }
                            std::fprintf(stderr,
                                "  current-frame nUpvalues=%u\n", nUp);
                            for (uint16_t i = 0; i < nUp && i < 8; ++i) {
                                std::fprintf(stderr,
                                    "    upvalue[%u] tag=%u",
                                    i, (unsigned)uvs[i].tag());
                                if (uvs[i].tag() == Tag::Closure
                                    && uvs[i].payload.closure
                                    && uvs[i].payload.closure->desc) {
                                    std::fprintf(stderr, " closure=%s nUp=%u",
                                        !uvs[i].payload.closure->desc->name.empty()
                                            ? uvs[i].payload.closure->desc->name.c_str()
                                            : "<anon>",
                                        uvs[i].payload.closure->nUpvalues);
                                } else if (uvs[i].tag() == Tag::Attrs
                                           && uvs[i].payload.bindings) {
                                    auto * b2 = uvs[i].payload.bindings;
                                    std::fprintf(stderr, " attrs size=%u {",
                                        (unsigned)b2->size);
                                    const auto & st2 = ir::globalSymbolTable();
                                    for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                        SymbolId nm = b2->entries[k].name;
                                        std::fprintf(stderr, "%s%s",
                                            k ? "," : "",
                                            nm < st2.size() ? st2[nm].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "}");
                                    // For each attrs upvalue, also dump
                                    // tag of `preHook` slot (the bug
                                    // chases this attribute specifically).
                                    static const SymbolId preHookSym2 =
                                        ir::globalInternSymbol("preHook");
                                    const Value * ph = b2->lookup(preHookSym2);
                                    if (ph) {
                                        Tag pht = ph->tag();
                                        std::fprintf(stderr,
                                            " preHook=tag%u", (unsigned)pht);
                                        if (pht == Tag::Closure
                                            && ph->payload.closure
                                            && ph->payload.closure->desc) {
                                            auto * d3 = ph->payload.closure->desc;
                                            std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                d3->codeOffset,
                                                ph->payload.closure->nUpvalues);
                                        } else if (pht == Tag::Thunk && ph->payload.thunk) {
                                            Thunk * pt = ph->payload.thunk;
                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                (int)pt->state, (unsigned)pt->nUpvalues);
                                            if (pt->state == ThunkState::Suspended) {
                                                auto * d3 = reinterpret_cast<const LambdaDescriptor *>(
                                                    pt->suspended.desc);
                                                if (d3)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                        d3->codeOffset);
                                            } else if (pt->state == ThunkState::Evaluated) {
                                                std::fprintf(stderr, " EVAL=tag%u",
                                                    (unsigned)pt->evaluated.tag());
                                                // Recurse one level deep
                                                if (pt->evaluated.tag() == Tag::Thunk
                                                    && pt->evaluated.payload.thunk) {
                                                    Thunk * pt2 = pt->evaluated.payload.thunk;
                                                    std::fprintf(stderr, "(state=%d nUp=%u",
                                                        (int)pt2->state, (unsigned)pt2->nUpvalues);
                                                    if (pt2->state == ThunkState::Suspended) {
                                                        auto * d4 = reinterpret_cast<const LambdaDescriptor *>(
                                                            pt2->suspended.desc);
                                                        if (d4)
                                                            std::fprintf(stderr, " %s [%u..)",
                                                                !d4->name.empty() ? d4->name.c_str() : "<anon>",
                                                                d4->codeOffset);
                                                    } else if (pt2->state == ThunkState::Evaluated) {
                                                        std::fprintf(stderr, " EVAL=tag%u",
                                                            (unsigned)pt2->evaluated.tag());
                                                        if (pt2->evaluated.tag() == Tag::Closure
                                                            && pt2->evaluated.payload.closure
                                                            && pt2->evaluated.payload.closure->desc) {
                                                            auto * d5 = pt2->evaluated.payload.closure->desc;
                                                            std::fprintf(stderr, "(closure=%s [%u..) nUp=%u)",
                                                                !d5->name.empty() ? d5->name.c_str() : "<anon>",
                                                                d5->codeOffset,
                                                                pt2->evaluated.payload.closure->nUpvalues);
                                                        } else if (pt2->evaluated.tag() == Tag::Thunk
                                                            && pt2->evaluated.payload.thunk) {
                                                            Thunk * pt3 = pt2->evaluated.payload.thunk;
                                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                                (int)pt3->state, (unsigned)pt3->nUpvalues);
                                                            if (pt3->state == ThunkState::Suspended) {
                                                                auto * d6 = reinterpret_cast<const LambdaDescriptor *>(
                                                                    pt3->suspended.desc);
                                                                if (d6)
                                                                    std::fprintf(stderr, " %s [%u..)",
                                                                        !d6->name.empty() ? d6->name.c_str() : "<anon>",
                                                                        d6->codeOffset);
                                                            }
                                                            std::fprintf(stderr, ")");
                                                        }
                                                    }
                                                    std::fprintf(stderr, ")");
                                                }
                                            }
                                            std::fprintf(stderr, ")");
                                        }
                                    }
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                        size_t lim = vm.frames.size();
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                            else if (fr.closure) d = fr.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                                i - 1,
                                d && !d->name.empty() ? d->name.c_str()
                                    : (d ? "<anon>" : "<closure-body>"),
                                d ? d->codeOffset : 0, fr.ip,
                                (unsigned)fr.flags);
                        }
                        // Disasm from the frame's prologue to the failing
                        // OP_STR_CONCAT — full body lets us trace slot
                        // assignments back to their source.
                        if (cu && !vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                            else if (fr.closure) d = fr.closure->desc;
                            uint32_t lo = d ? d->codeOffset : (ip > 32 ? ip - 32 : 0);
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr,
                                "  current frame disasm [%u..%u) (prologue→ip):\n", lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm functions referenced by MAKE_THUNK
                            // / MAKE_CLOSURE in the prologue — these are
                            // the inner thunks that produce slot values.
                            std::fprintf(stderr, "  --- referenced functions ---\n");
                            for (uint32_t cur = lo; cur < hi - 1; ) {
                                Op op = decodeOp(cu->code[cur]);
                                uint32_t operand = decodeOperand(cu->code[cur]);
                                if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                    if (operand < cu->lambdas.size()) {
                                        const LambdaDescriptor & d2 = cu->lambdas[operand];
                                        uint32_t flo = d2.codeOffset;
                                        uint32_t fhi = flo + 24;
                                        std::fprintf(stderr,
                                            "  fn[%u] (%s, nUp=%u) [%u..%u):\n",
                                            operand,
                                            !d2.name.empty() ? d2.name.c_str() : "<anon>",
                                            d2.nUpvalues, flo, fhi);
                                        disassembleWindow(stderr, *cu, flo, fhi);
                                    }
                                    cur += 2;  // op + nUp data
                                } else {
                                    cur++;
                                }
                            }
                        }
                    }
                }
            }

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
                    for (uint32_t i = 0; i < n; ++i) {
                        if (__builtin_add_overflow(sum, parts[i].payload.i, &sum))
                            throw std::runtime_error("v3 OP_STR_CONCAT: integer overflow");
                    }
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
            // Accumulate string contexts from all parts.  Path parts
            // produce a fresh Opaque entry (the store path of the
            // copied content); String parts inherit any context their
            // payload buffer was tagged with.  Attrset parts coerce
            // via __toString/outPath like before — we treat the
            // resulting string identically.
            std::vector<std::string> ctxAccum;
            auto addCtx = [&](const std::vector<std::string> * v) {
                if (!v) return;
                for (auto & s : *v) ctxAccum.push_back(s);
            };
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
                        if (s.isString()) {
                            out.append(s.payload.str);
                            addCtx(lookupStringContextEntries(s.payload.str));
                            continue;
                        }
                    }
                    if (auto * op = p.payload.bindings->lookup(outId)) {
                        Value forced = forceValue(vm, *op);
                        if (forced.isString()) {
                            out.append(forced.payload.str);
                            addCtx(lookupStringContextEntries(forced.payload.str));
                            continue;
                        }
                        if (forced.isPath())   { out.append(forced.payload.path); continue; }
                    }
                }
                if (p.isString())
                    addCtx(lookupStringContextEntries(p.payload.str));
                if (p.isPath() && forceStr) {
                    // coerceToString will copy this path to the store and
                    // produce its `/nix/store/...` representation; tag the
                    // resulting string with that store path as an Opaque
                    // context entry.  Encoded form is the StorePath's
                    // basename (`<hash>-<name>`) — what
                    // NixStringContextElem::to_string()/parse roundtrip.
                    // Exceptions propagate: tree-walker raises on missing
                    // paths during interpolation, so v3 must too.
                    if (auto * ns = getNixEvalState()) {
                        nix::NixStringContext tmp;
                        nix::SourcePath sp(ns->rootFS,
                                            nix::CanonPath(p.payload.path ? p.payload.path : ""));
                        auto storePath = ns->copyPathToStore(tmp, sp);
                        ctxAccum.push_back(std::string(storePath.to_string()));
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
                // De-duplicate context entries (a sorted-unique pass) and
                // record on the new buffer.  Empty input → no entry left.
                if (!ctxAccum.empty()) {
                    std::sort(ctxAccum.begin(), ctxAccum.end());
                    ctxAccum.erase(std::unique(ctxAccum.begin(), ctxAccum.end()),
                                   ctxAccum.end());
                    setStringContextEntries(buf, std::move(ctxAccum));
                }
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

        case OP_LIT_BUILTINS: {
            // Lazy singleton: build the `builtins` attrset on first
            // execution, reuse it for every subsequent reference.
            // Same shape every time (every registered primop), so
            // sharing is safe.  Static lifetime — never freed.
            static Value vBuiltins = []{
                const auto & reg = allRegisteredPrimOps();
                Bindings * b = Alloc::allocBindings(static_cast<uint32_t>(reg.size()));
                uint32_t i = 0;
                for (auto & [poName, po] : reg) {
                    Value v;
                    v.tag_payload = static_cast<uint64_t>(Tag::PrimOp);
                    v.payload.primop = &po;
                    SymbolId sid = ir::globalInternSymbol(poName);
                    b->entries[i] = { sid, v };
                    ++i;
                }
                // Bindings expects entries to be sorted by SymbolId for
                // O(log n) lookup via binary search.  std::sort is fine
                // here — runs once at process startup.
                std::sort(&b->entries[0], &b->entries[b->size],
                    [](const auto & a, const auto & b){ return a.name < b.name; });
                Value v;
                v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                v.payload.bindings = b;
                return v;
            }();
            push(vm, vBuiltins);
            break;
        }

        case OP_CALL_PRIMOP: {
            uint32_t nArgs = operand;
            uint32_t poIdx = cu->code[ip++];
            const PrimOp * po = cu->primops[poIdx];
            // Profiling counter (gated on NIX_VM_STATS at process exit).
            // The bump is unconditional — the primop dispatch already
            // does substantially more work, so the cost is invisible.
            bumpPrimOpCallCount(po);
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

// WC-5: clear Black marks on any thunk frames currently in the VM.
// Used as the exception-recovery hook around dispatchLoop calls.
// Tree-walker's mkFailed stores the exception and re-throws; we
// take the cheaper-but-still-correct path of reverting Black to
// Suspended so the next force re-runs (idempotent throws then
// re-throw the same error).
static void clearBlackMarksOnException(VMState & vm, size_t exitDepth)
{
    for (size_t i = vm.frames.size(); i > exitDepth; --i) {
        auto & fr = vm.frames[i - 1];
        if ((fr.flags & CFF_THUNK_RETURN) && fr.thunk
            && fr.thunk->state == ThunkState::Blackhole) {
            fr.thunk->state = ThunkState::Suspended;
        }
    }
    // WC-37: also unwind the leftover frames pushed by the failed
    // dispatchLoop call.  Without this, a thunk frame that was on
    // the stack when the exception was thrown remains as a "ghost
    // frame" — when a subsequent OP_RETURN in an outer dispatchLoop
    // pops vm.frames.back(), it pops the ghost frame, finds
    // CFF_THUNK_RETURN + a thunk pointer, and stores whatever was on
    // the value stack into that thunk's evaluated slot.  In the
    // pure-VM nixpkgs case this corrupts the +chain preHook thunk
    // (codeOffset=1346, nUp=5) by storing a closure (e.g.
    // bintoolsPackages's body's MAKE_CLOSURE result) where a string
    // was expected.  Resize valueStack & withStack to the FIRST
    // popped frame's bases (= the stack/with depth when that frame
    // was pushed), preserving everything below.
    if (vm.frames.size() > exitDepth) {
        const auto & firstPopped = vm.frames[exitDepth];
        uint32_t targetStackBase = firstPopped.stackBaseOffset;
        uint32_t targetWithBase  = firstPopped.withStackBase;
        vm.frames.resize(exitDepth);
        if (vm.valueStack.size() > targetStackBase)
            vm.valueStack.resize(targetStackBase);
        if (vm.withStack.size() > targetWithBase)
            vm.withStack.resize(targetWithBase);
    }
}

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

    try {
        Value r = dispatchLoop(vm, /*exitDepth=*/0);
        // WC-15 defensive: even on success, residual Black marks
        // can persist on the frame stack from incomplete sub-evals
        // that were intentionally orphaned (e.g., transitive thunk
        // chains where intermediate frames don't reach OP_RETURN).
        // Reset them so subsequent forces of the same Thunk don't
        // see a stale Black mark.
        clearBlackMarksOnException(vm, 0);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, 0);
        throw;
    }
}

/// CO-3: run an arbitrary FuncId in `cu` as if it were a thunk body.
/// No upvalues, no args.  Used by the forceValue cutover hook for
/// per-thunk-body Functions whose `nUpvalues == 0` — i.e., closed
/// thunks the lowerer recorded in `Module::subExprFuncs`.
Value runFunction(const CompilationUnit & cu, uint32_t funcIdx)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunction: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != 0)
        throw std::runtime_error("v3 runFunction: function expects upvalues; use runFunctionWithUpvalues");

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    vm.valueStack.resize(desc.nLocals);

    try {
        Value r = dispatchLoop(vm, /*exitDepth=*/0);
        // WC-15 defensive: even on success, residual Black marks
        // can persist on the frame stack from incomplete sub-evals
        // that were intentionally orphaned (e.g., transitive thunk
        // chains where intermediate frames don't reach OP_RETURN).
        // Reset them so subsequent forces of the same Thunk don't
        // see a stale Black mark.
        clearBlackMarksOnException(vm, 0);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, 0);
        throw;
    }
}

/// CO-2 phase B: run a per-thunk Function with caller-provided
/// upvalues.  The forceValue cutover walks tree-walker's Env to
/// collect upvalue values, then calls here.  We synthesize a
/// Closure on the heap (allocated via Boehm GC; lives as long as
/// the call's frame), point the frame's closure to it, and run.
Value runFunctionWithUpvalues(const CompilationUnit & cu, uint32_t funcIdx,
                               const Value * upvalues, uint32_t nUpvalues)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunctionWithUpvalues: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != nUpvalues)
        throw std::runtime_error("v3 runFunctionWithUpvalues: nUpvalues mismatch");

    Closure * fakeClo = Alloc::allocClosure(nUpvalues);
    fakeClo->desc = &desc;
    fakeClo->cu   = &cu;
    fakeClo->capturedWiths = nullptr;
    fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
    for (uint32_t i = 0; i < nUpvalues; ++i)
        fakeClo->upvalues[i] = upvalues[i];

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    vm.valueStack.resize(desc.nLocals);

    try {
        Value r = dispatchLoop(vm, /*exitDepth=*/0);
        // WC-15 defensive: even on success, residual Black marks
        // can persist on the frame stack from incomplete sub-evals
        // that were intentionally orphaned (e.g., transitive thunk
        // chains where intermediate frames don't reach OP_RETURN).
        // Reset them so subsequent forces of the same Thunk don't
        // see a stale Black mark.
        clearBlackMarksOnException(vm, 0);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, 0);
        throw;
    }
}

Value forceValue(VMState & vm, Value v)
{
    // Loop until WHNF: a thunk's body might itself yield a thunk
    // (e.g., `let inherit outer; in outer` returns the outer thunk),
    // and we want to chase the chain until we land on a real value.
    while (true) {
        // Same call-depth guard — `let x = x; in x` lands here in
        // a C++ recursion via dispatchLoop → forceValue → dispatchLoop
        // and never grows through the bytecode-level OP_CALL/OP_FORCE
        // guards.  Match those guards.
        if (__builtin_expect(vm.frames.size() >= 5000, 0))
            throw std::runtime_error("v3 forceValue: stack overflow; call depth exceeded 5000");
        // Tag::Slot — SECD-style indirection.  The slot pointer
        // references another stable Value that gets mutated when its
        // let-rec body publishes a result.  Dereference and continue
        // chasing.  See `with self;` semantics in Tag::Slot's docstring
        // for why this matters: sub-thunks captured under `with self;`
        // must observe the latest slot contents at force time, not a
        // snapshot from when the with-stack was pushed.
        if (v.tag() == Tag::Slot) {
            Value * p = v.payload.slot;
            if (!p) throw std::runtime_error("v3 forceValue: null slot pointer");
            v = *p;
            continue;
        }
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
        if (t->state == ThunkState::Blackhole) {
            // Same diagnostic as OP_FORCE's blackhole path — V3_DBG_OPCYCLE
            // dumps the frame stack so the cycle source is visible.
            static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
            if (s_dbg) {
                auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t fip) -> std::string {
                    const LambdaDescriptor * desc = nullptr;
                    if (th) desc = reinterpret_cast<const LambdaDescriptor *>(th->suspended.desc);
                    else if (cl) desc = cl->desc;
                    if (!desc) return "<closure-body>";
                    char buf[256];
                    std::snprintf(buf, sizeof buf,
                        "%s code=[%u..) nUp=%u nLocals=%u",
                        !desc->name.empty() ? desc->name.c_str() : "<anon>",
                        desc->codeOffset, desc->nUpvalues, desc->nLocals);
                    return buf;
                };
                std::fprintf(stderr,
                    "v3 forceValue Black thunk=%p frames=%zu\n",
                    (void*)t, vm.frames.size());
                size_t lim = vm.frames.size();
                ssize_t blackIdx = -1;
                for (size_t i = lim; i > 0; --i) {
                    const auto & fr = vm.frames[i - 1];
                    bool isBlack = (fr.thunk == t);
                    if (isBlack) blackIdx = (ssize_t)(i - 1);
                    std::fprintf(stderr,
                        "  frame[%zu]:%s %s flags=%u ip=%u thunk=%p\n",
                        i - 1, isBlack ? " <-BLACK" : "",
                        frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                        (unsigned)fr.flags, fr.ip, (void*)fr.thunk);
                }
                static const bool s_dbg_disasm =
                    std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                if (s_dbg_disasm && blackIdx >= 0) {
                    // Dump the BLACK frame's prologue (start of body)
                    // through current ip — captures every OP_FORCE the
                    // body ran before re-entering itself.
                    const auto & fr = vm.frames[blackIdx];
                    if (fr.cu) {
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        if (desc) {
                            uint32_t lo = desc->codeOffset;
                            uint32_t hi = fr.ip + 8;
                            std::fprintf(stderr,
                                "  BLACK frame[%zd] disasm [%u..%u) (prologue→ip):\n",
                                blackIdx, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                    }
                    // Also dump the innermost frame's prologue → ip.
                    const auto & inner = vm.frames.back();
                    if (inner.cu) {
                        const LambdaDescriptor * idesc = nullptr;
                        if (inner.thunk)
                            idesc = reinterpret_cast<const LambdaDescriptor *>(inner.thunk->suspended.desc);
                        else if (inner.closure)
                            idesc = inner.closure->desc;
                        if (idesc) {
                            uint32_t lo = idesc->codeOffset;
                            uint32_t hi = inner.ip + 8;
                            std::fprintf(stderr,
                                "  INNER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                lim - 1, lo, hi);
                            disassembleWindow(stderr, *inner.cu, lo, hi);
                        }
                    }
                    // Also dump frame[33] — caller of innermost.  Often
                    // the App's `left` was a closure call return, which
                    // is the actual divergence source.
                    if (lim >= 2) {
                        const auto & f33 = vm.frames[lim - 2];
                        if (f33.cu) {
                            const LambdaDescriptor * d33 = nullptr;
                            if (f33.thunk)
                                d33 = reinterpret_cast<const LambdaDescriptor *>(f33.thunk->suspended.desc);
                            else if (f33.closure)
                                d33 = f33.closure->desc;
                            if (d33) {
                                uint32_t lo = d33->codeOffset;
                                uint32_t hi = f33.ip + 8;
                                std::fprintf(stderr,
                                    "  CALLER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                    lim - 2, lo, hi);
                                disassembleWindow(stderr, *f33.cu, lo, hi);
                            }
                        }
                    }
                }
            }
            throw std::runtime_error("v3 forceValue: infinite recursion (blackhole)");
        }
        if (t->state == ThunkState::Bridge) {
            v = forceBridgeThunk(t);
            t->state = ThunkState::Evaluated;
            t->evaluated = v;
            continue;
        }

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

        {
            static const bool s_dbg_fv =
                std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
            if (s_dbg_fv && t->nUpvalues == 5) {
                std::fprintf(stderr,
                    "v3 forceValue: pushing thunk %p desc=%s codeOffset=%u nUp=%u cu=%p\n",
                    (void*)t,
                    !desc->name.empty() ? desc->name.c_str() : "<anon>",
                    desc->codeOffset, (unsigned)t->nUpvalues,
                    (void*)thunkCu);
            }
        }

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

        // WC-5: if dispatchLoop throws, every thunk frame we'd unwind
        // is currently marked Blackhole.  Without cleanup, a later
        // force of the same thunk (e.g. when tree-walker takes over
        // and accesses the same lib attr) would hit the stale mark
        // and report "infinite recursion (blackhole)" — masking the
        // real error.  Tree-walker's mkFailed stores the exception
        // and re-throws on subsequent forces (eval-inline.hh:125);
        // the bare-minimum equivalent here is to revert each frame's
        // thunk back to Suspended so the next force re-runs.
        //
        // We don't store the exception (would need a Failed state
        // and re-throw machinery), so the next force simply re-runs
        // the body — slow but correct, and idempotent throws will
        // re-throw the same error consistently.
        try {
            v = dispatchLoop(vm, exitDepth);
        } catch (...) {
            // WC-37: clear blackmarks AND unwind the leftover frames
            // so they can't become "ghost frames" picked up by a later
            // OP_RETURN in an outer dispatchLoop (which would corrupt
            // the thunk pointed-to by the ghost frame).
            clearBlackMarksOnException(vm, exitDepth);
            // Also clear the outer Black mark we set just above.
            if (t->state == ThunkState::Blackhole)
                t->state = ThunkState::Suspended;
            throw;
        }
        // WC-14.5 success-path defensive cleanup: if the outer thunk
        // somehow remains Black after a successful dispatchLoop
        // (theoretical impossibility per the invariant, but observed
        // in cross-VMState bridge scenarios where another VMState's
        // frames interleave with this one), revert it to Suspended
        // so subsequent forces re-run idempotently rather than
        // throwing "infinite recursion (blackhole)" on a stale mark.
        if (t->state == ThunkState::Blackhole) {
            static const bool s_dbg = std::getenv("V3_DBG_BLACK") != nullptr;
            if (s_dbg) std::fprintf(stderr,
                "v3 forceValue: SUCCESS-path Black leak; reverting "
                "thunk=%p Suspended\n", (void*)t);
            t->state = ThunkState::Suspended;
        }
    }
    return v;
}

Value callClosure(VMState & vm, Value fun, Value arg)
{
    // Mirror tree-walker's `callFunction`: callable values must be in
    // WHNF before we dispatch on shape.  Most callers force first
    // (OP_CALL's preceding OP_FORCE; OP_RETURN's transitive chase),
    // but a few internal paths (the __functor recursion below; primop
    // map-style App entries forced inline) leave a Tag::Thunk or
    // Tag::App on `fun`.  forceValue is idempotent on already-WHNF
    // values, so the cost is one tag check on the hot path.
    fun = forceValue(vm, fun);
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
        for (uint32_t i = 0; i < po->arity; ++i) {
            if (po->lazyArgs & (1u << i)) continue;
            buf[i] = forceValue(vm, buf[i]);
        }
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

    if (!fun.isClosure()) {
        static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
        if (dbg) {
            std::fprintf(stderr,
                "v3 callClosure: not callable tag=%u frames=%zu\n",
                (unsigned)fun.tag(), vm.frames.size());
            size_t lim = vm.frames.size();
            for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                const auto & fr = vm.frames[i - 1];
                const LambdaDescriptor * desc = nullptr;
                if (fr.thunk)
                    desc = reinterpret_cast<const LambdaDescriptor *>(fr.thunk->suspended.desc);
                else if (fr.closure)
                    desc = fr.closure->desc;
                std::fprintf(stderr,
                    "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                    i - 1,
                    desc && !desc->name.empty() ? desc->name.c_str()
                        : (desc ? "<anon>" : "<closure-body>"),
                    desc ? desc->codeOffset : 0,
                    fr.ip, (unsigned)fr.flags);
            }
        }
        throw std::runtime_error("v3 callClosure: not callable");
    }

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

    try {
        return dispatchLoop(vm, exitDepth);
    } catch (...) {
        clearBlackMarksOnException(vm, exitDepth);
        throw;
    }
}

} // namespace nix::v3
