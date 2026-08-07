/// @file
/// IR optimisation pass: 1-shot beta reduction.
///
/// Inlines `App(VarRef→Lambda, arg)` patterns when SAFE to do so —
/// i.e. when the Lambda is a same-block, OnceLinear, single-arg, no-
/// formals, no-intrinsic, body-has-no-nested-Function-creators,
/// body-has-no-sub-blocks lambda.
///
/// Motivation: per the IR optimization plan (lode/IR_OPTIMIZATION_PLAN_
/// 2026-05-18.md Phase A), every immediate-call lambda otherwise
/// allocates a Closure at runtime (OP_MAKE_CLOSURE) AND pays for an
/// OP_CALL frame push + OP_RETURN teardown — for trivial bodies like
/// `(x: x + 1) 5`, that's ~30 dispatched opcodes for a 2-op
/// computation.  After beta reduction the bytecode emits literally
/// the body's operations inline at the call site.
///
/// Algorithm (single forward walk per block):
///
///   For each block B in the Module:
///     For each binding (v, expr) in B:
///       If expr is `App{fun, arg}` AND we can prove inlining is safe:
///         Find the Lambda IR node at `fun`'s definition site.
///         Find its target Function F.
///         Clone F's entryBlock body, substituting:
///           paramVar              → arg
///           original body VarIds  → freshly-allocated VarIds
///         Append cloned bindings to the output.
///         Emit `v = VarRef{clonedTailVar}` to replace the App.
///       Else:
///         Emit the original binding unchanged.
///     Replace B's bindings with the output.
///
/// "Safe to inline" requires ALL of:
///   1. App's `fun` operand resolves (within the same block, via
///      VarRef alias chains) to a Lambda IR node.
///   2. The Lambda's funcIdx points at a Function F where:
///        - F.argName != kInvalidSymbol (has a single named arg)
///        - !F.hasFormals (not formals-style)
///        - F.entryBlock != kInvalidBlock
///   3. F.entryBlock's bindings contain NO:
///        - Lambda    (nested closure — VarId scoping gets complex)
///        - MkThunk   (nested thunk — same)
///        - LetRec    (introduces inner rec scope)
///        - If        (owns a sub-block; cross-block clone needed)
///        - With      (owns a sub-block)
///        - Assert    (owns a sub-block)
///        - And/Or/Impl (own a sub-block via rhsBlock)
///      (These are the IR nodes that carry BlockId / FuncId
///      references which complicate cloning.  All other Expr kinds
///      have only VarId / SymbolId / literal operands.)
///   4. The Lambda VarId has ONE use (the App itself).  This
///      conservatively prevents work-duplication if the Lambda
///      appears as an upvalue in another closure or in lexicalWiths.
///
/// Unconditional (the NIX_V3_NO_BETA_REDUCE A/B opt-out was retired once
/// it shipped byte-identical across the property + lang test corpus).
///
/// Runs BEFORE inlineTrivialBindings (so the cloned bindings' VarRefs
/// get path-compressed) and BEFORE constantFold (so cloned literal
/// arithmetic folds).  See opt_const_fold.cc::optimise pipeline.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_util.hh"

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// VarId remapping — apply a substitution map to every VarId operand
// inside an Expr.  Returns true if the Expr's structure is "simple"
// in the sense required for cloning (no nested Function/Block refs).
// ---------------------------------------------------------------------------

// Helper: apply substitution to a single VarId in place.
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

// Apply substitution to the operand VarIds of an Expr.  Returns false
// for Expr kinds that this pass refuses to clone (carries sub-Block
// or FuncId refs — see "simple" list in the file docstring).
bool remapExprVars(Expr & e, const std::unordered_map<VarId, VarId> & sub)
{
    return std::visit([&](auto & v) -> bool {
        using T = std::decay_t<decltype(v)>;

        // Literals — no operands.
        if constexpr (std::is_same_v<T, LitInt>    || std::is_same_v<T, LitFloat>
                   || std::is_same_v<T, LitBool>   || std::is_same_v<T, LitNull>
                   || std::is_same_v<T, LitString> || std::is_same_v<T, LitPath>
                   || std::is_same_v<T, LitPrimOp> || std::is_same_v<T, LitBuiltins>
                   || std::is_same_v<T, WithLookup>)
        {
            return true;
        }

        // Single-VarId operands.
        else if constexpr (std::is_same_v<T, VarRef>)            { remapVar(v.var, sub);     return true; }
        else if constexpr (std::is_same_v<T, Force>)             { remapVar(v.thunk, sub);   return true; }
        else if constexpr (std::is_same_v<T, AttrSelect>)        { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, HasAttr>)           { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, RecBindingSlotRef>) { remapVar(v.attrs, sub);   return true; }
        else if constexpr (std::is_same_v<T, Not>)               { remapVar(v.operand, sub); return true; }

        // Two-VarId operands (binary ops).
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

        // List of VarIds.
        else if constexpr (std::is_same_v<T, ListExpr>)
            { remapVarVec(v.elems, sub); return true; }
        else if constexpr (std::is_same_v<T, ConcatStrings>)
            { remapVarVec(v.parts, sub); return true; }
        else if constexpr (std::is_same_v<T, PrimOpCall>)
            { remapVarVec(v.args, sub); return true; }

        // AttrSet: each entry has a VarId value.  But AttrSet also
        // carries SymbolId names which are stable.
        else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & ent : v.entries)
                remapVar(ent.value, sub);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) {
            remapVar(v.attrSetVar, sub);
            for (auto & ent : v.entries) remapVar(ent.valueVar, sub);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (auto & s : v.statics)  remapVar(s.value, sub);
            for (auto & d : v.dynamics) {
                remapVar(d.nameVar, sub);
                remapVar(d.value, sub);
            }
            return true;
        }

        // BlockId / FuncId carriers — refuse to clone.  These need
        // recursive cloning of the sub-block / function, which is
        // Phase A out-of-scope.
        else if constexpr (std::is_same_v<T, Lambda>)   return false;
        else if constexpr (std::is_same_v<T, MkThunk>)  return false;
        else if constexpr (std::is_same_v<T, If>)       return false;
        else if constexpr (std::is_same_v<T, With>)     return false;
        else if constexpr (std::is_same_v<T, Assert>)   return false;
        else if constexpr (std::is_same_v<T, And>)      return false;
        else if constexpr (std::is_same_v<T, Or>)       return false;
        else if constexpr (std::is_same_v<T, Impl>)     return false;
        else if constexpr (std::is_same_v<T, LetRec>)   return false;

        else {
            (void)v;
            return false;  // unknown / future variant — be conservative
        }
    }, e);
}

// ---------------------------------------------------------------------------
// P-11 (CODEBASE_REVIEW_2026-06-11): recursive sub-block cloning, so
// beta-reduce can inline lambda bodies whose entry block ends in / contains
// an If / With / Assert / And / Or / Impl (each owns a sub-Block).  Before
// this, `bodyIsSimple` rejected any body touching those — the pass fired only
// on toy shapes.  Algorithm ported from opt_strict_call_unthunk.cc's #744 v4.2
// machinery, but deliberately bound to *this* file's `remapExprVars` (which
// still REFUSES Lambda / MkThunk / LetRec — see the carrier list above), so
// the extension is exactly { If, With, Assert, And, Or, Impl } and nothing
// more.  (The two passes' remapExprVars have diverged — opt_strict clones
// Lambda/MkThunk via freeVars remap (#743 v4.1); beta-reduce must not — so the
// trio is duplicated rather than shared.  A future ir_clone.{hh,cc} extraction
// could unify them once that divergence is reconciled.)
// ---------------------------------------------------------------------------

bool bodyIsCloneable(const Module & m, BlockId srcBid,
                     std::unordered_set<BlockId> & visited);
VarId cloneBlockBindings(Module & m, BlockId srcBid,
                         std::unordered_map<VarId, VarId> & sub,
                         std::vector<Binding> & out);
BlockId cloneSubBlock(Module & m, BlockId srcBid,
                      const std::unordered_map<VarId, VarId> & parentSub);

/// Can the block at `srcBid` (and any sub-blocks reachable through
/// If/With/Assert/And/Or/Impl) be cloned by `cloneBlockBindings`?
/// Recursive; tracks visited blocks to avoid cycles.  Returns false on an
/// invalid BlockId, a LetRec binding (out of scope — would need to clone the
/// per-entry Function bodies with recVar substitution), or any nested
/// sub-block that itself fails the check.  Non-sub-block exprs are probed
/// through `remapExprVars` with an empty substitution.
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
                return false;  // out of scope (see header note)
            } else {
                // Non-sub-block expr — probe via remapExprVars.  This is the
                // SAME predicate bodyIsSimple used, so Lambda/MkThunk are
                // still refused here (remapExprVars returns false for them).
                Expr probe = bd.expr;
                static const std::unordered_map<VarId, VarId> empty;
                return remapExprVars(probe, empty);
            }
        }, bd.expr);
        if (!ok) return false;
    }
    return true;
}

/// Clone the bindings of `srcBid` into `out` with fresh local VarIds.
/// Sub-blocks (If/With/Assert/And/Or/Impl) get fresh BlockIds via
/// `cloneSubBlock`.  Returns the cloned tail VarId, or `kInvalid` if any
/// binding refuses to clone (callers must NOT splice `out` on failure).
/// `sub` is mutable: each binding adds (oldVar → newVar); outer/free VarIds
/// are absent from `sub`, so remapVar leaves them unchanged.
VarId cloneBlockBindings(Module & m, BlockId srcBid,
                         std::unordered_map<VarId, VarId> & sub,
                         std::vector<Binding> & out)
{
    if (srcBid == kInvalidBlock || srcBid >= (BlockId)m.blocks.size())
        return kInvalid;
    // Snapshot by value — `m.blocks` may reallocate during nested
    // `cloneSubBlock` → `freshBlock()` (which resizes the vector).  Holding
    // references into m.blocks[srcBid] across that point would dangle.
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

/// Clone a sub-block referenced from If/With/etc.  Allocates a fresh
/// BlockId, clones bindings into it, sets the terminal.  Returns the new
/// BlockId or kInvalidBlock on failure.  `parentSub` is COPIED so the
/// sub-block's local bindings don't leak back to the parent's map.
BlockId cloneSubBlock(Module & m, BlockId srcBid,
                      const std::unordered_map<VarId, VarId> & parentSub)
{
    if (srcBid == kInvalidBlock || srcBid >= (BlockId)m.blocks.size())
        return kInvalidBlock;
    BlockId newBid = m.freshBlock();  // may grow m.blocks — hold no refs
    std::unordered_map<VarId, VarId> sub = parentSub;
    std::vector<Binding> newBindings;
    VarId tail = cloneBlockBindings(m, srcBid, sub, newBindings);
    if (tail == kInvalid) return kInvalidBlock;
    m.blocks[newBid].bindings = std::move(newBindings);
    m.blocks[newBid].terminal = TermReturn{tail};
    return newBid;
}

// chaseInBlock() is shared across opt_*.cc passes — see v3/ir_util.hh.

// (P-11: mapBlockDefs removed — betaReduce now builds its defs map from a
// by-value snapshot of the block's bindings, since cloneSubBlock can realloc
// m.blocks and dangle Expr* pointers into the live block.)

// ---------------------------------------------------------------------------
// Count references to each VarId across the entire Module.  Returns a
// map var → use-count.  Used to enforce the "Lambda has exactly one
// use" safety condition.  We count references in:
//   - every Binding's RHS (operand VarIds)
//   - every Block's terminal (TermReturn{value})
//   - every Function's freeVars / paramVar
//
// Walking VarId-only references is enough because we only care about
// counting USES, not knowing what they are.  Cross-Function counts
// (a Lambda captured as freeVar in another Function) WILL be reflected
// because computeFreeVars has populated freeVars by this point — but
// computeFreeVars runs AFTER optimise(), so freeVars are EMPTY here.
// Two consequences:
//   1. We can't see Lambdas captured by inner Functions via freeVars.
//   2. The "OnceLinear inside this block" check is the actual safety
//      check; cross-block / cross-function uses are visible only via
//      VarRef bindings in those other contexts.
//
// For Phase A's conservatism, we tighten further to "Lambda and its
// App in the same block, with no other references to the Lambda's
// VarId in any other Module location."  That's stricter than needed
// in theory but bullet-proof in practice.
// ---------------------------------------------------------------------------

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
    // Function paramVar isn't a use (it's a def); freeVars are populated
    // post-optimise so they're empty here.
    return uc;
}

// ---------------------------------------------------------------------------
// Check whether a Function's body block (rooted at `bid`) is cloneable.
// P-11 (CODEBASE_REVIEW_2026-06-11): delegates to the recursive
// `bodyIsCloneable`, which now accepts bodies whose bindings contain
// If/With/Assert/And/Or/Impl (the sub-blocks are cloned with fresh BlockIds);
// it still refuses Lambda/MkThunk/LetRec (via this file's remapExprVars).
// Before P-11 this iterated only the entry block's own bindings and rejected
// any sub-block carrier, so the pass fired on toy shapes only.
// ---------------------------------------------------------------------------

bool bodyIsSimple(const Module & m, BlockId bid)
{
    std::unordered_set<BlockId> visited;
    return bodyIsCloneable(m, bid, visited);
}

// ---------------------------------------------------------------------------
// Perform the inline: clone `body` into `out` with the substitution
// paramVar → arg, allocating fresh VarIds for body bindings.  Returns
// the cloned tail VarId (i.e. what the App's VarId should VarRef to).
// ---------------------------------------------------------------------------

// P-11: clone the body block `bodyBid` (and any sub-blocks it owns) into a
// TEMP vector with paramVar→arg seeded, then splice into `out` only on
// success.  Returns the cloned tail VarId, or kInvalid if the clone failed
// (bodyIsSimple/bodyIsCloneable should have guaranteed success, but a sub-
// block clone can still bail defensively — in which case `out` is untouched
// and the caller keeps the original App binding).  Cloning into a temp first
// (rather than directly into `out`) keeps `out` intact on a mid-clone bail.
VarId inlineBody(Module & m,
                 BlockId bodyBid,
                 VarId paramVar,
                 VarId arg,
                 std::vector<Binding> & out)
{
    std::unordered_map<VarId, VarId> sub;
    sub[paramVar] = arg;
    std::vector<Binding> tmp;
    VarId tail = cloneBlockBindings(m, bodyBid, sub, tmp);
    if (tail == kInvalid) return kInvalid;
    for (auto & b : tmp) out.push_back(std::move(b));
    return tail;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: betaReduce.
// ---------------------------------------------------------------------------

size_t betaReduce(Module & m)
{
    // Pass 1: count uses across the entire module.
    UseCounter uses = countModuleUses(m);

    size_t inlined = 0;

    // Pass 2: walk each block; rewrite App-of-Lambda patterns where safe.
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        // P-11 (CODEBASE_REVIEW_2026-06-11): snapshot this block's bindings
        // by VALUE before rewriting.  inlineBody now clones sub-blocks via
        // cloneSubBlock → freshBlock, which can REALLOCATE m.blocks and dangle
        // any reference/iterator into m.blocks[bid] — the `defs` Expr* map, the
        // range-for iterator, and any held `Block&`.  Iterating a stable local
        // copy + writing the result back by index (not via a cached reference)
        // removes that hazard by construction.  Behaviour is identical to
        // iterating the live bindings: `out` is built separately and assigned
        // only at the end, so nothing observes mid-loop mutation either way.
        std::vector<Binding> srcBindings = m.blocks[bid].bindings;

        // Build the same-block defs map from the SNAPSHOT (so chase lookups
        // stay valid across freshBlock reallocs).
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(srcBindings.size());
        for (const auto & bd : srcBindings) defs.emplace(bd.var, &bd.expr);

        // Single output vector; emit-in-order.  Reserve generously since
        // beta-reduced blocks grow (one App becomes N body bindings + a VarRef).
        std::vector<Binding> out;
        out.reserve(srcBindings.size() * 2);
        size_t inlinedHere = 0;

        for (const auto & bd : srcBindings) {
            // Only App bindings are candidates.
            const App * app = std::get_if<App>(&bd.expr);
            if (!app) {
                out.push_back(bd);
                continue;
            }

            // Resolve app->fun to a Lambda (within this block).
            const Expr * funDef = chaseInBlock(app->fun, defs);
            if (!funDef) { out.push_back(bd); continue; }
            const Lambda * lam = std::get_if<Lambda>(funDef);
            if (!lam) { out.push_back(bd); continue; }

            // Find the underlying Function and check the safety
            // preconditions.  `f` references m.functions (NOT m.blocks), which
            // cloneSubBlock never touches — so it stays valid across inlining.
            if (lam->funcIdx == 0 || lam->funcIdx >= m.functions.size()) {
                out.push_back(bd); continue;
            }
            const Function & f = m.functions[lam->funcIdx];
            if (f.argName == kInvalidSymbol) { out.push_back(bd); continue; }
            if (f.hasFormals)                 { out.push_back(bd); continue; }
            // eval/apply (#3): an uncurried multi-arity Function (collapsed
            // curried chain) has params beyond paramVar.  Beta-reducing it by
            // a SINGLE arg would substitute paramVar and leave the extraParams
            // dangling as free vars.  Skip — the runtime PAP handles arity-N
            // application.  (extraParams is empty unless NIX_V3_EVAL_APPLY.)
            if (!f.extraParams.empty())       { out.push_back(bd); continue; }
            if (f.entryBlock == kInvalidBlock
                || f.entryBlock >= m.blocks.size()) {
                out.push_back(bd); continue;
            }

            // P-11: bodyIsSimple now recurses sub-blocks (If/With/And/Or/...).
            if (!bodyIsSimple(m, f.entryBlock)) { out.push_back(bd); continue; }

            // Use-count safety: the Lambda's VarId must have exactly
            // one use across the entire module (the App we're about
            // to inline).  This prevents:
            //   - Inlining when another callee will Apply it too
            //     (would duplicate work + need fresh body each time)
            //   - Inlining when the Lambda is captured as an upvalue
            //     in another Function (would orphan the capture)
            //
            // Note: `app->fun` may be a VarRef chain; we walk it and
            // require the FINAL Lambda binding to be uniquely used.
            // The intermediate VarRefs themselves may have multiple
            // uses; that's fine — they're cheap aliases that DCE
            // will sweep if/when their RHS becomes unreferenced.
            VarId lambdaVar = app->fun;
            while (true) {
                auto it = defs.find(lambdaVar);
                if (it == defs.end()) break;
                const VarRef * vr = std::get_if<VarRef>(it->second);
                if (!vr) break;
                lambdaVar = vr->var;
            }
            if (uses.at(lambdaVar) != 1) { out.push_back(bd); continue; }

            // ALL preconditions met — inline.  inlineBody clones the body
            // (and its sub-blocks) into `out`; on a defensive clone failure it
            // returns kInvalid WITHOUT touching `out`, so keep the original.
            VarId tail = inlineBody(m, f.entryBlock, f.paramVar, app->arg, out);
            if (tail == kInvalid) { out.push_back(bd); continue; }
            out.push_back({bd.var, VarRef{tail}});
            ++inlined;
            ++inlinedHere;
        }

        if (inlinedHere > 0)
            m.blocks[bid].bindings = std::move(out);
    }

    return inlined;
}

} // namespace nix::v3::ir
