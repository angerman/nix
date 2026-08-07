/// @file
/// IR Phase F — static App-spine folding (2026-05-18).
///
/// Recognises App spines `App(App(...App(f_var, a_0), ...), a_N-1)`
/// where:
///   1. `f_var` resolves (via VarRef chain) to a Lambda `L_0` whose
///      body is a canonical curried-Lambda chain of depth N or
///      deeper (each intermediate Lambda's body is exactly `return
///      Lambda L_{i+1}`).
///   2. All args `a_i` are PURE (LitInt/Float/Bool/Null/String/Path/
///      VarRef/LitPrimOp/LitBuiltins) — cheap to substitute multiple
///      times if a param is used >1 inside the deepest body.
///   3. The deepest body (Lambda L_{N-1}'s body block) passes the
///      "simple body" safety check — no nested Lambda / MkThunk /
///      LetRec / If / With / Assert / And / Or / Impl bindings.
///   4. Every intermediate Lambda's binding has use-count == 1
///      across the module (only this spine uses them).  Without
///      this, inlining would orphan a captured Lambda.
///
/// When all conditions hold, the entire spine is collapsed:
///   - Clone the deepest body's bindings into the outer block with
///     N-way substitution (each param_i → arg_i, plus fresh VarIds
///     for body-local bindings).
///   - Rewrite the outermost App binding's expr to
///     `VarRef(cloned_deepest_terminal)`.
///   - Intermediate App bindings become orphan (DCE will sweep them).
///
/// Hypothesis killed (Rule 0): "Multi-arg currying chains can only
/// be reduced one App at a time."  Falsified by this pass: an N-arg
/// spine `(x: y: z: x+y+z) 1 2 3` collapses in a SINGLE rewrite to
/// the deepest body with all three params substituted, eliminating
/// N-1 PartialApp allocations + N-1 OP_CALL dispatches.
///
/// Dependency: Phase A (betaReduce) — the per-block defs map is the
/// same shape Phase A relies on, and Phase A's bodyIsSimple +
/// remapExprVars helpers (re-duplicated here for namespace
/// isolation) give us the substitution machinery.
///
/// Unconditional (the NIX_V3_NO_APP_SPINE_FOLD / NIX_V3_NO_DETHUNK_STRICT
/// A/B opt-outs were retired once they shipped byte-identical).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_util.hh"
#include "v3/primop.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nix::v3::ir {

namespace {

constexpr size_t kMaxSpineDepth = 8;

// chaseInBlock() is shared across opt_*.cc passes — see v3/ir_util.hh.

VarId chaseToBindingVar(VarId v,
                       const std::unordered_map<VarId, const Expr *> & defs)
{
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        auto it = defs.find(v);
        if (it == defs.end()) return v;
        const VarRef * vr = std::get_if<VarRef>(it->second);
        if (!vr) return v;
        v = vr->var;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Walk the App spine.  Given a binding VarId, follow `App.fun` operands
// downward, collecting args.  Stops when we reach a non-App expression
// (which should be the leaf — typically a Lambda or a VarRef to one).
// Returns (leaf VarId, args in application order [a_0, a_1, ..., a_N-1]).
// On any structural mismatch, returns nullopt.
// ---------------------------------------------------------------------------

struct SpineWalk {
    VarId leafFun;
    std::vector<VarId> args;
};

std::optional<SpineWalk> walkAppSpine(
    VarId topVar,
    const std::unordered_map<VarId, const Expr *> & defs)
{
    SpineWalk sw;
    VarId v = topVar;
    size_t hops = 0;
    while (hops++ < kMaxSpineDepth + 1) {
        const Expr * e = chaseInBlock(v, defs);
        if (!e) {
            // Reached a VarId not defined in this block (free var).
            // The leaf is whatever this VarId is.
            sw.leafFun = v;
            // Reverse args — we walked outermost-to-innermost; flip
            // to application order.
            std::reverse(sw.args.begin(), sw.args.end());
            return sw;
        }
        if (const auto * app = std::get_if<App>(e)) {
            sw.args.push_back(app->arg);
            v = app->fun;
            continue;
        }
        // Non-App leaf.
        sw.leafFun = v;
        std::reverse(sw.args.begin(), sw.args.end());
        return sw;
    }
    return std::nullopt;  // ran past max depth
}

// ---------------------------------------------------------------------------
// Walk the curried Lambda chain.  Given the leaf Lambda's funcIdx,
// follow body→Lambda links collecting (param, funcIdx) pairs.
// ---------------------------------------------------------------------------

struct CurriedLink {
    VarId paramVar;
    FuncId funcIdx;       // the Lambda function being entered
    BlockId bodyBlock;    // its entry block
};

std::optional<std::vector<CurriedLink>>
walkCurriedChain(const Module & m, FuncId startFid, size_t needDepth)
{
    std::vector<CurriedLink> chain;
    chain.reserve(needDepth);
    FuncId fid = startFid;
    for (size_t i = 0; i < needDepth; ++i) {
        if (fid == 0 || fid >= m.functions.size()) return std::nullopt;
        const Function & f = m.functions[fid];
        // Safety preconditions — same as Phase A.
        if (f.argName == kInvalidSymbol) return std::nullopt;
        if (f.hasFormals)                return std::nullopt;
        // eval/apply (#3): a collapsed uncurried multi-arity Function carries
        // its later params in extraParams (not as nested Lambda bodies).  The
        // curried-chain fold would substitute only paramVar and leave the
        // extraParams dangling — skip and let the runtime PAP apply it.
        // (extraParams is empty unless NIX_V3_EVAL_APPLY.)
        if (!f.extraParams.empty())      return std::nullopt;
        if (f.entryBlock == kInvalidBlock
            || f.entryBlock >= m.blocks.size()) return std::nullopt;

        CurriedLink link;
        link.paramVar = f.paramVar;
        link.funcIdx = fid;
        link.bodyBlock = f.entryBlock;
        chain.push_back(link);

        // If we still need to descend, the body block must be the
        // canonical "return Lambda L_{i+1}" form: exactly one binding
        // (the Lambda), terminal returns that VarId.
        if (i + 1 < needDepth) {
            const Block & body = m.blocks[f.entryBlock];
            if (body.bindings.size() != 1) return std::nullopt;
            const Lambda * inner = std::get_if<Lambda>(&body.bindings[0].expr);
            if (!inner) return std::nullopt;
            const auto * term = std::get_if<TermReturn>(&body.terminal);
            if (!term || term->value != body.bindings[0].var)
                return std::nullopt;
            fid = inner->funcIdx;
        }
    }
    return chain;
}

// ---------------------------------------------------------------------------
// eval/apply collapsed-arity-N variant: when the curried chain `x: y: …`
// has been collapsed by the lowerer into ONE Function carrying paramVar +
// extraParams (NIX_V3_EVAL_APPLY, default-on), there are no nested Lambda
// bodies to walk — the single Function's entry block IS the deepest body and
// its N params are [paramVar, extraParams...].  Build the equivalent N-link
// chain (all links share the one funcIdx / body block) so the saturated-call
// fold below applies unchanged.  Returns nullopt unless the Function is a
// collapsed arity-N lambda whose param count matches the spine depth.
// ---------------------------------------------------------------------------

std::optional<std::vector<CurriedLink>>
walkCollapsedChain(const Module & m, FuncId fid, size_t needDepth)
{
    if (fid == 0 || fid >= m.functions.size()) return std::nullopt;
    const Function & f = m.functions[fid];
    if (f.argName == kInvalidSymbol)              return std::nullopt;
    if (f.hasFormals)                             return std::nullopt;
    if (f.extraParams.empty())                    return std::nullopt;  // not collapsed
    if (1 + f.extraParams.size() != needDepth)    return std::nullopt;  // arity ≠ spine
    if (f.entryBlock == kInvalidBlock
        || f.entryBlock >= m.blocks.size())       return std::nullopt;

    std::vector<CurriedLink> chain;
    chain.reserve(needDepth);
    chain.push_back({ f.paramVar, fid, f.entryBlock });   // arg 0 → paramVar
    for (VarId ep : f.extraParams)                        // args 1..N-1 → extraParams
        chain.push_back({ ep, fid, f.entryBlock });
    return chain;
}

// ---------------------------------------------------------------------------
// Purity of args — same shortlist Phase A's per-arg substitution
// safety check would consult.  Pure args can be substituted multiple
// times into the body without changing semantics (their evaluation
// is free or trivially shareable).
// ---------------------------------------------------------------------------

bool isPureArg(VarId v,
               const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(v, defs);
    if (!e) return true;  // free var — assume pure (just an upvalue ref)
    return std::visit([](const auto & x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>    ||
                      std::is_same_v<T, LitFloat>  ||
                      std::is_same_v<T, LitBool>   ||
                      std::is_same_v<T, LitNull>   ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath>   ||
                      std::is_same_v<T, VarRef>    ||
                      std::is_same_v<T, LitPrimOp> ||
                      std::is_same_v<T, LitBuiltins>) {
            (void)x;
            return true;
        } else {
            (void)x;
            return false;
        }
    }, *e);
}

// ---------------------------------------------------------------------------
// Apply a substitution map to every VarId operand in `e`.  Returns
// false if `e` is a Kind we refuse to remap (carries sub-blocks or
// captured-state machinery).  Identical to opt_beta_reduce's helper
// — duplicated here to keep the pass self-contained.
// ---------------------------------------------------------------------------

void sub1(VarId & v, const std::unordered_map<VarId, VarId> & subm)
{
    auto it = subm.find(v);
    if (it != subm.end()) v = it->second;
}

bool remapExprVars(Expr & e, const std::unordered_map<VarId, VarId> & subm)
{
    return std::visit([&](auto & x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>    ||
                      std::is_same_v<T, LitFloat>  ||
                      std::is_same_v<T, LitBool>   ||
                      std::is_same_v<T, LitNull>   ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath>   ||
                      std::is_same_v<T, LitPrimOp> ||
                      std::is_same_v<T, LitBuiltins> ||
                      std::is_same_v<T, WithLookup>) {
            (void)x;
            return true;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            sub1(x.var, subm); return true;
        } else if constexpr (std::is_same_v<T, App>) {
            sub1(x.fun, subm); sub1(x.arg, subm); return true;
        } else if constexpr (std::is_same_v<T, Force>) {
            sub1(x.thunk, subm); return true;
        } else if constexpr (std::is_same_v<T, AttrSelect> ||
                             std::is_same_v<T, HasAttr> ||
                             std::is_same_v<T, RecBindingSlotRef>) {
            sub1(x.attrs, subm); return true;
        } else if constexpr (std::is_same_v<T, AttrSelectDyn> ||
                             std::is_same_v<T, HasAttrDyn>) {
            sub1(x.attrs, subm); sub1(x.nameVar, subm); return true;
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & en : x.entries)
                if (en.value != kInvalid) sub1(en.value, subm);
            return true;
        } else if constexpr (std::is_same_v<T, ListExpr>) {
            for (auto & v : x.elems) sub1(v, subm);
            return true;
        } else if constexpr (std::is_same_v<T, ConcatStrings>) {
            for (auto & v : x.parts) sub1(v, subm);
            return true;
        } else if constexpr (std::is_same_v<T, ConcatLists> ||
                             std::is_same_v<T, Update> ||
                             std::is_same_v<T, Add> ||
                             std::is_same_v<T, Sub> ||
                             std::is_same_v<T, Mul> ||
                             std::is_same_v<T, Div> ||
                             std::is_same_v<T, Eq>  ||
                             std::is_same_v<T, NEq> ||
                             std::is_same_v<T, Less>) {
            sub1(x.lhs, subm); sub1(x.rhs, subm); return true;
        } else if constexpr (std::is_same_v<T, Not>) {
            sub1(x.operand, subm); return true;
        } else if constexpr (std::is_same_v<T, PrimOpCall>) {
            for (auto & v : x.args) sub1(v, subm);
            return true;
        } else {
            // Lambda, MkThunk, AttrSetSetInheritFrom, AttrSetDyn,
            // If, With, Assert, And, Or, Impl, LetRec — all refused.
            (void)x;
            return false;
        }
    }, e);
}

// ---------------------------------------------------------------------------
// Check whether the deepest body's bindings are all remappable.
// ---------------------------------------------------------------------------

bool deepestBodyIsSimple(const Block & body)
{
    static const std::unordered_map<VarId, VarId> empty;
    for (const auto & bd : body.bindings) {
        Expr probe = bd.expr;
        if (!remapExprVars(probe, empty)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Module-wide use counter (matches opt_beta_reduce's).  Used to ensure
// every intermediate Lambda in the curried chain is only referenced
// once across the module — otherwise the inline would orphan a
// capturing closure elsewhere.
// ---------------------------------------------------------------------------

void bump(VarId v, std::unordered_map<VarId, uint32_t> & uses)
{
    if (v == kInvalid) return;
    ++uses[v];
}

void collectExprUses(const Expr & e,
                     std::unordered_map<VarId, uint32_t> & uses)
{
    std::visit([&](const auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>    ||
                      std::is_same_v<T, LitFloat>  ||
                      std::is_same_v<T, LitBool>   ||
                      std::is_same_v<T, LitNull>   ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath>   ||
                      std::is_same_v<T, LitPrimOp> ||
                      std::is_same_v<T, LitBuiltins> ||
                      std::is_same_v<T, WithLookup>) {
            (void)x;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            bump(x.var, uses);
        } else if constexpr (std::is_same_v<T, App>) {
            bump(x.fun, uses); bump(x.arg, uses);
        } else if constexpr (std::is_same_v<T, Force>) {
            bump(x.thunk, uses);
        } else if constexpr (std::is_same_v<T, AttrSelect> ||
                             std::is_same_v<T, HasAttr> ||
                             std::is_same_v<T, RecBindingSlotRef>) {
            bump(x.attrs, uses);
        } else if constexpr (std::is_same_v<T, AttrSelectDyn> ||
                             std::is_same_v<T, HasAttrDyn>) {
            bump(x.attrs, uses); bump(x.nameVar, uses);
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            for (const auto & en : x.entries)
                if (en.value != kInvalid) bump(en.value, uses);
        } else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) {
            bump(x.attrSetVar, uses);
            for (const auto & en : x.entries) bump(en.valueVar, uses);
        } else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (const auto & en : x.statics)  bump(en.value, uses);
            for (const auto & en : x.dynamics) { bump(en.nameVar, uses); bump(en.value, uses); }
        } else if constexpr (std::is_same_v<T, ListExpr>) {
            for (auto v : x.elems) bump(v, uses);
        } else if constexpr (std::is_same_v<T, ConcatStrings>) {
            for (auto v : x.parts) bump(v, uses);
        } else if constexpr (std::is_same_v<T, ConcatLists> ||
                             std::is_same_v<T, Update> ||
                             std::is_same_v<T, Add> ||
                             std::is_same_v<T, Sub> ||
                             std::is_same_v<T, Mul> ||
                             std::is_same_v<T, Div> ||
                             std::is_same_v<T, Eq>  ||
                             std::is_same_v<T, NEq> ||
                             std::is_same_v<T, Less>) {
            bump(x.lhs, uses); bump(x.rhs, uses);
        } else if constexpr (std::is_same_v<T, Not>) {
            bump(x.operand, uses);
        } else if constexpr (std::is_same_v<T, PrimOpCall>) {
            for (auto v : x.args) bump(v, uses);
        } else if constexpr (std::is_same_v<T, Lambda> ||
                             std::is_same_v<T, MkThunk>) {
            for (auto v : x.freeVars) bump(v, uses);
            for (auto v : x.lexicalWiths) bump(v, uses);
        } else if constexpr (std::is_same_v<T, If>) {
            bump(x.cond, uses);
        } else if constexpr (std::is_same_v<T, With>) {
            bump(x.attrs, uses);
        } else if constexpr (std::is_same_v<T, Assert>) {
            bump(x.cond, uses);
        } else if constexpr (std::is_same_v<T, And> ||
                             std::is_same_v<T, Or>  ||
                             std::is_same_v<T, Impl>) {
            bump(x.lhs, uses);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            for (const auto & en : x.entries)
                for (auto v : en.outerUpvalues) bump(v, uses);
            for (const auto & he : x.hiddenEntries)
                for (auto v : he.outerUpvalues) bump(v, uses);
        }
    }, e);
}

std::unordered_map<VarId, uint32_t> countModuleUses(const Module & m)
{
    std::unordered_map<VarId, uint32_t> uses;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & b = m.blocks[bid];
        for (const auto & bind : b.bindings) collectExprUses(bind.expr, uses);
        if (auto * ret = std::get_if<TermReturn>(&b.terminal))
            if (ret->value != kInvalid) bump(ret->value, uses);
    }
    return uses;
}

} // namespace

// ---------------------------------------------------------------------------
// De-thunk use-once MkThunk args to strict arithmetic primops.
//
// The lowerer thunks every primop arg for laziness.  But __add/__sub/__mul/
// __div/__lessThan force EVERY arg, so a use-once arg-thunk's deferral is
// redundant: the primop forces it the moment its block runs, exactly as an
// inlined (eager) body would.  Inlining the thunk body is therefore
// byte-identical AND exposes nested arithmetic (`x*y*z`'s inner `x*y` thunk)
// to appSpineFold + constant folding.  Use-once is required so we never make
// a SHARED thunk eager (which could be referenced in an unforced context).
// ---------------------------------------------------------------------------
size_t deThunkForcedStrictArgs(Module & m)
{
    auto isStrict = [](std::string_view n) {
        return n == "__add" || n == "__sub" || n == "__mul"
            || n == "__div" || n == "__lessThan";
    };
    static const bool dbg = std::getenv("V3_DBG_DETHUNK") != nullptr;

    auto uses = countModuleUses(m);
    size_t count = 0;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        if (blk.bindings.empty()) continue;

        // defs over THIS block — valid for the whole scan (we mutate the
        // block only in the rebuild phase after the scan completes).
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(blk.bindings.size());
        for (const auto & bd : blk.bindings) defs.emplace(bd.var, &bd.expr);

        struct Action {
            size_t bindingIdx;
            size_t argIdx;
            std::vector<Binding> inlined;  // thunk-body bindings (remapped)
            VarId newArg;                  // remapped body return value
        };
        std::vector<Action> actions;
        // The de-thunked MkThunk bindings become dead (use-once, arg
        // rewritten); drop them in the rebuild so appSpineFold's
        // deepestBodyIsSimple — which runs before the end-of-pipeline DCE —
        // doesn't see a (dead) MkThunk and reject the now-foldable body.
        std::unordered_set<VarId> deadThunks;

        for (size_t bi = 0; bi < blk.bindings.size(); ++bi) {
            const PrimOpCall * pc = std::get_if<PrimOpCall>(&blk.bindings[bi].expr);
            if (!(pc && pc->primop && isStrict(pc->primop->name))) continue;

            if (dbg) std::fprintf(stderr, "v3 dethunk: B%u strict primop %.*s argc=%zu\n",
                (unsigned)bid, (int)pc->primop->name.size(), pc->primop->name.data(),
                pc->args.size());
            for (size_t ai = 0; ai < pc->args.size(); ++ai) {
                VarId tvar = chaseToBindingVar(pc->args[ai], defs);
                const Expr * te = chaseInBlock(pc->args[ai], defs);
                const MkThunk * th = te ? std::get_if<MkThunk>(te) : nullptr;
                if (dbg) std::fprintf(stderr, "  arg %zu var=%u tvar=%u isThunk=%d uses=%u\n",
                    ai, (unsigned)pc->args[ai], (unsigned)tvar, th ? 1 : 0,
                    uses.count(tvar) ? uses[tvar] : 0u);
                if (!th) continue;
                auto uit = uses.find(tvar);
                if (uit == uses.end() || uit->second != 1) continue;  // not use-once

                FuncId fid = th->funcIdx;
                if (fid == 0 || fid >= m.functions.size()) continue;
                const Function & f = m.functions[fid];
                if (f.paramVar != kInvalid || f.hasFormals
                    || !f.extraParams.empty()) continue;
                if (f.entryBlock == kInvalidBlock
                    || f.entryBlock >= m.blocks.size()) continue;
                const Block & body = m.blocks[f.entryBlock];
                const auto * term = std::get_if<TermReturn>(&body.terminal);
                if (!term || term->value == kInvalid) continue;

                // The whole body must be cloneable (remappable) — refuses
                // control flow / captured-state Exprs (remapExprVars false).
                bool ok = true;
                for (const auto & bb : body.bindings) {
                    Expr probe = bb.expr;
                    std::unordered_map<VarId, VarId> none;
                    if (!remapExprVars(probe, none)) { ok = false; break; }
                }
                if (!ok) continue;

                // sub: only the thunk-body LOCALS are renamed to fresh VarIds
                // (to keep SSA unique).  The body references its captured outer
                // vars DIRECTLY (v3's lowerer doesn't rebind upvalues), and
                // those vars are in scope wherever this use-once thunk sits —
                // so they are left untouched.
                std::unordered_map<VarId, VarId> sub;
                for (const auto & bb : body.bindings)
                    sub[bb.var] = m.freshVar();

                Action act;
                act.bindingIdx = bi;
                act.argIdx = ai;
                for (const auto & bb : body.bindings) {
                    Expr cloned = bb.expr;
                    (void)remapExprVars(cloned, sub);
                    act.inlined.push_back({ sub[bb.var], std::move(cloned) });
                }
                auto rit = sub.find(term->value);
                act.newArg = (rit != sub.end()) ? rit->second : term->value;
                actions.push_back(std::move(act));
                deadThunks.insert(tvar);
                ++count;
            }
        }

        if (actions.empty()) continue;

        // Rebuild: insert each target's inlined bindings BEFORE the binding,
        // and rewrite the primop arg to the inlined return value.
        std::vector<Binding> out;
        out.reserve(blk.bindings.size() + actions.size() * 2);
        size_t aidx = 0;
        for (size_t bi = 0; bi < blk.bindings.size(); ++bi) {
            while (aidx < actions.size() && actions[aidx].bindingIdx == bi) {
                for (auto & ib : actions[aidx].inlined)
                    out.push_back(std::move(ib));
                if (auto * pc = std::get_if<PrimOpCall>(&blk.bindings[bi].expr))
                    if (actions[aidx].argIdx < pc->args.size())
                        pc->args[actions[aidx].argIdx] = actions[aidx].newArg;
                ++aidx;
            }
            // Drop the now-dead de-thunked MkThunk binding.
            if (deadThunks.count(blk.bindings[bi].var)) continue;
            out.push_back(std::move(blk.bindings[bi]));
        }
        blk.bindings = std::move(out);
    }
    return count;
}

size_t appSpineFold(Module & m)
{
    static const bool s_dbg =
        std::getenv("V3_DBG_APP_SPINE_FOLD") != nullptr;

    // Use-count map: shared across blocks since lambdas may be
    // referenced from any function in the module.
    auto uses = countModuleUses(m);

    size_t folded = 0;

    const BlockId nOrigBlocks = static_cast<BlockId>(m.blocks.size());

    for (BlockId bid = 1; bid < nOrigBlocks; ++bid) {
        // Build defs map from the ORIGINAL bindings.
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(m.blocks[bid].bindings.size());
        for (const auto & bd : m.blocks[bid].bindings)
            defs.emplace(bd.var, &bd.expr);

        std::vector<Binding> out;
        out.reserve(m.blocks[bid].bindings.size() + 16);

        for (auto & bd : m.blocks[bid].bindings) {
            // Only App bindings are candidates.
            const App * app = std::get_if<App>(&bd.expr);
            if (!app) {
                out.push_back(std::move(bd));
                continue;
            }

            // Walk the App spine to collect (leafFun, args).
            auto sw = walkAppSpine(bd.var, defs);
            if (!sw) {
                out.push_back(std::move(bd));
                continue;
            }
            const size_t N = sw->args.size();
            if (N < 2 || N > kMaxSpineDepth) {
                // N == 1 is Phase A's domain; N > kMaxSpineDepth
                // exceeds our complexity budget.
                out.push_back(std::move(bd));
                continue;
            }
            if (s_dbg) std::fprintf(stderr,
                "v3 appSpineFold: B%u var=%u spine N=%zu leafFun=%u\n",
                (unsigned)bid, (unsigned)bd.var, N, (unsigned)sw->leafFun);

            // Resolve the leaf to a Lambda.
            VarId lambdaVar = chaseToBindingVar(sw->leafFun, defs);
            const Expr * leafExpr = chaseInBlock(sw->leafFun, defs);
            if (!leafExpr) {
                out.push_back(std::move(bd));
                continue;
            }
            const Lambda * leafLam = std::get_if<Lambda>(leafExpr);
            if (!leafLam) {
                out.push_back(std::move(bd));
                continue;
            }

            // Walk the curried chain N deep.  Returns nullopt if
            // the chain isn't N-deep curried form — then try the
            // eval/apply collapsed-arity-N form (one Function, paramVar +
            // extraParams) before giving up.
            auto chain = walkCurriedChain(m, leafLam->funcIdx, N);
            if (!chain)
                chain = walkCollapsedChain(m, leafLam->funcIdx, N);
            if (!chain) {
                if (s_dbg) std::fprintf(stderr, "  → skip: no N=%zu chain for f%u\n",
                    N, (unsigned)leafLam->funcIdx);
                out.push_back(std::move(bd));
                continue;
            }

            // Use-count safety: the leaf Lambda must be uniquely
            // referenced.  (Each intermediate Lambda is implicitly
            // also uniquely referenced — it's only ever returned by
            // its enclosing function's body, and the enclosing
            // function is itself uniquely referenced.)
            auto itU = uses.find(lambdaVar);
            if (itU == uses.end() || itU->second != 1) {
                if (s_dbg) std::fprintf(stderr, "  → skip: lambdaVar %u uses=%u (need 1)\n",
                    (unsigned)lambdaVar, itU == uses.end() ? 0u : itU->second);
                out.push_back(std::move(bd));
                continue;
            }

            // Safety: all args must be pure.
            bool allPure = true;
            for (VarId av : sw->args) {
                if (!isPureArg(av, defs)) { allPure = false; break; }
            }
            if (!allPure) {
                if (s_dbg) std::fprintf(stderr, "  → skip: an arg is impure\n");
                out.push_back(std::move(bd));
                continue;
            }

            // The deepest body must be simple.
            const Block & deepest = m.blocks[chain->back().bodyBlock];
            if (!deepestBodyIsSimple(deepest)) {
                if (s_dbg) std::fprintf(stderr, "  → skip: deepest body B%u not simple\n",
                    (unsigned)chain->back().bodyBlock);
                out.push_back(std::move(bd));
                continue;
            }

            // ALL preconditions met — clone deepest body with
            // N-way substitution.
            std::unordered_map<VarId, VarId> sub;
            sub.reserve(deepest.bindings.size() + N);
            // param_i → arg_i
            for (size_t i = 0; i < N; ++i)
                sub[(*chain)[i].paramVar] = sw->args[i];
            // body-local → fresh VarIds
            for (const auto & sub_bd : deepest.bindings)
                sub[sub_bd.var] = m.freshVar();

            // Emit cloned bindings into out.
            for (const auto & sub_bd : deepest.bindings) {
                Expr cloned = sub_bd.expr;
                (void)remapExprVars(cloned, sub);
                out.push_back({ sub[sub_bd.var], std::move(cloned) });
            }

            // Rewrite this binding's expr to VarRef the deepest
            // body's terminal value (substituted).
            VarId tailOrig = std::get<TermReturn>(deepest.terminal).value;
            auto itT = sub.find(tailOrig);
            VarId tail = (itT != sub.end()) ? itT->second : tailOrig;
            bd.expr = VarRef{ tail };
            out.push_back(std::move(bd));

            ++folded;
            if (s_dbg) {
                std::fprintf(stderr,
                    "v3 appSpineFold: B%u: folded N=%zu spine "
                    "(leaf Lambda f%u, %zu body bindings cloned)\n",
                    (unsigned)bid, N, (unsigned)leafLam->funcIdx,
                    deepest.bindings.size());
            }
        }
        m.blocks[bid].bindings = std::move(out);
    }

    return folded;
}

} // namespace nix::v3::ir
