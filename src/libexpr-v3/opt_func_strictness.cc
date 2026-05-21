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
#include <unordered_map>
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

    // #740 Stage 4 v3 (2026-05-21) — extend to formals-style lambdas.
    // For `{a, b}: body`, formal references in the body lower to
    // `RecBindingSlotRef{formalsRecVar, name}` bindings (default
    // path; see lower.cc inlineRecSlot).  Build a map var→formalIdx
    // for each such binding so the existing forced-set analysis can
    // determine whether formal i is strict-used.

    for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
        Function & f = m.functions[fid];

        // Build the strictArgs vector layout:
        //   [0] paramVar (the @arg attrset alias, if present)
        //   [1..N] formals[0..N-1] (in the same order as `f.formals`)
        // For single-arg lambdas (no formals): just [paramVar].
        // For formals-style without @arg: [formals[0..N-1]].
        const bool hasParam   = (f.paramVar != kInvalid);
        const bool hasFormals = f.hasFormals && !f.formals.empty();
        std::vector<VarId> argVars;
        size_t paramSlot = (size_t)-1;
        size_t formalsStart = 0;
        if (hasParam) {
            paramSlot = argVars.size();
            argVars.push_back(f.paramVar);
        }
        if (hasFormals) {
            // We use kInvalid as a placeholder here; the actual
            // VarId discovery happens by walking the body for
            // RecBindingSlotRef bindings.  The MAP we build is
            // (varId → formalIdx).
            formalsStart = argVars.size();
            for (size_t i = 0; i < f.formals.size(); ++i) {
                argVars.push_back(kInvalid);
            }
        }

        f.strictArgs.assign(argVars.size(), false);
        ++fnsTotal;
        formalsTotal += argVars.size();
        if (argVars.empty()) continue;
        if (f.entryBlock == kInvalidBlock
            || f.entryBlock >= (BlockId)m.blocks.size()) continue;
        const Block & b = m.blocks[f.entryBlock];

        // First pass: discover formal-reference VarIds.
        // formalVarToIdx[var] = index in f.formals[] (0-based).
        std::unordered_map<VarId, size_t> formalVarToIdx;
        if (hasFormals && f.formalsRecVar != kInvalid) {
            // Build (name → formals[] index).
            std::unordered_map<SymbolId, size_t> nameToIdx;
            for (size_t i = 0; i < f.formals.size(); ++i) {
                nameToIdx.emplace(f.formals[i].name, i);
            }
            for (const auto & bind : b.bindings) {
                if (auto * rb = std::get_if<RecBindingSlotRef>(&bind.expr)) {
                    if (rb->attrs == f.formalsRecVar) {
                        auto it = nameToIdx.find(rb->name);
                        if (it != nameToIdx.end()) {
                            formalVarToIdx.emplace(bind.var, it->second);
                        }
                    }
                }
                // Also chase VarRef aliases — the body might
                // re-alias via inlineTrivialBindings or similar
                // optimisations.  Handle one alias hop here (rare
                // in practice but cheap).
                else if (auto * vr = std::get_if<VarRef>(&bind.expr)) {
                    auto it = formalVarToIdx.find(vr->var);
                    if (it != formalVarToIdx.end())
                        formalVarToIdx.emplace(bind.var, it->second);
                }
            }
        }

        // Second pass: existing forced-set analysis.
        std::unordered_set<VarId> forced;
        for (const auto & bind : b.bindings) {
            if (!collectForced(bind.expr, forced)) break;
        }
        // TermReturn: the returned value is NOT forced by the body
        // itself — the caller forces it.  Skip.

        size_t strictForFn = 0;
        // paramVar slot.
        if (hasParam) {
            if (forced.count(f.paramVar)) {
                f.strictArgs[paramSlot] = true;
                ++strictForFn;
            }
        }
        // formals[i] slots.
        if (hasFormals) {
            for (const auto & [varId, formalIdx] : formalVarToIdx) {
                if (forced.count(varId)) {
                    const size_t slot = formalsStart + formalIdx;
                    if (slot < f.strictArgs.size()
                        && !f.strictArgs[slot]) {
                        f.strictArgs[slot] = true;
                        ++strictForFn;
                    }
                }
            }
        }
        // #743 v4.1 — for formals-style lambdas, if ANY formal is
        // strict, paramVar is implicitly strict.  Reason: every formal
        // access (RecBindingSlotRef{formalsRec, name}) forces the
        // formalsRec entry's thunk, which in turn calls AttrSelect on
        // paramVar.  An unconditional formal access therefore forces
        // paramVar.  This lets caller-side strictness elide the
        // outer MkThunk wrap around the App's arg attrset.
        if (hasFormals && hasParam && !f.strictArgs.empty()
            && !f.strictArgs[paramSlot])
        {
            bool anyFormalStrict = false;
            for (size_t i = 0; i < f.formals.size(); ++i) {
                const size_t slot = formalsStart + i;
                if (slot < f.strictArgs.size() && f.strictArgs[slot]) {
                    anyFormalStrict = true;
                    break;
                }
            }
            if (anyFormalStrict) {
                f.strictArgs[paramSlot] = true;
                ++strictForFn;
            }
        }
        if (strictForFn > 0) ++fnsWithArgs;
        formalsStrict += strictForFn;
    }

    if (s_dbg) {
        // v4.1 verbose: dump per-Function strictArgs when any are set.
        static const bool sv_dbg_verbose =
            std::getenv("NIX_V3_DBG_STRICTNESS_VERBOSE") != nullptr;
        if (sv_dbg_verbose) {
            for (FuncId fid = 0; fid < (FuncId)m.functions.size(); ++fid) {
                const Function & f = m.functions[fid];
                if (f.strictArgs.empty()) continue;
                bool any = false;
                for (auto b : f.strictArgs) if (b) { any = true; break; }
                if (!any) continue;
                std::fprintf(stderr,
                    "  fid=%u name=%s hasFormals=%d strictArgs=[",
                    (unsigned)fid, f.name.empty() ? "?" : f.name.c_str(),
                    (int)f.hasFormals);
                for (size_t i = 0; i < f.strictArgs.size(); ++i)
                    std::fprintf(stderr, "%s%d",
                        i ? "," : "", (int)bool(f.strictArgs[i]));
                std::fprintf(stderr, "]\n");
            }
        }
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
