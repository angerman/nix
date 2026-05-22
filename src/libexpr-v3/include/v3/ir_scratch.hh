#pragma once
/// @file
/// #766 (2026-05-22) — scratch buffer pool for IR optimisation
/// passes.  Profile data (#765) showed that
/// `std::__hash_table<VarId, Expr*>::emplace_unique_key_args` was
/// the largest single alloc-traffic contributor in the IR pipeline
/// on hello.drvPath (~28% of v3-attributable malloc/free
/// samples).  Each opt pass builds a fresh
/// `unordered_map<VarId, const Expr*>` per block via per-node
/// malloc, destroyed via per-node free.
///
/// `FlatBlockMap` is a sorted-vector replacement:
///
///   * O(n + n log n) build (vector push + sort)
///   * O(log n) lookup (binary search)
///   * One vector allocation per thread (amortised across all
///     opt-pass calls), no per-entry malloc/free
///   * Held in a `thread_local` instance via `scratch()`
///
/// Migration shape for each pass:
///
///   ```cpp
///   auto & bm = FlatBlockMap::scratch();
///   bm.rebuild(block);
///   const Expr * e = bm.find(var);
///   ```
///
/// Constraints (important):
///
///   * The scratch instance is shared across all passes on a
///     thread.  Recursion that needs a DIFFERENT BlockMap (e.g.,
///     a helper that iterates a sub-block while the outer caller
///     also wants its block's BlockMap available) would clobber
///     the scratch.  Audit each migration site: if the pass
///     calls into anything that might rebuild the scratch, do
///     NOT use the scratch — fall back to a stack-local map.
///   * The pointers stored in entries point INTO the block's
///     `bindings` vector.  Pointers stay valid as long as the
///     block isn't resized (the standard assumption every
///     mapBlock-using pass already relies on).
///   * `clear()` does NOT shrink capacity; the largest block
///     ever seen pins the scratch size.  At nixpkgs scale this
///     is bounded by the largest single LetRec / AttrSet block
///     — typically ~few hundred entries, ~few KB.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"

#include <algorithm>
#include <utility>
#include <vector>

namespace nix::v3::ir {

/// Sorted-vector backed map: VarId -> const Expr*.  Held in a
/// thread-local scratch buffer to amortise the per-block allocation
/// across many opt-pass invocations.
class FlatBlockMap
{
public:
    using value_type = std::pair<VarId, const Expr *>;

    /// Thread-local scratch instance.  Caller MUST call `rebuild(block)`
    /// before any `find()` to ensure the contents match the current
    /// block.  Single instance per thread — do not nest uses.
    static FlatBlockMap & scratch() noexcept
    {
        thread_local FlatBlockMap m;
        return m;
    }

    /// Repopulate from a block's bindings.  After this returns,
    /// `find(v)` for any `v` in `block.bindings` returns the
    /// corresponding `&binding.expr`, and other lookups return
    /// `nullptr`.
    void rebuild(const Block & block)
    {
        v_.clear();
        v_.reserve(block.bindings.size());
        for (const auto & b : block.bindings)
            v_.emplace_back(b.var, &b.expr);
        std::sort(v_.begin(), v_.end(),
            [](const value_type & a, const value_type & b) {
                return a.first < b.first;
            });
    }

    /// Look up a VarId.  Returns the stored Expr* on hit, nullptr
    /// on miss.  O(log n) binary search.
    const Expr * find(VarId k) const noexcept
    {
        auto it = std::lower_bound(v_.begin(), v_.end(), k,
            [](const value_type & a, VarId k2) { return a.first < k2; });
        if (it == v_.end() || it->first != k) return nullptr;
        return it->second;
    }

    size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }

private:
    std::vector<value_type> v_;
};

} // namespace nix::v3::ir
