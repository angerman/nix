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
#include <stdexcept>
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
//
// Public entry-point: every IR pass that asks "what VarIds does this
// Expr consume?" funnels through here.  Keeping a single source of
// truth means a new variant only has to be added in one place.

void collectExprRefs(const Expr & expr, std::unordered_set<VarId> & refs)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, LitInt> ||
                      std::is_same_v<T, LitFloat> ||
                      std::is_same_v<T, LitBool> ||
                      std::is_same_v<T, LitNull> ||
                      std::is_same_v<T, LitString> ||
                      std::is_same_v<T, LitPath> ||
                      std::is_same_v<T, WithLookup>) {
            (void)e;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            if (e.var != kInvalid) refs.insert(e.var);
        } else if constexpr (std::is_same_v<T, Lambda> ||
                             std::is_same_v<T, MkThunk>) {
            for (auto v : e.freeVars) refs.insert(v);
            // #530 lexical-with chain: lexicalWiths VarIds are
            // emitted from the maker's frame at MAKE_THUNK /
            // MAKE_CLOSURE time (alongside freeVars), so they must
            // appear in the maker function's freeVars set too if the
            // with-target was bound in an enclosing function.  Treat
            // them as direct refs of this binding to give the same
            // free-var-analysis treatment that freeVars receive.
            for (auto v : e.lexicalWiths) refs.insert(v);
        } else if constexpr (std::is_same_v<T, App>) {
            refs.insert(e.fun); refs.insert(e.arg);
        } else if constexpr (std::is_same_v<T, Force>) {
            refs.insert(e.thunk);
        } else if constexpr (std::is_same_v<T, AttrSelect> ||
                             std::is_same_v<T, HasAttr> ||
                             std::is_same_v<T, RecBindingSlotRef>) {
            refs.insert(e.attrs);
        } else if constexpr (std::is_same_v<T, AttrSelectDyn> ||
                             std::is_same_v<T, HasAttrDyn>) {
            refs.insert(e.attrs); refs.insert(e.nameVar);
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            for (auto & en : e.entries) refs.insert(en.value);
        } else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (auto & en : e.statics)  refs.insert(en.value);
            for (auto & en : e.dynamics) { refs.insert(en.nameVar); refs.insert(en.value); }
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
            // Hidden from-expr thunks (REVIEW HIGH-4 follow-up): same
            // discipline -- their outerUpvalues are MAKE_THUNK
            // captures from the LetRec's containing block.
            for (auto & he : e.hiddenEntries)
                for (auto v : he.outerUpvalues) refs.insert(v);
            // #530 lexical-with chain: the lexicalWiths VarIds are
            // also emitted from the maker frame's value stack at
            // OP_MAKE_THUNK time (alongside outerUpvalues), so they
            // need to flow into the surrounding function's freeVars.
            for (auto & en : e.entries)
                for (auto v : en.lexicalWiths) refs.insert(v);
            for (auto & he : e.hiddenEntries)
                for (auto v : he.lexicalWiths) refs.insert(v);
        } else if constexpr (std::is_same_v<T, Not>) {
            refs.insert(e.operand);
        } else if constexpr (std::is_same_v<T, And> ||
                             std::is_same_v<T, Or>  ||
                             std::is_same_v<T, Impl>) {
            refs.insert(e.lhs);
        }
    }, expr);
}

namespace {

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

    // Defined within this block: each binding's var, plus any
    // hidden VarIds the binding's expr defines internally (e.g.
    // ir::LetRec::hiddenEntries -- REVIEW HIGH-4 follow-up).
    std::unordered_set<VarId> defined;
    for (auto & bd : b.bindings) {
        defined.insert(bd.var);
        std::visit([&](auto & e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, LetRec>) {
                for (auto & he : e.hiddenEntries)
                    defined.insert(he.hiddenVar);
                // #458 step 3/6: the LetRec emit also binds a
                // synthetic `recSlotVar` (heap-stable Tag::Slot) to a
                // local in the containing function via the
                // OP_REC_SLOT_PUBLISH + OP_SET_LOCAL pair.  The
                // recSlotVar isn't bound by any ir::Binding so it
                // wouldn't otherwise appear in the `defined` set --
                // making it leak into the function's freeVars
                // wherever it's referenced.  Look it up via
                // Module::recVarToSlotVar (keyed by recVar).
                if (auto sit = m.recVarToSlotVar.find(e.recVar);
                    sit != m.recVarToSlotVar.end())
                {
                    defined.insert(sit->second);
                }
            }
        }, bd.expr);
    }

    // Refs from this block's expressions and sub-blocks.
    std::unordered_set<VarId> raw;
    for (auto & bd : b.bindings) {
        collectExprRefs(bd.expr, raw);
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

    // Phase-13 review MED-7: assert the fixed point actually
    // converged.  The cap exists only as a runaway-loop guard; if we
    // hit it without `changed` going false, freeVars are still in
    // flux and downstream upvalue capture lists / MAKE_CLOSURE
    // operands are wrong — silent miscompilation.  Promote to a
    // hard error so pathological mutual recursion is loud.
    //
    // REVIEW MED-8: dirty-set propagation.  A function's freeVars
    // depends only on its own blocks' refs, which include nested
    // Lambda/MkThunk/LetRec freeVars.  When a child function's
    // freeVars don't change in iteration N, no parent that captures
    // it can change either — so we only need to re-compute parents
    // of functions that DID change.  Build a reverse-deps map once,
    // then iterate by working set.
    constexpr int kMaxIters = 16;
    bool converged = false;
    const size_t nFuncs = m.functions.size();

    // reverseDeps[fid] = list of parent FuncIds that contain a
    // binding referencing fid.  Built once below by walking every
    // block's bindings.  A LetRec entry's thunkBody is also a child.
    std::vector<std::vector<FuncId>> reverseDeps(nFuncs);
    {
        // funcOfBlock[bid] = the FuncId whose entryBlock walk
        // reaches block bid.  Computed via per-function reachability.
        std::vector<FuncId> funcOfBlock(m.blocks.size(),
            static_cast<FuncId>(nFuncs));  // sentinel = none
        std::vector<BlockId> stack;
        for (size_t fid = 0; fid < nFuncs; ++fid) {
            if (m.functions[fid].entryBlock == kInvalidBlock) continue;
            stack.push_back(m.functions[fid].entryBlock);
            while (!stack.empty()) {
                BlockId bid = stack.back(); stack.pop_back();
                if (bid == kInvalidBlock || bid >= m.blocks.size()) continue;
                if (funcOfBlock[bid] != static_cast<FuncId>(nFuncs)) continue;
                funcOfBlock[bid] = static_cast<FuncId>(fid);
                std::vector<BlockId> subs;
                for (auto & bd : m.blocks[bid].bindings) {
                    collectExprSubBlocks(bd.expr, subs);
                }
                for (auto sub : subs) stack.push_back(sub);
            }
        }
        for (size_t bid = 1; bid < m.blocks.size(); ++bid) {
            FuncId parent = funcOfBlock[bid];
            if (parent == static_cast<FuncId>(nFuncs)) continue;
            for (auto & bd : m.blocks[bid].bindings) {
                std::visit([&](auto & e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, Lambda> ||
                                  std::is_same_v<T, MkThunk>) {
                        if (e.funcIdx < nFuncs)
                            reverseDeps[e.funcIdx].push_back(parent);
                    } else if constexpr (std::is_same_v<T, LetRec>) {
                        for (auto & en : e.entries)
                            if (en.thunkBody < nFuncs)
                                reverseDeps[en.thunkBody].push_back(parent);
                        for (auto & he : e.hiddenEntries)
                            if (he.thunkBody < nFuncs)
                                reverseDeps[he.thunkBody].push_back(parent);
                    }
                }, bd.expr);
            }
        }
    }

    // Initially all functions are dirty (must compute at least once).
    std::vector<uint8_t> dirty(nFuncs, 1);

    for (int iter = 0; iter < kMaxIters; ++iter) {
        bool changed = false;
        std::vector<uint8_t> nextDirty(nFuncs, 0);

        // Recompute freeVars only for dirty functions.
        for (size_t fid = 0; fid < nFuncs; ++fid) {
            if (!dirty[fid]) continue;
            auto fv = computeOne(static_cast<FuncId>(fid));
            if (m.functions[fid].freeVars != fv) {
                m.functions[fid].freeVars = std::move(fv);
                changed = true;
                // Mark every parent that captures this function as
                // dirty for the next iteration.
                for (auto p : reverseDeps[fid]) nextDirty[p] = 1;
            }
        }

        // Propagate to Lambda/MkThunk/LetRec binding freeVars (used
        // as upvalue capture order at MAKE_CLOSURE / MAKE_THUNK time).
        // This pass is cheap and global -- needs to see every binding
        // to keep the IR consistent before emit.
        for (auto & blk : m.blocks) {
            for (auto & bd : blk.bindings) {
                std::visit([&](auto & e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, Lambda> ||
                                  std::is_same_v<T, MkThunk>) {
                        if (e.funcIdx < nFuncs) {
                            const auto & ff = m.functions[e.funcIdx].freeVars;
                            if (e.freeVars != ff) {
                                e.freeVars = ff;
                                changed = true;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, LetRec>) {
                        for (auto & en : e.entries) {
                            if (en.thunkBody >= nFuncs) continue;
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
                        // Hidden from-expr thunks: same outerUpvalues
                        // synthesis as regular entries (REVIEW HIGH-4
                        // follow-up).  Skip the recVar from upvalues
                        // since it's bound by the SET_LOCAL above.
                        for (auto & he : e.hiddenEntries) {
                            if (he.thunkBody >= nFuncs) continue;
                            const auto & ff = m.functions[he.thunkBody].freeVars;
                            std::vector<VarId> outers;
                            outers.reserve(ff.size());
                            for (auto v : ff)
                                if (v != e.recVar) outers.push_back(v);
                            if (he.outerUpvalues != outers) {
                                he.outerUpvalues = std::move(outers);
                                changed = true;
                            }
                        }
                    }
                }, bd.expr);
            }
        }

        if (!changed) { converged = true; break; }
        dirty = std::move(nextDirty);
    }
    if (!converged)
        throw std::runtime_error(
            "v3 computeFreeVars: free-var fixed-point did not converge "
            "within 16 iterations; pathological mutual recursion likely.  "
            "Aborting compilation rather than emitting wrong upvalue lists.");
}

} // namespace nix::v3::ir
