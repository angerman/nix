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
/// Unconditional (the NIX_V3_NO_STRICT_CALL_UNTHUNK A/B opt-out was retired
/// once it shipped byte-identical).
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
#include "v3/ir_util.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
        // #743 v4.1 — Lambda / MkThunk are cloneable WITHOUT
        // recursively cloning their sub-Function: both old and new
        // bindings share the same `funcIdx`.  Only `freeVars` (and
        // for Lambda/MkThunk `lws` — lexical-with capture list)
        // need VarId remapping.  This unblocks inlining of outer
        // thunks that wrap an AttrSet whose entries are themselves
        // MkThunk bindings — the common formals-style call site
        // shape `f { a = e1; b = e2; }`.
        //
        // Note: `freeVars` is populated by computeFreeVars, which
        // runs AFTER this pass.  So at clone time freeVars is
        // empty and remapping is a no-op — the post-pass freeVars
        // analysis rebuilds them in the cloned binding's location.
        else if constexpr (std::is_same_v<T, Lambda>) {
            remapVarVec(v.freeVars, sub);
            return true;
        }
        else if constexpr (std::is_same_v<T, MkThunk>) {
            remapVarVec(v.freeVars, sub);
            return true;
        }
        // Refuse cloning: sub-block carriers that would need
        // recursive block cloning (out of v4.1 scope).
        else if constexpr (std::is_same_v<T, If>       || std::is_same_v<T, With>
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

// chaseInBlock() is shared across opt_*.cc passes — see v3/ir_util.hh.

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

// mapBlockDefs() is shared across opt_*.cc passes — see v3/ir_util.hh.

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

// #744 v4.2 — forward declarations for recursive sub-block cloning.
bool bodyIsCloneable(const Module & m, BlockId srcBid,
                     std::unordered_set<BlockId> & visited);
VarId cloneBlockBindings(Module & m, BlockId srcBid,
                         std::unordered_map<VarId, VarId> & sub,
                         std::vector<Binding> & out);
BlockId cloneSubBlock(Module & m, BlockId srcBid,
                      const std::unordered_map<VarId, VarId> & parentSub);

/// #744 v4.2 — predicate: can the block at `srcBid` (and any
/// sub-blocks reachable through If/With/Assert/And/Or/Impl) be
/// cloned by `cloneBlockBindings`?  Recursive; tracks visited
/// blocks to avoid infinite cycles (shouldn't happen in a
/// well-formed IR but defensive).  Returns false on:
///   - Invalid BlockId.
///   - Any binding containing LetRec (entry-Function clone is
///     out of scope; would also need to clone the per-entry
///     Functions' bodies with recVar substitution).
///   - Any nested sub-block that itself fails the check.
bool bodyIsCloneable(const Module & m, BlockId srcBid,
                     std::unordered_set<BlockId> & visited)
{
    if (srcBid == kInvalidBlock || srcBid >= (BlockId)m.blocks.size())
        return false;
    if (!visited.insert(srcBid).second) return true;
    const Block & b = m.blocks[srcBid];
    for (const auto & bd : b.bindings) {
        bool ok = std::visit([&](const auto & v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, If>) {
                return bodyIsCloneable(m, v.thenBlock, visited)
                    && bodyIsCloneable(m, v.elseBlock, visited);
            } else if constexpr (std::is_same_v<T, With>
                              || std::is_same_v<T, Assert>) {
                return bodyIsCloneable(m, v.bodyBlock, visited);
            } else if constexpr (std::is_same_v<T, And>
                              || std::is_same_v<T, Or>
                              || std::is_same_v<T, Impl>) {
                return bodyIsCloneable(m, v.rhsBlock, visited);
            } else if constexpr (std::is_same_v<T, LetRec>) {
                // LetRec entry Functions reference recVar; cloning
                // the LetRec with a fresh recVar would require
                // cloning the per-entry Function bodies too.
                // Refuse for v4.2.  (v4.3 could lift this.)
                return false;
            } else {
                // Non-sub-block expr — check via remapExprVars
                // with empty substitution (probe only).
                Expr probe = bd.expr;
                static const std::unordered_map<VarId, VarId> empty;
                return remapExprVars(probe, empty);
            }
        }, bd.expr);
        if (!ok) return false;
    }
    return true;
}

/// #744 v4.2 — clone the bindings of `srcBid` into `out` with
/// fresh local VarIds.  Sub-blocks (If/With/Assert/And/Or/Impl)
/// get fresh BlockIds via `cloneSubBlock`; their bindings are
/// cloned recursively.  Returns the tail VarId (the cloned
/// terminal's return value) or `kInvalid` if any binding refuses
/// to clone (callers should treat as failure and not splice
/// `out`).
///
/// `sub` is mutable: each binding adds (oldVar → newVar) for
/// downstream remapping.  Outer VarIds (free variables of the
/// thunk body that reference the surrounding scope) stay
/// unchanged — they're not in `sub`, so remapVar leaves them.
VarId cloneBlockBindings(Module & m, BlockId srcBid,
                         std::unordered_map<VarId, VarId> & sub,
                         std::vector<Binding> & out)
{
    if (srcBid == kInvalidBlock || srcBid >= (BlockId)m.blocks.size())
        return kInvalid;
    // Snapshot by value — `m.blocks` may reallocate during nested
    // `cloneSubBlock` invocations (when `freshBlock()` resizes
    // the underlying vector).  References into m.blocks[srcBid]
    // would dangle after that point; copying the bindings + terminal
    // up front avoids the hazard.
    std::vector<Binding> srcBindings = m.blocks[srcBid].bindings;
    Terminal srcTerm = m.blocks[srcBid].terminal;
    for (auto & bd : srcBindings) {
        VarId newVar = m.freshVar();
        sub[bd.var] = newVar;
        Expr cloned = bd.expr;
        bool ok = std::visit([&](auto & v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, If>) {
                remapVar(v.cond, sub);
                v.thenBlock = cloneSubBlock(m, v.thenBlock, sub);
                v.elseBlock = cloneSubBlock(m, v.elseBlock, sub);
                return v.thenBlock != kInvalidBlock
                    && v.elseBlock != kInvalidBlock;
            } else if constexpr (std::is_same_v<T, With>) {
                remapVar(v.attrs, sub);
                v.bodyBlock = cloneSubBlock(m, v.bodyBlock, sub);
                return v.bodyBlock != kInvalidBlock;
            } else if constexpr (std::is_same_v<T, Assert>) {
                remapVar(v.cond, sub);
                v.bodyBlock = cloneSubBlock(m, v.bodyBlock, sub);
                return v.bodyBlock != kInvalidBlock;
            } else if constexpr (std::is_same_v<T, And>
                              || std::is_same_v<T, Or>
                              || std::is_same_v<T, Impl>) {
                remapVar(v.lhs, sub);
                v.rhsBlock = cloneSubBlock(m, v.rhsBlock, sub);
                return v.rhsBlock != kInvalidBlock;
            } else {
                return remapExprVars(cloned, sub);
            }
        }, cloned);
        if (!ok) return kInvalid;
        out.push_back({newVar, std::move(cloned)});
    }
    if (const auto * tr = std::get_if<TermReturn>(&srcTerm)) {
        auto it = sub.find(tr->value);
        return (it != sub.end()) ? it->second : tr->value;
    }
    return kInvalid;
}

/// #744 v4.2 — clone a sub-block referenced from If/With/etc.
/// Allocates a fresh BlockId, clones bindings into the new block's
/// vector, sets the terminal.  Returns the new BlockId or
/// kInvalidBlock on failure.  `parentSub` is COPIED so the
/// sub-block's local bindings don't leak back to the parent's
/// substitution map.
BlockId cloneSubBlock(Module & m, BlockId srcBid,
                      const std::unordered_map<VarId, VarId> & parentSub)
{
    if (srcBid == kInvalidBlock || srcBid >= (BlockId)m.blocks.size())
        return kInvalidBlock;
    // freshBlock may grow m.blocks — DO NOT hold references.
    BlockId newBid = m.freshBlock();
    std::unordered_map<VarId, VarId> sub = parentSub;
    std::vector<Binding> newBindings;
    VarId tail = cloneBlockBindings(m, srcBid, sub, newBindings);
    if (tail == kInvalid) {
        // The cloned block has been allocated but won't be referenced.
        // It's a wasted slot but harmless.  Return failure so the
        // caller knows not to use this BlockId.
        return kInvalidBlock;
    }
    m.blocks[newBid].bindings = std::move(newBindings);
    m.blocks[newBid].terminal = TermReturn{tail};
    return newBid;
}

/// Inline a thunk body into `out`.  Thin wrapper over
/// `cloneBlockBindings` that initializes an empty substitution map.
/// Returns the cloned tail VarId or kInvalid on failure.
VarId inlineThunkBody(Module & m, BlockId srcBid,
                      std::vector<Binding> & out)
{
    std::unordered_map<VarId, VarId> sub;
    return cloneBlockBindings(m, srcBid, sub, out);
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

    // RecBindingSlotRef: trace through the LetRec.  Three resolution
    // strategies tried in order:
    //   1. rb->attrs is the LetRec binding's defining VarId directly.
    //   2. rb->attrs is a recSlotVar, mapped via
    //      Module::recVarToSlotVar (reverse lookup).
    //   3. Structural match (#743 v4.1 fallback): walk all LetRec
    //      bindings in same block; pick one whose entries contains a
    //      name matching rb->name.  Robust to post-optimise VarId
    //      renumbering that breaks the strict (recVar, slotVar)
    //      bookkeeping path.
    const auto * rb = std::get_if<RecBindingSlotRef>(e);
    if (!rb) return nullptr;
    const LetRec * lr = nullptr;
    auto recIt = defs.find(rb->attrs);
    if (recIt != defs.end()) {
        if (const auto * direct = std::get_if<LetRec>(recIt->second))
            lr = direct;
    }
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
    if (!lr) {
        // Structural fallback — pick first same-block LetRec whose
        // entries contain `rb->name`.  In real Nix, multiple LetRecs
        // in one block with the same entry name is rare; if it
        // happens, the conservative outcome (picking the first match
        // for a shadowed name) is "miss the strictness opportunity,"
        // not a correctness violation.
        //
        // R1 trigger follow-up (2026-05-26, after Schema 14 PosIdx
        // remap landed): `defs` is `std::unordered_map<VarId, const
        // Expr *>` — iteration order is per-process non-deterministic.
        // Two compilations of the same source can pick DIFFERENT
        // matching LetRecs, which then cascades into different
        // downstream VarId / local-slot assignments and produces the
        // OP_GET_LOCAL operand drift V3_DBG_DESERIALIZE_VERIFY catches
        // post-Schema-14.  Sort by VarId first — VarIds are
        // monotonically assigned during lowering, so sorted iteration
        // is structurally deterministic.
        std::vector<std::pair<VarId, const Expr *>> sortedDefs;
        sortedDefs.reserve(defs.size());
        for (const auto & kv : defs) sortedDefs.push_back(kv);
        std::sort(sortedDefs.begin(), sortedDefs.end(),
            [](const auto & a, const auto & b) {
                return a.first < b.first;
            });
        // C-15 (CODEBASE_REVIEW_2026-06-11): require a UNIQUE match.  Picking
        // the first same-block LetRec with an entry named rb->name is NOT
        // merely "miss the strictness opportunity" when two different LetRecs
        // share that entry name: the picked function's strictness mask may be
        // STRICTER than the real callee's, so the call site de-thunks an arg
        // the actual callee leaves lazy → over-forcing that throws where TW
        // doesn't.  If the match is ambiguous, bail (return nullptr below).
        const LetRec * uniqueMatch = nullptr;
        bool ambiguous = false;
        for (const auto & [varId, exprPtr] : sortedDefs) {
            const auto * candidate = std::get_if<LetRec>(exprPtr);
            if (!candidate) continue;
            for (const auto & ent : candidate->entries) {
                if (ent.name == rb->name) {
                    if (uniqueMatch && uniqueMatch != candidate)
                        ambiguous = true;
                    else
                        uniqueMatch = candidate;
                    break;
                }
            }
        }
        if (!ambiguous) lr = uniqueMatch;
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

// #743 v4.1 / #744 v4.2 helper: is the MkThunk safe to inline at
// this call site?  Checks: arg is a MkThunk in same block, exactly
// one use, body is `bodyIsCloneable` (transitively cloneable
// including sub-blocks).  Returns true on success and fills the
// out-params; false on any safety failure.  Note that `outBody`
// is no longer used post-v4.2 (cloneBlockBindings takes the
// BlockId directly via the MkThunk's funcIdx), but kept for
// signature compatibility.
static bool isInlinableMkThunk(VarId argVar, const Module & m,
                                const std::unordered_map<VarId, const Expr *> & defs,
                                const UseCounter & uses,
                                VarId & outDefVar,
                                const MkThunk * & outMkt,
                                const Block * & outBody)
{
    auto res = chaseInBlockResolved(argVar, defs);
    if (!res.expr) return false;
    const MkThunk * mkt = std::get_if<MkThunk>(res.expr);
    if (!mkt) return false;
    if (uses.at(res.definer) != 1) return false;
    if (mkt->funcIdx >= (FuncId)m.functions.size()) return false;
    const Function & thunkFn = m.functions[mkt->funcIdx];
    if (thunkFn.entryBlock == kInvalidBlock
        || thunkFn.entryBlock >= (BlockId)m.blocks.size()) return false;
    // v4.2: recursive cloneability check.
    std::unordered_set<BlockId> visited;
    if (!bodyIsCloneable(m, thunkFn.entryBlock, visited)) return false;
    outDefVar = res.definer;
    outMkt    = mkt;
    outBody   = &m.blocks[thunkFn.entryBlock];
    return true;
}

/// Resolve a LetRec entry `name` to the funcIdx of the Lambda its thunk body
/// returns (mirrors resolveCalleeLambda's entry→Lambda chase, yielding a
/// pointer-STABLE FuncId).  Returns m.functions.size() when it doesn't resolve
/// to a Lambda.  Used to pre-resolve SELF-RECURSIVE callees before the pass
/// rebuilds block-binding vectors.
static FuncId resolveLetRecEntryFuncIdx(const Module & m, const LetRec & lr,
                                        SymbolId name)
{
    const FuncId none = (FuncId)m.functions.size();
    for (const auto & ent : lr.entries) {
        if (ent.name != name) continue;
        if (ent.thunkBody >= none) return none;
        const Function & entryFn = m.functions[ent.thunkBody];
        if (entryFn.entryBlock == kInvalidBlock
            || entryFn.entryBlock >= (BlockId)m.blocks.size()) return none;
        const Block & entryBlk = m.blocks[entryFn.entryBlock];
        const auto * tr = std::get_if<TermReturn>(&entryBlk.terminal);
        if (!tr || tr->value == kInvalid) return none;
        VarId target = tr->value;
        for (size_t hops = 0; hops < entryBlk.bindings.size() + 1; ++hops) {
            bool advanced = false;
            for (const auto & bd : entryBlk.bindings) {
                if (bd.var != target) continue;
                if (const auto * vr = std::get_if<VarRef>(&bd.expr)) {
                    target = vr->var; advanced = true; break;
                }
                if (const auto * lam = std::get_if<Lambda>(&bd.expr))
                    return lam->funcIdx;
                return none;
            }
            if (!advanced) break;
        }
        return none;
    }
    return none;
}

size_t applyStrictnessAtCallSites(Module & m)
{
    static const bool dbg =
        std::getenv("NIX_V3_DBG_STRICT_CALL_UNTHUNK") != nullptr;
    // #2(B) eager-forced-let: a MkThunk that is unconditionally Force'd in
    // its block is eager-ized (body inlined at the binding site, x kept as
    // the value).  Unlike the single-use elide, this is MULTI-use-safe — the
    // Force is unconditional, so x is forced regardless of its other uses, so
    // evaluating it eagerly forces nothing the program wouldn't have.  The
    // bytecode foldl''s `let next = op acc elem; in seq next (go (i+1) next)`
    // (next: forced by seq, also captured by the go-call thunk) is the
    // motivating shape — now the unconditional eager path.

    // Module-wide use count for the "MkThunk has one use" safety check.
    UseCounter uses = countModuleUses(m);

    size_t elidedSingleArg = 0;
    size_t elidedFormals   = 0;
    size_t elidedForceMkt  = 0;  // #775 let-inline-strict via Force(MkThunk)
    size_t consideredApps  = 0;
    size_t consideredForces= 0;
    size_t passedToInlineForce = 0;
    // #775 Case (C) sub-funnel: same buckets as App funnel.
    size_t forceFailNotMkThunk   = 0;
    size_t forceFailMultiUse     = 0;
    size_t forceFailNotCloneable = 0;
    // #776 measurement spike — when forceFailNotMkThunk fires (operand
    // not resolvable in same block), classify what the operand WOULD
    // resolve to under a module-wide chase.  Quantifies the upper
    // bound of let-floating's payoff before we commit to the lift.
    //
    //   globalNotFound         — no def in any block (function param /
    //                            RecBindingSlotRef target / unresolved).
    //   globalMkThunkRemote    — chase finds MkThunk in a DIFFERENT block
    //                            (the LIFT candidate population).
    //   globalMkThunkRemoteOne — globalMkThunkRemote AND single-use
    //                            module-wide (REALIST lift candidate).
    //   globalOtherExpr        — chase finds something other than MkThunk
    //                            (App, Force, AttrSelect, ...).
    size_t globalNotFound          = 0;
    size_t globalMkThunkRemote     = 0;
    size_t globalMkThunkRemoteOne  = 0;
    size_t globalOtherExpr         = 0;

    // #776 measurement spike — module-wide var→Expr map for cross-block
    // chasing of Force operands.  Gated by `dbg` (NIX_V3_DBG_STRICT_CALL_
    // UNTHUNK) so production builds pay zero cost.  Falsified the
    // let-floating premise on hello.drvPath:
    //   379 modules / 64175 Apps / 12356 Force-of-MkThunk failures
    //   globalMkThunkRemote = 0 / 12356  (zero lift candidates ANYWHERE)
    //   globalNotFound      = 6332 (51.2% — operand is lambda param /
    //                               formal / RecBindingSlotRef target)
    //   globalOtherExpr     = 6024 (48.8% — operand chases to App /
    //                               AttrSelect / Force / non-MkThunk)
    // Kept as a re-run probe if upstream lowering ever changes shape.
    std::unordered_map<VarId, const Expr *> globalDefs;
    if (dbg) {
        size_t total = 0;
        for (const auto & b : m.blocks) total += b.bindings.size();
        globalDefs.reserve(total);
        for (const auto & b : m.blocks)
            for (const auto & bd : b.bindings)
                globalDefs.emplace(bd.var, &bd.expr);
    }
    // #775 instrumentation: funnel breakdown to find WHY isInlinableMkThunk
    // rejects most strict-hit candidates.  Goal: identify whether (a) the
    // arg isn't a MkThunk in same block, (b) MkThunk has multiple uses, or
    // (c) body isn't cloneable.  Each is a different remediation lane.
    size_t failResolveLambda = 0;
    size_t failStrictArgs    = 0;
    size_t passedToInline    = 0;
    size_t failNotMkThunk    = 0;
    size_t failMultiUse      = 0;
    size_t failNotCloneable  = 0;
    size_t elidedRecursive   = 0;

    // #3(B) strictArgs through recursion: a self-recursive `go (i+1) next`
    // references `go` via a RecBindingSlotRef whose LetRec is in an ENCLOSING
    // block, which the per-block `defs` resolveCalleeLambda consults never sees
    // — so the recursive call failed to resolve and its (now arity-N, eval/
    // apply) strictArgs were never applied.  Pre-resolve (recVar|slotVar,
    // entryName) → callee FuncId on the UN-mutated module: store FuncIds
    // (stable), never Expr*, so the per-block rebuilds below can't dangle them.
    // rb->attrs may be the recVar OR its Tag::Slot companion → key both.
    std::unordered_map<VarId, std::unordered_map<SymbolId, FuncId>> recCallTarget;
    for (const auto & b : m.blocks)
        for (const auto & bd : b.bindings)
            if (const auto * lr = std::get_if<LetRec>(&bd.expr)) {
                std::vector<VarId> keys{lr->recVar};
                auto sv = m.recVarToSlotVar.find(lr->recVar);
                if (sv != m.recVarToSlotVar.end()) keys.push_back(sv->second);
                for (const auto & ent : lr->entries) {
                    FuncId fi = resolveLetRecEntryFuncIdx(m, *lr, ent.name);
                    if (fi < (FuncId)m.functions.size())
                        for (VarId k : keys) recCallTarget[k][ent.name] = fi;
                }
            }

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        auto defs = mapBlockDefs(blk);

        // ----- PRE-ANALYSIS -----
        // Identify all MkThunk binding VarIds that should be elided
        // (their body inlined and the MkThunk binding skipped during
        // the main rewrite pass).  Two patterns:
        //
        //   single-arg: App{fun, arg} where arg is a MkThunk and the
        //               callee is single-arg with strictArgs[0]=true.
        //
        //   formals-style: App{fun, attrSet} where attrSet is an
        //               AttrSet binding and the callee is formals-
        //               style.  For each strict-formal name with a
        //               matching entry whose value is a MkThunk,
        //               mark for elision.
        std::unordered_set<VarId> mkthunksToElide;
        std::unordered_set<VarId> mkthunksToEager;  // #2(B) eager-keep

        for (const auto & bd : blk.bindings) {
            // #775 Case (C): Force(MkThunk_binding) — let-inline-strict
            // via syntactic Force.  Force is strict by definition;
            // if its thunk argument is a single-use MkThunk in this
            // block, inline the body (rewrite Force binding to a
            // VarRef into the inlined body's tail).  Captures the
            // `let x = e; in x + 1` pattern that resolveCalleeLambda-
            // based elision misses (the strict-callee chain only
            // fires at App sites, not at Force sites).
            if (const auto * f = std::get_if<Force>(&bd.expr)) {
                ++consideredForces;
                // Inline funnel: figure out which leg of isInlinable*
                // rejects.  We can't reuse the App's funnel because the
                // resolveCalleeLambda step doesn't apply here — Force is
                // strict by definition.
                auto fres = chaseInBlockResolved(f->thunk, defs);
                if (!fres.expr || !std::get_if<MkThunk>(fres.expr)) {
                    ++forceFailNotMkThunk;
                    // #776 spike — classify under module-wide chase.
                    if (dbg) {
                        auto gres = chaseInBlockResolved(f->thunk, globalDefs);
                        if (!gres.expr) {
                            ++globalNotFound;
                        } else if (std::get_if<MkThunk>(gres.expr)) {
                            ++globalMkThunkRemote;
                            if (uses.at(gres.definer) == 1)
                                ++globalMkThunkRemoteOne;
                        } else {
                            ++globalOtherExpr;
                        }
                    }
                } else {
                    // Forced MkThunk in same block.  Cloneability gate first,
                    // then: single-use → ELIDE (inline at use, drop binding);
                    // multi-use → EAGER-KEEP (#2(B): inline the body at the
                    // binding site, keep x as the value so the other uses —
                    // incl. lazy freeVar captures — still resolve).  Multi-use
                    // is safe because this Force is unconditional in the block.
                    const MkThunk * mktDiag2 = std::get_if<MkThunk>(fres.expr);
                    bool cloneable = false;
                    if (mktDiag2->funcIdx < (FuncId)m.functions.size()) {
                        const Function & thunkFn = m.functions[mktDiag2->funcIdx];
                        if (thunkFn.entryBlock != kInvalidBlock
                            && thunkFn.entryBlock < (BlockId)m.blocks.size()) {
                            std::unordered_set<BlockId> visited;
                            cloneable = bodyIsCloneable(m, thunkFn.entryBlock, visited);
                        }
                    }
                    if (!cloneable) {
                        ++forceFailNotCloneable;
                    } else if (uses.at(fres.definer) == 1) {
                        ++passedToInlineForce;
                        mkthunksToElide.insert(fres.definer);
                    } else {
                        ++passedToInlineForce;
                        mkthunksToEager.insert(fres.definer);
                    }
                }
                continue;
            }
            const App * app = std::get_if<App>(&bd.expr);
            if (!app) continue;
            ++consideredApps;

            const Lambda * lam = resolveCalleeLambda(app->fun, m, defs);
            FuncId calleeFid = (lam && lam->funcIdx < (FuncId)m.functions.size())
                ? lam->funcIdx : (FuncId)m.functions.size();
            // #3(B) self-recursive fallback: resolveCalleeLambda misses a callee
            // whose LetRec is in an enclosing block (the recursive self-call,
            // e.g. `go (i+1)`).  Recover via the pre-resolved, pointer-stable map.
            bool viaRec = false;
            if (calleeFid >= (FuncId)m.functions.size()) {
                if (const Expr * fe = chaseInBlock(app->fun, defs))
                    if (const auto * rb = std::get_if<RecBindingSlotRef>(fe)) {
                        auto it = recCallTarget.find(rb->attrs);
                        if (it != recCallTarget.end()) {
                            auto jt = it->second.find(rb->name);
                            if (jt != it->second.end()) {
                                calleeFid = jt->second; viaRec = true;
                            }
                        }
                    }
            }
            if (calleeFid >= (FuncId)m.functions.size()) { ++failResolveLambda; continue; }
            const Function & callee = m.functions[calleeFid];
            if (callee.strictArgs.empty()) { ++failStrictArgs; continue; }
            if (viaRec && !callee.strictArgs.empty() && callee.strictArgs[0])
                ++elidedRecursive;

            // (A) Outer-thunk elision — applies to any callee
            // whose strictArgs[0] is true (the paramVar position).
            // For single-arg lambdas this is the only arg; for
            // formals-style this is the entire attrset.  Stage 4 v3's
            // "any-formal-strict → paramVar-strict" heuristic ensures
            // formals-style lambdas with strict formals trip this
            // branch too.
            if (callee.strictArgs[0]) {
                ++passedToInline;
                // Inline funnel diagnostic: replicate isInlinableMkThunk
                // logic to see which of (not-MkThunk / multi-use /
                // not-cloneable) blocks each candidate.
                auto res = chaseInBlockResolved(app->arg, defs);
                if (!res.expr || !std::get_if<MkThunk>(res.expr)) {
                    ++failNotMkThunk;
                } else {
                    const MkThunk * mktDiag = std::get_if<MkThunk>(res.expr);
                    if (uses.at(res.definer) != 1) {
                        ++failMultiUse;
                    } else if (mktDiag->funcIdx >= (FuncId)m.functions.size()) {
                        ++failNotCloneable;
                    } else {
                        const Function & thunkFn = m.functions[mktDiag->funcIdx];
                        if (thunkFn.entryBlock == kInvalidBlock
                            || thunkFn.entryBlock >= (BlockId)m.blocks.size()) {
                            ++failNotCloneable;
                        } else {
                            std::unordered_set<BlockId> visited;
                            if (!bodyIsCloneable(m, thunkFn.entryBlock, visited))
                                ++failNotCloneable;
                        }
                    }
                }
                VarId defVar = kInvalid;
                const MkThunk * mkt = nullptr;
                const Block * body = nullptr;
                if (isInlinableMkThunk(app->arg, m, defs, uses,
                                       defVar, mkt, body)) {
                    mkthunksToElide.insert(defVar);
                }
            }

            // (B) Inner attrset-entry elision — applies to formals-
            // style callees where individual formals are strict.
            // Requires App's arg to be a DIRECT AttrSet binding in
            // the same block.  Hit on iteration 2 after outer elision
            // has inlined any wrapping MkThunk in pass 1.
            if (callee.hasFormals) {
                const bool hasParam = (callee.paramVar != kInvalid);
                const size_t formalsStart = hasParam ? 1 : 0;
                std::unordered_map<SymbolId, bool> strictByName;
                for (size_t i = 0; i < callee.formals.size(); ++i) {
                    const size_t slot = formalsStart + i;
                    if (slot >= callee.strictArgs.size()) break;
                    if (callee.strictArgs[slot])
                        strictByName.emplace(callee.formals[i].name, true);
                }
                if (strictByName.empty()) continue;

                // Resolve App's arg to an AttrSet binding in same block.
                auto argRes = chaseInBlockResolved(app->arg, defs);
                if (!argRes.expr) continue;
                const AttrSet * as = std::get_if<AttrSet>(argRes.expr);
                if (!as) continue;

                // For each strict-formal name with a matching entry,
                // check if the entry's value VarId resolves to a
                // single-use MkThunk with a simple body.
                for (const auto & ent : as->entries) {
                    if (!strictByName.count(ent.name)) continue;
                    VarId defVar = kInvalid;
                    const MkThunk * mkt = nullptr;
                    const Block * body = nullptr;
                    if (!isInlinableMkThunk(ent.value, m, defs, uses,
                                            defVar, mkt, body)) continue;
                    mkthunksToElide.insert(defVar);
                }
            }
        }

        // ----- MAIN PASS -----
        // Walk bindings; inline elide-marked MkThunks (replacing the
        // MkThunk binding with its cloned body) and rewrite any
        // downstream AttrSet / App that references the elided VarId.
        std::unordered_map<VarId, VarId> elidedToInline;
        std::vector<Binding> out;
        out.reserve(blk.bindings.size() * 2);

        for (const auto & bd : blk.bindings) {
            // #2(B) eager-keep: inline a forced multi-use MkThunk's body at
            // its binding site and KEEP x bound to the eager value, so its
            // other (possibly lazy-captured) uses still resolve.  No
            // elidedToInline entry — uses of x are left intact and read the
            // now-eager x.  The Force(x) that triggered this becomes a Force
            // on a WHNF value (a no-op) and is harmless.
            if (mkthunksToEager.count(bd.var)) {
                const auto * mkt = std::get_if<MkThunk>(&bd.expr);
                if (mkt && mkt->funcIdx < (FuncId)m.functions.size()) {
                    VarId tail = inlineThunkBody(
                        m, m.functions[mkt->funcIdx].entryBlock, out);
                    if (tail != kInvalid) {
                        out.push_back({bd.var, VarRef{tail}});
                        ++elidedForceMkt;
                        continue;
                    }
                }
                out.push_back(bd);  // defensive: keep original on failure
                continue;
            }
            // Elide MkThunk binding by inlining its body.
            if (mkthunksToElide.count(bd.var)) {
                const auto * mkt = std::get_if<MkThunk>(&bd.expr);
                // We pre-verified this in the analysis pass.
                if (!mkt) { out.push_back(bd); continue; }
                const Function & thunkFn = m.functions[mkt->funcIdx];
                BlockId thunkBodyBid = thunkFn.entryBlock;
                // v4.2: cloneBlockBindings handles sub-blocks via
                // fresh-block allocation.  Returns kInvalid on
                // failure (in which case we keep the original
                // MkThunk binding — bodyIsCloneable should have
                // caught this in pre-analysis, but defensive).
                VarId tail = inlineThunkBody(m, thunkBodyBid, out);
                if (tail == kInvalid) {
                    out.push_back(bd);
                    continue;
                }
                elidedToInline[bd.var] = tail;
                // SKIP emitting the MkThunk binding itself.
                continue;
            }

            // Rewrite downstream references via elidedToInline.
            // The bindings we need to rewrite are:
            //   - App.arg (single-arg case)
            //   - AttrSet.entries[i].value (formals-style case)
            // Other Expr kinds may reference an elided var indirectly
            // through VarRef chains, but those should also resolve
            // correctly without explicit rewriting since we left the
            // VarRef bindings intact.  (A VarRef pointing at an
            // elided MkThunk var becomes a dangling reference; we
            // need to remap those too.)
            if (auto * app = std::get_if<App>(&bd.expr)) {
                App copy = *app;
                auto it = elidedToInline.find(copy.arg);
                if (it != elidedToInline.end()) {
                    copy.arg = it->second;
                    ++elidedSingleArg;
                }
                out.push_back({bd.var, std::move(copy)});
                continue;
            }
            if (auto * as = std::get_if<AttrSet>(&bd.expr)) {
                AttrSet copy = *as;
                bool anyChange = false;
                for (auto & ent : copy.entries) {
                    auto it = elidedToInline.find(ent.value);
                    if (it != elidedToInline.end()) {
                        ent.value = it->second;
                        anyChange = true;
                        ++elidedFormals;
                    }
                }
                (void)anyChange;
                out.push_back({bd.var, std::move(copy)});
                continue;
            }
            if (auto * vr = std::get_if<VarRef>(&bd.expr)) {
                VarRef copy = *vr;
                auto it = elidedToInline.find(copy.var);
                if (it != elidedToInline.end()) copy.var = it->second;
                out.push_back({bd.var, std::move(copy)});
                continue;
            }
            // #775 Case (C) rewrite: Force(b) where b was an elided
            // MkThunk → VarRef(body_tail).  The body is already
            // inlined; the forced value IS the body's TermReturn.
            if (auto * f = std::get_if<Force>(&bd.expr)) {
                auto it = elidedToInline.find(f->thunk);
                if (it != elidedToInline.end()) {
                    out.push_back({bd.var, VarRef{it->second}});
                    ++elidedForceMkt;
                    continue;
                }
                out.push_back(bd);
                continue;
            }
            // Default: emit as-is.
            out.push_back(bd);
        }

        blk.bindings = std::move(out);
    }

    const size_t elided = elidedSingleArg + elidedFormals + elidedForceMkt;
    if (dbg) {
        std::fprintf(stderr,
            "v3 stage4 v4.1 strict-call-unthunk: elided=%zu "
            "(single-arg=%zu formals=%zu forceMkt=%zu) of %zu Apps considered "
            "[funnel: failResolve=%zu failStrictArgs=%zu "
            "passedToInline=%zu failNotMkThunk=%zu failMultiUse=%zu "
            "failNotCloneable=%zu | force-of-mkt: considered=%zu passed=%zu "
            "fail{notMkt=%zu multiUse=%zu notClone=%zu} | "
            "#776 spike (notMkt classified globally): "
            "notFound=%zu mktRemote=%zu mktRemoteOne=%zu other=%zu | "
            "#3(B) recursive-callee-resolved=%zu]\n",
            elided, elidedSingleArg, elidedFormals, elidedForceMkt,
            consideredApps,
            failResolveLambda, failStrictArgs,
            passedToInline, failNotMkThunk, failMultiUse,
            failNotCloneable,
            consideredForces, passedToInlineForce,
            forceFailNotMkThunk, forceFailMultiUse, forceFailNotCloneable,
            globalNotFound, globalMkThunkRemote,
            globalMkThunkRemoteOne, globalOtherExpr,
            elidedRecursive);
    }
    (void)elidedRecursive;

    return elided;
}

// Shared strictness sequence — see ir.hh.  The production eval path (run.cc)
// and the `--emit-bytecode` dump BOTH call this so the disassembly reflects
// the form that actually runs (a divergence here caused a bytecode review to
// miss that recursive-call args are de-thunked at eval time).  The 8-round
// cap matches run.cc: compound shapes (an outer MkThunk over an AttrSet whose
// entries are themselves thunked) need a pass per nesting level.
void applyStrictnessPasses(Module & m)
{
    computeFunctionStrictness(m);
    for (int it = 0; it < 8; ++it)
        if (applyStrictnessAtCallSites(m) == 0) break;

    // Sweep strictness's OWN residue.  De-thunking a strict call arg inlines
    // the thunk body at the call site and drops the MkThunk binding, but the
    // thunk *function* it referenced is now unreachable dead code (e.g. fib's
    // (n-1)/(n-2) `__sub` thunks).  `deadFunctionElim` inside `optimise()`
    // runs BEFORE this, so it cannot see the orphans — clear them here.  This
    // is the only place that observes the post-de-thunk shape, and because
    // every production caller (run.cc, --emit-bytecode, --optimize) routes
    // through this function, the sweep can't drift from the eval path.
    deadFunctionElim(m);
}

} // namespace nix::v3::ir
