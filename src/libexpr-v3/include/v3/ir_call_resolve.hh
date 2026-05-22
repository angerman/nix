#pragma once
/// @file
/// #745 Stage 4 v4.3 (2026-05-22) — shared call-site resolution
/// helpers used by both `computeFunctionStrictness` (strictness
/// analysis) and `applyStrictnessAtCallSites` (elision pass).
///
/// Pre-#745 these helpers were file-local to opt_strict_call_unthunk.cc.
/// v4.3's cross-function strictness propagation needs them in the
/// analysis pass too — chasing `App.fun` back to the callee Lambda
/// so we can read its `strictArgs` and propagate strictness to the
/// caller's formals.
///
/// Inline functions so the dispatch stays at the call site (no
/// indirection); the header is included only by the two optimizer
/// passes that need it, so the inline cost is bounded.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <unordered_map>

namespace nix::v3::ir {

/// Result of `chaseInBlockResolved` — both the VarId where the
/// resolution stopped (the actual definer of the underlying
/// expression) AND the Expr*.  Callers that need the "definer"
/// for use-count lookup use this variant; callers that only need
/// the underlying expression use `chaseInBlock`.
struct ResolveResult {
    VarId        definer;
    const Expr * expr;
};

/// Chase a VarId through same-block VarRef aliases to the underlying
/// Expr.  Returns nullptr if the VarId isn't defined in this block
/// (e.g., free variable from an outer scope) or if the chase hits a
/// cycle longer than `defs.size() + 1` hops.  defs is `(VarId →
/// Expr*)` for the current block's bindings.
inline const Expr * chaseInBlock(
    VarId v,
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

inline ResolveResult chaseInBlockResolved(
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

/// Resolve a call site's `fun` VarId to a Lambda IR node, following
/// VarRef alias chains AND single-step RecBindingSlotRef → LetRec
/// entry → entry-thunk-body → returned-Lambda chains.
///
/// Three resolution strategies tried in order:
///   1. rb->attrs is the LetRec binding's defining VarId directly.
///   2. rb->attrs is a recSlotVar, mapped via Module::recVarToSlotVar
///      (reverse lookup).
///   3. Structural match (#743 v4.1 fallback): walk all LetRec
///      bindings in same block; pick one whose entries contains a
///      name matching rb->name.  Robust to post-optimise VarId
///      renumbering that breaks the strict (recVar, slotVar)
///      bookkeeping path.
///
/// Returns nullptr if the callee can't be statically resolved
/// (e.g., higher-order use, `with` lookup, etc.).
inline const Lambda * resolveCalleeLambda(
    VarId funVar,
    const Module & m,
    const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(funVar, defs);
    if (!e) return nullptr;

    // Direct Lambda binding (the inline `((x: body) arg)` case).
    if (const auto * lam = std::get_if<Lambda>(e))
        return lam;

    // RecBindingSlotRef: trace through the LetRec.
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
        for (const auto & [varId, exprPtr] : defs) {
            const auto * candidate = std::get_if<LetRec>(exprPtr);
            if (!candidate) continue;
            for (const auto & ent : candidate->entries) {
                if (ent.name == rb->name) { lr = candidate; break; }
            }
            if (lr) break;
        }
    }
    if (!lr) return nullptr;
    // Find entry whose name matches.
    for (const auto & ent : lr->entries) {
        if (ent.name != rb->name) continue;
        if (ent.thunkBody >= (FuncId)m.functions.size()) return nullptr;
        const Function & entryFn = m.functions[ent.thunkBody];
        if (entryFn.entryBlock == kInvalidBlock
            || entryFn.entryBlock >= (BlockId)m.blocks.size())
            return nullptr;
        const Block & entryBlk = m.blocks[entryFn.entryBlock];
        const auto * tr = std::get_if<TermReturn>(&entryBlk.terminal);
        if (!tr || tr->value == kInvalid) return nullptr;
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
                return std::get_if<Lambda>(&bd.expr);
            }
            if (!advanced) break;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace nix::v3::ir
