/// @file
/// #737 Stage 4 v2 (2026-05-21) — per-Function strictness inference.
///
/// Computes the `ir::Function::strictArgs` bitmap (one bit per
/// formal arg).  A bit is set iff EVERY execution path through
/// the function body forces the corresponding formal BEFORE any
/// branching point — meaning the call site is safe to pre-force
/// the arg and skip the MkThunk wrap.
///
/// Why this exists: today every non-trivial function-call arg
/// goes through `thunkifyForArg` (lower.cc), which emits a
/// MkThunk binding even when the callee's body would have forced
/// the value immediately.  On hello.drvPath: 663k thunks alloc'd,
/// many of them function args that the body would have forced
/// anyway.  A strictness signature lets the caller side skip the
/// MkThunk when the callee is statically known.
///
/// Current Status: ANALYSIS ONLY (v2).  This commit lands the
/// per-Function `strictArgs` bitmap + telemetry; the call-site
/// emitter does NOT yet consume the signature.  A follow-on (v3)
/// will wire caller-side use through OP_CALL_STRICT or equivalent.
///
/// Conservative direction: under-marking strictness is safe (just
/// keeps unnecessary thunks).  Over-marking is UNSAFE (eager-
/// evaluating an arg the body might never force can throw on `f
/// (throw "x")` patterns that the caller expects to be lazy).
///
/// Branch handling: v2 walks the linear prefix of the body's
/// entry block.  If a binding's RHS contains a branch (If, With,
/// Assert, And/Or/Impl with their right-side blocks), we mark
/// the scrutinee strict but stop after that binding — branches
/// have divergent demand and joining them needs a real lattice
/// pass (deferred to v3).
///
/// Telemetry: `NIX_V3_DBG_STRICTNESS=1` triggers a one-line
/// summary on the first call to `computeFunctionStrictness`.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/primop.hh"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

/// Add the VarIds that `e` definitely forces to `forced`.  Returns
/// true if `e` is a "linear" expression that does not introduce a
/// branch — caller continues walking; false if `e` contains a
/// branching sub-block we cannot trivially follow (caller stops).
///
/// Conservative: ANY expression we don't explicitly recognize
/// (App, MkThunk, AttrSet, ListExpr, LitFunction, etc.) is
/// non-branching but contributes NO forced operands (we don't
/// know).
bool collectForced(const Expr & e, std::unordered_set<VarId> & forced)
{
    return std::visit([&](const auto & x) -> bool {
        using T = std::decay_t<decltype(x)>;

        // --- Branchers: scrutinee is forced, body is not walked ---
        if constexpr (std::is_same_v<T, If>) {
            forced.insert(x.cond);
            return false;  // both branches unreachable from linear walk
        }
        else if constexpr (std::is_same_v<T, Assert>) {
            forced.insert(x.cond);
            return false;
        }
        else if constexpr (std::is_same_v<T, With>) {
            forced.insert(x.attrs);
            return false;
        }
        else if constexpr (std::is_same_v<T, And>
                        || std::is_same_v<T, Or>
                        || std::is_same_v<T, Impl>) {
            // lhs is forced; rhs is a sub-block (rhsBlock), branched.
            forced.insert(x.lhs);
            return false;
        }

        // --- Strict-operand bindings: both operands are forced ---
        else if constexpr (std::is_same_v<T, Add>
                        || std::is_same_v<T, Sub>
                        || std::is_same_v<T, Mul>
                        || std::is_same_v<T, Div>
                        || std::is_same_v<T, Eq>
                        || std::is_same_v<T, NEq>
                        || std::is_same_v<T, Less>
                        || std::is_same_v<T, ConcatLists>
                        || std::is_same_v<T, Update>) {
            forced.insert(x.lhs);
            forced.insert(x.rhs);
            return true;
        }

        // --- Single-operand strict bindings ---
        else if constexpr (std::is_same_v<T, Force>) {
            forced.insert(x.thunk);
            return true;
        }
        else if constexpr (std::is_same_v<T, Not>) {
            forced.insert(x.operand);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSelect>
                        || std::is_same_v<T, HasAttr>) {
            forced.insert(x.attrs);
            return true;
        }
        else if constexpr (std::is_same_v<T, AttrSelectDyn>
                        || std::is_same_v<T, HasAttrDyn>) {
            forced.insert(x.attrs);
            forced.insert(x.nameVar);
            return true;
        }
        else if constexpr (std::is_same_v<T, RecBindingSlotRef>) {
            // Forces the rec attrset (the slot is published lazily,
            // but accessing it via this opcode forces the source).
            forced.insert(x.attrs);
            return true;
        }
        else if constexpr (std::is_same_v<T, App>) {
            // App forces the function (to dispatch on it).  The
            // arg is NOT forced — Nix is lazy in arguments.
            forced.insert(x.fun);
            return true;
        }
        else if constexpr (std::is_same_v<T, ConcatStrings>) {
            // Each interpolation part is forced (and coerced to
            // string).  Arithmetic `a + b` parses to ExprCall(__add)
            // — a PrimOpCall, not a ConcatStrings node — so this
            // branch is the string-interp form.
            for (auto v : x.parts) forced.insert(v);
            return true;
        }
        else if constexpr (std::is_same_v<T, PrimOpCall>) {
            // Strict positions per primop.lazyArgs bitmask
            // (mirrors lower.cc's `if (po->lazyArgs & (1u << i))`
            // decision).
            if (x.primop) {
                const uint32_t lazyMask = x.primop->lazyArgs;
                for (size_t i = 0; i < x.args.size(); ++i) {
                    if ((lazyMask & (1u << i)) == 0)
                        forced.insert(x.args[i]);
                }
            }
            return true;
        }

        // --- Non-forcing expressions: keep walking but contribute
        // no forced operands.  Lambda/MkThunk/AttrSet/ListExpr/
        // LetRec/LitFunction/LitPrimOp/LitBuiltins/literals/VarRef/
        // WithLookup — none of them forces operands during their
        // own construction step.
        else {
            (void)x;
            return true;
        }
    }, e);
}

} // namespace

void computeFunctionStrictness(Module & m)
{
    static const bool s_dbg =
        std::getenv("NIX_V3_DBG_STRICTNESS") != nullptr;
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_FUNC_STRICTNESS") != nullptr;
    if (s_disabled) return;

    size_t fnsTotal     = 0;
    size_t fnsWithArgs  = 0;
    size_t formalsTotal = 0;
    size_t formalsStrict = 0;

    for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
        Function & f = m.functions[fid];

        // v2 scope: single-arg lambdas only (`x: body`).  Formals-
        // style lambdas (`{a, b}: body`) bind each formal to a fresh
        // VarId inside the body's prologue rather than via paramVar;
        // ir::Formal doesn't carry that VarId so we can't trace it
        // back to a call-ABI position.  Skip until v3 wires the
        // formal-VarIds into ir::Function.
        std::vector<VarId> formals;
        if (f.paramVar != kInvalid && !f.hasFormals)
            formals.push_back(f.paramVar);

        f.strictArgs.assign(formals.size(), false);
        ++fnsTotal;
        formalsTotal += formals.size();
        if (formals.empty()) continue;

        // Forward walk of the entry block's bindings.  Stop at the
        // first branching expression (If/With/Assert/And/Or/Impl).
        std::unordered_set<VarId> forced;
        if (f.entryBlock == kInvalidBlock
            || f.entryBlock >= (BlockId)m.blocks.size()) continue;
        const Block & b = m.blocks[f.entryBlock];
        for (const auto & bind : b.bindings) {
            if (!collectForced(bind.expr, forced)) break;
        }
        // TermReturn: the returned value is NOT forced by the body
        // itself — the caller forces it.  Skip.

        size_t strictForFn = 0;
        for (size_t i = 0; i < formals.size(); ++i) {
            if (forced.count(formals[i])) {
                f.strictArgs[i] = true;
                ++strictForFn;
            }
        }
        if (strictForFn > 0) ++fnsWithArgs;
        formalsStrict += strictForFn;
    }

    if (s_dbg) {
        std::fprintf(stderr,
            "v3 stage4 strictness: functions=%zu with-strict-args=%zu "
            "formals=%zu strict=%zu (%.1f%%)\n",
            fnsTotal, fnsWithArgs, formalsTotal, formalsStrict,
            formalsTotal > 0
                ? (double(formalsStrict) * 100.0 / double(formalsTotal))
                : 0.0);
    }
}

} // namespace nix::v3::ir
