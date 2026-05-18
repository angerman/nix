/// @file
/// IR optimisation pass: stream fusion (Phase C).
///
/// Recognises `foldl'(op, init, map(f, xs))` patterns and rewrites
/// to `__foldlMap(op, init, f, xs)` — a single-pass FFI leaf that
/// fuses the map and foldl' loops.  The intermediate map result list
/// is never allocated, saving N ValuePair allocations + N callClosure
/// invocations + one list traversal for an N-element list.
///
/// Why this matters: nixpkgs / stdenv code does
///   `foldl' op init (map f xs)`
/// repeatedly inside the Option 4 derivation wrapper (env-attrset
/// construction, args list coerce, output-list mapping).  Each
/// invocation, pre-fusion, allocates an intermediate N-element list
/// with N Tag::App entries — visible in the alloc stats and the
/// dominant cost on the v3-vs-TW hello.name perf gap.  After fusion,
/// allocations drop to zero for these calls; the FFI leaf walks `xs`
/// once and feeds each (f x) directly to op.
///
/// Pattern (matched per binding, within one block):
///
///     v_map = PrimOpCall(map, [f, xs])
///     v_foldl = PrimOpCall(foldl', [op, init, v_map])
///   →
///     v_foldl = PrimOpCall(__foldlMap, [op, init, f, xs])
///
/// Safety preconditions (ALL must hold):
///   1. v_foldl's expr is `PrimOpCall(foldl', [op, init, listArg])`.
///   2. listArg resolves (within the same block, via VarRef chase) to
///      `PrimOpCall(map, [f, xs])`.
///   3. The map result's VarId is used EXACTLY ONCE — only by the
///      foldl' we're fusing.  If used elsewhere, the map's
///      observable behavior must be preserved (we'd duplicate work).
///   4. Both `map` and `foldl'` resolve to primops in the v3
///      registry (we look up `__foldlMap` to confirm it's present).
///
/// The match is conservative — only the exact `foldl' op init (map
/// f xs)` shape, no `map (map ...)`-chain fusion (which would need
/// a recursive walk and additional cases for `concatMap` /
/// `filter`).  Phase C+ extensions can be added incrementally.
///
/// Gate: NIX_V3_NO_STREAM_FUSION=1 disables for A/B measurement.
///
/// Pipeline placement: AFTER fusePrimOpApps (so we see canonical
/// PrimOpCall shapes for both `map` and `foldl'`).  Runs alongside
/// primOpFold (Phase B).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_dump.hh"
#include "v3/primop.hh"

#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Same-block VarRef chase — local utility identical to opt_beta_reduce.cc
// and opt_primop_fold.cc.  Returns the resolved Expr* or nullptr.
// ---------------------------------------------------------------------------

const Expr * chaseInBlock(VarId v,
                          const std::unordered_map<VarId, const Expr *> & defs)
{
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        auto it = defs.find(v);
        if (it == defs.end()) return nullptr;
        const Expr * e = it->second;
        if (const auto * vr = std::get_if<VarRef>(e)) {
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

std::unordered_map<VarId, const Expr *> mapBlockDefs(const Block & b)
{
    std::unordered_map<VarId, const Expr *> defs;
    defs.reserve(b.bindings.size());
    for (const auto & bd : b.bindings)
        defs.emplace(bd.var, &bd.expr);
    return defs;
}

// ---------------------------------------------------------------------------
// Lookup the __foldlMap PrimOp by name.  Cached after first call —
// the v3 PrimOp registry doesn't change at runtime.
// ---------------------------------------------------------------------------

const PrimOp * foldlMapPrimOp()
{
    static const PrimOp * cached = findPrimOp("__foldlMap");
    return cached;
}

// Recognise a "primop call with N args" at VarId `v`, accepting both:
//   - The canonical PrimOpCall(p, [args]) shape  (opt_primop_fuse output)
//   - The unfused App-chain App(App(...App(LitPrimOp{p}, a0), ...), aN-1)
//
// Returns the primop pointer + arg VarIds in call order on success,
// or std::nullopt.  Caller checks the primop name + arity.
//
// Why both shapes: opt_primop_fuse.cc SKIPS primops that have a
// bytecode-closure replacement installed (via installBytecodePrimop
// in bytecode_primops.cc) — `foldl'` and `map` are exactly those.
// Such primops stay in App-chain form even after the fuse pass.  This
// helper bridges both forms so streamFusion catches them uniformly.
struct PrimopCallShape {
    const PrimOp * primop = nullptr;
    std::vector<VarId> args;
    // The "outermost" VarId at which the call is rooted — for the
    // PrimOpCall form, this is the binding's `var`; for the App-chain
    // form, it's the VarId of the saturated-call App binding.  Used
    // by the use-once safety check.
    VarId rootVar = kInvalid;
};

// Pass `m` by reference because MkThunk recognition needs to walk
// into the thunk's body Function (which lives in m.functions /
// m.blocks).  The thunk's body upvalues are VarIds from the OUTER
// scope (the IR lowerer doesn't rebind them; the body block's
// expressions reference the outer VarIds directly), so extracting
// call args from the thunk body is valid at the call site —
// provided the body is "simple" (just the call + its trivial
// setup, with no nested control flow).
std::optional<PrimopCallShape> recogniseCall(
    VarId v,
    const Module & m,
    const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(v, defs);
    if (!e) return std::nullopt;

    // Form 0: MkThunk wrapping a single trivial call.  thunkifyForArg
    // (lower.cc:1640) wraps non-trivial arg expressions in a thunk;
    // the most common shape for `foldl' op init (map f xs)` is a
    // thunk wrapping `App(App(LitPrimOp{map}, f), xs)` in its body
    // block.  Recognise that case by chasing into the thunk body's
    // TermReturn target.
    //
    // Safety: we only descend into thunks whose body block contains
    // EXCLUSIVELY the bindings forming the call — no extra side-
    // effecting bindings, no nested thunks/lambdas/control flow.
    // Each binding must be either a LitPrimOp or an App over already-
    // seen VarIds.
    // Tag the variant kind for diagnostics.
    static const bool s_dbg2 =
        std::getenv("V3_DBG_STREAM_FUSION") != nullptr;
    if (s_dbg2) {
        std::fprintf(stderr, "  recogniseCall var=%u kind=%zu\n",
            (unsigned)v, e->index());
    }
    if (const auto * th = std::get_if<MkThunk>(e)) {
        if (th->funcIdx == 0 || th->funcIdx >= m.functions.size())
            return std::nullopt;
        const Function & f = m.functions[th->funcIdx];
        if (f.entryBlock == kInvalidBlock || f.entryBlock >= m.blocks.size())
            return std::nullopt;
        const Block & body = m.blocks[f.entryBlock];
        // Allow body bindings that are "hoistable" — i.e. their RHS
        // expression CAN be moved unchanged into the outer block.
        // VarRef / LitPrimOp / App / Force / Lambda / MkThunk /
        // LitInt / LitString / LitFloat / LitBool / LitNull / LitPath /
        // ListExpr / AttrSet — all of these are pure IR data whose
        // VarId operands are either body-local (allocated by the
        // lowerer in this body block) or are outer-scope upvalues
        // (the lowerer references outer VarIds directly).  Hoisting
        // them is sound because their semantics doesn't depend on
        // the body-block's lifetime.
        //
        // REFUSE bindings that carry SUB-BLOCK references (If / With
        // / Assert / And / Or / Impl / LetRec) — hoisting those would
        // tangle the body's sub-blocks into the outer block.  Phase
        // C scope: trivial map-of-lambda over list constructions.
        for (const auto & bd : body.bindings) {
            bool ok = std::holds_alternative<VarRef>(bd.expr)
                   || std::holds_alternative<LitPrimOp>(bd.expr)
                   || std::holds_alternative<App>(bd.expr)
                   || std::holds_alternative<Force>(bd.expr)
                   || std::holds_alternative<Lambda>(bd.expr)
                   || std::holds_alternative<MkThunk>(bd.expr)
                   || std::holds_alternative<LitInt>(bd.expr)
                   || std::holds_alternative<LitFloat>(bd.expr)
                   || std::holds_alternative<LitBool>(bd.expr)
                   || std::holds_alternative<LitNull>(bd.expr)
                   || std::holds_alternative<LitString>(bd.expr)
                   || std::holds_alternative<LitPath>(bd.expr)
                   || std::holds_alternative<ListExpr>(bd.expr)
                   || std::holds_alternative<AttrSet>(bd.expr)
                   || std::holds_alternative<AttrSelect>(bd.expr)
                   || std::holds_alternative<LitBuiltins>(bd.expr)
                   || std::holds_alternative<RecBindingSlotRef>(bd.expr);
            if (!ok) {
                if (s_dbg2) {
                    std::fprintf(stderr,
                        "  recogniseCall (MkThunk body): binding var=%u "
                        "kind=%zu not hoistable — skip\n",
                        (unsigned)bd.var, bd.expr.index());
                }
                return std::nullopt;
            }
        }
        // Build defs for the body block (now that all bindings are
        // hoistable).  Recurse into the body with the body's defs.
        std::unordered_map<VarId, const Expr *> bodyDefs;
        bodyDefs.reserve(body.bindings.size());
        for (const auto & bd : body.bindings)
            bodyDefs.emplace(bd.var, &bd.expr);
        const auto * term = std::get_if<TermReturn>(&body.terminal);
        if (!term || term->value == kInvalid) return std::nullopt;
        return recogniseCall(term->value, m, bodyDefs);
    }

    // Form 1: PrimOpCall.  Cheap path; opt_primop_fuse emitted it
    // for primops without bytecode-closure replacements.
    if (const auto * pc = std::get_if<PrimOpCall>(e)) {
        if (!pc->primop) return std::nullopt;
        PrimopCallShape r;
        r.primop = pc->primop;
        r.args = pc->args;
        r.rootVar = v;
        return r;
    }

    // Form 2: App-chain.  Walk down the .fun chain collecting .arg
    // values until the leaf is a LitPrimOp.
    std::vector<VarId> argsReversed;
    const Expr * cur = e;
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        if (const auto * app = std::get_if<App>(cur)) {
            argsReversed.push_back(app->arg);
            const Expr * funE = chaseInBlock(app->fun, defs);
            if (!funE) return std::nullopt;
            cur = funE;
            continue;
        }
        if (const auto * lp = std::get_if<LitPrimOp>(cur)) {
            if (!lp->primop) return std::nullopt;
            PrimopCallShape r;
            r.primop = lp->primop;
            r.args.assign(argsReversed.rbegin(), argsReversed.rend());
            r.rootVar = v;
            return r;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Count uses of `v` across the entire Module — same as opt_beta_reduce.cc
// (we duplicate rather than share because the use counts there
// excluded freeVars; here we likewise focus on PRE-computeFreeVars
// shape).
struct UseCounter {
    std::unordered_map<VarId, uint32_t> count;
    void bump(VarId v) { ++count[v]; }
    uint32_t at(VarId v) const {
        auto it = count.find(v);
        return it == count.end() ? 0 : it->second;
    }
};

void countOperandsExpr(const Expr & e, UseCounter & uc)
{
    std::visit([&](const auto & v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, VarRef>) uc.bump(v.var);
        else if constexpr (std::is_same_v<T, Force>) uc.bump(v.thunk);
        else if constexpr (std::is_same_v<T, AttrSelect>) uc.bump(v.attrs);
        else if constexpr (std::is_same_v<T, HasAttr>) uc.bump(v.attrs);
        else if constexpr (std::is_same_v<T, RecBindingSlotRef>) uc.bump(v.attrs);
        else if constexpr (std::is_same_v<T, Not>) uc.bump(v.operand);
        else if constexpr (std::is_same_v<T, App>) { uc.bump(v.fun); uc.bump(v.arg); }
        else if constexpr (std::is_same_v<T, AttrSelectDyn>) { uc.bump(v.attrs); uc.bump(v.nameVar); }
        else if constexpr (std::is_same_v<T, HasAttrDyn>) { uc.bump(v.attrs); uc.bump(v.nameVar); }
        else if constexpr (std::is_same_v<T, ConcatLists>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Update>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Add>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Sub>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Mul>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Div>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Eq>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, NEq>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, Less>) { uc.bump(v.lhs); uc.bump(v.rhs); }
        else if constexpr (std::is_same_v<T, And>) uc.bump(v.lhs);
        else if constexpr (std::is_same_v<T, Or>) uc.bump(v.lhs);
        else if constexpr (std::is_same_v<T, Impl>) uc.bump(v.lhs);
        else if constexpr (std::is_same_v<T, If>) uc.bump(v.cond);
        else if constexpr (std::is_same_v<T, Assert>) uc.bump(v.cond);
        else if constexpr (std::is_same_v<T, ListExpr>) for (auto x : v.elems) uc.bump(x);
        else if constexpr (std::is_same_v<T, ConcatStrings>) for (auto x : v.parts) uc.bump(x);
        else if constexpr (std::is_same_v<T, PrimOpCall>) for (auto x : v.args) uc.bump(x);
        else if constexpr (std::is_same_v<T, AttrSet>)
            for (const auto & ent : v.entries) uc.bump(ent.value);
        else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) {
            uc.bump(v.attrSetVar);
            for (const auto & ent : v.entries) uc.bump(ent.valueVar);
        }
        else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (const auto & s : v.statics)  uc.bump(s.value);
            for (const auto & d : v.dynamics) { uc.bump(d.nameVar); uc.bump(d.value); }
        }
        else if constexpr (std::is_same_v<T, Lambda>) for (auto x : v.freeVars) uc.bump(x);
        else if constexpr (std::is_same_v<T, MkThunk>) for (auto x : v.freeVars) uc.bump(x);
        else { (void)v; }
    }, e);
}

UseCounter countModuleUses(const Module & m)
{
    UseCounter uc;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        for (const auto & bd : m.blocks[bid].bindings)
            countOperandsExpr(bd.expr, uc);
        if (const auto * t = std::get_if<TermReturn>(&m.blocks[bid].terminal))
            if (t->value != kInvalid) uc.bump(t->value);
    }
    return uc;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: streamFusion.
// ---------------------------------------------------------------------------

size_t streamFusion(Module & m)
{
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_STREAM_FUSION") != nullptr;
    if (s_disabled) return 0;

    const PrimOp * foldlMap = foldlMapPrimOp();
    if (!foldlMap) return 0;  // __foldlMap not registered — skip

    UseCounter uses = countModuleUses(m);

    size_t fused = 0;

    // V3_DBG_STREAM_FUSION_IR=1 dumps the entire IR for diagnostic
    // walks.  Cheap when disabled.
    {
        static const bool s_dbgIR =
            std::getenv("V3_DBG_STREAM_FUSION_IR") != nullptr;
        if (s_dbgIR) {
            std::fprintf(stderr, "v3 stream-fusion: IR DUMP BEGIN\n%s\nIR DUMP END\n",
                dumpModule(m).c_str());
        }
    }

    // Helper: collect the bindings of `funcIdx`'s entry-block body
    // INTO `out` (excluding the body's terminal which is just a
    // VarRef-to-tail).  Used to hoist thunk bodies into the outer
    // block when fusing through a MkThunk-wrapped arg.
    auto hoistBodyInto = [&](FuncId fid,
                              std::vector<Binding> & out)
    {
        if (fid == 0 || fid >= m.functions.size()) return;
        const Function & f = m.functions[fid];
        if (f.entryBlock == kInvalidBlock || f.entryBlock >= m.blocks.size()) return;
        const Block & body = m.blocks[f.entryBlock];
        for (const auto & bd : body.bindings) {
            // Copy the binding into the outer block.  VarIds are
            // unique across the Module, so no rebinding needed.
            out.push_back(bd);
        }
    };

    // V3_DBG_STREAM_FUSION=1: per-attempt trace, prints why each
    // foldl' candidate was accepted or skipped.  Retire when bench
    // shows stable wins across the corpus.
    static const bool s_dbg =
        std::getenv("V3_DBG_STREAM_FUSION") != nullptr;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        auto defs = mapBlockDefs(blk);

        // Track which inner-call MkThunk funcIdxs we need to hoist.
        // The same thunk may be referenced multiple times across
        // bindings (rare), but the hoist itself is idempotent — VarIds
        // are unique and inserting twice would be wasteful.
        std::unordered_set<FuncId> hoistedFuncs;
        std::vector<Binding> hoisted;

        // Single forward pass: emit hoisted bindings BEFORE any
        // fused binding that requires them.  out collects the new
        // binding order.
        std::vector<Binding> out;
        out.reserve(blk.bindings.size() + 16);

        for (auto & bd : blk.bindings) {
            // Recognise the OUTER foldl' call shape — both
            // PrimOpCall (canonical) and App-chain (bytecode-
            // replaced primops escape opt_primop_fuse and stay as
            // App chains).  We start the recogniser from this
            // binding's VarId so chase walks via VarRef + App
            // through the same-block defs.
            auto outer = recogniseCall(bd.var, m, defs);
            if (!outer || !outer->primop) { out.push_back(bd); continue; }
            std::string_view name = outer->primop->name;
            if (s_dbg) std::fprintf(stderr,
                "v3 stream-fusion: scan call %.*s argc=%zu\n",
                (int)name.size(), name.data(), outer->args.size());
            if (name != "foldl'" && name != "__foldl'") { out.push_back(bd); continue; }
            if (outer->args.size() != 3) { out.push_back(bd); continue; }

            // Recognise the INNER map call — same machinery.
            auto inner = recogniseCall(outer->args[2], m, defs);
            if (!inner || !inner->primop) {
                if (s_dbg) std::fprintf(stderr,
                    "  → skip: arg[2] (var %u) is not a recognisable call\n",
                    (unsigned)outer->args[2]);
                out.push_back(bd); continue;
            }
            std::string_view innerName = inner->primop->name;
            if (innerName != "map" && innerName != "__map") {
                if (s_dbg) std::fprintf(stderr,
                    "  → skip: inner call is %.*s (expected map)\n",
                    (int)innerName.size(), innerName.data());
                out.push_back(bd); continue;
            }
            if (inner->args.size() != 2) { out.push_back(bd); continue; }
            if (s_dbg) std::fprintf(stderr,
                "  → candidate: inner is map(f=%u, xs=%u)\n",
                (unsigned)inner->args[0], (unsigned)inner->args[1]);

            // Use-once safety: the map's result VarId must be used
            // exactly once — only by this foldl' invocation.  Walk
            // VarRef chain to find the actual map-binding VarId and
            // check its module-wide use count.
            VarId mapBindingVar = outer->args[2];
            while (true) {
                auto it = defs.find(mapBindingVar);
                if (it == defs.end()) break;
                if (const VarRef * vr = std::get_if<VarRef>(it->second)) {
                    mapBindingVar = vr->var;
                } else {
                    break;
                }
            }
            if (uses.at(mapBindingVar) != 1) {
                if (s_dbg) std::fprintf(stderr,
                    "  → skip: map binding var %u has %u uses (need 1)\n",
                    (unsigned)mapBindingVar, uses.at(mapBindingVar));
                out.push_back(bd); continue;
            }

            // ALL preconditions met — fuse.
            //
            // If outer->args[2] resolves through a MkThunk wrapper,
            // we need to HOIST that thunk's body bindings into the
            // outer block so the args we extract (`f`, `xs`) are
            // in scope at the fused call site.  Same for any nested
            // thunks reachable via the inner call's args (e.g.
            // `xs` itself may be a MkThunk-wrapped expression).
            //
            // We hoist greedily: any MkThunk we walked through during
            // recogniseCall gets its body bindings inserted into the
            // outer block.  The hoisted Function's entry block then
            // becomes orphan (no references), which DCE doesn't touch
            // (functions[] isn't DCE'd) — but it costs no runtime
            // alloc because the MkThunk binding itself becomes
            // unreferenced too.
            //
            // Walk MkThunk chain from outer->args[2] and from
            // inner->args[1] (xs may be thunked).
            auto hoistChain = [&](VarId v) {
                const auto * e = chaseInBlock(v, defs);
                while (e) {
                    const auto * th = std::get_if<MkThunk>(e);
                    if (!th) break;
                    if (hoistedFuncs.insert(th->funcIdx).second) {
                        hoistBodyInto(th->funcIdx, hoisted);
                    }
                    // Continue walking — the thunk body's TermReturn
                    // may itself be another MkThunk-of-thunk shape.
                    if (th->funcIdx == 0 || th->funcIdx >= m.functions.size()) break;
                    const Function & f = m.functions[th->funcIdx];
                    if (f.entryBlock == kInvalidBlock
                        || f.entryBlock >= m.blocks.size()) break;
                    const Block & body = m.blocks[f.entryBlock];
                    const auto * term = std::get_if<TermReturn>(&body.terminal);
                    if (!term) break;
                    // Continue with the body block's defs to walk
                    // into a nested MkThunk if the term points at one.
                    std::unordered_map<VarId, const Expr *> bodyDefs;
                    for (const auto & b : body.bindings)
                        bodyDefs.emplace(b.var, &b.expr);
                    auto it = bodyDefs.find(term->value);
                    if (it == bodyDefs.end()) break;
                    e = it->second;
                }
            };
            hoistChain(outer->args[2]);
            hoistChain(inner->args[1]);  // xs may itself be a thunk

            // Build the fused call as an App-chain over LitPrimOp.
            // __foldlMap has a bytecode-closure replacement installed
            // via installBytecodePrimop (bytecode_primops.cc).  The
            // OP_LIT_PRIMOP redirect pushes the closure onto the
            // stack; the subsequent OP_CALL chain dispatches it
            // iteratively (matching `foldl'`'s fast path).  Emitting
            // PrimOpCall would bypass the redirect and invoke the
            // C-recursive `primFoldlMap` body — measured 60x slower
            // on N=200K (15s vs 0.24s; commit message in this batch).
            //
            // Shape:
            //   v_lp = LitPrimOp{__foldlMap}    // → bytecode closure
            //   v_a0 = App(v_lp, op)
            //   v_a1 = App(v_a0, init)
            //   v_a2 = App(v_a1, f)
            //   bd.var = App(v_a2, xs)
            VarId v_lp = m.freshVar();
            VarId v_a0 = m.freshVar();
            VarId v_a1 = m.freshVar();
            VarId v_a2 = m.freshVar();
            LitPrimOp lp;  lp.primop = foldlMap;
            out.push_back({ v_lp, lp });
            out.push_back({ v_a0, App{ v_lp, outer->args[0] } });
            out.push_back({ v_a1, App{ v_a0, outer->args[1] } });
            out.push_back({ v_a2, App{ v_a1, inner->args[0] } });
            bd.expr = App{ v_a2, inner->args[1] };
            out.push_back(bd);
            ++fused;
            if (s_dbg) std::fprintf(stderr,
                "  → FUSE: binding %u rewritten to __foldlMap App-chain "
                "(hoisted %zu bindings)\n",
                (unsigned)bd.var, hoisted.size());
        }

        if (fused > 0 || !hoisted.empty()) {
            // Prepend hoisted bindings to the output.  Their VarIds
            // dominate the fused call (they were body bindings of a
            // MkThunk that, in source order, was constructed BEFORE
            // the call).  Order of hoisted bindings amongst themselves
            // is preserved from the body block (which is dependency-
            // ordered by the lowerer).
            std::vector<Binding> finalBindings;
            finalBindings.reserve(hoisted.size() + out.size());
            for (auto & b : hoisted) finalBindings.push_back(std::move(b));
            for (auto & b : out)     finalBindings.push_back(std::move(b));
            blk.bindings = std::move(finalBindings);
        }
    }

    return fused;
}

} // namespace nix::v3::ir
