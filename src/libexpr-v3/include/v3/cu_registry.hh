#pragma once

// WS5-D1 (2026-07-16): process-global reverse map from a `LambdaDescriptor *`
// to its owning `CompilationUnit *`.
//
// Background: to let the compiled `CompilationUnit::lambdas[]` array become
// read-only-after-load (so the descriptor pages can be `Shared_Clean` across
// independent `nix` processes / stay clean under COW — see
// lode/WS5_D2_INPLACE_AOT_DESIGN_2026-07-16.md + lode/WS5_COW_BASELINE...),
// the runtime-mutable `cu` back-pointer was REMOVED from `LambdaDescriptor`.
// Previously `closureCU(c) == c->desc->cu` and `thunkCU(t) == t->..desc->cu`
// were a single dependent load off a field the VM STAMPED at every
// OP_MAKE_CLOSURE / OP_MAKE_THUNK / fakeClo (dirtying the descriptor page).
//
// The descriptor for a closure/thunk ALWAYS lives inside its owning CU's
// `lambdas` vector (OP_MAKE_CLOSURE/THUNK set `desc = &cu->lambdas[funcIdx]`;
// runFunctionWithUpvalues / runLambda pass `desc = &cu.lambdas[funcIdx]` to
// fakeClo/runOnExistingVm).  So a CU's descriptors occupy the contiguous
// address range `[lambdas.data(), lambdas.data() + size())`, and a descriptor
// pointer uniquely identifies its CU (and its funcId = desc - lambdas.data()).
//
// This registry maps those per-CU intervals → CU.  It is populated idempotently
// at each closure/thunk creation site (exactly where `desc->cu = cu` used to be
// written) and read by `closureCU` / `thunkCU`.  A thread-local last-hit
// interval cache makes the hot reader O(1) in the common case (evaluation stays
// within one CU for long stretches); the slow path binary-searches the sorted
// interval vector.  Because the mapping is derived STRUCTURALLY from the
// descriptor's address (not stored per-descriptor), it is byte-identical to the
// old field: for any `desc` a live closure/thunk points at, its CU's interval
// has been registered, so the lookup returns the same CU the old stamp did.
//
// GLOBAL STATE JUSTIFICATION (per repo CLAUDE.md): the reverse map is
// necessarily process/thread-scoped side state — the whole point of D1 is that
// it must NOT live in the (now read-only) descriptor.  It is thread-local to
// mirror the existing per-thread GC registries (barrier.cc's
// singletonClosureRegistry etc.); the v3 VM is single-threaded per eval.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nix::v3 {

struct LambdaDescriptor;
struct CompilationUnit;

/// Idempotently register `cu`'s `lambdas[]` address range so `cuForDesc` can
/// map any descriptor in that range back to `cu`.  Called at every closure /
/// thunk creation site (the former `desc->cu = cu` stamp).  No-op when the CU
/// has no descriptors.  Overlap-eviction on register handles the (rare) case of
/// a destroyed CU whose freed `lambdas` storage is reused by a new CU.
void registerCuLambdaRange(const CompilationUnit * cu) noexcept;

/// Slow path for `cuForDesc`: binary-search the interval registry, update the
/// thread-local last-hit cache, and return the owning CU (or nullptr if `desc`
/// is not in any registered range — matching the old null `desc->cu`).
const CompilationUnit * lookupCuForDescSlow(const LambdaDescriptor * desc) noexcept;

/// COMPILE-WASTE spike (2026-08-07): enumerate every CU registered on this
/// thread (i.e. every CU that created at least one closure/thunk during this
/// eval).  COLD — walked exactly once at eval-end by the NIX_V3_COMPILE_WASTE
/// report; NOT on any hot path.  Remove with the instrument (Rule 0).
std::vector<const CompilationUnit *> allRegisteredCus();

namespace detail {
// Thread-local last-hit interval cache (address range + its CU).  `tl_cuLo` is
// initialised so the empty interval `[1, 0)` never matches → first lookup takes
// the slow path.  Defined in cu_registry.cc.
extern thread_local std::uintptr_t          tl_cuLo;
extern thread_local std::uintptr_t          tl_cuHi;
extern thread_local const CompilationUnit * tl_cuCu;
}  // namespace detail

/// Map `desc` → its owning CU.  Fast path: the thread-local last-hit interval;
/// slow path: `lookupCuForDescSlow`.  Returns nullptr for a null descriptor.
[[gnu::always_inline]] inline const CompilationUnit *
cuForDesc(const LambdaDescriptor * desc) noexcept
{
    if (!desc) return nullptr;
    const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(desc);
    if (p >= detail::tl_cuLo && p < detail::tl_cuHi) return detail::tl_cuCu;
    return lookupCuForDescSlow(desc);
}

}  // namespace nix::v3
