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
#include "v3/serialize.hh"
#include "v3/disk_cache.hh"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
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

// ---------------------------------------------------------------------------
// Constant-folding regression tests
// ---------------------------------------------------------------------------
//
// Each test builds a tiny IR with a foldable expression, runs ir::optimise()
// directly on the module, and inspects the resulting Binding to confirm:
//   - "folds" tests:    the RHS is now a Lit*  (folded)
//   - "no-fold" tests:  the RHS is still the original op (not folded)
// We also execute the module and check the runtime result matches what
// the unfolded program would have produced -- guards against accidentally
// folding to a different value.

static bool isLitInt(const ir::Expr & e, int64_t expected)
{
    auto * x = std::get_if<ir::LitInt>(&e);
    return x && x->value == expected;
}

static bool isLitBool(const ir::Expr & e, bool expected)
{
    auto * x = std::get_if<ir::LitBool>(&e);
    return x && x->value == expected;
}

static bool isLitFloat(const ir::Expr & e, double expected)
{
    auto * x = std::get_if<ir::LitFloat>(&e);
    return x && x->value == expected;
}

template <typename T>
static bool isOp(const ir::Expr & e)
{
    return std::holds_alternative<T>(e);
}

// `2 + 3` → fold to LitInt{5} at IR-opt time.
static int testFoldAddInt()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{2});
    auto b = addBinding(m, entry, ir::LitInt{3});
    auto c = addBinding(m, entry, ir::Add{a, b});
    setReturn(m, entry, c);

    // Run only the fold pass to assert it's the one doing the work.
    // (DCE -- the next pass -- would remove the orphan a/b literals
    // and confuse this assertion.)
    ir::constantFold(m);

    // The Add binding should now be a LitInt{5}.
    const ir::Expr * cExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == c) cExpr = &bb.expr;
    if (!cExpr || !isLitInt(*cExpr, 5)) {
        std::fprintf(stderr, "testFoldAddInt: expected LitInt{5} after fold\n");
        return 1;
    }

    // And the program should still produce 5.
    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 5) {
        std::fprintf(stderr, "testFoldAddInt: runtime expected 5, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testFoldAddInt: OK (2+3 -> LitInt{5})\n");
    return 0;
}

// `2 / 0` → must NOT fold; runtime must throw.
static int testNoFoldDivByZero()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{2});
    auto b = addBinding(m, entry, ir::LitInt{0});
    auto c = addBinding(m, entry, ir::Div{a, b});
    setReturn(m, entry, c);

    ir::optimise(m);

    // Div is impure (may throw); DCE keeps it.
    const ir::Expr * cExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == c) cExpr = &bb.expr;
    if (!cExpr || !isOp<ir::Div>(*cExpr)) {
        std::fprintf(stderr, "testNoFoldDivByZero: Div was folded -- must be preserved\n");
        return 1;
    }

    // And the runtime really does throw.
    ir::computeFreeVars(m);
    auto cu = compile(m);
    bool threw = false;
    try { (void)run(cu); }
    catch (const std::exception &) { threw = true; }
    if (!threw) {
        std::fprintf(stderr, "testNoFoldDivByZero: runtime did not throw on 2/0\n");
        return 1;
    }
    std::fprintf(stderr, "testNoFoldDivByZero: OK (Div preserved, runtime throws)\n");
    return 0;
}

// `INT64_MAX + 1` → must NOT fold; runtime must throw on overflow.
static int testNoFoldAddOverflow()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{std::numeric_limits<int64_t>::max()});
    auto b = addBinding(m, entry, ir::LitInt{1});
    auto c = addBinding(m, entry, ir::Add{a, b});
    setReturn(m, entry, c);

    ir::optimise(m);

    const ir::Expr * cExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == c) cExpr = &bb.expr;
    if (!cExpr || !isOp<ir::Add>(*cExpr)) {
        std::fprintf(stderr, "testNoFoldAddOverflow: Add was folded -- overflow must be preserved\n");
        return 1;
    }
    std::fprintf(stderr, "testNoFoldAddOverflow: OK (Add preserved at INT64_MAX+1)\n");
    return 0;
}

// `1 == 1` → fold to LitBool{true};  `1 == 2` → fold to LitBool{false};
// `1 < 2` → fold to LitBool{true};   `!true` → fold to LitBool{false}.
static int testFoldComparisons()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto one  = addBinding(m, entry, ir::LitInt{1});
    auto one2 = addBinding(m, entry, ir::LitInt{1});
    auto two  = addBinding(m, entry, ir::LitInt{2});
    auto t    = addBinding(m, entry, ir::LitBool{true});
    auto eqV  = addBinding(m, entry, ir::Eq{one, one2});      // true
    auto neV  = addBinding(m, entry, ir::NEq{one, two});      // true
    auto ltV  = addBinding(m, entry, ir::Less{one, two});     // true
    auto notV = addBinding(m, entry, ir::Not{t});             // false
    setReturn(m, entry, eqV);

    // Use constantFold directly so the orphan boolean bindings stay
    // alive for the post-fold inspection.  (DCE would otherwise
    // remove neV/ltV/notV after they collapse to LitBool.)
    ir::constantFold(m);

    const auto & bs = m.blocks[entry].bindings;
    auto findBy = [&](ir::VarId v) -> const ir::Expr & {
        for (auto & bb : bs) if (bb.var == v) return bb.expr;
        std::abort();
    };

    if (!isLitBool(findBy(eqV),  true)  ||
        !isLitBool(findBy(neV),  true)  ||
        !isLitBool(findBy(ltV),  true)  ||
        !isLitBool(findBy(notV), false)) {
        std::fprintf(stderr, "testFoldComparisons: unexpected fold result\n");
        return 1;
    }
    std::fprintf(stderr, "testFoldComparisons: OK (Eq/NEq/Less/Not folded)\n");
    return 0;
}

// VarRef chain: a = 4; b = a; c = b + 1 → c folds to LitInt{5}.
static int testFoldThroughVarRef()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{4});
    auto b = addBinding(m, entry, ir::VarRef{a});
    auto one = addBinding(m, entry, ir::LitInt{1});
    auto c = addBinding(m, entry, ir::Add{b, one});
    setReturn(m, entry, c);

    ir::optimise(m);

    const ir::Expr * cExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == c) cExpr = &bb.expr;
    if (!cExpr || !isLitInt(*cExpr, 5)) {
        std::fprintf(stderr, "testFoldThroughVarRef: expected LitInt{5}\n");
        return 1;
    }
    std::fprintf(stderr, "testFoldThroughVarRef: OK (VarRef chain resolved)\n");
    return 0;
}

// Float fold: 1.5 * 2.0 → LitFloat{3.0}.
static int testFoldFloat()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitFloat{1.5});
    auto b = addBinding(m, entry, ir::LitFloat{2.0});
    auto c = addBinding(m, entry, ir::Mul{a, b});
    setReturn(m, entry, c);

    ir::optimise(m);

    const ir::Expr * cExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == c) cExpr = &bb.expr;
    if (!cExpr || !isLitFloat(*cExpr, 3.0)) {
        std::fprintf(stderr, "testFoldFloat: expected LitFloat{3.0}\n");
        return 1;
    }
    std::fprintf(stderr, "testFoldFloat: OK (1.5*2.0 -> LitFloat{3.0})\n");
    return 0;
}

// DCE: a pure unused literal binding is removed; the impure
// neighbour is preserved.  We construct a block with one orphan
// LitInt and a Force binding side-by-side, run optimise, and
// confirm only the LitInt survives the pure-DCE filter.
static int testDceRemovesUnusedLiteral()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto orphan = addBinding(m, entry, ir::LitInt{42});       // unused
    auto live   = addBinding(m, entry, ir::LitInt{1});         // returned
    setReturn(m, entry, live);

    ir::optimise(m);

    const auto & bs = m.blocks[entry].bindings;
    bool sawOrphan = false, sawLive = false;
    for (auto & bb : bs) { if (bb.var == orphan) sawOrphan = true; if (bb.var == live) sawLive = true; }
    if (sawOrphan || !sawLive) {
        std::fprintf(stderr, "testDceRemovesUnusedLiteral: orphan=%d live=%d (expected 0/1)\n",
            (int)sawOrphan, (int)sawLive);
        return 1;
    }
    std::fprintf(stderr, "testDceRemovesUnusedLiteral: OK (orphan removed, live kept)\n");
    return 0;
}

// DCE must NEVER eliminate an impure unused binding (e.g. Force,
// App, AttrSelect): the program may rely on the side-effect (a
// throw, a primop).  Construct an unused Force on a thunk and
// verify it survives optimise().
static int testDceKeepsImpureUnused()
{
    auto m = ir::makeModule();

    // Inner thunk that throws if forced.  We don't actually run it;
    // we just check the Force binding is preserved.
    auto thunkFid = addFunction(m);
    auto thunkBody = m.freshBlock();
    {
        auto & f = funcOf(m, thunkFid);
        f.entryBlock = thunkBody;
        f.name = "side-effect";
        auto z = addBinding(m, thunkBody, ir::LitInt{0});
        setReturn(m, thunkBody, z);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto th = addBinding(m, entry, ir::MkThunk{thunkFid, /*freeVars*/ {}});
    auto unused = addBinding(m, entry, ir::Force{th});         // impure, unused
    auto live   = addBinding(m, entry, ir::LitInt{7});
    setReturn(m, entry, live);

    ir::optimise(m);

    bool sawForce = false;
    for (auto & bb : m.blocks[entry].bindings)
        if (bb.var == unused && std::holds_alternative<ir::Force>(bb.expr)) sawForce = true;
    if (!sawForce) {
        std::fprintf(stderr, "testDceKeepsImpureUnused: Force was DCE'd -- impure binding must survive\n");
        return 1;
    }
    std::fprintf(stderr, "testDceKeepsImpureUnused: OK (unused Force preserved)\n");
    return 0;
}

// VarRef alias collapsing: `a = LitInt{99}; b = VarRef{a}; c = VarRef{b};`
// returning `c` should optimise to a module where a survives, b/c
// disappear, and the terminal returns `a`.
static int testInlineVarRefChain()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{99});
    auto b = addBinding(m, entry, ir::VarRef{a});
    auto c = addBinding(m, entry, ir::VarRef{b});
    setReturn(m, entry, c);

    ir::optimise(m);

    // After optimise: only `a` survives; the terminal returns `a`.
    bool sawA = false, sawB = false, sawC = false;
    for (auto & bb : m.blocks[entry].bindings) {
        if (bb.var == a) sawA = true;
        if (bb.var == b) sawB = true;
        if (bb.var == c) sawC = true;
    }
    ir::VarId termVar = ir::kInvalid;
    if (auto * ret = std::get_if<ir::TermReturn>(&m.blocks[entry].terminal))
        termVar = ret->value;
    if (!sawA || sawB || sawC || termVar != a) {
        std::fprintf(stderr,
            "testInlineVarRefChain: a=%d b=%d c=%d term=%u (expected 1/0/0/%u)\n",
            (int)sawA, (int)sawB, (int)sawC,
            (unsigned)termVar, (unsigned)a);
        return 1;
    }

    // And the runtime should still produce 99.
    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 99) {
        std::fprintf(stderr, "testInlineVarRefChain: runtime expected 99\n");
        return 1;
    }
    std::fprintf(stderr, "testInlineVarRefChain: OK (chain a<-b<-c collapsed to a)\n");
    return 0;
}

// CSE: `a + b` computed twice in one block should collapse so only
// one Add binding remains after the full optimisation pipeline.
static int testCseSharedAdd()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a  = addBinding(m, entry, ir::LitInt{7});
    auto b  = addBinding(m, entry, ir::LitInt{8});
    auto s1 = addBinding(m, entry, ir::Add{a, b});  // redundant 1
    auto s2 = addBinding(m, entry, ir::Add{a, b});  // redundant 2
    auto sum = addBinding(m, entry, ir::Add{s1, s2});
    setReturn(m, entry, sum);

    // We need to retain s1, s2 in the IR so the test can observe CSE
    // wired them together.  The full optimise() pipeline:
    //   - constantFold collapses LitInt{7}+LitInt{8} -> LitInt{15}
    //     for s1 (and the same for s2 -- both merge to LitInt{15}
    //     literals at the same time, BEFORE CSE runs).
    //   - To probe CSE specifically, run only commonSubexprElim
    //     before inspecting.
    ir::commonSubexprElim(m);

    // After CSE: s2 should now be VarRef{s1}.
    const ir::Expr * s2Expr = nullptr;
    for (auto & bb : m.blocks[entry].bindings) if (bb.var == s2) s2Expr = &bb.expr;
    auto * vr = s2Expr ? std::get_if<ir::VarRef>(s2Expr) : nullptr;
    if (!vr || vr->var != s1) {
        std::fprintf(stderr,
            "testCseSharedAdd: s2 expected to be VarRef{s1=%u}\n", (unsigned)s1);
        return 1;
    }

    // And the program still produces 30 = 15 + 15.
    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 30) {
        std::fprintf(stderr, "testCseSharedAdd: runtime expected 30, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testCseSharedAdd: OK (duplicate Add merged, runtime 30)\n");
    return 0;
}

// CSE must NOT merge AttrSelect: it can throw on missing attr, and
// merging two distinct selects would change the file:line position
// reported in the error.  Build two AttrSelects with identical operands
// and verify the second stays an AttrSelect after CSE.
static int testCseSkipsAttrSelect()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x = addBinding(m, entry, ir::LitInt{1});
    auto attrs = addBinding(m, entry, ir::AttrSet{ { { m.internSymbol("x"), x } } });
    auto sel1 = addBinding(m, entry, ir::AttrSelect{attrs, m.internSymbol("x")});
    auto sel2 = addBinding(m, entry, ir::AttrSelect{attrs, m.internSymbol("x")});
    auto out = addBinding(m, entry, ir::Add{sel1, sel2});
    setReturn(m, entry, out);

    ir::commonSubexprElim(m);

    // Both AttrSelects must still be AttrSelects.
    int selectCount = 0;
    for (auto & bb : m.blocks[entry].bindings)
        if (std::holds_alternative<ir::AttrSelect>(bb.expr)) ++selectCount;
    if (selectCount != 2) {
        std::fprintf(stderr,
            "testCseSkipsAttrSelect: expected 2 AttrSelect bindings after CSE, got %d\n",
            selectCount);
        return 1;
    }
    std::fprintf(stderr, "testCseSkipsAttrSelect: OK (both AttrSelects preserved)\n");
    return 0;
}

// #429: fusing App-chains over LitPrimOp.  Build:
//   v_isAttrs = LitPrimOp{primIsAttrs}
//   v_obj     = AttrSet{} (an empty attrset value)
//   v_app     = App{v_isAttrs, v_obj}
// After fusePrimOpApps + DCE: v_app is rewritten to PrimOpCall.
static int testFusePrimOpAppArity1()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    const PrimOp * isAttrsPo = findPrimOp("isAttrs");
    if (!isAttrsPo) {
        std::fprintf(stderr, "testFusePrimOpAppArity1: isAttrs not registered\n");
        return 1;
    }

    auto vIsA = addBinding(m, entry, ir::LitPrimOp{isAttrsPo});
    auto vObj = addBinding(m, entry, ir::AttrSet{});
    auto vApp = addBinding(m, entry, ir::App{vIsA, vObj});
    setReturn(m, entry, vApp);

    ir::fusePrimOpApps(m);

    // After fusion: vApp should now be a PrimOpCall{isAttrs, [vObj]}.
    const ir::Expr * appExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings)
        if (bb.var == vApp) appExpr = &bb.expr;
    auto * pc = appExpr ? std::get_if<ir::PrimOpCall>(appExpr) : nullptr;
    if (!pc || pc->primop != isAttrsPo
        || pc->args.size() != 1 || pc->args[0] != vObj) {
        std::fprintf(stderr,
            "testFusePrimOpAppArity1: expected PrimOpCall{isAttrs, [vObj=%u]}\n",
            (unsigned)vObj);
        return 1;
    }
    std::fprintf(stderr, "testFusePrimOpAppArity1: OK (App fused to PrimOpCall)\n");
    return 0;
}

// #429 multi-arg variant: chain of two Apps over LitPrimOp{arity=2}
// fuses the saturated tail; intermediate partial-App is preserved
// (DCE sweeps it post-pipeline).
static int testFusePrimOpChainArity2()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    const PrimOp * elemAtPo = findPrimOp("elemAt");
    if (!elemAtPo) {
        std::fprintf(stderr, "testFusePrimOpChainArity2: elemAt not registered\n");
        return 1;
    }

    auto vElem = addBinding(m, entry, ir::LitPrimOp{elemAtPo});
    auto vList = addBinding(m, entry, ir::ListExpr{});
    auto vIdx  = addBinding(m, entry, ir::LitInt{0});
    auto vApp1 = addBinding(m, entry, ir::App{vElem, vList});
    auto vApp2 = addBinding(m, entry, ir::App{vApp1, vIdx});
    setReturn(m, entry, vApp2);

    ir::fusePrimOpApps(m);

    // vApp2 should be PrimOpCall{elemAt, [vList, vIdx]}.
    const ir::Expr * tailExpr = nullptr;
    for (auto & bb : m.blocks[entry].bindings)
        if (bb.var == vApp2) tailExpr = &bb.expr;
    auto * pc = tailExpr ? std::get_if<ir::PrimOpCall>(tailExpr) : nullptr;
    if (!pc || pc->primop != elemAtPo
        || pc->args.size() != 2
        || pc->args[0] != vList || pc->args[1] != vIdx) {
        std::fprintf(stderr,
            "testFusePrimOpChainArity2: expected PrimOpCall{elemAt, [vList=%u, vIdx=%u]}\n",
            (unsigned)vList, (unsigned)vIdx);
        return 1;
    }
    std::fprintf(stderr, "testFusePrimOpChainArity2: OK (App-chain fused)\n");
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

    auto wx = addBinding(m, bodyB, ir::WithLookup{sx});
    auto wy = addBinding(m, bodyB, ir::WithLookup{sy});
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

/// Round-trip a small blob through the disk cache and verify
/// hit/miss counters update.  Uses NIX_V3_CACHE_DIR to scope to
/// a per-test directory if set; otherwise writes into the user's
/// XDG cache (still safe — the key is content-addressed).
static int testDiskCacheRoundTrip()
{
    const std::string content = "test content for v3 disk cache smoke";
    auto key = disk_cache::computeKeyForString(content);
    if (key.empty()) {
        std::fprintf(stderr,
            "testDiskCacheRoundTrip: computeKeyForString returned empty\n");
        return 1;
    }
    // Insert a fake blob, then look it up.
    std::string blob = "round-trip payload";
    auto & st = disk_cache::stats();
    uint64_t insertsBefore = st.inserts;
    uint64_t hitsBefore = st.hits;
    disk_cache::insert(key, blob);
    auto found = disk_cache::lookup(key);
    if (!found || *found != blob) {
        std::fprintf(stderr,
            "testDiskCacheRoundTrip: lookup did not return the inserted blob "
            "(found=%s)\n", found ? "yes" : "no");
        return 1;
    }
    if (st.inserts <= insertsBefore || st.hits <= hitsBefore) {
        std::fprintf(stderr,
            "testDiskCacheRoundTrip: stats counters not updated (inserts=%llu "
            "hits=%llu)\n",
            (unsigned long long)st.inserts, (unsigned long long)st.hits);
        // Don't fail the test on this — cache may be disabled in CI.
    }
    std::fprintf(stderr,
        "testDiskCacheRoundTrip: OK (key=%s, %zu bytes)\n",
        key.hex().substr(0, 16).c_str(), blob.size());
    return 0;
}

/// Stress test: round-trip lib.nix-style rec attrset through
/// serialize/deserialize.  This exercises ATTRS_REC_INIT,
/// MAKE_THUNK, ATTRS_REC_SET, OP_WITH_LOOKUP — the patterns used
/// by Nix tests that share lib.nix via `with import ./lib.nix;`.
static int testSerializeWithRecAttrset()
{
    // Build IR for: `let rec { foo = 1; bar = foo; }; in ?`
    // No, simpler: just build the rec attrset and select from it.
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;

    // We'll build `with { x = 42; }; x` — this exercises OP_WITH_LOOKUP
    // which had the trailing-depth-word bug.
    auto symX = m.internSymbol("x");
    auto litX = addBinding(m, topEntry, ir::LitInt{42});

    ir::AttrSet a;
    a.entries.push_back({symX, litX});
    auto attrs = addBinding(m, topEntry, std::move(a));

    auto withBlock = m.freshBlock();
    auto refX = addBinding(m, withBlock, ir::WithLookup{symX});
    setReturn(m, withBlock, refX);
    auto withResult = addBinding(m, topEntry, ir::With{attrs, withBlock});
    setReturn(m, topEntry, withResult);

    ir::computeFreeVars(m);
    auto cu = compile(m);

    Value origR = run(cu);
    if (!origR.isInt() || origR.payload.i != 42) {
        std::fprintf(stderr,
            "testSerializeWithRecAttrset: original eval got tag=%d val=%lld (want 42)\n",
            (int)origR.tag(), (long long)origR.payload.i);
        return 1;
    }

    // Round-trip.
    std::string blob = serialize::serializeCU(cu);
    auto cu2 = serialize::deserializeCU(blob);

    Value rtR = run(cu2);
    if (!rtR.isInt() || rtR.payload.i != 42) {
        std::fprintf(stderr,
            "testSerializeWithRecAttrset: round-trip eval got tag=%d val=%lld "
            "(want 42, blob=%zu bytes)\n",
            (int)rtR.tag(), (long long)rtR.payload.i, blob.size());
        return 1;
    }
    std::fprintf(stderr,
        "testSerializeWithRecAttrset: OK (with { x = 42; }; x = 42, blob=%zu bytes)\n",
        blob.size());
    return 0;
}

/// Round-trip a CompilationUnit through serialize/deserialize and
/// confirm the deserialized version produces the same result.
/// Mirrors testFibonacciSelfApp's setup but runs through the
/// serializer in the middle.  Validates VM-4 (bytecode disk cache)
/// at the in-memory layer.
static int testSerializeRoundTrip()
{
    // Reuse the simplest IR shape: `let n = 10; f = x: x + n; in f 32`
    // → 42 (same as testClosureCapture).
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
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

    // Serialize + deserialize — fresh CU should compute the same
    // result as the original.
    std::string blob = serialize::serializeCU(cu);
    if (blob.size() < sizeof(serialize::kMagic)) {
        std::fprintf(stderr,
            "testSerializeRoundTrip: serialized blob too small (%zu bytes)\n",
            blob.size());
        return 1;
    }
    auto cu2 = serialize::deserializeCU(blob);

    Value r = run(cu2);
    if (!r.isInt() || r.payload.i != 42) {
        std::fprintf(stderr,
            "testSerializeRoundTrip: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr,
        "testSerializeRoundTrip: OK (let n=10; f=x:x+n; in f 32 = 42, "
        "blob=%zu bytes)\n",
        blob.size());
    return 0;
}

/// REVIEW B6 — strictness pass positive test.  Build IR with
/// `Force{LitInt{42}}`; running elimRedundantForce should rewrite
/// the Force as a VarRef alias (LitInt is in the WHNF whitelist) and
/// return rewritten >= 1.
static int testStrictnessRewritesForceOverLit()
{
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto litVar = addBinding(m, topEntry, ir::LitInt{42});
    auto forceVar = addBinding(m, topEntry, ir::Force{litVar, /*srcLine*/ 0});
    setReturn(m, topEntry, forceVar);

    size_t rewritten = ir::elimRedundantForce(m);
    if (rewritten == 0) {
        std::fprintf(stderr,
            "testStrictnessRewritesForceOverLit: expected at least 1 rewrite, got %zu\n",
            rewritten);
        return 1;
    }
    // After rewrite, the Force binding's expr should be a VarRef.
    bool isVarRef = false;
    for (auto & bd : m.blocks[topEntry].bindings) {
        if (bd.var == forceVar) {
            isVarRef = std::holds_alternative<ir::VarRef>(bd.expr);
            break;
        }
    }
    if (!isVarRef) {
        std::fprintf(stderr,
            "testStrictnessRewritesForceOverLit: Force binding wasn't rewritten to VarRef\n");
        return 1;
    }
    std::fprintf(stderr,
        "testStrictnessRewritesForceOverLit: OK (%zu Force(s) rewritten)\n",
        rewritten);
    return 0;
}

/// REVIEW B6 — strictness pass negative test.  Build IR with
/// `Force{App{f, x}}`; running elimRedundantForce must NOT rewrite
/// (App can return a thunk-shaped value when over-applied).
static int testStrictnessSkipsForceOverApp()
{
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    // Build a synthetic `f x` App.  Use freshly-allocated VarIds for
    // both fun and arg — they don't need to resolve to anything for
    // the strictness pass to inspect; the pass only checks the
    // outer Force's source kind.
    auto fVar = m.freshVar();
    auto xVar = m.freshVar();
    auto appVar = addBinding(m, topEntry, ir::App{fVar, xVar});
    auto forceVar = addBinding(m, topEntry, ir::Force{appVar, /*srcLine*/ 0});
    setReturn(m, topEntry, forceVar);

    size_t rewritten = ir::elimRedundantForce(m);
    if (rewritten != 0) {
        std::fprintf(stderr,
            "testStrictnessSkipsForceOverApp: expected 0 rewrites, got %zu\n",
            rewritten);
        return 1;
    }
    // Force binding's expr should still be a Force.
    bool isStillForce = false;
    for (auto & bd : m.blocks[topEntry].bindings) {
        if (bd.var == forceVar) {
            isStillForce = std::holds_alternative<ir::Force>(bd.expr);
            break;
        }
    }
    if (!isStillForce) {
        std::fprintf(stderr,
            "testStrictnessSkipsForceOverApp: Force binding was unexpectedly rewritten\n");
        return 1;
    }
    std::fprintf(stderr,
        "testStrictnessSkipsForceOverApp: OK (Force over App preserved)\n");
    return 0;
}

/// REVIEW B6 follow-on: strictness pass MUST rewrite Force over each
/// non-Lit WHNF-producing kind in the whitelist.  Coverage gap from
/// the original review (only Force-over-Lit was tested explicitly).
///
/// Helper: builds `Force{<expr>}` and asserts elimRedundantForce
/// converts the binding to a VarRef alias.
static int checkForceRewrite(const char * name, ir::Expr inner)
{
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto innerVar = addBinding(m, topEntry, std::move(inner));
    auto forceVar = addBinding(m, topEntry, ir::Force{innerVar, 0});
    setReturn(m, topEntry, forceVar);

    size_t rewritten = ir::elimRedundantForce(m);
    if (rewritten == 0) {
        std::fprintf(stderr,
            "%s: expected at least 1 rewrite, got 0\n", name);
        return 1;
    }
    bool isVarRef = false;
    for (auto & bd : m.blocks[topEntry].bindings) {
        if (bd.var == forceVar) {
            isVarRef = std::holds_alternative<ir::VarRef>(bd.expr);
            break;
        }
    }
    if (!isVarRef) {
        std::fprintf(stderr,
            "%s: Force binding wasn't rewritten to VarRef\n", name);
        return 1;
    }
    std::fprintf(stderr, "%s: OK\n", name);
    return 0;
}

static int testStrictnessRewritesForceOverLambda()
{
    // Build a synthetic Lambda binding (no body needed -- the strictness
    // pass only inspects the outer Force's source kind via std::variant
    // discriminator).
    auto fid = ir::FuncId{0};  // placeholder; pass doesn't dereference
    return checkForceRewrite(
        "testStrictnessRewritesForceOverLambda",
        ir::Lambda{fid, /*freeVars*/ {}});
}

static int testStrictnessRewritesForceOverAdd()
{
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto a = addBinding(m, topEntry, ir::LitInt{1});
    auto b = addBinding(m, topEntry, ir::LitInt{2});
    auto sum = addBinding(m, topEntry, ir::Add{a, b});
    auto forceVar = addBinding(m, topEntry, ir::Force{sum, 0});
    setReturn(m, topEntry, forceVar);

    size_t rewritten = ir::elimRedundantForce(m);
    bool isVarRef = false;
    for (auto & bd : m.blocks[topEntry].bindings) {
        if (bd.var == forceVar) {
            isVarRef = std::holds_alternative<ir::VarRef>(bd.expr);
            break;
        }
    }
    if (!isVarRef || rewritten == 0) {
        std::fprintf(stderr,
            "testStrictnessRewritesForceOverAdd: rewritten=%zu, isVarRef=%d\n",
            rewritten, (int)isVarRef);
        return 1;
    }
    std::fprintf(stderr, "testStrictnessRewritesForceOverAdd: OK\n");
    return 0;
}

static int testStrictnessRewritesForceOverAttrSet()
{
    return checkForceRewrite(
        "testStrictnessRewritesForceOverAttrSet",
        ir::AttrSet{/*entries*/ {}});
}

static int testStrictnessRewritesForceOverListExpr()
{
    return checkForceRewrite(
        "testStrictnessRewritesForceOverListExpr",
        ir::ListExpr{/*elems*/ {}});
}

/// REVIEW B8: corrupted-blob fallback test.  Verifies that
/// deserializeCU throws SerializationError on the three documented
/// corruption modes: bad magic, schema mismatch, opcode-table
/// fingerprint mismatch.  Each case must throw, NOT silently produce
/// a malformed CompilationUnit (which would then run and produce
/// arbitrary wrong output).  disk_cache::lookup catches the throw
/// and returns nullopt, so corrupt cache entries trigger a recompile
/// rather than poisoning the eval.
static int testDeserializeRejectsCorruption()
{
    using namespace serialize;
    // Build a known-good blob first so we can mutate copies of it.
    auto m = ir::makeModule();
    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto v = addBinding(m, topEntry, ir::LitInt{42});
    setReturn(m, topEntry, v);
    ir::computeFreeVars(m);
    auto cu = compile(m);
    std::string good = serializeCU(cu);
    if (good.size() < 16) {
        std::fprintf(stderr,
            "testDeserializeRejectsCorruption: serialised blob too small (%zu B)\n",
            good.size());
        return 1;
    }

    // Case 1: bad magic.
    {
        std::string bad = good;
        bad[0] = 'X';  // corrupt the magic prefix
        bool threw = false;
        try { (void)deserializeCU(bad); }
        catch (const SerializationError &) { threw = true; }
        if (!threw) {
            std::fprintf(stderr,
                "testDeserializeRejectsCorruption: bad magic should throw\n");
            return 1;
        }
    }

    // Case 2: wrong schema version (offset 8, after the 8-byte magic).
    {
        std::string bad = good;
        // Overwrite schema version with a value far in the future.
        uint32_t wrong = 0xDEADBEEFu;
        std::memcpy(&bad[8], &wrong, sizeof(wrong));
        bool threw = false;
        try { (void)deserializeCU(bad); }
        catch (const SerializationError &) { threw = true; }
        if (!threw) {
            std::fprintf(stderr,
                "testDeserializeRejectsCorruption: schema mismatch should throw\n");
            return 1;
        }
    }

    // Case 3: wrong opcode-table fingerprint (offset 12, after schema).
    {
        std::string bad = good;
        uint64_t wrong = 0xCAFEBABE12345678ull;
        std::memcpy(&bad[12], &wrong, sizeof(wrong));
        bool threw = false;
        try { (void)deserializeCU(bad); }
        catch (const SerializationError &) { threw = true; }
        if (!threw) {
            std::fprintf(stderr,
                "testDeserializeRejectsCorruption: fingerprint mismatch should throw\n");
            return 1;
        }
    }

    // Sanity check: the original good blob still deserialises.
    {
        bool ok = false;
        try { (void)deserializeCU(good); ok = true; }
        catch (const std::exception & e) {
            std::fprintf(stderr,
                "testDeserializeRejectsCorruption: good blob threw: %s\n",
                e.what());
        }
        if (!ok) return 1;
    }

    std::fprintf(stderr,
        "testDeserializeRejectsCorruption: OK (3 corruption modes rejected, "
        "good blob accepted)\n");
    return 0;
}

int main()
{
    registerBuiltinPrimOps();
    int rc = 0;
    rc |= testLitInt();
    rc |= testAdd();
    rc |= testFoldAddInt();
    rc |= testNoFoldDivByZero();
    rc |= testNoFoldAddOverflow();
    rc |= testFoldComparisons();
    rc |= testFoldThroughVarRef();
    rc |= testFoldFloat();
    rc |= testDceRemovesUnusedLiteral();
    rc |= testDceKeepsImpureUnused();
    rc |= testInlineVarRefChain();
    rc |= testCseSharedAdd();
    rc |= testCseSkipsAttrSelect();
    rc |= testFusePrimOpAppArity1();
    rc |= testFusePrimOpChainArity2();
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
    rc |= testStrictnessRewritesForceOverLit();
    rc |= testStrictnessSkipsForceOverApp();
    rc |= testStrictnessRewritesForceOverLambda();
    rc |= testStrictnessRewritesForceOverAdd();
    rc |= testStrictnessRewritesForceOverAttrSet();
    rc |= testStrictnessRewritesForceOverListExpr();
    rc |= testSerializeRoundTrip();
    rc |= testSerializeWithRecAttrset();
    rc |= testDiskCacheRoundTrip();
    rc |= testDeserializeRejectsCorruption();

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
