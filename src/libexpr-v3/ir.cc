/// @file
/// v3 IR: free-vars analysis.
///
/// For each function, compute the set of VarIds it references that are NOT
/// defined within itself.  Those become the closure's upvalues (in sorted
/// order), accessed via OP_GET_UPVALUE inside the body.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <algorithm>
#include <unordered_set>

namespace nix::v3::ir {

static void collectExprRefs(const Expr & expr, std::unordered_set<VarId> & refs)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, LitInt>) {
            (void)e;
        } else if constexpr (std::is_same_v<T, VarRef>) {
            if (e.var != kInvalid) refs.insert(e.var);
        } else if constexpr (std::is_same_v<T, Lambda>) {
            for (auto v : e.freeVars) refs.insert(v);
        } else if constexpr (std::is_same_v<T, App>) {
            refs.insert(e.fun);
            refs.insert(e.arg);
        } else if constexpr (std::is_same_v<T, Add> ||
                             std::is_same_v<T, Sub> ||
                             std::is_same_v<T, Mul>) {
            refs.insert(e.lhs);
            refs.insert(e.rhs);
        }
    }, expr);
}

static void computeOne(Module & m, uint32_t funcIdx)
{
    auto & f = m.functions[funcIdx];

    // First, compute free vars of any nested lambdas (post-order).
    for (auto & b : f.bindings) {
        if (auto * lam = std::get_if<Lambda>(&b.expr)) {
            computeOne(m, lam->funcIdx);
            // Now lam->freeVars is set; copy into the IR for emit.
            lam->freeVars = m.functions[lam->funcIdx].freeVars;
        }
    }

    // Defined locally = the parameter + every binding's `var`.
    std::unordered_set<VarId> defined;
    if (f.param != kInvalid) defined.insert(f.param);
    for (auto & b : f.bindings) defined.insert(b.var);

    // Referenced anywhere in the function.
    std::unordered_set<VarId> referenced;
    for (auto & b : f.bindings) collectExprRefs(b.expr, referenced);
    if (f.returnVar != kInvalid) referenced.insert(f.returnVar);

    // Free = referenced - defined.
    std::vector<VarId> freeVars;
    for (auto v : referenced)
        if (defined.find(v) == defined.end())
            freeVars.push_back(v);
    std::sort(freeVars.begin(), freeVars.end());

    f.freeVars = std::move(freeVars);
}

void computeFreeVars(Module & m)
{
    // Walk from top-level (functions[0]) down; each function recurses on
    // its nested lambdas internally.
    if (!m.functions.empty()) computeOne(m, 0);
}

} // namespace nix::v3::ir
