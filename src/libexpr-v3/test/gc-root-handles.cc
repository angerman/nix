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

// S1.1: a RELOCATING visitor — models a moving GC that relocates each pointee,
// rewriting the field to the cell's new address.  Used to prove GcRoot is
// relocation-AWARE: walkCppStackRoots → RootVisitor::visitValue (read p →
// visitX(&p) → writeback via mkX) rewrites the rooted C++ local's Value IN
// PLACE, so a primop local that survives a mid-eval safepoint follows the moved
// cell instead of dangling.  Sentinels are distinct per type to catch dispatch
// mix-ups.
struct RelocatingVisitor : RootVisitor
{
    void visitClosure  (Closure   * & p) override { p = reinterpret_cast<Closure   *>(0xC0FFEE00); }
    void visitThunk    (Thunk     * & p) override { p = reinterpret_cast<Thunk     *>(0xC0FFEE10); }
    void visitBindings (Bindings  * & p) override { p = reinterpret_cast<Bindings  *>(0xC0FFEE20); }
    void visitList     (ListVec   * & p) override { p = reinterpret_cast<ListVec   *>(0xC0FFEE30); }
    void visitPair     (ValuePair * & p) override { p = reinterpret_cast<ValuePair *>(0xC0FFEE40); }
    void visitSlot     (Value     * & p) override { (void)p; }
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

void testRelocationRewrite()
{
    // S1.1 relocation-awareness: a GcRoot'd Value whose pointee is "relocated"
    // by a moving visitor must have its pointer REWRITTEN in place.  This is the
    // property the safepoint foundation relies on — a primop's GcRoot'd local
    // following a mid-eval compaction instead of dangling.
    Value vB; vB.mkAttrs  (reinterpret_cast<Bindings *>(0x5678));
    Value vC; vC.mkClosure(reinterpret_cast<Closure  *>(0x1234));
    Value vL; vL.mkList   (reinterpret_cast<ListVec  *>(0x9abc));
    Value vI; vI.mkInt(99);  // scalar — must be UNTOUCHED by relocation
    Value vU; vU.mkAttrs(reinterpret_cast<Bindings *>(0xDEAD));  // NOT rooted (- contrast)
    {
        GcRoot rB(vB), rC(vC), rL(vL), rI(vI);  // vU deliberately NOT GcRoot'd
        RelocatingVisitor rv;
        walkCppStackRoots(rv);
    }
    // + : rooted pointers followed the relocation.
    ASSERT_EQ((long long)(uintptr_t)vB.asAttrs(),   (long long)0xC0FFEE20, "GcRoot Bindings rewritten on relocation");
    ASSERT_EQ((long long)(uintptr_t)vC.asClosure(), (long long)0xC0FFEE00, "GcRoot Closure rewritten on relocation");
    ASSERT_EQ((long long)(uintptr_t)vL.asList(),    (long long)0xC0FFEE30, "GcRoot List rewritten on relocation");
    ASSERT_EQ((long long)vI.asInt(),                (long long)99,         "GcRoot scalar untouched by relocation");
    // - : an un-rooted Value is NOT in gcRootStack → never visited → unchanged.
    ASSERT_EQ((long long)(uintptr_t)vU.asAttrs(),   (long long)0xDEAD,     "un-rooted Value NOT rewritten (contrast)");
}

void testRangeRelocation()
{
    // S1.2: GcRootRange roots a contiguous Value[] (the primop args[] case).
    // Registers n entries; a relocating visitor rewrites EVERY element in place;
    // pops n on dtor.
    Value args[4];
    args[0].mkAttrs  (reinterpret_cast<Bindings *>(0x1000));
    args[1].mkClosure(reinterpret_cast<Closure  *>(0x2000));
    args[2].mkInt(7);  // scalar — untouched
    args[3].mkList   (reinterpret_cast<ListVec  *>(0x3000));
    {
        GcRootRange rr(args, 4);
        ASSERT_EQ(gcRootStack().size(), 4u, "GcRootRange registers n entries");
        RelocatingVisitor rv;
        walkCppStackRoots(rv);
    }
    ASSERT_EQ(gcRootStack().size(), 0u, "GcRootRange pops n on dtor");
    ASSERT_EQ((long long)(uintptr_t)args[0].asAttrs(),   (long long)0xC0FFEE20, "range[0] Bindings rewritten");
    ASSERT_EQ((long long)(uintptr_t)args[1].asClosure(), (long long)0xC0FFEE00, "range[1] Closure rewritten");
    ASSERT_EQ((long long)args[2].asInt(),                (long long)7,          "range[2] scalar untouched");
    ASSERT_EQ((long long)(uintptr_t)args[3].asList(),    (long long)0xC0FFEE30, "range[3] List rewritten");
}

} // anon ns

int main()
{
    testEmptyRegistry();
    testPushPop();
    testLifo();
    testWalkDispatch();
    testNullSlot();
    testRelocationRewrite();
    testRangeRelocation();

    if (failures > 0) {
        std::fprintf(stderr, "gc-root-handles: %d FAILURE%s\n",
            failures, failures == 1 ? "" : "S");
        return 1;
    }
    std::fprintf(stdout, "gc-root-handles: ALL OK\n");
    return 0;
}
