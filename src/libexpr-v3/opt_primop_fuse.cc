/// @file
/// IR optimisation pass: fuse App-chains over LitPrimOp into PrimOpCall.
///
/// When the lowerer can statically recognise `builtins.foo arg1 arg2`
/// as a primop call it emits PrimOpCall directly (lowerCall in
/// lower.cc).  But when the primop reference goes through a let /
/// inherit-from indirection -- the dominant nixpkgs pattern, e.g.
///
///     let inherit (builtins) map; in map f xs
///
/// -- lowerCall sees a plain ExprVar, can't pattern-match it as a
/// primop, and emits an App-chain.  After lowering this looks like:
///
///     v_map  = LitPrimOp{primMap}
///     v_app1 = App{v_map, f}      -- partial: 1 of 2 args
///     v_app2 = App{v_app1, xs}    -- saturated: 2 of 2 args
///
/// At runtime each App allocates a PrimOpApp wrapper plus dispatches
/// through OP_CALL.  The peephole fuses saturated chains into a
/// single PrimOpCall (the same node lowerCall would have emitted
/// for a direct `builtins.map f xs` call):
///
///     v_app2 = PrimOpCall{primMap, [f, xs]}
///     -- v_app1 unused, DCE sweeps it
///
/// Algorithm (single forward pass per Module):
///   1. Build a global map (VarId -> const PrimOp *) from every
///      LitPrimOp binding.  VarIds are unique across the Module so
///      one map suffices.
///   2. Build a global map (VarId -> partial-app accumulator) where
///      each entry is (PrimOp *, vector<VarId> argsSoFar).  Seeded
///      by App{primOpVar, x} and extended by App{partialAppVar, x}
///      until argsSoFar.size() == primop arity, at which point the
///      App binding is rewritten to PrimOpCall.
///   3. Skip primops with non-zero `lazyArgs` -- those have
///      per-arg laziness rules that the lowerer enforces in the
///      direct-call path (thunkifying lazy args).  Fusing a chain
///      whose intermediate Apps may have already forced/forwarded
///      a lazy arg differently could change throw-position
///      semantics.  Conservative.
///
/// Runs after constantFold + commonSubexprElim + inlineTrivialBindings
/// and before deadBindingElim, so the alias-resolved IR has stable
/// VarRef shape and DCE can sweep the now-orphan partial-App
/// bindings.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/primop.hh"

#include <unordered_map>

namespace nix::v3::ir {

namespace {

struct PartialApp {
    const v3::PrimOp *  primop;
    std::vector<VarId>  args;
};

} // namespace

size_t fusePrimOpApps(Module & m)
{
    // Pass 1: collect (VarId -> PrimOp *) for every LitPrimOp binding.
    std::unordered_map<VarId, const v3::PrimOp *> primopOf;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid)
        for (auto & b : m.blocks[bid].bindings)
            if (auto * lp = std::get_if<LitPrimOp>(&b.expr))
                if (lp->primop && lp->primop->lazyArgs == 0)
                    primopOf[b.var] = lp->primop;

    if (primopOf.empty()) return 0;

    // Pass 2: walk every block forward; track partial apps; fuse on
    // saturation.  We re-walk the binding vector directly so the
    // rewrite of App -> PrimOpCall is in place.
    std::unordered_map<VarId, PartialApp> partial;
    size_t fused = 0;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        for (auto & b : blk.bindings) {
            auto * app = std::get_if<App>(&b.expr);
            if (!app) continue;

            // Case A: App{primOpVar, x}.  Seed a partial.
            if (auto pit = primopOf.find(app->fun); pit != primopOf.end()) {
                const v3::PrimOp * po = pit->second;
                if (po->arity == 1) {
                    // Saturated immediately.
                    b.expr = PrimOpCall{po, {app->arg}};
                    ++fused;
                } else if (po->arity > 1) {
                    PartialApp p;
                    p.primop = po;
                    p.args   = {app->arg};
                    partial.emplace(b.var, std::move(p));
                }
                continue;
            }

            // Case B: App{partialVar, x}.  Extend the chain; on
            // saturation rewrite to PrimOpCall.  On overshoot
            // (partial.args.size() == arity already, App takes a
            // saturated PrimOp result) DON'T fuse -- the result is a
            // value and applying it further means the primop returned
            // a function, which we can't statically saturate.
            if (auto qit = partial.find(app->fun); qit != partial.end()) {
                const PartialApp & prev = qit->second;
                if (prev.args.size() < prev.primop->arity) {
                    PartialApp p = prev; // copy + extend
                    p.args.push_back(app->arg);
                    if (p.args.size() == p.primop->arity) {
                        b.expr = PrimOpCall{p.primop, std::move(p.args)};
                        ++fused;
                    } else {
                        partial.emplace(b.var, std::move(p));
                    }
                }
                continue;
            }
        }
    }
    return fused;
}

} // namespace nix::v3::ir
