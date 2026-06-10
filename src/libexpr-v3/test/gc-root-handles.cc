/// @file
/// Stage 5 unit test — exercises the `GcRoot` RAII helper +
/// `gcRootStack()` registry + `walkCppStackRoots` walker.
///
/// Validates four invariants:
///   1. The registry is initially empty (per-thread).
///   2. Constructing a `GcRoot(v)` pushes &v onto the registry.
///   3. Destructing the `GcRoot` pops the entry (LIFO).
///   4. `walkCppStackRoots(visitor)` visits every registered Value
///      with the correct visitValue dispatch on the Value's tag.
///
/// No nixpkgs dependency; no eval state; no disk cache.  Pure
/// in-memory API correctness check.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/gc_root.hh"
#include "v3/precise_root.hh"
#include "v3/value.hh"
#include "v3/closure.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace nix::v3;

// A counting visitor that just records the tag of every value
// visitValue() dispatches on.  Records null-counts separately so
// we can verify ALL the typed callbacks fire correctly.
struct TagCountVisitor : RootVisitor
{
    int closures = 0, thunks = 0, bindings = 0, lists = 0;
    int pairs = 0, slots = 0;

    void visitClosure  (Closure   * &) override { ++closures; }
    void visitThunk    (Thunk     * &) override { ++thunks; }
    void visitBindings (Bindings  * &) override { ++bindings; }
    void visitList     (ListVec   * &) override { ++lists; }
    void visitPair     (ValuePair * &) override { ++pairs; }
    void visitSlot     (Value     * &) override { ++slots; }
};

int failures = 0;

#define ASSERT_EQ(actual, expected, label) do { \
    auto __a = (actual); \
    auto __e = (expected); \
    if (__a != __e) { \
        std::fprintf(stderr, \
            "FAIL %s: expected %lld, got %lld\n", \
            (label), (long long)__e, (long long)__a); \
        ++failures; \
    } \
} while (0)

void testEmptyRegistry()
{
    // Per-thread registry starts empty.  No prior GcRoot construction
    // has happened in this test thread.
    ASSERT_EQ(gcRootStack().size(), 0u, "empty-registry initial size");
}

void testPushPop()
{
    // Push a Value via GcRoot, verify size grows by 1, then drop the
    // RAII scope and verify size returns to 0.
    Value v;
    v.mkInt(42);
    {
        GcRoot r(v);
        ASSERT_EQ(gcRootStack().size(), 1u, "size after one GcRoot");
        ASSERT_EQ(gcRootStack().back(), &v, "registry top points to v");
    }
    ASSERT_EQ(gcRootStack().size(), 0u, "size after GcRoot dtor");
}

void testLifo()
{
    // Two GcRoots in scope: stack should grow to 2, top should be the
    // SECOND one (LIFO).  After both dtors fire, stack should be 0.
    Value v1, v2;
    v1.mkInt(1); v2.mkInt(2);
    {
        GcRoot r1(v1);
        ASSERT_EQ(gcRootStack().size(), 1u, "LIFO mid-1 size");
        ASSERT_EQ(gcRootStack().back(), &v1, "LIFO mid-1 top");
        {
            GcRoot r2(v2);
            ASSERT_EQ(gcRootStack().size(), 2u, "LIFO mid-2 size");
            ASSERT_EQ(gcRootStack().back(), &v2, "LIFO mid-2 top");
        }
        ASSERT_EQ(gcRootStack().size(), 1u, "LIFO after inner dtor");
        ASSERT_EQ(gcRootStack().back(), &v1, "LIFO after inner dtor top");
    }
    ASSERT_EQ(gcRootStack().size(), 0u, "LIFO after outer dtor");
}

void testWalkDispatch()
{
    // Each typed callback should fire once for a Value with the
    // corresponding tag.  Scalar tags (Int / Bool / Null / etc.) are
    // no-ops in visitValue — they should NOT increment any counter.
    Value vInt, vNull, vClosure, vBindings;
    vInt.mkInt(7);
    vNull.mkNull();
    vClosure.mkClosure(reinterpret_cast<Closure *>(0x1234));
    vBindings.mkAttrs(reinterpret_cast<Bindings *>(0x5678));

    GcRoot r1(vInt);
    GcRoot r2(vNull);
    GcRoot r3(vClosure);
    GcRoot r4(vBindings);

    TagCountVisitor v;
    walkCppStackRoots(v);

    ASSERT_EQ(v.closures, 1, "walkCppStackRoots visitClosure count");
    ASSERT_EQ(v.bindings, 1, "walkCppStackRoots visitBindings count");
    ASSERT_EQ(v.thunks,   0, "walkCppStackRoots visitThunk count (no thunk reg)");
    ASSERT_EQ(v.lists,    0, "walkCppStackRoots visitList count (no list reg)");
    ASSERT_EQ(v.pairs,    0, "walkCppStackRoots visitPair count (no pair reg)");
    ASSERT_EQ(v.slots,    0, "walkCppStackRoots visitSlot count (no slot reg)");

    // GcRoots auto-dtor at scope end.
}

void testNullSlot()
{
    // GcRoot constructed from a null pointer (Value *) — gcRootStack
    // should hold the null entry, walkCppStackRoots should skip it
    // without dispatching to any callback.
    GcRoot rNull(nullptr);
    ASSERT_EQ(gcRootStack().size(), 1u, "null-pointer registered");
    ASSERT_EQ(gcRootStack().back(), (Value *)nullptr, "null entry top");

    TagCountVisitor v;
    walkCppStackRoots(v);
    int total = v.closures + v.thunks + v.bindings + v.lists + v.pairs + v.slots;
    ASSERT_EQ(total, 0, "walkCppStackRoots null entry not dispatched");
}

} // anon ns

int main()
{
    testEmptyRegistry();
    testPushPop();
    testLifo();
    testWalkDispatch();
    testNullSlot();

    if (failures > 0) {
        std::fprintf(stderr, "gc-root-handles: %d FAILURE%s\n",
            failures, failures == 1 ? "" : "S");
        return 1;
    }
    std::fprintf(stdout, "gc-root-handles: ALL OK\n");
    return 0;
}
