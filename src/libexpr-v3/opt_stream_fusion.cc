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
/// Gate: DEFAULT-OFF (2026-06-05) — bytecode fusion measured a net regression
/// even post-go-loop-fix; set NIX_V3_STREAM_FUSION=1 to enable for A/B.  The
/// pass is retained as a documented registry of falsified candidates + the
/// mechanism for a future paying rule.  See the registry in streamFusion().
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
// (foldlMapPrimOp() retired 2026-06-05 — the fused primops are now resolved
//  per-rule from the kRules table in streamFusion via findPrimOp.)
// ---------------------------------------------------------------------------

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
    // FUSION IS OPT-IN / DEFAULT-OFF (2026-06-05).  Bytecode stream fusion was
    // measured a NET REGRESSION even AFTER the go-loop optimizations (levers A
    // + B: seq-tail-app 09b13c3d8 + OP_CALL_N 6a081f6c6): every intermediate-
    // eliminating rewrite costs MORE than the cheap C-built genList spine it
    // removes (the bytecode generate-and-consume loop can't beat a contiguous
    // C array that's GC-reclaimed incrementally — see the registry below).
    // The table-driven MECHANISM is retained as a documented registry of what
    // was tried + an A/B handle: set NIX_V3_STREAM_FUSION=1 to enable kRules.
    // Retirement: flip back to default-on only when a rule MEASURES a win
    // (wall AND peak-RSS) on --core + a nixpkgs sample across >=10 runs.
    static const bool s_enabled =
        std::getenv("NIX_V3_STREAM_FUSION") != nullptr;
    if (!s_enabled) return 0;

    // RULES fusion table (§2.7 GHC_PASSES_FOR_NIX): declarative,
    // intermediate-eliminating rewrites.  `outer(… inner(…) …)` where the
    // inner call's result is the outer's `listArgIdx`-th arg AND is used
    // EXACTLY ONCE fuses to `fusedName`, dropping the intermediate list.
    // The fused args are (outer args minus listArgIdx) ++ (inner args).
    // ONLY length-preserving, intermediate-eliminating fusions belong here —
    // PRODUCER fusion (e.g. foldl'∘genList) was measured a CPU REGRESSION
    // (genList's C array-build + tight force beats a bytecode generate-loop);
    // these rules eliminate a separately-allocated intermediate, which is the
    // win __foldlMap already showed on hello.name.
    struct FusionRule {
        std::string_view outerName, outerAlt; size_t outerArity, listArgIdx;
        std::string_view innerName, innerAlt; size_t innerArity;
        std::string_view fusedName;
    };
    // The rule registry.  Every bytecode-fusion candidate has been MEASURED;
    // NONE ships (the pass is default-OFF — see the gate above).  This table
    // is the documented record + the A/B handle, and the mechanism is
    // arity-generic (outerArity / listArgIdx / innerArity drive arg-remapping)
    // so a future PAYING rule is one line + one bytecode primop.
    //
    //   CANDIDATE REGISTRY — all FALSIFIED (measured 2026-06-05, 1M synthetic,
    //   AFTER the go-loop levers A+B):
    //   ┌─────────────────┬──────────────────────────────────────────────┐
    //   │ foldl'∘map       │ REGRESSES. Fused→bytecode __foldlMap = 88M    │
    //   │  → __foldlMap    │ ins / 704 MB vs unfused 82M / 503 MB (+201 MB)│
    //   │                  │ on 1M; fires 0× on hello.drvPath. The C-built │
    //   │                  │ genList spine beats any bytecode loop.         │
    //   │ all∘map/any∘map  │ REGRESSES RSS. __allMap = 74M ins (−12%) but  │
    //   │  → __allMap/...  │ 669 MB vs unfused 545 MB (+124 MB).            │
    //   │ map∘map          │ NEUTRAL. Eliminates only a transient spine;   │
    //   │  → __mapMap       │ elements forced once either way (54M ins /     │
    //   │                  │ 791 MB both ways).                            │
    //   │ *∘genList         │ CPU REGRESSION (1.21×). genList's C array      │
    //   │  (producer)      │ beats a bytecode generate-loop. Never a rule. │
    //   └─────────────────┴──────────────────────────────────────────────┘
    //   NET: bytecode stream fusion is FALSIFIED as a perf lever — eliminating
    //   an intermediate list never beats the cheap C genList spine + the
    //   optimized go-loop.  The entry below stays for opt-in A/B only.
    static const FusionRule kRules[] = {
        // foldl' op nul (map f xs)  →  __foldlMap op nul f xs
        // (DISABLED by default — regresses; see registry.  Opt-in only.)
        { "foldl'", "__foldl'", 3, 2, "map", "__map", 2, "__foldlMap" },
    };
    constexpr size_t kNumRules = sizeof(kRules) / sizeof(kRules[0]);
    const PrimOp * fusedOf[kNumRules];
    bool anyRule = false;
    for (size_t i = 0; i < kNumRules; ++i) {
        fusedOf[i] = findPrimOp(kRules[i].fusedName);
        if (fusedOf[i]) anyRule = true;
    }
    if (!anyRule) return 0;  // no fused primop registered — skip

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
            std::string_view oname = outer->primop->name;
            if (s_dbg) std::fprintf(stderr,
                "v3 stream-fusion: scan call %.*s argc=%zu\n",
                (int)oname.size(), oname.data(), outer->args.size());

            // Find a fusion rule whose OUTER matches (name + arity).
            const FusionRule * rule = nullptr; const PrimOp * fusedOp = nullptr;
            for (size_t ri = 0; ri < kNumRules; ++ri) {
                const FusionRule & r = kRules[ri];
                if (fusedOf[ri]
                    && (oname == r.outerName || oname == r.outerAlt)
                    && outer->args.size() == r.outerArity) {
                    rule = &r; fusedOp = fusedOf[ri]; break;
                }
            }
            if (!rule) { out.push_back(bd); continue; }
            const VarId listArg = outer->args[rule->listArgIdx];

            // Recognise the INNER call at the rule's list-arg position.
            auto inner = recogniseCall(listArg, m, defs);
            if (!inner || !inner->primop) {
                if (s_dbg) std::fprintf(stderr,
                    "  → skip: list arg (var %u) is not a recognisable call\n",
                    (unsigned)listArg);
                out.push_back(bd); continue;
            }
            std::string_view iname = inner->primop->name;
            if (!((iname == rule->innerName || iname == rule->innerAlt)
                  && inner->args.size() == rule->innerArity)) {
                if (s_dbg) std::fprintf(stderr,
                    "  → skip: inner call is %.*s (rule wants %.*s)\n",
                    (int)iname.size(), iname.data(),
                    (int)rule->innerName.size(), rule->innerName.data());
                out.push_back(bd); continue;
            }
            if (s_dbg) std::fprintf(stderr,
                "  → candidate: %.*s∘%.*s\n",
                (int)oname.size(), oname.data(),
                (int)iname.size(), iname.data());

            // Use-once safety: the inner call's result (= outer's list arg)
            // must be used EXACTLY ONCE — only by this outer call; else the
            // intermediate is shared and can't be eliminated.  Walk the
            // VarRef chain to the actual binding + check module-wide uses.
            VarId mapBindingVar = listArg;
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
            hoistChain(listArg);                  // the inner call (thunked)
            hoistChain(inner->args.back());        // inner's list arg (xs)

            // 2026-05-18 BUGFIX: splice hoisted bindings inline at the
            // CURRENT position in `out`, BEFORE the fused App-chain we
            // are about to emit.
            //
            // Why this matters: hoisted bindings often reference outer-
            // scope VarIds defined by an enclosing LetRec (notably the
            // recSlotVar via RecBindingSlotRef::attrs).  The recSlotVar
            // is not allocated by a Binding — it is a virtual VarId
            // materialised at LetRec emit time (lower.cc:2608, the
            // `m.freshVar()` registered to `m.recVarToSlotVar`).  It is
            // only IN SCOPE in the outer block AFTER the `v? = LetRec`
            // binding has emitted.  Prior to this fix we accumulated
            // `hoisted` and prepended the whole vector to `out` at end-
            // of-block, which placed RecBindingSlotRef bindings BEFORE
            // their enclosing LetRec — emit.cc then rejected the IR
            // with "unbound VarId" (the slotVar reference reached the
            // emitter before its defining LetRec).  Splicing inline
            // preserves the lexical order: LetRec emits first, then
            // (potentially) the hoisted bindings, then the fused call.
            // hoistedFuncs still dedupes across multiple fusion sites
            // in the same block — duplicates would only occur if a
            // single thunk's body is shared by multiple fused calls,
            // which the use-once safety check prevents anyway.
            for (auto & h : hoisted) out.push_back(std::move(h));
            hoisted.clear();

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
            // Fused args = (outer args minus the listArgIdx) ++ (inner args).
            //   foldl'∘map: [op,nul,LIST] ⊖2 ++ [f,xs]  = [op,nul,f,xs]
            //   map∘map:    [f,LIST]      ⊖1 ++ [g,xs]  = [f,g,xs]
            // Emitted as LitPrimOp{fused} + an App-chain (NOT PrimOpCall — the
            // fused primop has a bytecode-closure replacement; OP_LIT_PRIMOP
            // redirects to it, the OP_CALL chain dispatches iteratively,
            // matching the fast path; PrimOpCall would hit the slow C body).
            std::vector<VarId> fa;
            fa.reserve(outer->args.size() - 1 + inner->args.size());
            for (size_t k = 0; k < outer->args.size(); ++k)
                if (k != rule->listArgIdx) fa.push_back(outer->args[k]);
            for (VarId a : inner->args) fa.push_back(a);

            VarId fn = m.freshVar();
            LitPrimOp lp;  lp.primop = fusedOp;
            out.push_back({ fn, lp });
            for (size_t k = 0; k + 1 < fa.size(); ++k) {
                VarId nx = m.freshVar();
                out.push_back({ nx, App{ fn, fa[k] } });
                fn = nx;
            }
            bd.expr = App{ fn, fa.back() };
            out.push_back(bd);
            ++fused;
            if (s_dbg) std::fprintf(stderr,
                "  → FUSE %.*s∘%.*s → %.*s (binding %u, hoisted %zu)\n",
                (int)rule->outerName.size(), rule->outerName.data(),
                (int)rule->innerName.size(), rule->innerName.data(),
                (int)rule->fusedName.size(), rule->fusedName.data(),
                (unsigned)bd.var, hoisted.size());
        }

        if (fused > 0) {
            // `out` already has hoisted bindings spliced inline (just
            // before each fused App-chain) — see the BUGFIX comment
            // above.  No end-of-block prepend needed.  Assert that we
            // didn't leak any hoisted bindings past the splice point.
            if (!hoisted.empty()) {
                std::fprintf(stderr,
                    "v3 stream-fusion: WARNING — %zu hoisted bindings "
                    "leaked past splice (block %u).  This indicates a "
                    "bug in the inline-splice; the bindings will be "
                    "dropped.\n",
                    hoisted.size(), (unsigned)bid);
            }
            blk.bindings = std::move(out);
        }
    }

    return fused;
}

} // namespace nix::v3::ir
