/// @file
/// v3 bring-up smoke tests.  Hand-builds the IR for several small
/// programs and checks that the v3 pipeline produces the expected
/// values.
///
/// Each test:
///   1. Constructs an ir::Module via makeModule() (reserves slot 0).
///   2. Sets up Function descriptors and Blocks.
///   3. Runs computeFreeVars + compile + run.
///   4. Asserts the resulting Value.
///
/// The helpers below take BlockId / FuncId rather than Block& / Function&
/// so that subsequent freshBlock / functions.emplace_back calls cannot
/// invalidate the references held by callers.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/vm.hh"
#include "v3/alloc.hh"
#include "v3/primop.hh"

#include <cassert>
#include <cstdio>
#include <stdexcept>

using namespace nix::v3;

// ---------------------------------------------------------------------------
// Helpers — work via BlockId / FuncId, so vector reallocations are safe.
// ---------------------------------------------------------------------------

static ir::Function & funcOf(ir::Module & m, ir::FuncId fid) { return m.functions[fid]; }

static void setReturn(ir::Module & m, ir::BlockId bid, ir::VarId v)
{
    m.blocks[bid].terminal = ir::TermReturn{v};
}

static ir::VarId addBinding(ir::Module & m, ir::BlockId bid, ir::Expr e)
{
    auto v = m.freshVar();
    m.blocks[bid].bindings.push_back({v, std::move(e)});
    return v;
}

static ir::FuncId addFunction(ir::Module & m)
{
    m.functions.emplace_back();
    return static_cast<ir::FuncId>(m.functions.size() - 1);
}

// ---------------------------------------------------------------------------

static int testLitInt()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto v = addBinding(m, entry, ir::LitInt{42});
    setReturn(m, entry, v);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 42) {
        std::fprintf(stderr, "testLitInt: expected 42, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testLitInt: OK (42)\n");
    return 0;
}

static int testAdd()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{1});
    auto b = addBinding(m, entry, ir::LitInt{2});
    auto c = addBinding(m, entry, ir::Add{a, b});
    setReturn(m, entry, c);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 3) {
        std::fprintf(stderr, "testAdd: expected 3, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testAdd: OK (1+2=3)\n");
    return 0;
}

// `(x: x + 1) 41` → 42
static int testLambdaCall()
{
    auto m = ir::makeModule();

    auto innerFid = addFunction(m);
    auto innerEntry = m.freshBlock();
    auto innerArgName = m.internSymbol("x");
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerEntry;
        f.argName = innerArgName;
        f.paramVar = innerParam;
        f.name = "f";
        auto v1 = addBinding(m, innerEntry, ir::LitInt{1});
        auto v2 = addBinding(m, innerEntry, ir::Add{innerParam, v1});
        setReturn(m, innerEntry, v2);
    }

    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto fv = addBinding(m, topEntry, ir::Lambda{ innerFid, /*freeVars*/ {} });
    auto av = addBinding(m, topEntry, ir::LitInt{41});
    auto rv = addBinding(m, topEntry, ir::App{fv, av});
    setReturn(m, topEntry, rv);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 42) {
        std::fprintf(stderr, "testLambdaCall: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testLambdaCall: OK ((x: x+1) 41 = 42)\n");
    return 0;
}

// `let n = 10; f = x: x + n; in f 32` → 42
static int testClosureCapture()
{
    auto m = ir::makeModule();

    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;

    // Bind n in the top scope first; the inner function captures it.
    auto n = addBinding(m, topEntry, ir::LitInt{10});

    auto innerFid = addFunction(m);
    auto innerEntry = m.freshBlock();
    auto argName = m.internSymbol("x");
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerEntry;
        f.argName = argName;
        f.paramVar = innerParam;
        f.name = "f";
        auto added = addBinding(m, innerEntry, ir::Add{innerParam, n});
        setReturn(m, innerEntry, added);
    }

    auto fv = addBinding(m, topEntry, ir::Lambda{ innerFid, /*freeVars*/ {} });
    auto av = addBinding(m, topEntry, ir::LitInt{32});
    auto rv = addBinding(m, topEntry, ir::App{fv, av});
    setReturn(m, topEntry, rv);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 42) {
        std::fprintf(stderr, "testClosureCapture: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testClosureCapture: OK ((let n=10; f=x:x+n; in f 32) = 42)\n");
    return 0;
}

// `if 1 < 2 then 100 else 200` → 100
static int testIf()
{
    auto m = ir::makeModule();
    auto entry  = m.freshBlock();
    auto thenB  = m.freshBlock();
    auto elseB  = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto a = addBinding(m, entry, ir::LitInt{1});
    auto b = addBinding(m, entry, ir::LitInt{2});
    auto cond = addBinding(m, entry, ir::Less{a, b});
    auto result = addBinding(m, entry, ir::If{cond, thenB, elseB});
    setReturn(m, entry, result);

    auto t1 = addBinding(m, thenB, ir::LitInt{100});
    setReturn(m, thenB, t1);

    auto e1 = addBinding(m, elseB, ir::LitInt{200});
    setReturn(m, elseB, e1);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 100) {
        std::fprintf(stderr, "testIf: expected 100, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testIf: OK (if 1<2 then 100 else 200 = 100)\n");
    return 0;
}

// `[1 2 3] ++ [4 5]` → list of 5
static int testListConcat()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto i1 = addBinding(m, entry, ir::LitInt{1});
    auto i2 = addBinding(m, entry, ir::LitInt{2});
    auto i3 = addBinding(m, entry, ir::LitInt{3});
    auto i4 = addBinding(m, entry, ir::LitInt{4});
    auto i5 = addBinding(m, entry, ir::LitInt{5});
    auto l1 = addBinding(m, entry, ir::ListExpr{ {i1, i2, i3} });
    auto l2 = addBinding(m, entry, ir::ListExpr{ {i4, i5} });
    auto cc = addBinding(m, entry, ir::ConcatLists{l1, l2});
    setReturn(m, entry, cc);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isList() || r.payload.list->size != 5) {
        std::fprintf(stderr, "testListConcat: expected list of 5, got tag=%d\n", (int)r.tag());
        return 1;
    }
    for (uint32_t i = 0; i < 5; ++i) {
        Value & el = r.payload.list->elems[i];
        if (!el.isInt() || el.payload.i != i + 1) {
            std::fprintf(stderr, "testListConcat: elem[%u] expected %u, got %lld\n",
                i, i + 1, (long long)el.payload.i);
            return 1;
        }
    }
    std::fprintf(stderr, "testListConcat: OK ([1 2 3] ++ [4 5] = [1 2 3 4 5])\n");
    return 0;
}

// `{ a = 1; b = 2; }.a` → 1
static int testAttrSelect()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto va = addBinding(m, entry, ir::LitInt{1});
    auto vb = addBinding(m, entry, ir::LitInt{2});
    auto sa = m.internSymbol("a");
    auto sb = m.internSymbol("b");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {sa, va}, {sb, vb} } });
    auto sel = addBinding(m, entry, ir::AttrSelect{attrs, sa});
    setReturn(m, entry, sel);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 1) {
        std::fprintf(stderr, "testAttrSelect: expected 1, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testAttrSelect: OK ({ a=1; b=2; }.a = 1)\n");
    return 0;
}

// `({a=1;}//{b=2;}).b` -> 2
static int testAttrUpdate()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto sa = m.internSymbol("a");
    auto sb = m.internSymbol("b");
    auto va = addBinding(m, entry, ir::LitInt{1});
    auto vb = addBinding(m, entry, ir::LitInt{2});
    auto a1 = addBinding(m, entry, ir::AttrSet{ { {sa, va} } });
    auto a2 = addBinding(m, entry, ir::AttrSet{ { {sb, vb} } });
    auto u  = addBinding(m, entry, ir::Update{a1, a2});
    auto sel= addBinding(m, entry, ir::AttrSelect{u, sb});
    setReturn(m, entry, sel);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 2) {
        std::fprintf(stderr, "testAttrUpdate: expected 2, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testAttrUpdate: OK (({a=1;}//{b=2;}).b = 2)\n");
    return 0;
}

// `with { x = 7; y = 11; }; x + y` → 18
static int testWith()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    auto bodyB = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto sx = m.internSymbol("x");
    auto sy = m.internSymbol("y");

    auto v7 = addBinding(m, entry, ir::LitInt{7});
    auto v11 = addBinding(m, entry, ir::LitInt{11});
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {sx, v7}, {sy, v11} } });
    auto wResult = addBinding(m, entry, ir::With{attrs, bodyB});
    setReturn(m, entry, wResult);

    auto wx = addBinding(m, bodyB, ir::WithLookup{sx, 0});
    auto wy = addBinding(m, bodyB, ir::WithLookup{sy, 0});
    auto sum = addBinding(m, bodyB, ir::Add{wx, wy});
    setReturn(m, bodyB, sum);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 18) {
        std::fprintf(stderr, "testWith: expected 18, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testWith: OK (with {x=7;y=11;}; x+y = 18)\n");
    return 0;
}

// `force(thunk{10}) + 5` → 15
static int testThunkForce()
{
    auto m = ir::makeModule();

    auto innerFid = addFunction(m);
    auto innerEntry = m.freshBlock();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerEntry;
        f.name = "thunk_body";
        auto v = addBinding(m, innerEntry, ir::LitInt{10});
        setReturn(m, innerEntry, v);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto thunk = addBinding(m, entry, ir::MkThunk{ innerFid, /*freeVars*/ {} });
    auto forced = addBinding(m, entry, ir::Force{thunk});
    auto five = addBinding(m, entry, ir::LitInt{5});
    auto sum = addBinding(m, entry, ir::Add{forced, five});
    setReturn(m, entry, sum);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 15) {
        std::fprintf(stderr, "testThunkForce: expected 15, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testThunkForce: OK (force(thunk{10}) + 5 = 15)\n");
    return 0;
}

// `(true && false) || true` → true
static int testShortCircuit()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    auto andRhs = m.freshBlock();
    auto orRhs = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto vt = addBinding(m, entry, ir::LitBool{true});

    auto vfalse = addBinding(m, andRhs, ir::LitBool{false});
    setReturn(m, andRhs, vfalse);
    auto andResult = addBinding(m, entry, ir::And{vt, andRhs});

    auto vt2 = addBinding(m, orRhs, ir::LitBool{true});
    setReturn(m, orRhs, vt2);
    auto orResult = addBinding(m, entry, ir::Or{andResult, orRhs});
    setReturn(m, entry, orResult);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isBool() || r.payload.i != 1) {
        std::fprintf(stderr, "testShortCircuit: expected true, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testShortCircuit: OK ((true && false) || true = true)\n");
    return 0;
}

// `builtins.length [10 20 30]` -> 3
static int testPrimOpLength()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto a = addBinding(m, entry, ir::LitInt{10});
    auto b = addBinding(m, entry, ir::LitInt{20});
    auto c = addBinding(m, entry, ir::LitInt{30});
    auto lst = addBinding(m, entry, ir::ListExpr{ {a, b, c} });

    const PrimOp * po = findPrimOp("length");
    if (!po) { std::fprintf(stderr, "testPrimOpLength: missing 'length' primop\n"); return 1; }
    auto r = addBinding(m, entry, ir::PrimOpCall{po, {lst}});
    setReturn(m, entry, r);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value res = run(cu);
    if (!res.isInt() || res.payload.i != 3) {
        std::fprintf(stderr, "testPrimOpLength: expected 3, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testPrimOpLength: OK (length [10 20 30] = 3)\n");
    return 0;
}

// `builtins.head (builtins.tail [10 20 30])` -> 20
static int testPrimOpHeadTail()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto a = addBinding(m, entry, ir::LitInt{10});
    auto b = addBinding(m, entry, ir::LitInt{20});
    auto c = addBinding(m, entry, ir::LitInt{30});
    auto lst = addBinding(m, entry, ir::ListExpr{ {a, b, c} });

    auto tailOp = findPrimOp("tail");
    auto headOp = findPrimOp("head");
    if (!tailOp || !headOp) { std::fprintf(stderr, "testPrimOpHeadTail: missing primops\n"); return 1; }
    auto t = addBinding(m, entry, ir::PrimOpCall{tailOp, {lst}});
    auto h = addBinding(m, entry, ir::PrimOpCall{headOp, {t}});
    setReturn(m, entry, h);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value res = run(cu);
    if (!res.isInt() || res.payload.i != 20) {
        std::fprintf(stderr, "testPrimOpHeadTail: expected 20, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testPrimOpHeadTail: OK (head (tail [10 20 30]) = 20)\n");
    return 0;
}

// Fibonacci via self-application (avoids needing let-rec).
//   fibImpl = self: n: if n < 2 then n else self self (n-1) + self self (n-2)
//   fib = fibImpl fibImpl
//   fib 10 = 55
//
// IR layout:
//   function 0 (top):
//     fibImpl = Lambda funcIdx=1
//     fib = App fibImpl fibImpl
//     ten = LitInt 10
//     r = App fib ten
//     return r
//
//   function 1 (fibImpl): param self; body returns Lambda funcIdx=2 with self captured
//     return (Lambda funcIdx=2, freeVars=[self])
//
//   function 2 (the body): param n; freeVars = [self];
//     if n < 2 then n else (self self) (n-1) + (self self) (n-2)
static int testFibonacciSelfApp()
{
    auto m = ir::makeModule();

    // Allocate FuncIds + Blocks up front so block references stay valid.
    auto fibImplFid = addFunction(m); // 1
    auto bodyFid    = addFunction(m); // 2
    auto topEntry   = m.freshBlock();
    auto fibImplEntry = m.freshBlock();
    auto bodyEntry  = m.freshBlock();
    auto thenB      = m.freshBlock();
    auto elseB      = m.freshBlock();

    funcOf(m, 0).entryBlock = topEntry;
    funcOf(m, fibImplFid).entryBlock = fibImplEntry;
    funcOf(m, bodyFid).entryBlock = bodyEntry;

    auto symSelf = m.internSymbol("self");
    auto symN    = m.internSymbol("n");

    // function 1 (fibImpl): param self -> returns Lambda(bodyFid, freeVars=[self]).
    auto selfParam = m.freshVar();
    {
        auto & f = funcOf(m, fibImplFid);
        f.argName = symSelf;
        f.paramVar = selfParam;
        f.name = "fibImpl";
        // The body returns Lambda(bodyFid) capturing selfParam.
        auto lam = addBinding(m, fibImplEntry,
                              ir::Lambda{ bodyFid, /*freeVars*/ {selfParam} });
        setReturn(m, fibImplEntry, lam);
    }

    // function 2 (body): param n; free var = self (the one passed to fibImpl).
    auto nParam = m.freshVar();
    {
        auto & f = funcOf(m, bodyFid);
        f.argName = symN;
        f.paramVar = nParam;
        f.name = "fib_body";

        auto two   = addBinding(m, bodyEntry, ir::LitInt{2});
        auto cond  = addBinding(m, bodyEntry, ir::Less{nParam, two});
        auto res   = addBinding(m, bodyEntry, ir::If{cond, thenB, elseB});
        setReturn(m, bodyEntry, res);

        // then: return n
        setReturn(m, thenB, nParam);

        // else: (self self) (n-1) + (self self) (n-2)
        auto one  = addBinding(m, elseB, ir::LitInt{1});
        auto two2 = addBinding(m, elseB, ir::LitInt{2});
        auto nm1  = addBinding(m, elseB, ir::Sub{nParam, one});
        auto nm2  = addBinding(m, elseB, ir::Sub{nParam, two2});
        // Build "fib = self self" twice (could share but keep simple).
        auto fibA = addBinding(m, elseB, ir::App{selfParam, selfParam});
        auto callA = addBinding(m, elseB, ir::App{fibA, nm1});
        auto fibB = addBinding(m, elseB, ir::App{selfParam, selfParam});
        auto callB = addBinding(m, elseB, ir::App{fibB, nm2});
        auto sum = addBinding(m, elseB, ir::Add{callA, callB});
        setReturn(m, elseB, sum);
    }

    // function 0 (top): build fibImpl, apply to itself, then to 10.
    auto fibImplVar = addBinding(m, topEntry, ir::Lambda{ fibImplFid, /*freeVars*/ {} });
    auto fibVar     = addBinding(m, topEntry, ir::App{fibImplVar, fibImplVar});
    auto ten        = addBinding(m, topEntry, ir::LitInt{10});
    auto r          = addBinding(m, topEntry, ir::App{fibVar, ten});
    setReturn(m, topEntry, r);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value res = run(cu);
    if (!res.isInt() || res.payload.i != 55) {
        std::fprintf(stderr, "testFibonacciSelfApp: expected 55, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testFibonacciSelfApp: OK (fib 10 = 55)\n");
    return 0;
}

int main()
{
    registerBuiltinPrimOps();
    int rc = 0;
    rc |= testLitInt();
    rc |= testAdd();
    rc |= testLambdaCall();
    rc |= testClosureCapture();
    rc |= testIf();
    rc |= testListConcat();
    rc |= testAttrSelect();
    rc |= testAttrUpdate();
    rc |= testWith();
    rc |= testThunkForce();
    rc |= testShortCircuit();
    rc |= testPrimOpLength();
    rc |= testPrimOpHeadTail();
    rc |= testFibonacciSelfApp();

    auto & st = allocStats();
    std::fprintf(stderr,
        "\nv3 alloc stats: closures=%llu thunks=%llu lists=%llu attrsets=%llu\n",
        (unsigned long long)st.closuresAllocated,
        (unsigned long long)st.thunksAllocated,
        (unsigned long long)st.listsAllocated,
        (unsigned long long)st.attrsetsAllocated);

    if (rc == 0)
        std::fprintf(stderr, "\nALL v3 SMOKE TESTS PASSED\n");
    else
        std::fprintf(stderr, "\nSOME v3 TESTS FAILED\n");
    return rc;
}
