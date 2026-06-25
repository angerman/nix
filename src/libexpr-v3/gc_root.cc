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

// S1.2: registry of growing Value-vector accumulators (GcRootVec).  Walked with
// CURRENT data()/size() each GC → realloc-safe + covers post-construct growth.
std::vector<std::vector<Value> *> & gcRootVecStack() noexcept
{
    thread_local std::vector<std::vector<Value> *> s;
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
    // GcRootVec: re-read each registered vector's CURRENT elements (data()/size()
    // at THIS instant — realloc-safe) and rewrite them in place.
    for (std::vector<Value> * v : gcRootVecStack()) {
        if (!v) continue;
        for (Value & e : *v) visitor.visitValue(e);
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

GcRootRange::GcRootRange(Value * data, size_t n) noexcept
    : n_(data ? n : 0)
{
    auto & s = gcRootStack();
    s.reserve(s.size() + n_);
    for (size_t i = 0; i < n_; ++i) s.push_back(&data[i]);
}

GcRootRange::~GcRootRange() noexcept
{
    // LIFO: pop exactly the n_ entries we pushed (defensive against an empty
    // stack, mirroring GcRoot::~GcRoot).
    auto & s = gcRootStack();
    for (size_t i = 0; i < n_ && !s.empty(); ++i) s.pop_back();
}

GcRootVec::GcRootVec(std::vector<Value> & v) noexcept
{
    gcRootVecStack().push_back(&v);
}

GcRootVec::~GcRootVec() noexcept
{
    auto & s = gcRootVecStack();
    if (!s.empty()) s.pop_back();
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
