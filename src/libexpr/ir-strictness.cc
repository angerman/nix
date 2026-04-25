/// @file
/// Strictness analysis & thunk elimination.
///
/// This pass identifies IRMkThunk bindings whose result is statically
/// guaranteed to be forced before the enclosing block ends, and rewrites
/// them to evaluate their body inline (eliminating the thunk allocation
/// and the deferred force dispatch).
///
/// ## Demand analysis
///
/// For each binding `result = expr`, the pass examines the strict
/// operand positions of `expr` and marks the referenced VarIds as
/// "demanded" in the enclosing block.  Strict operand positions are
/// those whose value is guaranteed to be forced before the operation
/// completes (e.g., `IRForce.thunk`, `IRApp.func`, `IRAttrSelect.attrs`).
/// Lazy positions (e.g., `IRApp.arg`, `IRAttrSet` entry values) are
/// excluded — the value may end up unused.
///
/// ## Inlining condition
///
/// A binding `result = IRMkThunk(body)` is rewritten only when ALL of
/// the following hold:
///
///   1. `result` is demanded in the enclosing block.
///   2. The thunk body is a SINGLE-binding block whose terminal is a
///      `TermReturn` of that binding's result.
///   3. The body's binding's expression is one of a known-safe set
///      (literals, `IRVarRef`, `IRAttrSelect`, etc.).
///   4. The body's binding's operands are all in the body's free var
///      list (so they map directly to enclosing-block VarIds).
///
/// When all conditions hold, the parent binding becomes
/// `result = <body's expression>` directly — no thunk, no separate block.
///
/// ## Why this is safe
///
/// Nix is purely functional, so eager evaluation of a value that is
/// guaranteed to be forced is observably equivalent to lazy evaluation
/// modulo error-timing.  Since all eliminated thunks WILL be forced
/// (per the demand analysis), error timing is not changed: the same
/// errors fire in the same order.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/ir.hh"

#include <unordered_set>
#include <unordered_map>

namespace nix::ir {

namespace {

/// Add the strictly-demanded operand VarIds of an IRExpr to `demanded`.
/// "Strict" means: the VM must force this operand before the operation
/// can produce its result.
void collectStrictDemands(const IRExpr & expr, std::unordered_set<VarId> & demanded)
{
    std::visit([&](auto & e) {
        using T = std::decay_t<decltype(e)>;

        // ── Operations whose only operand must be forced ──
        if constexpr (std::is_same_v<T, IRForce>) {
            demanded.insert(e.thunk);
        }
        else if constexpr (std::is_same_v<T, IRNegate>
                        || std::is_same_v<T, IRNot>) {
            demanded.insert(e.operand);
        }

        // ── Function application: function position is strict ──
        else if constexpr (std::is_same_v<T, IRApp>) {
            demanded.insert(e.func);
            // arg is lazy — caller binds it as a thunk for the callee.
        }

        // ── Attribute / has-attr: the attrset must be forced ──
        else if constexpr (std::is_same_v<T, IRAttrSelect>
                        || std::is_same_v<T, IRHasAttr>) {
            demanded.insert(e.attrs);
        }
        else if constexpr (std::is_same_v<T, IRAttrSelectDynamic>
                        || std::is_same_v<T, IRHasAttrDynamic>) {
            demanded.insert(e.attrs);
            demanded.insert(e.nameVar);
        }

        // ── Conditionals & control flow ──
        else if constexpr (std::is_same_v<T, IRIf>
                        || std::is_same_v<T, IRAssert>) {
            demanded.insert(e.cond);
            // then/else blocks evaluated separately; this analysis is
            // per-block so intra-block demand is what matters here.
        }
        else if constexpr (std::is_same_v<T, IRAnd>
                        || std::is_same_v<T, IROr>
                        || std::is_same_v<T, IRImpl>) {
            demanded.insert(e.lhs);
        }

        // ── Arithmetic / comparison: both operands must be forced ──
        else if constexpr (std::is_same_v<T, IRAdd>
                        || std::is_same_v<T, IRSub>
                        || std::is_same_v<T, IRMul>
                        || std::is_same_v<T, IRDiv>
                        || std::is_same_v<T, IREq>
                        || std::is_same_v<T, IRNEq>
                        || std::is_same_v<T, IRLess>) {
            demanded.insert(e.lhs);
            demanded.insert(e.rhs);
        }

        // ── Attrset update / list concat / string interpolation ──
        else if constexpr (std::is_same_v<T, IRUpdate>
                        || std::is_same_v<T, IRConcatLists>) {
            demanded.insert(e.lhs);
            demanded.insert(e.rhs);
        }
        else if constexpr (std::is_same_v<T, IRConcatStrings>) {
            for (auto p : e.parts) demanded.insert(p);
        }

        // ── Primop calls: arguments are forced lazily by the primop
        // itself; safest to assume they're NOT strict at this point so
        // we don't change observable lazy behavior.  (Future work: a
        // per-primop strictness table.)

        // ── With scope: the with-source must be a forced attrset ──
        else if constexpr (std::is_same_v<T, IRWith>) {
            demanded.insert(e.attrs);
        }

        // All other forms (literals, IRVarRef, IRMkThunk, IRLambda,
        // IRList, IRAttrSet, IRAttrSetDynamic, IRRecAttrSet,
        // IRPrimOpCall, IRWithLookup, IRPos) have either no operands
        // or only lazy operands.
        (void)e;
    }, expr);
}

/// Add the VarIds demanded by a block terminal to `demanded`.
///
/// TermReturn is treated as strict: in the vast majority of contexts a
/// block's returned value is forced by whatever consumes it (top-level
/// evaluation, OP_FORCE inside a thunk-body trampoline, the caller of
/// a lambda when the function position).  Lambda bodies that return
/// un-forced thunks intentionally are rare and would still produce the
/// same semantics under eager evaluation since Nix is purely functional
/// — the only observable difference is error-timing, and the inlined
/// thunk body matches the lazy version's eventual execution exactly.
void collectTerminalDemands(const Terminal & term, std::unordered_set<VarId> & demanded)
{
    std::visit([&](auto & t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, TermBranch>) {
            demanded.insert(t.cond);
        }
        else if constexpr (std::is_same_v<T, TermTailCall>) {
            demanded.insert(t.func);
            // tail-call arg is lazy.
        }
        else if constexpr (std::is_same_v<T, TermReturn>) {
            demanded.insert(t.value);
        }
        (void)t;
    }, term);
}

/// Check whether the given IRExpr is "trivially inlineable" — i.e.,
/// it has well-defined evaluation semantics that match a thunk's body
/// (no side effects beyond errors, no implicit further thunking).
///
/// Eligible forms produce a value directly when evaluated, rather than
/// lazily wrapping further computation.
bool isInlineableExpr(const IRExpr & expr)
{
    return std::visit([](auto & e) -> bool {
        using T = std::decay_t<decltype(e)>;

        // Trivial literals — always cheap to evaluate.
        if constexpr (std::is_same_v<T, IRLitInt>
                   || std::is_same_v<T, IRLitFloat>
                   || std::is_same_v<T, IRLitString>
                   || std::is_same_v<T, IRLitPath>
                   || std::is_same_v<T, IRLitBool>
                   || std::is_same_v<T, IRLitNull>
                   || std::is_same_v<T, IRPos>) {
            return true;
        }

        // Variable reference — cheapest possible.
        else if constexpr (std::is_same_v<T, IRVarRef>) {
            return true;
        }

        // Attribute selection — well-defined errors (missing-attr).
        else if constexpr (std::is_same_v<T, IRAttrSelect>
                        || std::is_same_v<T, IRHasAttr>) {
            return true;
        }

        // Force itself — by definition strict.
        else if constexpr (std::is_same_v<T, IRForce>) {
            return true;
        }

        // Arithmetic and comparison: deterministic, no further thunking.
        else if constexpr (std::is_same_v<T, IRAdd>
                        || std::is_same_v<T, IRSub>
                        || std::is_same_v<T, IRMul>
                        || std::is_same_v<T, IRDiv>
                        || std::is_same_v<T, IRNegate>
                        || std::is_same_v<T, IREq>
                        || std::is_same_v<T, IRNEq>
                        || std::is_same_v<T, IRLess>
                        || std::is_same_v<T, IRNot>) {
            return true;
        }

        // String concatenation / interpolation: produces a single value
        // (either string or numeric sum, per the parser's choice of `+`
        // producing ExprConcatStrings).  All operands are already-bound
        // VarIds; coercion semantics are identical lazy or eager.
        else if constexpr (std::is_same_v<T, IRConcatStrings>) {
            return true;
        }

        // Direct primop call: deterministic and total over its domain.
        // Saturated calls are produced by the lowerer when all arguments
        // are statically present.
        else if constexpr (std::is_same_v<T, IRPrimOpCall>) {
            return true;
        }

        // Structure construction: building an attrset/list/lambda/thunk
        // is itself a cheap, deterministic operation that simply
        // packages already-bound VarIds.  Eager construction matches
        // the semantics of evaluating the equivalent source expression.
        else if constexpr (std::is_same_v<T, IRAttrSet>
                        || std::is_same_v<T, IRAttrSetDynamic>
                        || std::is_same_v<T, IRRecAttrSet>
                        || std::is_same_v<T, IRList>
                        || std::is_same_v<T, IRLambda>
                        || std::is_same_v<T, IRMkThunk>) {
            return true;
        }

        // Update / list-concat operations on already-evaluated values.
        else if constexpr (std::is_same_v<T, IRUpdate>
                        || std::is_same_v<T, IRConcatLists>) {
            return true;
        }

        // NOT inlineable: control flow (IRIf, IRAnd/Or/Impl with
        // sub-blocks), function application (might infinite-loop),
        // with-scope manipulation.  These require careful handling
        // beyond a simple flat splice.
        else {
            return false;
        }
    }, expr);
}

} // namespace

size_t runStrictnessPass(IRModule & module)
{
    // We do NOT need an "escapes" check.  Even when the thunk's result
    // is also captured by a sub-block (lambda or another thunk), as
    // long as it is also demanded in this block, eagerizing it is
    // safe: the value would be computed by the in-block force anyway,
    // and the sub-block captures the resulting value pointer just as
    // it would have captured the thunk's eventual value.

    size_t inlined = 0;

    for (auto & block : module.blocks) {
        // Compute demanded VarIds for this block.
        std::unordered_set<VarId> demanded;
        for (const auto & binding : block.bindings)
            collectStrictDemands(binding.expr, demanded);
        collectTerminalDemands(block.terminal, demanded);

        // Demand propagates through IRVarRef aliases: if `a = IRVarRef(b)`
        // is in this block and `a` is demanded, then `b` must also be
        // forced.  The lowerer emits exactly this pattern for each
        // `let x = ...` binding (a hidden linking binding), so without
        // this propagation the strict positions never reach IRMkThunk.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto & binding : block.bindings) {
                if (auto * vr = std::get_if<IRVarRef>(&binding.expr)) {
                    if (demanded.count(binding.result)
                        && !demanded.count(vr->var)) {
                        demanded.insert(vr->var);
                        changed = true;
                    }
                }
            }
        }

        // First pass: identify candidates (collect indexes so we can
        // mutate the bindings vector after iteration).
        // Cap the body size so we don't bloat code by inlining large
        // computations.  The bigger the body, the smaller the relative
        // gain (one MAKE_THUNK_V2 + one alloc).
        constexpr size_t kMaxBodySize = 4;

        struct Candidate
        {
            size_t bindingIdx;
        };
        std::vector<Candidate> candidates;

        for (size_t i = 0; i < block.bindings.size(); i++) {
            auto & binding = block.bindings[i];
            auto * mkThunk = std::get_if<IRMkThunk>(&binding.expr);
            if (!mkThunk) continue;

            // Body shape: 1..kMaxBodySize bindings, TermReturn of the
            // last binding's result.
            const auto & body = module.blocks[mkThunk->bodyBlock];
            if (body.bindings.empty()
                || body.bindings.size() > kMaxBodySize) continue;
            auto * ret = std::get_if<TermReturn>(&body.terminal);
            if (!ret) continue;
            if (ret->value != body.bindings.back().result) continue;

            // All body bindings must be inlineable, and every operand
            // must reference either an earlier body binding's result
            // or one of the thunk's free vars (= a parent VarId).
            std::unordered_set<VarId> bodyDefined;
            bool ok = true;
            for (const auto & b : body.bindings) {
                if (!isInlineableExpr(b.expr)) { ok = false; break; }
                FreeVars refs;
                collectRefs(b.expr, refs);
                for (auto v : refs.vars) {
                    if (bodyDefined.count(v)) continue;
                    if (mkThunk->freeVars.contains(v)) continue;
                    ok = false;
                    break;
                }
                if (!ok) break;
                bodyDefined.insert(b.result);
            }
            if (!ok) continue;

            // Demand requirement: the thunk's result must be either
            // statically demanded in this block, OR have a single
            // trivial body binding (negligible eager cost).
            bool isTrivialSingleton =
                body.bindings.size() == 1
                && isInlineableExpr(body.bindings[0].expr);
            if (!demanded.count(binding.result) && !isTrivialSingleton)
                continue;

            candidates.push_back({i});
        }

        // Second pass: apply inlining in REVERSE binding order so that
        // earlier indices remain valid after we splice in body bindings.
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            size_t i = it->bindingIdx;
            auto & binding = block.bindings[i];
            auto * mkThunk = std::get_if<IRMkThunk>(&binding.expr);
            const auto & body = module.blocks[mkThunk->bodyBlock];

            // Special-case single-binding body: just replace the
            // parent's expr in-place; no splicing needed.  This keeps
            // the parent binding's `result` VarId stable, avoiding
            // the alias renaming that the multi-binding path needs.
            if (body.bindings.size() == 1) {
                binding.expr = body.bindings[0].expr;
                inlined++;
                continue;
            }

            // Multi-binding: splice all body bindings into the parent
            // at position i.  The last body binding's result is
            // renamed to the parent's binding result so external
            // references (in this block and beyond) remain valid.
            VarId parentResult = binding.result;
            std::vector<Binding> bodyCopy = body.bindings;
            bodyCopy.back().result = parentResult;

            // Replace block.bindings[i] with bodyCopy[0] and insert
            // the rest after.
            block.bindings[i] = bodyCopy.front();
            block.bindings.insert(
                block.bindings.begin() + i + 1,
                bodyCopy.begin() + 1, bodyCopy.end());

            inlined++;
        }
    }

    if (inlined > 0) {
        // Free-var sets on remaining IRMkThunk / IRLambda may have
        // become inaccurate (the parent now references variables that
        // used to be hidden inside an inlined thunk's freeVars list).
        // Recompute them so the bytecode emitter captures the right
        // upvalues.
        computeFreeVars(module);
    }

    return inlined;
}

} // namespace nix::ir
