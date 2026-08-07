#pragma once
/// @file
/// Shared IR optimizer-pass helpers.
///
/// Small, behaviour-neutral utilities that several `opt_*.cc` passes carried
/// as byte-identical copies.  Extracted here as `inline` free functions in
/// `nix::v3::ir` (matching ir.hh's `makeModule()` convention) so there is one
/// definition, not N — pure code motion, no semantic change.
///
/// ONLY verbatim-identical helpers belong here.  Passes that need a divergent
/// variant keep their own local copy: e.g. opt_beta_reduce.cc's and
/// opt_strict_call_unthunk.cc's `remapExprVars` differ intentionally in their
/// Lambda/MkThunk handling, and the per-pass `UseCounter`/`countModuleUses`
/// families are NOT byte-identical (member ordering + return type + comments
/// diverge), so they are deliberately left in place.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <cstddef>
#include <unordered_map>
#include <variant>

namespace nix::v3::ir {

// ---------------------------------------------------------------------------
// Same-block VarRef chase: resolve a VarId through VarRef aliases inside one
// block.  Returns the underlying defining Expr*, or nullptr if the chain
// leads outside the block (v is bound elsewhere) or exceeds defs.size()+1
// hops (cycle guard).
// ---------------------------------------------------------------------------
inline const Expr * chaseInBlock(VarId v,
                          const std::unordered_map<VarId, const Expr *> & defs)
{
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        auto it = defs.find(v);
        if (it == defs.end()) return nullptr;
        const Expr * e = it->second;
        if (const auto * vr = std::get_if<VarRef>(e)) {
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Build a var → defining-Expr* map for one block's bindings.
// ---------------------------------------------------------------------------
inline std::unordered_map<VarId, const Expr *> mapBlockDefs(const Block & b)
{
    std::unordered_map<VarId, const Expr *> defs;
    defs.reserve(b.bindings.size());
    for (const auto & bd : b.bindings)
        defs.emplace(bd.var, &bd.expr);
    return defs;
}

} // namespace nix::v3::ir
