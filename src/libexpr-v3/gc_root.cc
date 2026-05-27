/// @file
/// Stage 5 of the precise-root foundation — implementation.
///
/// See include/v3/gc_root.hh for the API + design rationale.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/gc_root.hh"
#include "v3/precise_root.hh"
#include "v3/value.hh"

namespace nix::v3 {

std::vector<Value *> & gcRootStack() noexcept
{
    thread_local std::vector<Value *> s;
    return s;
}

void walkCppStackRoots(RootVisitor & visitor) noexcept
{
    // Vector iteration is bottom-to-top of the LIFO push history;
    // for non-moving GC the order is irrelevant.  For a future
    // moving GC, walk-then-rewrite is correct either way because
    // each visitor.visitValue() call rewrites in place.
    for (Value * p : gcRootStack()) {
        if (p) visitor.visitValue(*p);
    }
}

GcRoot::GcRoot(Value & v) noexcept
{
    gcRootStack().push_back(&v);
}

GcRoot::GcRoot(Value * p) noexcept
{
    gcRootStack().push_back(p);
}

GcRoot::~GcRoot() noexcept
{
    // LIFO discipline: we always pop the top.  Mismatched ordering
    // (e.g., two GcRoot objects destructed out-of-order via
    // exception unwinding from inside the registered scope) is
    // impossible because we deleted move + copy and require the
    // RAII to be stack-allocated.  Defensive: nothing fires if the
    // stack is somehow empty.
    auto & s = gcRootStack();
    if (!s.empty()) s.pop_back();
}

} // namespace nix::v3
