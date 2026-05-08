/// @file
/// IR optimisation pass: VarRef alias collapsing.
///
/// In v3's A-normal IR every operand is a VarId, so the only form of
/// "trivial" binding that can be folded into its consumers is a pure
/// alias:
///
///     v = VarRef{u}
///
/// We never inline literals or other Exprs: operand slots are VarIds,
/// not Exprs.  A binding like `v = LitInt{42}` is the canonical form
/// already; "inlining" it would just allocate a fresh VarId binding
/// the same literal, which is no improvement.  De-duplicating
/// equal-valued literals is a separate concern (block-local CSE,
/// future pass).
///
/// Algorithm:
///   1. Walk every Block; for each binding `v = VarRef{u}` record
///      alias[v] = u.  This is unconditional: VarRef bindings are
///      pure aliases, and replacing `v` with `u` everywhere is
///      semantically a no-op.
///   2. Path-compress so alias[v] points all the way through any
///      chain of aliases (`v = VarRef{u}; u = VarRef{w}` → alias[v]=w,
///      alias[u]=w).
///   3. Visit every Expr in every binding + every Terminal + every
///      Function::paramVar; rewrite operand VarIds via alias[].
///   4. Drop the alias bindings themselves.  DCE would have removed
///      them anyway, but doing it here keeps the module compact for
///      downstream passes that walk bindings.
///
/// IMPORTANT:
///   - We run BEFORE computeFreeVars, so Lambda::freeVars,
///     MkThunk::freeVars, LetRec entry/hiddenEntry outerUpvalues are
///     all empty at this point.  Nothing to rewrite there.
///   - Function::paramVar is the *defining* var inside the function,
///     not a use, but if it happens to equal an alias source we still
///     leave it alone (paramVar is bound externally at call time).
///     In practice the lowerer never assigns a `VarRef` binding for
///     the parameter slot, so this is not exercised.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <unordered_map>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Build alias map
// ---------------------------------------------------------------------------

void buildAliasMap(const Module & m, std::unordered_map<VarId, VarId> & alias)
{
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid)
        for (const auto & bind : m.blocks[bid].bindings)
            if (auto * vr = std::get_if<VarRef>(&bind.expr))
                if (vr->var != kInvalid)
                    alias[bind.var] = vr->var;

    // Path compression: chase each chain to its terminal target.
    // Capped at the map size to defend against pathological cycles
    // (the lowerer cannot produce them, but defence in depth).
    const size_t cap = alias.size() + 1;
    for (auto & [k, v] : alias) {
        size_t hops = 0;
        VarId cur = v;
        while (hops++ < cap) {
            auto it = alias.find(cur);
            if (it == alias.end()) break;
            if (it->second == cur) break; // self-alias
            cur = it->second;
        }
        v = cur;
    }
}

// ---------------------------------------------------------------------------
// Operand rewrite
// ---------------------------------------------------------------------------

/// Rewrite a single VarId operand through the alias map (in place).
inline void rewriteVar(VarId & v, const std::unordered_map<VarId, VarId> & alias)
{
    if (v == kInvalid) return;
    auto it = alias.find(v);
    if (it != alias.end()) v = it->second;
}

void rewriteExpr(Expr & expr, const std::unordered_map<VarId, VarId> & alias)
{
    std::visit([&](auto & e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, LitInt>    ||
                      std::is_same_v<T, LitFloat>  ||
                      std::is_same_v<T, LitBool>   ||
                      std::is_same_v<T, LitNull>   ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath>   ||
                      std::is_same_v<T, LitPrimOp> ||
                      std::is_same_v<T, LitBuiltins> ||
                      std::is_same_v<T, WithLookup>) {
            (void)e;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            rewriteVar(e.var, alias);
        } else if constexpr (std::is_same_v<T, Lambda> ||
                             std::is_same_v<T, MkThunk>) {
            // freeVars empty pre-computeFreeVars; rewrite anyway in
            // case a future pipeline change populates it earlier.
            for (auto & v : e.freeVars) rewriteVar(v, alias);
            // #530 lexical-with chain — same discipline.
            for (auto & v : e.lexicalWiths) rewriteVar(v, alias);
        } else if constexpr (std::is_same_v<T, App>) {
            rewriteVar(e.fun, alias); rewriteVar(e.arg, alias);
        } else if constexpr (std::is_same_v<T, Force>) {
            rewriteVar(e.thunk, alias);
        } else if constexpr (std::is_same_v<T, AttrSelect> ||
                             std::is_same_v<T, HasAttr>    ||
                             std::is_same_v<T, RecBindingSlotRef>) {
            rewriteVar(e.attrs, alias);
        } else if constexpr (std::is_same_v<T, AttrSelectDyn> ||
                             std::is_same_v<T, HasAttrDyn>) {
            rewriteVar(e.attrs, alias); rewriteVar(e.nameVar, alias);
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & en : e.entries) rewriteVar(en.value, alias);
        } else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (auto & en : e.statics)  rewriteVar(en.value, alias);
            for (auto & en : e.dynamics) { rewriteVar(en.nameVar, alias); rewriteVar(en.value, alias); }
        } else if constexpr (std::is_same_v<T, ListExpr>) {
            for (auto & v : e.elems) rewriteVar(v, alias);
        } else if constexpr (std::is_same_v<T, ConcatLists> ||
                             std::is_same_v<T, Update>      ||
                             std::is_same_v<T, Add>         ||
                             std::is_same_v<T, Sub>         ||
                             std::is_same_v<T, Mul>         ||
                             std::is_same_v<T, Div>         ||
                             std::is_same_v<T, Eq>          ||
                             std::is_same_v<T, NEq>         ||
                             std::is_same_v<T, Less>) {
            rewriteVar(e.lhs, alias); rewriteVar(e.rhs, alias);
        } else if constexpr (std::is_same_v<T, If>) {
            rewriteVar(e.cond, alias);
        } else if constexpr (std::is_same_v<T, With>) {
            rewriteVar(e.attrs, alias);
            rewriteVar(e.recAttrsVar, alias);
        } else if constexpr (std::is_same_v<T, Assert>) {
            rewriteVar(e.cond, alias);
        } else if constexpr (std::is_same_v<T, ConcatStrings>) {
            for (auto & v : e.parts) rewriteVar(v, alias);
        } else if constexpr (std::is_same_v<T, PrimOpCall>) {
            for (auto & v : e.args) rewriteVar(v, alias);
        } else if constexpr (std::is_same_v<T, Not>) {
            rewriteVar(e.operand, alias);
        } else if constexpr (std::is_same_v<T, And> ||
                             std::is_same_v<T, Or>  ||
                             std::is_same_v<T, Impl>) {
            rewriteVar(e.lhs, alias);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            // recVar is a defining var, not a use -- leave alone.
            // outerUpvalues empty pre-computeFreeVars; rewrite for
            // forward-compat.
            for (auto & en : e.entries)
                for (auto & v : en.outerUpvalues) rewriteVar(v, alias);
            for (auto & he : e.hiddenEntries)
                for (auto & v : he.outerUpvalues) rewriteVar(v, alias);
            // #530 lexical-with chain — these ARE populated at lower
            // time, so rewrite always.
            for (auto & en : e.entries)
                for (auto & v : en.lexicalWiths) rewriteVar(v, alias);
            for (auto & he : e.hiddenEntries)
                for (auto & v : he.lexicalWiths) rewriteVar(v, alias);
        }
    }, expr);
}

void rewriteTerminal(Terminal & t, const std::unordered_map<VarId, VarId> & alias)
{
    std::visit([&](auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, TermReturn>) {
            rewriteVar(x.value, alias);
        }
    }, t);
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: inlineTrivialBindings
// ---------------------------------------------------------------------------

size_t inlineTrivialBindings(Module & m)
{
    std::unordered_map<VarId, VarId> alias;
    buildAliasMap(m, alias);
    if (alias.empty()) return 0;

    // Apply rewrites everywhere.
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & b = m.blocks[bid];
        for (auto & bind : b.bindings)
            rewriteExpr(bind.expr, alias);
        rewriteTerminal(b.terminal, alias);
    }

    // Drop the alias bindings themselves: their VarIds are no longer
    // referenced anywhere.  DCE could pick them up, but it's tidier
    // to remove them here so subsequent passes see a smaller IR.
    size_t removed = 0;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & b = m.blocks[bid];
        if (b.bindings.empty()) continue;
        auto src = b.bindings.begin();
        auto dst = b.bindings.begin();
        for (; src != b.bindings.end(); ++src) {
            if (alias.find(src->var) != alias.end()) {
                ++removed;
                continue;
            }
            if (dst != src) *dst = std::move(*src);
            ++dst;
        }
        b.bindings.erase(dst, b.bindings.end());
    }
    return removed;
}

} // namespace nix::v3::ir
