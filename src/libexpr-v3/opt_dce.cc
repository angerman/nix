/// @file
/// IR optimisation pass: dead-binding elimination.
///
/// A binding `Binding{var, expr}` is "dead" when `var` is referenced
/// nowhere else in the Module (no other Expr operand, no Terminal,
/// no nested-function freeVar capture, no LetRec entry capture).
///
/// We only erase dead bindings whose `expr` is *obviously pure* —
/// allocates a value, but never forces a thunk, never runs user code,
/// never throws.  Anything that might throw at evaluation time stays:
/// the program may rely on that throw to abort with a specific error,
/// or to signal a deliberate `assert`/`abort`.
///
/// Pure (safe to remove if unused):
///   - LitInt, LitFloat, LitBool, LitNull, LitString, LitPath
///   - VarRef                  (just an alias for an existing VarId)
///   - LitPrimOp, LitBuiltins  (push a constant onto the stack)
///   - Lambda, MkThunk         (allocates closure / thunk; body unrun)
///   - AttrSet                 (allocates Bindings; no force)
///   - ListExpr                (allocates list; no force)
///
/// Impure (always keep — may force operands or invoke primops):
///   - Force, App, PrimOpCall
///   - AttrSelect, AttrSelectDyn, HasAttr, HasAttrDyn
///   - RecBindingSlotRef       (forces the rec attrset)
///   - Add, Sub, Mul, Div      (force operands; may throw)
///   - Eq, NEq, Less           (force operands; may throw)
///   - Not                     (forces operand)
///   - And, Or, Impl           (forces lhs; runs rhsBlock)
///   - ConcatLists, ConcatStrings, Update
///   - If, With, Assert        (control flow + sub-blocks)
///   - LetRec                  (allocates Bindings + thunks; not removable
///                              because emit threads recVar through state)
///   - AttrSetDyn              (dynamic name evaluation may throw)
///
/// We compute the "is referenced" set *globally* — VarIds are unique
/// across the whole Module, so a single pass suffices to know whether
/// any other point in the program reads the var.
///
/// IMPORTANT: we never erase a binding whose `var` equals
/// `Function::paramVar` for any Function in the module.  paramVars
/// are bound at function-entry time and may be referenced by the
/// emitter even when the body's block does not visibly use them
/// (defensive — the cost of preserving them is negligible since
/// only one VarId per Function is involved).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/primop.hh"

#include <unordered_map>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Purity classification
// ---------------------------------------------------------------------------

bool exprIsPure(const Expr & e)
{
    return std::visit([&](const auto & x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>    ||
                      std::is_same_v<T, LitFloat>  ||
                      std::is_same_v<T, LitBool>   ||
                      std::is_same_v<T, LitNull>   ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath>   ||
                      std::is_same_v<T, VarRef>    ||
                      std::is_same_v<T, LitPrimOp> ||
                      std::is_same_v<T, LitBuiltins> ||
                      std::is_same_v<T, Lambda>    ||
                      std::is_same_v<T, MkThunk>   ||
                      std::is_same_v<T, AttrSet>   ||
                      std::is_same_v<T, ListExpr>) {
            return true;
        } else {
            (void)x;
            return false;
        }
    }, e);
}

// ---------------------------------------------------------------------------
// Reference set: every VarId that appears as an operand somewhere.
// ---------------------------------------------------------------------------

void collectModuleRefs(const Module & m, std::unordered_set<VarId> & refs)
{
    // Every binding's RHS is potentially referenced by other bindings,
    // terminals, sub-block bindings/terminals, or by nested-function
    // captures (Lambda::freeVars, MkThunk::freeVars, LetRec entry
    // outerUpvalues, hidden-thunk outerUpvalues).  collectExprRefs
    // already handles all of those at the per-Expr level.
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & b = m.blocks[bid];
        for (const auto & bind : b.bindings)
            collectExprRefs(bind.expr, refs);
        if (auto * ret = std::get_if<TermReturn>(&b.terminal))
            if (ret->value != kInvalid)
                refs.insert(ret->value);
    }

    // Function paramVars: defensive -- the emitter binds the lambda
    // argument under this VarId at call time, even when the body's
    // bindings don't reference it directly (`x: 42` style ignored
    // arguments).  Treat as "referenced" so DCE never erases the
    // companion AttrSet/Lambda binding for an unused-but-still-bound
    // formal slot.
    for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
        const Function & f = m.functions[fid];
        if (f.paramVar != kInvalid) refs.insert(f.paramVar);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: deadBindingElim
// ---------------------------------------------------------------------------

// Pre-identify UNDER-APPLIED primop applications: a 1-arg `App` whose
// callee resolves to a `LitPrimOp` of arity > 1 is a partial application — a
// pure PrimOpApp VALUE with no force/effect — so it is safe to DCE when dead.
// This sweeps the partial-App residue fusePrimOpApps leaves behind on every
// fused primop call (e.g. `(x: x*2) 21` folds to LitInt 42 but the inlined
// `App(__mul, 21)` lingers).  We compute the set ONCE (structural property,
// stable across DCE rounds) so the move-compaction loop below can consult it
// without re-resolving defs through moved-from slots.
static void collectUnderAppliedPrimOpApps(const Module & m,
                                          std::unordered_set<VarId> & out)
{
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & b = m.blocks[bid];
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(b.bindings.size());
        for (const auto & bd : b.bindings) defs.emplace(bd.var, &bd.expr);
        for (const auto & bd : b.bindings) {
            const App * a = std::get_if<App>(&bd.expr);
            if (!a) continue;
            // Resolve the callee through VarRef aliases.
            const Expr * fe = nullptr;
            if (auto it = defs.find(a->fun); it != defs.end()) fe = it->second;
            while (fe) {
                const VarRef * vr = std::get_if<VarRef>(fe);
                if (!vr) break;
                auto it = defs.find(vr->var);
                fe = it != defs.end() ? it->second : nullptr;
            }
            if (!fe) continue;
            // A 1-arg App of a multi-arg primop is under-applied → pure value.
            if (const LitPrimOp * lp = std::get_if<LitPrimOp>(fe))
                if (lp->primop && lp->primop->arity > 1)
                    out.insert(bd.var);
        }
    }
}

size_t deadBindingElim(Module & m)
{
    std::unordered_set<VarId> refs;
    collectModuleRefs(m, refs);

    std::unordered_set<VarId> underAppliedApps;
    collectUnderAppliedPrimOpApps(m, underAppliedApps);

    // Iterate to a fixed point: removing one binding can make its
    // RHS-referenced VarIds no longer used elsewhere.  In practice
    // 1-2 rounds suffice; cap at a few to bound worst-case runtime.
    constexpr int kMaxRounds = 8;
    size_t total = 0;
    for (int round = 0; round < kMaxRounds; ++round) {
        size_t removed = 0;
        for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
            Block & b = m.blocks[bid];
            if (b.bindings.empty()) continue;

            auto src = b.bindings.begin();
            auto dst = b.bindings.begin();
            for (; src != b.bindings.end(); ++src) {
                if (refs.find(src->var) == refs.end()
                    && (exprIsPure(src->expr)
                        || underAppliedApps.count(src->var))) {
                    ++removed;
                    continue; // skip -- erase by not copying
                }
                if (dst != src) *dst = std::move(*src);
                ++dst;
            }
            b.bindings.erase(dst, b.bindings.end());
        }
        total += removed;
        if (removed == 0) break;
        // Re-collect refs from the smaller module before the next
        // iteration: a binding we removed may have been the only
        // consumer of an upstream VarId.
        refs.clear();
        collectModuleRefs(m, refs);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Public entry: deadBindingElimViaOccur  (OPT_OCCUR Phase B)
// ---------------------------------------------------------------------------
//
// Uses analyseOccurrence's OccMap instead of re-walking the global
// reference set each iteration.  Same purity rules as deadBindingElim
// (exprIsPure on the RHS).  Bounded at exactly two analyseOccurrence
// invocations: round 1 catches the obvious Dead bindings; round 2
// catches anything that became Dead because its sole consumer was a
// round-1 victim.  In practice both rounds together match the 8-round
// fixed point of deadBindingElim.
//
// One subtle correctness point: paramVars are classified as `Param`
// in OccMap, NOT Dead, regardless of count.  So a Function::paramVar
// is never erased.  This matches deadBindingElim's defensive `refs`
// pre-population.

size_t deadBindingElimViaOccur(Module & m)
{
    size_t total = 0;
    for (int round = 0; round < 2; ++round) {
        OccMap occ = analyseOccurrence(m);
        size_t removed = 0;
        for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
            Block & b = m.blocks[bid];
            if (b.bindings.empty()) continue;

            auto src = b.bindings.begin();
            auto dst = b.bindings.begin();
            for (; src != b.bindings.end(); ++src) {
                OccInfo info = occ.lookup(src->var);
                if (info.kind == OccKind::Dead && exprIsPure(src->expr)) {
                    ++removed;
                    continue;
                }
                if (dst != src) *dst = std::move(*src);
                ++dst;
            }
            b.bindings.erase(dst, b.bindings.end());
        }
        total += removed;
        if (removed == 0) break;
    }
    return total;
}

} // namespace nix::v3::ir
