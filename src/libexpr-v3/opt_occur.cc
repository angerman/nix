/// @file
/// Occurrence analysis (per `lode/OPT_OCCUR_PLAN_2026-05-08.md`).
///
/// Module-wide pass that classifies every VarId by use-count and by
/// whether any use crosses a function boundary.  Foundational input
/// for downstream passes:
///
///   - DCE (Dead bindings).
///   - Heuristic inliner (OnceLinear bindings — substitute RHS at
///     the unique use site, drop the binding).
///   - Selector thunks (OnceCaptured + AttrSelect-shaped RHS).
///   - Single-entry thunk elision (OnceLinear MkThunk).
///   - Demand analysis input (count + captured-use flag).
///
/// Algorithm walks every block once, accumulating operand uses into
/// per-VarId counters.  CRITICALLY: captured-list entries on Lambda /
/// MkThunk / LetRec are EXCLUDED — those are derived data populated
/// by `computeFreeVars` and counting them too would double-count
/// every captured operand.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <type_traits>
#include <variant>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// funcOfBlock[bid] -> fid  (the function that owns block bid)
// ---------------------------------------------------------------------------
//
// computeFreeVars also builds this; we don't share the helper yet
// (the existing one is local in ir.cc).  The plan calls for extracting
// a public `computeFuncOfBlock`; we inline it here for now to keep
// the diff minimal.  Sub-blocks (If/With/Assert/and/or/impl rhs) belong
// to the same function as their containing binding's block.

void collectSubBlocks(const Expr & e, std::vector<BlockId> & out)
{
    std::visit([&out](const auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, If>) {
            out.push_back(x.thenBlock);
            out.push_back(x.elseBlock);
        } else if constexpr (std::is_same_v<T, With>) {
            out.push_back(x.bodyBlock);
        } else if constexpr (std::is_same_v<T, Assert>) {
            out.push_back(x.bodyBlock);
        } else if constexpr (std::is_same_v<T, And>
                          || std::is_same_v<T, Or>
                          || std::is_same_v<T, Impl>) {
            out.push_back(x.rhsBlock);
        }
    }, e);
}

std::vector<FuncId> computeFuncOfBlock(const Module & m)
{
    const size_t nFuncs = m.functions.size();
    std::vector<FuncId> fob(m.blocks.size(), static_cast<FuncId>(nFuncs));
    std::vector<BlockId> stack;
    for (FuncId fid = 0; fid < (FuncId)nFuncs; ++fid) {
        BlockId entry = m.functions[fid].entryBlock;
        if (entry == kInvalidBlock) continue;
        stack.push_back(entry);
        while (!stack.empty()) {
            BlockId bid = stack.back(); stack.pop_back();
            if (bid == kInvalidBlock || bid >= m.blocks.size()) continue;
            if (fob[bid] != static_cast<FuncId>(nFuncs)) continue;
            fob[bid] = fid;
            for (const auto & bd : m.blocks[bid].bindings)
                collectSubBlocks(bd.expr, stack);
        }
    }
    return fob;
}

// ---------------------------------------------------------------------------
// Operand visitor — invokes `f(VarId)` for every DIRECT operand use.
// CAPTURED LISTS (freeVars / outerUpvalues / lexicalWiths) are SKIPPED —
// those are derived data populated by computeFreeVars and counting them
// double-counts every captured ref.  This is the single subtle
// correctness invariant of the pass.
// ---------------------------------------------------------------------------

template <class F>
void visitDirectOperandVars(const Expr & e, F && f)
{
    std::visit([&f](const auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, LitInt>
                   || std::is_same_v<T, LitFloat>
                   || std::is_same_v<T, LitBool>
                   || std::is_same_v<T, LitNull>
                   || std::is_same_v<T, LitString>
                   || std::is_same_v<T, LitPath>
                   || std::is_same_v<T, LitPrimOp>
                   || std::is_same_v<T, LitBuiltins>
                   || std::is_same_v<T, WithLookup>) {
            // No operands.
            (void)x;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            if (x.var != kInvalid) f(x.var);
        } else if constexpr (std::is_same_v<T, Lambda>
                          || std::is_same_v<T, MkThunk>) {
            // SKIP freeVars + lexicalWiths — those are captured-list
            // entries.  The body's direct operand uses already account
            // for the underlying refs.
            (void)x;
        } else if constexpr (std::is_same_v<T, App>) {
            f(x.fun); f(x.arg);
        } else if constexpr (std::is_same_v<T, Force>) {
            f(x.thunk);
        } else if constexpr (std::is_same_v<T, AttrSelect>
                          || std::is_same_v<T, HasAttr>
                          || std::is_same_v<T, RecBindingSlotRef>) {
            f(x.attrs);
        } else if constexpr (std::is_same_v<T, AttrSelectDyn>
                          || std::is_same_v<T, HasAttrDyn>) {
            f(x.attrs); f(x.nameVar);
        } else if constexpr (std::is_same_v<T, AttrSet>) {
            // #558: IF entries hold kInvalid placeholder values — skip
            // them.  Their actual values are referenced by the trailing
            // AttrSetSetInheritFrom binding.
            for (const auto & en : x.entries) {
                if (!en.isInheritFrom && en.value != kInvalid)
                    f(en.value);
            }
        } else if constexpr (std::is_same_v<T, AttrSetSetInheritFrom>) {
            f(x.attrSetVar);
            for (const auto & en : x.entries) f(en.valueVar);
        } else if constexpr (std::is_same_v<T, AttrSetDyn>) {
            for (const auto & en : x.statics)  f(en.value);
            for (const auto & en : x.dynamics) { f(en.nameVar); f(en.value); }
        } else if constexpr (std::is_same_v<T, ListExpr>) {
            for (auto v : x.elems) f(v);
        } else if constexpr (std::is_same_v<T, ConcatLists>
                          || std::is_same_v<T, Update>
                          || std::is_same_v<T, Add>
                          || std::is_same_v<T, Sub>
                          || std::is_same_v<T, Mul>
                          || std::is_same_v<T, Div>
                          || std::is_same_v<T, Eq>
                          || std::is_same_v<T, NEq>
                          || std::is_same_v<T, Less>) {
            f(x.lhs); f(x.rhs);
        } else if constexpr (std::is_same_v<T, If>) {
            f(x.cond);
        } else if constexpr (std::is_same_v<T, With>) {
            f(x.attrs);
            // recAttrsVar is also a direct ref when slot-capture is in
            // play — emit reads it via emitVarRef.  Count it.
            if (x.recAttrsVar != kInvalid) f(x.recAttrsVar);
        } else if constexpr (std::is_same_v<T, Assert>) {
            f(x.cond);
        } else if constexpr (std::is_same_v<T, ConcatStrings>) {
            for (auto v : x.parts) f(v);
        } else if constexpr (std::is_same_v<T, Not>) {
            f(x.operand);
        } else if constexpr (std::is_same_v<T, And>
                          || std::is_same_v<T, Or>
                          || std::is_same_v<T, Impl>) {
            f(x.lhs);
        } else if constexpr (std::is_same_v<T, PrimOpCall>) {
            for (auto v : x.args) f(v);
        } else if constexpr (std::is_same_v<T, LetRec>) {
            // SKIP outerUpvalues + lexicalWiths — same reasoning as
            // Lambda / MkThunk.  The bodies (each thunkBody Function)
            // have their own direct operand uses counted via their
            // entry block's bindings.
            (void)x;
        } else {
            static_assert(sizeof(T) == 0, "occur: unhandled IR variant");
        }
    }, e);
}

template <class F>
void visitTerminalVars(const Terminal & t, F && f)
{
    std::visit([&f](const auto & x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, TermReturn>) {
            if (x.value != kInvalid) f(x.value);
        }
    }, t);
}

// Saturating increment.  We cap at 2 because the only state above 1
// we care about is "Many".
inline uint16_t satInc(uint16_t v) noexcept {
    return v < 2 ? static_cast<uint16_t>(v + 1) : static_cast<uint16_t>(2);
}

} // namespace

// ---------------------------------------------------------------------------
// Public: isTrivialRhs
// ---------------------------------------------------------------------------

bool isTrivialRhs(const Expr & e) noexcept
{
    return std::holds_alternative<LitInt>(e)
        || std::holds_alternative<LitFloat>(e)
        || std::holds_alternative<LitBool>(e)
        || std::holds_alternative<LitNull>(e)
        || std::holds_alternative<LitString>(e)
        || std::holds_alternative<LitPath>(e)
        || std::holds_alternative<VarRef>(e)
        || std::holds_alternative<LitPrimOp>(e)
        || std::holds_alternative<LitBuiltins>(e);
}

// ---------------------------------------------------------------------------
// Public: analyseOccurrence
// ---------------------------------------------------------------------------

OccMap analyseOccurrence(const Module & m)
{
    OccMap result;
    result.data.assign(m.nextVar, OccInfo{});

    // Step 1: defFunc[var] = fid for binding/param/letrec/hidden vars.
    auto fob = computeFuncOfBlock(m);

    // Map VarId -> defining FuncId.  kInvalid'd FuncId means undefined
    // / unknown (e.g., upvalue from outside the module).
    const FuncId kUnknownFunc = static_cast<FuncId>(m.functions.size());
    std::vector<FuncId> defFunc(m.nextVar, kUnknownFunc);

    auto markVar = [&](VarId v, FuncId fid, OccKind initialKind) {
        if (v == kInvalid || v >= m.nextVar) return;
        defFunc[v] = fid;
        result.data[v].kind = initialKind;
    };

    // Function paramVars + LetRec recVars + hidden vars are Param.
    for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
        const Function & f = m.functions[fid];
        if (f.paramVar != kInvalid)
            markVar(f.paramVar, fid, OccKind::Param);
    }

    // Binding vars get Dead (will be promoted to Once*/Many on count).
    // LetRec recVars + hiddenVars get Param.
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & b = m.blocks[bid];
        FuncId fid = bid < fob.size() ? fob[bid] : kUnknownFunc;
        for (const auto & bind : b.bindings) {
            markVar(bind.var, fid, OccKind::Dead);
            // Also extract any hidden VarIds embedded in the expression
            // that should count as Param (LetRec recVar + hiddenVars).
            std::visit([&](const auto & x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, LetRec>) {
                    if (x.recVar != kInvalid)
                        markVar(x.recVar, fid, OccKind::Param);
                    for (const auto & he : x.hiddenEntries)
                        if (he.hiddenVar != kInvalid)
                            markVar(he.hiddenVar, fid, OccKind::Param);
                }
            }, bind.expr);
        }
    }

    // Step 2: count direct operand uses.
    auto recordUse = [&](VarId v, FuncId fidUse) {
        if (v == kInvalid || v >= m.nextVar) return;
        OccInfo & oi = result.data[v];
        oi.count = satInc(oi.count);
        FuncId fidDef = (v < defFunc.size()) ? defFunc[v] : kUnknownFunc;
        if (fidDef != kUnknownFunc && fidDef != fidUse)
            oi.capturedUse = true;
    };

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        const Block & b = m.blocks[bid];
        FuncId fid = bid < fob.size() ? fob[bid] : kUnknownFunc;
        for (const auto & bind : b.bindings)
            visitDirectOperandVars(bind.expr,
                [&](VarId v) { recordUse(v, fid); });
        visitTerminalVars(b.terminal,
            [&](VarId v) { recordUse(v, fid); });
    }

    // Step 3: finalise kinds.  Only Dead-classified bindings get
    // promoted; Param stays Param.  Out-of-range VarIds keep Unknown.
    for (VarId v = 1; v < m.nextVar; ++v) {
        OccInfo & oi = result.data[v];
        if (oi.kind != OccKind::Dead) continue;  // Param stays Param.
        if (oi.count == 0) {
            oi.kind = OccKind::Dead;
        } else if (oi.count == 1 && !oi.capturedUse) {
            oi.kind = OccKind::OnceLinear;
        } else if (oi.count == 1 && oi.capturedUse) {
            oi.kind = OccKind::OnceCaptured;
        } else {
            oi.kind = OccKind::Many;
        }
    }

    return result;
}

} // namespace nix::v3::ir
