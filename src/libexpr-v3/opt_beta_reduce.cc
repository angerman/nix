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
///        - F.intrinsicKind == 0 (not Fix/Extends/Compose intrinsic)
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
/// Gate: NIX_V3_NO_BETA_REDUCE=1 to disable the pass for A/B testing.
/// Retire when bench shows stable wins across the corpus and no
/// regressions in v3-property-tests or v3-lang-tests.
///
/// Runs BEFORE inlineTrivialBindings (so the cloned bindings' VarRefs
/// get path-compressed) and BEFORE constantFold (so cloned literal
/// arithmetic folds).  See opt_const_fold.cc::optimise pipeline.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

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
// Same-block VarRef chase: resolve a VarId through VarRef aliases
// inside one block.  Returns the underlying Expr*, or nullptr if the
// chain leads outside the block.
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

// Build a map from VarId to its defining Expr* within a single block.
// VarIds defined in outer blocks are absent.
std::unordered_map<VarId, const Expr *> mapBlockDefs(const Block & b)
{
    std::unordered_map<VarId, const Expr *> defs;
    defs.reserve(b.bindings.size());
    for (const auto & bd : b.bindings)
        defs.emplace(bd.var, &bd.expr);
    return defs;
}

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
// Check whether a Function's body block is "simple enough" to clone:
// only contains Expr kinds we know how to remap.  Returns true if
// every binding's Expr passes the remap predicate (without actually
// modifying anything — we just probe with an empty substitution map).
// ---------------------------------------------------------------------------

bool bodyIsSimple(const Block & body)
{
    static const std::unordered_map<VarId, VarId> empty;
    for (const auto & bd : body.bindings) {
        Expr probe = bd.expr;  // copy so we don't mutate the source
        if (!remapExprVars(probe, empty))
            return false;
    }
    // Terminal is TermReturn{VarId}, always cloneable.
    return true;
}

// ---------------------------------------------------------------------------
// Perform the inline: clone `body` into `out` with the substitution
// paramVar → arg, allocating fresh VarIds for body bindings.  Returns
// the cloned tail VarId (i.e. what the App's VarId should VarRef to).
// ---------------------------------------------------------------------------

VarId inlineBody(Module & m,
                 const Block & body,
                 VarId paramVar,
                 VarId arg,
                 std::vector<Binding> & out)
{
    std::unordered_map<VarId, VarId> sub;
    sub.reserve(body.bindings.size() + 1);
    sub[paramVar] = arg;

    for (const auto & bd : body.bindings) {
        VarId newVar = m.freshVar();
        sub[bd.var] = newVar;
        Expr cloned = bd.expr;
        // bodyIsSimple already verified remapExprVars will succeed.
        (void)remapExprVars(cloned, sub);
        out.push_back({newVar, std::move(cloned)});
    }

    // Terminal: substitute the return-value VarId via the same map.
    VarId tailOriginal = std::get<TermReturn>(body.terminal).value;
    auto it = sub.find(tailOriginal);
    return (it != sub.end()) ? it->second : tailOriginal;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: betaReduce.
// ---------------------------------------------------------------------------

size_t betaReduce(Module & m)
{
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_BETA_REDUCE") != nullptr;
    if (s_disabled) return 0;

    // Pass 1: count uses across the entire module.
    UseCounter uses = countModuleUses(m);

    size_t inlined = 0;

    // Pass 2: walk each block; rewrite App-of-Lambda patterns where safe.
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        // Build same-block defs map BEFORE rewriting (so we can chase
        // VarRef → Lambda lookups using the pre-rewrite defs).
        auto defs = mapBlockDefs(blk);

        // Single output vector; emit-in-order.  Reserve generously
        // since beta-reduced blocks grow (one App becomes N body
        // bindings + one VarRef).
        std::vector<Binding> out;
        out.reserve(blk.bindings.size() * 2);

        for (const auto & bd : blk.bindings) {
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
            // preconditions.
            if (lam->funcIdx == 0 || lam->funcIdx >= m.functions.size()) {
                out.push_back(bd); continue;
            }
            const Function & f = m.functions[lam->funcIdx];
            if (f.argName == kInvalidSymbol) { out.push_back(bd); continue; }
            if (f.hasFormals)                 { out.push_back(bd); continue; }
            if (f.intrinsicKind != 0)         { out.push_back(bd); continue; }
            if (f.entryBlock == kInvalidBlock
                || f.entryBlock >= m.blocks.size()) {
                out.push_back(bd); continue;
            }

            const Block & body = m.blocks[f.entryBlock];
            if (!bodyIsSimple(body))         { out.push_back(bd); continue; }

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

            // ALL preconditions met — inline.
            VarId tail = inlineBody(m, body, f.paramVar, app->arg, out);
            out.push_back({bd.var, VarRef{tail}});
            ++inlined;
        }

        if (inlined > 0)
            blk.bindings = std::move(out);
    }

    return inlined;
}

} // namespace nix::v3::ir
