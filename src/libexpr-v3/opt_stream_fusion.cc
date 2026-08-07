/// @file
/// IR pass file: retained `foldl'`-idiom detection helpers.
///
/// HISTORY: this file once held the IR Phase-C stream-fusion pass, which
/// recognised `foldl'(op, init, map(f, xs))` and rewrote it to a fused
/// `__foldlMap(op, init, f, xs)` FFI leaf.  That pass was RETIRED
/// (FALSIFIED 2026-06-05): the bytecode fusion measured a net regression
/// vs the cheap C-built genList spine, it sat default-OFF ever after, and
/// it was deleted — `streamFusion()` / `kRules` / `FusionRule` / the
/// `__foldlMap` primop are all gone (see git history for the pass + its
/// falsified-candidate registry).
///
/// WHAT REMAINS — deliberately retained, NOT falsified:
///   * detectFoldlAppendIdiom() — a detection-only (no-rewrite) probe for
///     the `foldl' (acc: x: acc ++ G) [] xs` O(n²) ++-accumulation idiom.
///     Gated by V3_DBG_FOLDL_APPEND; called from opt_const_fold.cc.  See
///     the block comment above the definition for the verdict + retirement
///     criterion.
///   * the shared call-recognition helpers (chaseInBlock / mapBlockDefs /
///     recogniseCall / PrimopCallShape) the probe uses to see a primop
///     call in both its canonical PrimOpCall and unfused App-chain shapes.
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
// helper bridges both forms so the idiom detector catches them uniformly.
struct PrimopCallShape {
    const PrimOp * primop = nullptr;
    std::vector<VarId> args;
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
            return r;
        }
        return std::nullopt;
    }
    return std::nullopt;
}


} // namespace

// ---------------------------------------------------------------------------
// Detection-only probe for the `foldl' (acc: x: acc ++ G) [] xs` O(n²)
// accumulation idiom (and the `if C then acc ++ G else acc` filter shape).
//
// This was the MEASURE-TWICE gate before a (never-built) IR-surgery rewrite
// to `concatLists (map (x: G) xs)`: it counts how often the idiom appears in
// real evals WITHOUT rewriting (no risk).  Gated logging via
// V3_DBG_FOLDL_APPEND=1.  Returns the per-module count.
//
// VERDICT (2026-06-07): 0 occurrences on hello/firefox/git/python3.drvPath +
// lib.unique.  The would-be rewrite only matches a *fully-saturated inline*
// `builtins.foldl' (λ) [] xs`; real nixpkgs uses `lib.foldl'` / partial
// application (`unique = foldl' step []`), so the step lambda is never inline
// at the foldl' call → the rewrite would fire ~never.  Kept as a RETAINED
// diagnostic to re-measure on future workloads; see the call site in
// opt_const_fold.cc::optimise for the full rationale + retirement criterion.
// The O(n²) bytecode primops that used this shape (filter/concatMap/partition/
// sort) were fixed directly (concatLists/mergesort) instead.
// ---------------------------------------------------------------------------

size_t detectFoldlAppendIdiom(const Module & m)
{
    static const bool s_log = std::getenv("V3_DBG_FOLDL_APPEND") != nullptr;
    size_t hits = 0;

    auto chaseVarFO = [&](VarId v,
                          const std::unordered_map<VarId, const Expr *> & d) -> VarId {
        size_t h = 0;
        while (h++ < d.size() + 1) {
            auto it = d.find(v);
            if (it == d.end()) return v;
            if (const auto * vr = std::get_if<VarRef>(it->second)) { v = vr->var; continue; }
            if (const auto * fo = std::get_if<Force>(it->second)) { v = fo->thunk; continue; }
            return v;
        }
        return v;
    };
    auto blockReturnsAcc = [&](BlockId b, VarId acc) -> bool {
        if (b == kInvalidBlock || b >= m.blocks.size()) return false;
        const Block & bb = m.blocks[b];
        const auto * t = std::get_if<TermReturn>(&bb.terminal);
        if (!t) return false;
        std::unordered_map<VarId, const Expr *> d;
        for (auto & x : bb.bindings) d.emplace(x.var, &x.expr);
        return chaseVarFO(t->value, d) == acc;
    };
    auto blockReturnsConcatAcc = [&](BlockId b, VarId acc) -> bool {
        if (b == kInvalidBlock || b >= m.blocks.size()) return false;
        const Block & bb = m.blocks[b];
        const auto * t = std::get_if<TermReturn>(&bb.terminal);
        if (!t) return false;
        std::unordered_map<VarId, const Expr *> d;
        for (auto & x : bb.bindings) d.emplace(x.var, &x.expr);
        const Expr * re = chaseInBlock(t->value, d);
        if (!re) return false;
        const auto * cc = std::get_if<ConcatLists>(re);
        return cc && chaseVarFO(cc->lhs, d) == acc;
    };
    auto isEmptyListInit = [&](VarId v,
                               const std::unordered_map<VarId, const Expr *> & defs) -> bool {
        const Expr * e = chaseInBlock(v, defs);
        if (!e) return false;
        if (const auto * le = std::get_if<ListExpr>(e)) return le->elems.empty();
        if (const auto * th = std::get_if<MkThunk>(e)) {        // init is lazyArg → thunked
            if (th->funcIdx == 0 || th->funcIdx >= m.functions.size()) return false;
            const Function & f = m.functions[th->funcIdx];
            if (f.entryBlock == kInvalidBlock || f.entryBlock >= m.blocks.size()) return false;
            const Block & b = m.blocks[f.entryBlock];
            const auto * t = std::get_if<TermReturn>(&b.terminal);
            if (!t) return false;
            std::unordered_map<VarId, const Expr *> d;
            for (auto & x : b.bindings) d.emplace(x.var, &x.expr);
            const Expr * re = chaseInBlock(t->value, d);
            if (const auto * le = std::get_if<ListExpr>(re)) return le->elems.empty();
        }
        return false;
    };

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & blk = m.blocks[bid];
        auto defs = mapBlockDefs(blk);
        for (const auto & bd : blk.bindings) {
            auto outer = recogniseCall(bd.var, m, defs);
            if (!outer || !outer->primop) continue;
            std::string_view nm = outer->primop->name;
            if (!((nm == "foldl'" || nm == "__foldl'") && outer->args.size() == 3)) continue;
            if (!isEmptyListInit(outer->args[1], defs)) continue;
            const Expr * se = chaseInBlock(outer->args[0], defs);
            if (!se) continue;
            const Lambda * lam = std::get_if<Lambda>(se);
            if (!lam || lam->funcIdx == 0 || lam->funcIdx >= m.functions.size()) continue;
            const Function & f = m.functions[lam->funcIdx];
            if (f.paramVar == kInvalid || f.extraParams.size() != 1) continue;   // (acc, x)
            VarId acc = f.paramVar;
            if (f.entryBlock == kInvalidBlock || f.entryBlock >= m.blocks.size()) continue;
            const Block & fb = m.blocks[f.entryBlock];
            const auto * term = std::get_if<TermReturn>(&fb.terminal);
            if (!term) continue;
            std::unordered_map<VarId, const Expr *> fdefs;
            for (auto & x : fb.bindings) fdefs.emplace(x.var, &x.expr);
            const Expr * re = chaseInBlock(term->value, fdefs);
            if (!re) continue;
            const char * shape = nullptr;
            if (const auto * cc = std::get_if<ConcatLists>(re)) {
                if (chaseVarFO(cc->lhs, fdefs) == acc) shape = "concatMap";
            } else if (const auto * iff = std::get_if<If>(re)) {
                if (blockReturnsConcatAcc(iff->thenBlock, acc) && blockReturnsAcc(iff->elseBlock, acc))
                    shape = "filter";
                else if (blockReturnsConcatAcc(iff->elseBlock, acc) && blockReturnsAcc(iff->thenBlock, acc))
                    shape = "filter-inv";
            }
            if (shape) {
                ++hits;
                if (s_log) std::fprintf(stderr,
                    "v3 foldl-append idiom [%s] at func f%u (O(n^2) ++-accumulation)\n",
                    shape, (unsigned)lam->funcIdx);
            }
        }
    }
    return hits;
}

} // namespace nix::v3::ir
