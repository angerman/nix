/// @file
/// v3 IR utilities — Module helpers and free-vars analysis.
///
/// Free-vars analysis is essential before emit:
///   - Each Lambda's `freeVars` becomes the upvalue list captured at
///     MAKE_CLOSURE.
///   - Each MkThunk's `freeVars` becomes the upvalue list captured at
///     MAKE_THUNK.
///   - The top-level Function::freeVars is empty (no enclosing scope).
///
/// Algorithm: bottom-up.  For each Block, collect referenced VarIds and
/// subtract those defined in the Block.  Whatever remains is "free relative
/// to this Block".  When a sub-block belongs to a Lambda/MkThunk, the free
/// set is propagated to the enclosing function as that function's free set.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace nix::v3::ir {

// ---------------------------------------------------------------------------
// Module helpers
// ---------------------------------------------------------------------------

BlockId Module::freshBlock()
{
    BlockId id = nextBlock++;
    if (blocks.size() <= id)
        blocks.resize(id + 1);
    return id;
}

// ---------------------------------------------------------------------------
// Global symbol table (process-wide).  internSymbol always goes through
// it so SymbolIds are consistent across imports / multiple CUs.
// ---------------------------------------------------------------------------

namespace {

/// Heterogeneous-lookup hash + equal so we can find a string_view in
/// a `unordered_map<std::string, ...>` without allocating an
/// intermediate std::string per lookup — important on the symbol
/// intern fast path which is hit hundreds of times per lower call.
struct StringHash
{
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    size_t operator()(const std::string & s) const noexcept { return std::hash<std::string_view>{}(s); }
    size_t operator()(const char * s) const noexcept { return std::hash<std::string_view>{}(s); }
};
struct StringEq
{
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
};

struct GlobalSymTab
{
    std::vector<std::string> table;
    std::unordered_map<std::string, SymbolId, StringHash, StringEq> index;
    GlobalSymTab() {
        // Reserve slot 0 for the kInvalidSymbol sentinel (empty string).
        table.emplace_back("");
        index.emplace("", 0u);
    }
};

GlobalSymTab & gst()
{
    static GlobalSymTab t;
    return t;
}

} // namespace

const std::vector<std::string> & globalSymbolTable() { return gst().table; }

SymbolId globalInternSymbol(std::string_view s)
{
    auto & t = gst();
    // Heterogeneous lookup avoids the std::string(s) allocation on
    // every probe.  Only on a miss do we materialise the string for
    // the table + index entries.
    auto it = t.index.find(s);
    if (it != t.index.end()) return it->second;
    SymbolId id = static_cast<SymbolId>(t.table.size());
    t.table.emplace_back(s);
    t.index.emplace(t.table.back(), id);
    return id;
}

SymbolId Module::internSymbol(std::string_view s)
{
    SymbolId id = globalInternSymbol(s);
    // Mirror into the per-module symbols vector for diagnostics.  Grow
    // sparsely.
    if (symbols.size() <= id) symbols.resize(id + 1);
    if (symbols[id].empty() && !s.empty()) symbols[id] = std::string(s);
    return id;
}

std::string_view Module::symbolName(SymbolId id) const
{
    if (id < symbols.size() && !symbols[id].empty()) return symbols[id];
    auto & g = globalSymbolTable();
    if (id < g.size()) return g[id];
    return "";
}

// ---------------------------------------------------------------------------
// Per-Expr direct VarId references (no recursion into sub-blocks).
// ---------------------------------------------------------------------------

namespace {

void collectExprDirect(const Expr & expr, std::unordered_set<VarId> & refs)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, LitInt> ||
                      std::is_same_v<T, LitFloat> ||
                      std::is_same_v<T, LitBool> ||
                      std::is_same_v<T, LitNull> ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath> ||
                      std::is_same_v<T, PosExpr> ||
                      std::is_same_v<T, WithLookup>) {
            (void)e;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            if (e.var != kInvalid) refs.insert(e.var);
        } else if constexpr (std::is_same_v<T, Lambda> ||
                             std::is_same_v<T, MkThunk>) {
            for (auto v : e.freeVars) refs.insert(v);
        } else if constexpr (std::is_same_v<T, App>) {
            refs.insert(e.fun); refs.insert(e.arg);
        } else if constexpr (std::is_same_v<T, Force>) {
            refs.insert(e.thunk);
        } else if constexpr (std::is_same_v<T, AttrSelect> ||
                             std::is_same_v<T, HasAttr>) {
            refs.insert(e.attrs);
        } else if constexpr (std::is_same_v<T, AttrSelectDyn> ||
                             std::is_same_v<T, HasAttrDyn>) {
            refs.insert(e.attrs); refs.insert(e.nameVar);
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & en : e.entries) refs.insert(en.value);
        } else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (auto & en : e.statics)  refs.insert(en.value);
            for (auto & en : e.dynamics) { refs.insert(en.nameVar); refs.insert(en.value); }
        } else if constexpr (std::is_same_v<T, RecAttrSet>) {
            for (auto & en : e.entries) refs.insert(en.value);
        } else if constexpr (std::is_same_v<T, ListExpr>) {
            for (auto v : e.elems) refs.insert(v);
        } else if constexpr (std::is_same_v<T, ConcatLists> ||
                             std::is_same_v<T, Update> ||
                             std::is_same_v<T, Add> ||
                             std::is_same_v<T, Sub> ||
                             std::is_same_v<T, Mul> ||
                             std::is_same_v<T, Div> ||
                             std::is_same_v<T, Eq>  ||
                             std::is_same_v<T, NEq> ||
                             std::is_same_v<T, Less>) {
            refs.insert(e.lhs); refs.insert(e.rhs);
        } else if constexpr (std::is_same_v<T, If>) {
            refs.insert(e.cond);
        } else if constexpr (std::is_same_v<T, With>) {
            refs.insert(e.attrs);
        } else if constexpr (std::is_same_v<T, Assert>) {
            refs.insert(e.cond);
        } else if constexpr (std::is_same_v<T, ConcatStrings>) {
            for (auto v : e.parts) refs.insert(v);
        } else if constexpr (std::is_same_v<T, PrimOpCall>) {
            for (auto v : e.args) refs.insert(v);
        } else if constexpr (std::is_same_v<T, LitPrimOp> ||
                             std::is_same_v<T, LitBuiltins>) {
            (void)e;
        } else if constexpr (std::is_same_v<T, LetRec>) {
            // The thunk-body Functions reference each thunk's outer
            // captures.  These VarIds are needed at MAKE_THUNK time so
            // they appear in the binding's direct refs.
            for (auto & en : e.entries)
                for (auto v : en.outerUpvalues) refs.insert(v);
        } else if constexpr (std::is_same_v<T, Not> ||
                             std::is_same_v<T, Negate>) {
            refs.insert(e.operand);
        } else if constexpr (std::is_same_v<T, And> ||
                             std::is_same_v<T, Or>  ||
                             std::is_same_v<T, Impl>) {
            refs.insert(e.lhs);
        }
    }, expr);
}

void collectBlockRefs(const Module & m, BlockId bid,
                      std::unordered_set<VarId> & refs);

/// Sub-blocks reachable from `expr` (NOT counting nested function bodies —
/// those are isolated frames whose refs become upvalues at the call site).
void collectExprSubBlocks(const Expr & expr, std::vector<BlockId> & out)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, If>) {
            out.push_back(e.thenBlock); out.push_back(e.elseBlock);
        } else if constexpr (std::is_same_v<T, With>) {
            out.push_back(e.bodyBlock);
        } else if constexpr (std::is_same_v<T, Assert>) {
            out.push_back(e.bodyBlock);
        } else if constexpr (std::is_same_v<T, And> ||
                             std::is_same_v<T, Or> ||
                             std::is_same_v<T, Impl>) {
            out.push_back(e.rhsBlock);
        }
    }, expr);
}

void collectBlockRefs(const Module & m, BlockId bid,
                      std::unordered_set<VarId> & refs)
{
    const Block & b = m.blocks[bid];

    // Defined within this block: params + each binding's var.
    std::unordered_set<VarId> defined;
    for (auto v : b.params) defined.insert(v);
    for (auto & bd : b.bindings) defined.insert(bd.var);

    // Refs from this block's expressions and sub-blocks.
    std::unordered_set<VarId> raw;
    for (auto & bd : b.bindings) {
        collectExprDirect(bd.expr, raw);
        std::vector<BlockId> subs;
        collectExprSubBlocks(bd.expr, subs);
        for (auto sb : subs) collectBlockRefs(m, sb, raw);
    }
    if (auto * ret = std::get_if<TermReturn>(&b.terminal)) {
        if (ret->value != kInvalid) raw.insert(ret->value);
    }

    // refs += raw - defined.
    for (auto v : raw)
        if (defined.find(v) == defined.end())
            refs.insert(v);
}

} // namespace

// ---------------------------------------------------------------------------
// Public: computeFreeVars
// ---------------------------------------------------------------------------

void computeFreeVars(Module & m)
{
    // Free-vars analysis must converge across nested lambdas: the
    // surrounding function's free set depends on each Lambda binding's
    // freeVars, which in turn depends on the inner function's freeVars.
    // We iterate until a fixed point.  Convergence is fast — typical
    // programs need 1-3 iterations even with deep nesting.

    auto computeOne = [&](FuncId fid) -> std::vector<VarId> {
        const Function & f = m.functions[fid];
        if (f.entryBlock == kInvalidBlock) return {};
        std::unordered_set<VarId> refs;
        collectBlockRefs(m, f.entryBlock, refs);
        // The function's param VarId is bound at call time (not a normal
        // local binding), so it should not appear as a free variable —
        // even for `{a, b}: ...` formals where there's no `@arg` name.
        if (f.paramVar != kInvalid &&
            (f.argName != kInvalidSymbol || f.hasFormals))
            refs.erase(f.paramVar);
        std::vector<VarId> fv(refs.begin(), refs.end());
        std::sort(fv.begin(), fv.end());
        return fv;
    };

    constexpr int kMaxIters = 16;
    for (int iter = 0; iter < kMaxIters; ++iter) {
        bool changed = false;

        // Recompute each function's freeVars from its block refs.
        for (size_t fid = 0; fid < m.functions.size(); ++fid) {
            auto fv = computeOne(static_cast<FuncId>(fid));
            if (m.functions[fid].freeVars != fv) {
                m.functions[fid].freeVars = std::move(fv);
                changed = true;
            }
        }

        // Propagate to Lambda/MkThunk binding freeVars (used as upvalue
        // capture order at MAKE_CLOSURE / MAKE_THUNK time).  LetRec
        // entries get their outerUpvalues set to each thunk body's
        // freeVars minus the rec-self VarId (which is supplied as the
        // implicit first upvalue at MAKE_THUNK time).
        for (auto & blk : m.blocks) {
            for (auto & bd : blk.bindings) {
                std::visit([&](auto & e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, Lambda> ||
                                  std::is_same_v<T, MkThunk>) {
                        if (e.funcIdx < m.functions.size()) {
                            const auto & ff = m.functions[e.funcIdx].freeVars;
                            if (e.freeVars != ff) {
                                e.freeVars = ff;
                                changed = true;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, LetRec>) {
                        for (auto & en : e.entries) {
                            if (en.thunkBody >= m.functions.size()) continue;
                            const auto & ff = m.functions[en.thunkBody].freeVars;
                            std::vector<VarId> outers;
                            outers.reserve(ff.size());
                            for (auto v : ff)
                                if (v != e.recVar) outers.push_back(v);
                            if (en.outerUpvalues != outers) {
                                en.outerUpvalues = std::move(outers);
                                changed = true;
                            }
                        }
                    }
                }, bd.expr);
            }
        }

        if (!changed) break;
    }
}

} // namespace nix::v3::ir
