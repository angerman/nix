/// @file
/// IR optimisation pass: block-local common subexpression elimination.
///
/// Within a single Block, identical-shape Expr nodes that compute the
/// same value are merged: the second occurrence becomes a `VarRef` to
/// the first, and the alias-collapse pass (run later in the pipeline)
/// will fold it away on the next round.
///
/// We restrict CSE to a strict whitelist of *deterministic* and
/// *order-insensitive* operations:
///
///   - Arithmetic: Add, Sub, Mul, Div                 (same inputs → same result)
///   - Comparison: Eq, NEq, Less                       (same inputs → same bool)
///   - Boolean:    Not                                  (same input → flipped bool)
///   - Static attr test: HasAttr                       (same Bindings + name → same bool)
///
/// Excluded (must NOT CSE):
///   - Force / App / PrimOpCall    — observable forces, side effects.
///   - AttrSelect / AttrSelectDyn  — throw on missing attr; eliminating
///                                   the second occurrence subtly changes
///                                   the line where the missing-attr
///                                   error fires.
///   - HasAttrDyn                  — name comes from a forced thunk; if
///                                   that name builds a string context
///                                   the second occurrence may matter.
///   - WithLookup                  — depends on the with-stack at the
///                                   exact program point.
///   - And / Or / Impl             — short-circuit; the rhsBlock body
///                                   may have side effects whose
///                                   non-execution distinguishes copies.
///   - If / With / Assert          — control flow w/ sub-blocks.
///   - LetRec                      — allocates Bindings + entry thunks;
///                                   each occurrence is intended distinct.
///   - MkThunk / Lambda            — allocate fresh; merging changes the
///                                   memoisation identity for forces.
///                                   (Could be safe with care; deferred.)
///   - AttrSet / AttrSetDyn / ListExpr / ConcatLists / ConcatStrings /
///     Update                      — pure-allocation expressions whose
///                                   result identity affects nothing
///                                   (so CSE is correctness-safe), but
///                                   conservative for the first pass.
///                                   Easy follow-up.
///   - Lit*                        — DCE/inline already handle these;
///                                   CSE'ing literals is a wash at emit
///                                   time.
///
/// Block-locality is a hard constraint:
///   - The same Expr appearing in two different blocks may have
///     different operand VarIds in scope.  We don't try to detect
///     identical operands across block boundaries.
///   - Sub-blocks (If/With/Assert/And/Or/Impl bodies) are NOT walked
///     for cross-block matches.  Each block CSEs independently.
///
/// Algorithm: walk each Block top-to-bottom; key each whitelisted Expr
/// by `(variant index, operand VarIds)`; when a duplicate key is hit,
/// rewrite the binding's RHS to `VarRef{firstSeenVar}`.  The next pipeline
/// run of inlineTrivialBindings collapses the alias.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <unordered_map>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// CSE key: variant index + ordered operand VarIds (or symbol for HasAttr).
// ---------------------------------------------------------------------------
//
// We use a vector<uint64_t> so the same key shape works for binary
// arithmetic, unary Not, and HasAttr (which mixes a VarId and a
// SymbolId).  The first slot is the variant index; subsequent slots
// are the operand keys.  Stored as uint64_t to leave room for future
// variants whose operands include 64-bit literals.

using Key = std::vector<uint64_t>;

struct KeyHash {
    size_t operator()(const Key & k) const noexcept {
        // FNV-1a-ish 64-bit mix; cheap and adequate for in-pass dedup.
        uint64_t h = 0xcbf29ce484222325ull;
        for (auto v : k) { h ^= v; h *= 0x100000001b3ull; }
        return (size_t)h;
    }
};

/// Build a CSE key for the whitelisted variants.  Returns an empty
/// optional (via Key.size()==0) when the Expr is not eligible.
Key buildKey(const Expr & e)
{
    return std::visit([&](const auto & x) -> Key {
        using T = std::decay_t<decltype(x)>;
        const uint64_t tag = (uint64_t)e.index();
        if constexpr (std::is_same_v<T, Add> ||
                      std::is_same_v<T, Sub> ||
                      std::is_same_v<T, Mul> ||
                      std::is_same_v<T, Div> ||
                      std::is_same_v<T, Eq>  ||
                      std::is_same_v<T, NEq> ||
                      std::is_same_v<T, Less>) {
            return Key{tag, (uint64_t)x.lhs, (uint64_t)x.rhs};
        } else if constexpr (std::is_same_v<T, Not>) {
            return Key{tag, (uint64_t)x.operand};
        } else if constexpr (std::is_same_v<T, HasAttr>) {
            return Key{tag, (uint64_t)x.attrs, (uint64_t)x.name};
        } else {
            (void)x;
            return Key{};
        }
    }, e);
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: commonSubexprElim
// ---------------------------------------------------------------------------

size_t commonSubexprElim(Module & m)
{
    size_t merged = 0;
    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & b = m.blocks[bid];
        if (b.bindings.size() < 2) continue;

        // Per-block hash map keyed by Expr shape; value is the first
        // VarId we saw bound to this shape.
        std::unordered_map<Key, VarId, KeyHash> seen;

        for (auto & bind : b.bindings) {
            Key k = buildKey(bind.expr);
            if (k.empty()) continue; // not whitelisted

            auto [it, inserted] = seen.emplace(std::move(k), bind.var);
            if (!inserted) {
                // Duplicate: rewrite to alias.  inlineTrivialBindings
                // running after this pass collapses the chain and
                // deadBindingElim sweeps any further orphans.
                bind.expr = VarRef{it->second};
                ++merged;
            }
        }
    }
    return merged;
}

} // namespace nix::v3::ir
