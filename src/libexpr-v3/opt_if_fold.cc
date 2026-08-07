/// @file
/// IR Phase G — pure if-then-else folding (2026-05-18).
///
/// Recognises `If(cond, thenBlock, elseBlock)` where `cond` resolves
/// statically (via VarRef-chain chase) to `LitBool(true)` or
/// `LitBool(false)`, and rewrites the binding to inline the chosen
/// block's bindings + a VarRef to its terminal value.
///
/// Hypothesis killed (Rule 0): "branch elimination only happens at
/// runtime."  After this pass, `if true then a else b` lowers with
/// no `If` node and no `OP_BRANCH_FALSE` in compiled bytecode — just
/// the bindings of the chosen branch and a VarRef to its return.
///
/// Safety:
///   - The chosen block's bindings have module-globally unique
///     VarIds (the lowerer never re-numbers), so inlining into the
///     outer block introduces no collision.
///   - The discarded branch's bindings are dropped.  Per Nix
///     semantics, only one branch evaluates at runtime, so dropping
///     the other is sound — any `throw` / `abort` / side effect in
///     the discarded branch would not have run anyway.
///   - We refuse the rewrite if the chosen block carries anything
///     other than a TermReturn (defensive — current lowerer always
///     emits TermReturn for If branches, but a future change might
///     introduce other terminals).
///
/// Dependency: Phase B (constantFold) — the cond binding must
/// already resolve to a literal.  Runs AFTER primOpFold +
/// constantFold + inlineTrivialBindings so VarRef chains and
/// literal-folding have settled.
///
/// Unconditional (the NIX_V3_NO_IF_FOLD A/B opt-out was retired once it
/// shipped byte-identical).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_util.hh"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace nix::v3::ir {

namespace {

// chaseInBlock() is shared across opt_*.cc passes — see v3/ir_util.hh.

/// Try to resolve `v` to a literal boolean via same-block defs.
/// Returns std::nullopt when cond is not a known literal.
std::optional<bool> resolveBool(VarId v,
                                const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = chaseInBlock(v, defs);
    if (!e) return std::nullopt;
    if (const auto * b = std::get_if<LitBool>(e))
        return b->value;
    return std::nullopt;
}

} // namespace

size_t ifThenFold(Module & m)
{
    static const bool s_dbg =
        std::getenv("V3_DBG_IF_FOLD") != nullptr;

    size_t folded = 0;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];

        // Build a defs map fresh per block — this pass may rewrite
        // bindings within the block, but VarRef chases only look at
        // bindings DEFINED in this block (the lowerer's A-normal-form
        // discipline means every reference resolves locally).
        std::unordered_map<VarId, const Expr *> defs;
        defs.reserve(blk.bindings.size());
        for (const auto & bd : blk.bindings)
            defs.emplace(bd.var, &bd.expr);

        // First pass: scan for foldable Ifs without mutating.
        // Mutating in a single pass via `std::move(bd)` leaves the
        // original `blk.bindings` in moved-from state even when we
        // decide not to fold this block — which silently corrupts
        // downstream passes that iterate the same vector.  The
        // two-pass shape (scan, then commit) keeps `blk.bindings`
        // intact when no rewrite is needed.
        bool anyFold = false;
        for (const auto & bd : blk.bindings) {
            if (const If * ifE = std::get_if<If>(&bd.expr)) {
                if (auto cv = resolveBool(ifE->cond, defs)) {
                    BlockId chosen = *cv ? ifE->thenBlock : ifE->elseBlock;
                    if (chosen != kInvalidBlock && chosen < m.blocks.size()) {
                        const auto * term =
                            std::get_if<TermReturn>(&m.blocks[chosen].terminal);
                        if (term && term->value != kInvalid) {
                            anyFold = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!anyFold) continue;

        // Second pass: commit the rewrite.
        std::vector<Binding> out;
        out.reserve(blk.bindings.size());
        for (auto & bd : blk.bindings) {
            const If * ifE = std::get_if<If>(&bd.expr);
            if (!ifE) {
                out.push_back(std::move(bd));
                continue;
            }
            auto cv = resolveBool(ifE->cond, defs);
            if (!cv) {
                out.push_back(std::move(bd));
                continue;
            }
            BlockId chosen = *cv ? ifE->thenBlock : ifE->elseBlock;
            if (chosen == kInvalidBlock || chosen >= m.blocks.size()) {
                out.push_back(std::move(bd));
                continue;
            }
            const Block & chosenBlock = m.blocks[chosen];
            const auto * term = std::get_if<TermReturn>(&chosenBlock.terminal);
            if (!term || term->value == kInvalid) {
                out.push_back(std::move(bd));
                continue;
            }
            // Inline the chosen block's bindings + rewrite this
            // binding's RHS to a VarRef.  Bindings are deep-copied
            // from chosenBlock (the source block stays intact so the
            // module remains structurally valid — unreferenced blocks
            // are orphans, not removed).
            for (const auto & sub : chosenBlock.bindings)
                out.push_back(sub);
            VarId chosenTermVar = term->value;
            bd.expr = VarRef{ chosenTermVar };
            out.push_back(std::move(bd));
            ++folded;
            if (s_dbg) {
                std::fprintf(stderr,
                    "v3 ifThenFold: B%u: cond=%s → inlined B%u "
                    "(%zu sub-bindings) + VarRef v%u\n",
                    (unsigned)bid, *cv ? "true" : "false",
                    (unsigned)chosen,
                    chosenBlock.bindings.size(),
                    (unsigned)chosenTermVar);
            }
        }
        blk.bindings = std::move(out);
    }

    return folded;
}

} // namespace nix::v3::ir
