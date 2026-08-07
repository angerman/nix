// WS5-D1: process-global desc→CompilationUnit reverse map.  See cu_registry.hh
// for the design + Rule-0 rationale.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#include "v3/cu_registry.hh"
#include "v3/bytecode.hh"  // full CompilationUnit (lambdas.data()/size())

#include <algorithm>
#include <vector>

namespace nix::v3 {

namespace detail {
// The empty interval [1, 0) never contains any address → the first lookup on a
// fresh thread always falls through to the slow path.
thread_local std::uintptr_t          tl_cuLo = 1;
thread_local std::uintptr_t          tl_cuHi = 0;
thread_local const CompilationUnit * tl_cuCu = nullptr;
}  // namespace detail

namespace {

/// A CU's descriptor-address range.  `[lo, hi)` are the byte bounds of the CU's
/// `lambdas` vector storage; `cu` is the owner.
struct CuInterval {
    std::uintptr_t          lo;
    std::uintptr_t          hi;
    const CompilationUnit * cu;
};

// Sorted-by-`lo` vector of all registered CU ranges (thread-local: each eval
// thread self-populates as it creates closures/thunks — see cu_registry.hh).
thread_local std::vector<CuInterval> tl_intervals;

// Fast-skip memo for registerCuLambdaRange: the last CU registered on this
// thread + its lambdas base.  Re-registering the same (unchanged) CU is a
// no-op; storing the base too closes the rare "CU freed then a new CU reused
// the exact same struct address" hole (base differs → we re-register).
thread_local const CompilationUnit * tl_lastRegCu = nullptr;
thread_local std::uintptr_t          tl_lastRegLo = 0;

}  // namespace

void registerCuLambdaRange(const CompilationUnit * cu) noexcept
{
    if (!cu) return;
    const std::size_t n = cu->lambdas.size();
    if (n == 0) return;  // no descriptors → nothing maps here (closureCU → null)

    const std::uintptr_t lo =
        reinterpret_cast<std::uintptr_t>(cu->lambdas.data());

    // Hot path: this CU (with this lambdas storage) is already registered.
    if (cu == tl_lastRegCu && lo == tl_lastRegLo) return;

    const std::uintptr_t hi = lo + n * sizeof(LambdaDescriptor);
    auto & iv = tl_intervals;

    // Already present, identical? (e.g. re-register after another CU reset the
    // fast memo.)  Cheap binary search; leaves the vector untouched.
    auto lb = std::lower_bound(iv.begin(), iv.end(), lo,
        [](const CuInterval & a, std::uintptr_t v) { return a.lo < v; });
    if (lb != iv.end() && lb->lo == lo && lb->hi == hi && lb->cu == cu) {
        tl_lastRegCu = cu;
        tl_lastRegLo = lo;
        return;
    }

    // Evict any interval overlapping [lo, hi) (address reuse by a dead CU), then
    // insert the fresh one keeping the vector sorted by `lo`.  This is the cold
    // path (once per distinct CU); the vector holds hundreds of entries at most.
    iv.erase(std::remove_if(iv.begin(), iv.end(),
                 [&](const CuInterval & c) { return c.lo < hi && c.hi > lo; }),
             iv.end());
    auto ins = std::lower_bound(iv.begin(), iv.end(), lo,
        [](const CuInterval & a, std::uintptr_t v) { return a.lo < v; });
    iv.insert(ins, CuInterval{lo, hi, cu});

    tl_lastRegCu = cu;
    tl_lastRegLo = lo;
    // The reader cache may have named an evicted interval; reset it so the next
    // cuForDesc re-derives from the (now updated) registry.
    detail::tl_cuLo = 1;
    detail::tl_cuHi = 0;
    detail::tl_cuCu = nullptr;
}

std::vector<const CompilationUnit *> allRegisteredCus()
{
    // COMPILE-WASTE spike: snapshot the sorted interval registry's CUs.  One
    // entry per distinct CU that created a closure/thunk this eval.  Cold (once
    // at eval-end); no hot-path impact.
    std::vector<const CompilationUnit *> out;
    out.reserve(tl_intervals.size());
    for (const auto & iv : tl_intervals)
        out.push_back(iv.cu);
    return out;
}

const CompilationUnit *
lookupCuForDescSlow(const LambdaDescriptor * desc) noexcept
{
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(desc);
    const auto & iv = tl_intervals;
    if (iv.empty()) return nullptr;

    // Find the last interval whose `lo <= p`, then range-check its `hi`.
    auto ub = std::upper_bound(iv.begin(), iv.end(), p,
        [](std::uintptr_t v, const CuInterval & a) { return v < a.lo; });
    if (ub == iv.begin()) return nullptr;  // below all ranges
    const CuInterval & cand = *(ub - 1);
    if (p >= cand.lo && p < cand.hi) {
        detail::tl_cuLo = cand.lo;
        detail::tl_cuHi = cand.hi;
        detail::tl_cuCu = cand.cu;
        return cand.cu;
    }
    return nullptr;
}

}  // namespace nix::v3
