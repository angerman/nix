/// @file
/// #742 Stage 4 v4 (2026-05-21) — caller-side strictness application.
///
/// Stage 4 v3 (opt_func_strictness.cc) populates
/// `ir::Function::strictArgs` — a per-formal-arg bitmap of which
/// args the function unconditionally forces.  v4 uses that bitmap
/// to skip MkThunk wraps at call sites whose callee is statically
/// known.
///
/// Transformation:
///   Original IR (after lower + optimise + strictness):
///     b1: arg_v       = <non-trivial>      # original arg
///     b2: arg_thunk   = MkThunk{tfid, ...}  # thunkifyForArg wrap
///     b3: f_lambda    = Lambda{cfid, ...}   # callee
///     b4: result      = App{f_lambda, arg_thunk}
///
///   If callee.strictArgs[0] is true:
///     b1: arg_v       = <non-trivial>
///     b2: <cloned thunk body bindings>     # inlined
///     b3: f_lambda    = Lambda{cfid, ...}
///     b4: result      = App{f_lambda, cloned_tail}
///   (arg_thunk binding becomes dead; DCE sweeps it.)
///
/// Safety conditions (parallel beta-reduce's safety checks):
///   1. App's `fun` resolves (via same-block VarRef chain) to a
///      Lambda IR node.
///   2. The Lambda's funcIdx points at a Function `callee` where:
///      - `callee.strictArgs` is populated AND `strictArgs[0] == true`.
///      - `callee.hasFormals == false` (single-arg lambdas only;
///        formals-style would need attrset-entry-level rewriting,
///        deferred to v4.1).
///   3. App's `arg` resolves to a MkThunk IR node in same block.
///   4. The MkThunk's funcIdx points at a Function whose entryBlock's
///      bindings pass `bodyIsSimple` (no nested sub-blocks /
///      Function-creators that would require recursive cloning).
///   5. The MkThunk binding's VarId has exactly ONE use (the App
///      we're processing) — prevents work-duplication if the same
///      thunk feeds multiple call sites.
///
/// Gate: `NIX_V3_NO_STRICT_CALL_UNTHUNK=1` disables for A/B testing.
/// Telemetry: `NIX_V3_DBG_STRICT_CALL_UNTHUNK=1` prints the count of
/// elisions at the end of the pass.
///
/// Runs AFTER `computeFunctionStrictness` (so strictArgs is populated)
/// and BEFORE `computeFreeVars` (so the inlined bindings get included
/// in the post-pass freeVars walk).  Wired into run.cc directly
/// because optimise() runs before strictness.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

// Helpers duplicated from opt_beta_reduce.cc.  They're tightly bound
// to that pass's algorithm but the surface is small enough to copy
// rather than refactor into a shared header — the duplication keeps
// each pass's safety contract local.

inline void remapVar(VarId & v, const std::unordered_map<VarId, VarId> & sub)
{
    auto it = sub.find(v);
    if (it != sub.end()) v = it->second;
}

inline void remapVarVec(std::vector<VarId> & vs,
                        const std::unordered_map<VarId, VarId> & sub)
{
    for (auto & v : vs) remapVar(v, sub);
}

bool remapExprVars(Expr & e, const std::unordered_map<VarId, VarId> & sub)
{
    return std::visit([&](auto & v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, LitInt>    || std::is_same_v<T, LitFloat>
                   || std::is_same_v<T, LitBool>   || std::is_same_v<T, LitNull>
                   || std::is_same_v<T, LitString> || std::is_same_v<T, LitPath>
                   || std::is_same_v<T, LitPrimOp> || std::is_same_v<T, LitBuiltins>
                   || std::is_same_v<T, WithLookup>)
            return true;
        else if constexpr (std::is_same_v<T, VarRef>)            { remapVar(v.var, sub);     return true; }
        else if constexpr (std::is_same_v<T, Force>)             { remapVar(v.thunk, sub);   return true; }
        else if constexpr (std::is_same_v<T, AttrSelect>)        { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, HasAttr>)           { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, RecBindingSlotRef>) { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, Not>)               { remapVar(v.operand, sub); return true; }
        else if constexpr (std::is_same_v<T, App>)         { remapVar(v.fun, sub);   remapVar(v.arg, sub);     return true; }
        else if constexpr (std::is_same_v<T, AttrSelectDyn>) { remapVar(v.attrs, sub); remapVar(v.nameVar, sub); return true; }
        else if constexpr (std::is_same_v<T, HasAttrDyn>)   { remapVar(v.attrs, sub); remapVar(v.nameVar, sub); return true; }
        else if constexpr (std::is_same_v<T, ConcatLists>) { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Update>)      { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Add>)         { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Sub>)         { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Mul>)         { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Div>)         { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Eq>)          { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, NEq>)         { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, Less>)        { remapVar(v.lhs, sub);   remapVar(v.rhs, sub);     return true; }
        else if constexpr (std::is_same_v<T, ListExpr>)        { remapVarVec(v.elems, sub); return true; }
        else if constexpr (std::is_same_v<T, ConcatStrings>)   { remapVarVec(v.parts, sub); return true; }
        else if constexpr (std::is_same_v<T, PrimOpCall>)      { remapVarVec(v.args, sub); return true; }
        else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & ent : v.entries) remapVar(ent.value, sub);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) {
            remapVar(v.attrSetVar, sub);
            for (auto & ent : v.entries) remapVar(ent.valueVar, sub);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (auto & s : v.statics) remapVar(s.value, sub);
            for (auto & d : v.dynamics) {
                remapVar(d.nameVar, sub);
                remapVar(d.value, sub);
            }
            return true;
        }
        // Refuse cloning: sub-block / sub-Function carriers.
        else if constexpr (std::is_same_v<T, Lambda>   || std::is_same_v<T, MkThunk>
                        || std::is_same_v<T, If>       || std::is_same_v<T, With>
                        || std::is_same_v<T, Assert>   || std::is_same_v<T, And>
                        || std::is_same_v<T, Or>       || std::is_same_v<T, Impl>
                        || std::is_same_v<T, LetRec>)
            return false;
        else {
            (void)v;
            return false;
        }
    }, e);
}

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

// Variant that returns BOTH the VarId at which the resolution
// stopped (i.e. the actual definer of the underlying expression)
// AND the Expr*.  Used because v4 needs to look up the MkThunk's
// USE COUNT — the count for the binding's defining VarId is what
// matters (chasing through VarRefs the count includes those uses
// too, but for safety we want to ensure the MkThunk itself is
// used exactly once).
struct ResolveResult {
    VarId           definer;
    const Expr *    expr;
};

ResolveResult chaseInBlockResolved(
    VarId v,
    const std::unordered_map<VarId, const Expr *> & defs)
{
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        auto it = defs.find(v);
        if (it == defs.end()) return {kInvalid, nullptr};
        const Expr * e = it->second;
        if (const auto * vr = std::get_if<VarRef>(e)) {
            v = vr->var;
            continue;
        }
        return {v, e};
    }
    return {kInvalid, nullptr};
}

std::unordered_map<VarId, const Expr *> mapBlockDefs(const Block & b)
{
    std::unordered_map<VarId, const Expr *> defs;
    defs.reserve(b.bindings.size());
    for (const auto & bd : b.bindings)
        defs.emplace(bd.var, &bd.expr);
    return defs;
}

struct UseCounter {
    std::unordered_map<VarId, uint32_t> count;
    uint32_t at(VarId v) const {
        auto it = count.find(v);
        return it == count.end() ? 0 : it->second;
    }
    void bump(VarId v) { ++count[v]; }
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
        else if constexpr (std::is_same_v<T, Lambda>)  for (auto x : v.freeVars) uc.bump(x);
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

bool bodyIsSimple(const Block & body)
{
    static const std::unordered_map<VarId, VarId> empty;
    for (const auto & bd : body.bindings) {
        Expr probe = bd.expr;
        if (!remapExprVars(probe, empty))
            return false;
    }
    return true;
}

/// Clone body's bindings into `out` with fresh local VarIds.  Outer
/// VarIds (the thunk's free variables) stay unchanged.  Returns the
/// VarId that the body's TermReturn ultimately resolves to (after
/// substitution).  No paramVar substitution because thunks have no
/// formal parameter.
VarId inlineThunkBody(Module & m,
                      const Block & body,
                      std::vector<Binding> & out)
{
    std::unordered_map<VarId, VarId> sub;
    sub.reserve(body.bindings.size());
    for (const auto & bd : body.bindings) {
        VarId newVar = m.freshVar();
        sub[bd.var] = newVar;
        Expr cloned = bd.expr;
        (void)remapExprVars(cloned, sub);  // bodyIsSimple already verified
        out.push_back({newVar, std::move(cloned)});
    }
    VarId tailOriginal = std::get<TermReturn>(body.terminal).value;
    auto it = sub.find(tailOriginal);
    return (it != sub.end()) ? it->second : tailOriginal;
}

/// Resolve a call site's `fun` VarId to a Lambda IR node, following
/// VarRef alias chains AND single-step RecBindingSlotRef → LetRec
/// entry → entry-thunk-body → returned-Lambda chains.
///
/// The let-bound case is the dominant real-world pattern:
///
///   let f = x: body;        # lowers to LetRec entry whose thunkBody
///   in f arg;               # is a Function returning the Lambda value
///
/// Body's reference to `f` lowers as `RecBindingSlotRef{letRec, f_name}`.
/// The LetRec entry's thunkBody is a Function whose entryBlock is:
///   v_lam = Lambda{user_funcIdx, freeVars=...}
///   return v_lam
///
/// We chain through to find user_funcIdx so Stage 4 v4 can apply the
/// callee's strictArgs signature.
const Lambda * resolveCalleeLambda(
    VarId funVar,
    const Module & m,
    const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(funVar, defs);
    if (!e) return nullptr;

    // Direct Lambda binding (the inline `((x: body) arg)` case
    // — same as beta-reduce's safety check).
    if (const auto * lam = std::get_if<Lambda>(e))
        return lam;

    // RecBindingSlotRef: trace through the LetRec.  Note that
    // rb->attrs is typically the recSlotVar (heap-stable slot
    // pointer; v3 default with NIX_V3_NO_REC_SLOT_CAPTURE unset),
    // NOT the LetRec binding's defining VarId directly.  Resolve
    // via Module::recVarToSlotVar (reverse lookup) if available.
    const auto * rb = std::get_if<RecBindingSlotRef>(e);
    if (!rb) return nullptr;
    // First try: rb->attrs is the LetRec binding directly.
    const LetRec * lr = nullptr;
    auto recIt = defs.find(rb->attrs);
    if (recIt != defs.end()) {
        if (const auto * direct = std::get_if<LetRec>(recIt->second))
            lr = direct;
    }
    // Fallback: rb->attrs is a recSlotVar.  Find the recVar from
    // Module::recVarToSlotVar (which maps recVar → slotVar), then
    // look up the LetRec binding at recVar.
    if (!lr) {
        for (const auto & [recVar, slotVar] : m.recVarToSlotVar) {
            if (slotVar != rb->attrs) continue;
            auto it = defs.find(recVar);
            if (it != defs.end()) {
                if (const auto * direct = std::get_if<LetRec>(it->second)) {
                    lr = direct;
                    break;
                }
            }
        }
    }
    if (!lr) return nullptr;
    // Find entry whose name matches.
    for (const auto & ent : lr->entries) {
        if (ent.name != rb->name) continue;
        // ent.thunkBody is the FuncId of the entry's Function.
        if (ent.thunkBody >= (FuncId)m.functions.size()) return nullptr;
        const Function & entryFn = m.functions[ent.thunkBody];
        if (entryFn.entryBlock == kInvalidBlock
            || entryFn.entryBlock >= (BlockId)m.blocks.size())
            return nullptr;
        const Block & entryBlk = m.blocks[entryFn.entryBlock];
        const auto * tr = std::get_if<TermReturn>(&entryBlk.terminal);
        if (!tr || tr->value == kInvalid) return nullptr;
        // Find the binding whose VarId matches the return value.
        // Chase one VarRef hop within the entry block.
        VarId target = tr->value;
        for (size_t hops = 0; hops < entryBlk.bindings.size() + 1; ++hops) {
            bool advanced = false;
            for (const auto & bd : entryBlk.bindings) {
                if (bd.var != target) continue;
                if (const auto * vr = std::get_if<VarRef>(&bd.expr)) {
                    target = vr->var;
                    advanced = true;
                    break;
                }
                // Direct Lambda?  Return it.
                return std::get_if<Lambda>(&bd.expr);
            }
            if (!advanced) break;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: applyStrictnessAtCallSites.
// ---------------------------------------------------------------------------

size_t applyStrictnessAtCallSites(Module & m)
{
    static const bool disabled =
        std::getenv("NIX_V3_NO_STRICT_CALL_UNTHUNK") != nullptr;
    if (disabled) return 0;
    static const bool dbg =
        std::getenv("NIX_V3_DBG_STRICT_CALL_UNTHUNK") != nullptr;

    // Module-wide use count for the "MkThunk has one use" safety check.
    UseCounter uses = countModuleUses(m);

    size_t elided = 0;
    size_t consideredApps = 0;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        auto defs = mapBlockDefs(blk);

        std::vector<Binding> out;
        out.reserve(blk.bindings.size() * 2);

        for (const auto & bd : blk.bindings) {
            // Default action: keep binding unchanged.  Each `continue`
            // below short-circuits to this path.
            const App * app = std::get_if<App>(&bd.expr);
            if (!app) { out.push_back(bd); continue; }
            ++consideredApps;

            // Resolve `fun` to a Lambda — direct inline OR through
            // a same-block RecBindingSlotRef → LetRec.entry chain.
            const Lambda * lam = resolveCalleeLambda(app->fun, m, defs);
            if (!lam) { out.push_back(bd); continue; }

            // Look up callee Function.
            if (lam->funcIdx >= (FuncId)m.functions.size()) {
                out.push_back(bd); continue;
            }
            const Function & callee = m.functions[lam->funcIdx];

            // v4 scope: single-arg lambdas only.  Formals-style lambdas
            // would need attrset-entry-level rewriting (the App's arg is
            // the whole attrset, not individual formals); deferred.
            if (callee.hasFormals) { out.push_back(bd); continue; }

            // Strict-arg-0 required (paramVar slot).
            if (callee.strictArgs.empty() || !callee.strictArgs[0]) {
                out.push_back(bd); continue;
            }

            // Resolve `arg` to a MkThunk in the same block.
            auto argRes = chaseInBlockResolved(app->arg, defs);
            if (!argRes.expr) { out.push_back(bd); continue; }
            const MkThunk * mkt = std::get_if<MkThunk>(argRes.expr);
            if (!mkt) { out.push_back(bd); continue; }

            // Safety: the MkThunk's defining VarId must have exactly
            // ONE use — the App we're about to rewrite.  Otherwise
            // other consumers expect the thunk's lazy semantics.
            if (uses.at(argRes.definer) != 1) { out.push_back(bd); continue; }

            // Thunk body must be "simple" (no nested sub-block /
            // Function-creator nodes that would require recursive
            // cloning).
            if (mkt->funcIdx >= (FuncId)m.functions.size()) {
                out.push_back(bd); continue;
            }
            const Function & thunkFn = m.functions[mkt->funcIdx];
            if (thunkFn.entryBlock == kInvalidBlock
                || thunkFn.entryBlock >= (BlockId)m.blocks.size()) {
                out.push_back(bd); continue;
            }
            const Block & thunkBody = m.blocks[thunkFn.entryBlock];
            if (!bodyIsSimple(thunkBody)) {
                out.push_back(bd); continue;
            }

            // All safety checks passed — inline the thunk body.
            VarId tail = inlineThunkBody(m, thunkBody, out);
            // Emit the App with the elided arg.
            out.push_back({bd.var, App{app->fun, tail}});
            ++elided;
        }

        blk.bindings = std::move(out);
    }

    if (dbg) {
        std::fprintf(stderr,
            "v3 stage4 v4 strict-call-unthunk: elided=%zu "
            "of %zu Apps considered\n",
            elided, consideredApps);
    }

    return elided;
}

} // namespace nix::v3::ir
