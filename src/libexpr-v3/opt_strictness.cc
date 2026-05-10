/// @file
/// IR optimisation pass: redundant-Force elimination via local
/// strictness analysis.
///
/// Lower emits `forceVal(...)` (an `ir::Force` binding) defensively
/// at every place a strict context demands a value -- BinOp operands,
/// If conditions, AttrSelect roots, etc.  Force is a no-op at runtime
/// when its argument is already in WHNF, but the bytecode still
/// dispatches an OP_FORCE per call site.  On real workloads this is
/// a few percent of every loop iteration's instruction stream.
///
/// This pass identifies bindings whose RHS unconditionally produces
/// a WHNF value and rewrites every `Force{v}` over such a binding
/// as `VarRef{v}` -- a pure alias that the existing
/// `inlineTrivialBindings` pass folds away, after which `deadBindingElim`
/// removes the now-orphan binding.
///
/// "WHNF-producing" Expr kinds (the conservative whitelist):
///
///   - All literals (Lit{Int,Float,Bool,Null,String,Path}).
///   - Lambda (returns Tag::Closure -- a value, not a thunk).
///   - Force itself (Force's result is by definition forced).
///   - AttrSet / AttrSetDyn / Update / LetRec
///     (return Tag::Attrs; entries may be thunks but the *attrset*
///     value is WHNF).
///   - ListExpr / ConcatLists (return Tag::List).
///   - All primitive arithmetic / comparison / boolean / Not / HasAttr
///     (return Int / Float / Bool).
///   - ConcatStrings (returns String).
///   - LitPrimOp / LitBuiltins (return Tag::PrimOp / Tag::Attrs).
///
/// Deliberately NOT WHNF-guaranteed (left untouched):
///
///   - App: an over-applied call returns a thunk-shaped value sometimes.
///   - MkThunk: a thunk by construction.
///   - AttrSelect / AttrSelectDyn: stored entry values may be thunks.
///   - WithLookup: same.
///   - RecBindingSlotRef: returns Tag::Slot pointing at a potentially-
///     thunk Bindings entry.
///   - PrimOpCall: depends on the primop (most return WHNF, but
///     `__seq` / `__lazy`-style primops are pathological); conservative
///     skip avoids surprises.
///   - If / Assert / With: terminal value is in the sub-block, so a
///     cross-block analysis is needed -- future pass.
///
/// Block-local: only consults bindings defined in the same Block as
/// the Force, chasing VarRef chains within that block (mirrors
/// opt_const_fold's resolve pattern).  Outer-scope upvalues are
/// opaque -- their definition lives in another function.
///
/// Idempotent: each Force is examined once.  Run before
/// `inlineTrivialBindings` so the freshly-introduced VarRefs are
/// collapsed in the same pipeline pass.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace nix::v3::ir {

namespace {

/// True if `e` always evaluates to a WHNF value.  Whitelist; everything
/// else returns false (conservative).  Force itself is whitelisted so
/// `Force{Force{x}}` collapses to `Force{x}` after one pass + alias
/// fold.
bool producesWHNF(const Expr & e)
{
    return std::holds_alternative<LitInt>(e)
        || std::holds_alternative<LitFloat>(e)
        || std::holds_alternative<LitBool>(e)
        || std::holds_alternative<LitNull>(e)
        || std::holds_alternative<LitString>(e)
        || std::holds_alternative<LitPath>(e)
        || std::holds_alternative<Lambda>(e)
        || std::holds_alternative<Force>(e)
        || std::holds_alternative<AttrSet>(e)
        || std::holds_alternative<AttrSetSetInheritFrom>(e)
        || std::holds_alternative<AttrSetDyn>(e)
        || std::holds_alternative<Update>(e)
        || std::holds_alternative<LetRec>(e)
        || std::holds_alternative<ListExpr>(e)
        || std::holds_alternative<ConcatLists>(e)
        || std::holds_alternative<Add>(e)
        || std::holds_alternative<Sub>(e)
        || std::holds_alternative<Mul>(e)
        || std::holds_alternative<Div>(e)
        || std::holds_alternative<Eq>(e)
        || std::holds_alternative<NEq>(e)
        || std::holds_alternative<Less>(e)
        || std::holds_alternative<Not>(e)
        || std::holds_alternative<And>(e)
        || std::holds_alternative<Or>(e)
        || std::holds_alternative<Impl>(e)
        || std::holds_alternative<HasAttr>(e)
        || std::holds_alternative<HasAttrDyn>(e)
        || std::holds_alternative<ConcatStrings>(e)
        || std::holds_alternative<LitPrimOp>(e)
        || std::holds_alternative<LitBuiltins>(e);
}

/// VarId -> Expr* for one block's bindings.  Pointers stay valid as
/// long as the block's `bindings` vector isn't resized (the pass
/// only mutates `expr` in place; never appends or removes bindings).
using BlockMap = std::unordered_map<VarId, const Expr *>;

BlockMap mapBlock(const Block & block)
{
    BlockMap m;
    m.reserve(block.bindings.size());
    for (const auto & b : block.bindings)
        m.emplace(b.var, &b.expr);
    return m;
}

/// Walk VarRef chain inside one block until we hit a non-VarRef Expr.
/// Returns nullptr if the chain leaves the block (the source is an
/// upvalue / param / cross-block reference -- opaque to us).
const Expr * resolve(VarId v, const BlockMap & m)
{
    size_t hops = 0;
    const auto cap = m.size() + 1;
    while (hops++ < cap) {
        auto it = m.find(v);
        if (it == m.end()) return nullptr;
        const Expr * e = it->second;
        if (auto * vr = std::get_if<VarRef>(e)) {
            if (vr->var == kInvalid) return nullptr;
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

} // namespace

/// Eliminate redundant `Force{v}` bindings: if `v` (chasing local
/// VarRef chains) resolves to a WHNF-producing Expr in the same
/// block, rewrite the Force as a VarRef.  Returns the number of
/// Force bindings rewritten.
size_t elimRedundantForce(Module & m)
{
    static const bool disabled =
        std::getenv("NIX_V3_NO_OPT_STRICT") != nullptr;
    if (disabled) return 0;

    size_t rewritten = 0;
    size_t forceTotal = 0;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & block = m.blocks[bid];
        BlockMap bm = mapBlock(block);
        for (auto & bind : block.bindings) {
            auto * f = std::get_if<Force>(&bind.expr);
            if (!f) continue;
            ++forceTotal;
            if (f->thunk == kInvalid) continue;
            const Expr * src = resolve(f->thunk, bm);
            if (!src) continue;
            if (!producesWHNF(*src)) continue;
            VarId aliasedTo = f->thunk;
            bind.expr = VarRef{aliasedTo};
            // Update the local map so a later Force in the same block
            // sees the rewrite immediately (idempotent within a pass).
            bm[bind.var] = &bind.expr;
            ++rewritten;
        }
    }
    static const bool debug =
        std::getenv("NIX_V3_DBG_OPT_STRICT") != nullptr;
    if (debug && forceTotal > 0) {
        std::fprintf(stderr,
            "v3 opt strictness: %zu / %zu Force bindings rewritten\n",
            rewritten, forceTotal);
    }
    return rewritten;
}

} // namespace nix::v3::ir
