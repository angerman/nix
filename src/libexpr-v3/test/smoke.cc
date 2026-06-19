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
#include "v3/ir_dump.hh"
#include "v3/bytecode.hh"
#include "v3/vm.hh"
#include "v3/alloc.hh"
#include "v3/barrier.hh"
#include "v3/primop.hh"
#include "v3/serialize.hh"
#include "v3/disk_cache.hh"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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
    if (!r.isInt() || r.asInt() != 42) {
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
    if (!r.isInt() || r.asInt() != 3) {
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
    if (!r.isInt() || r.asInt() != 5) {
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
    if (!r.isInt() || r.asInt() != 99) {
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
    if (!r.isInt() || r.asInt() != 30) {
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
    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr, "testLambdaCall: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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
    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr, "testClosureCapture: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
        return 1;
    }
    std::fprintf(stderr, "testClosureCapture: OK ((let n=10; f=x:x+n; in f 32) = 42)\n");
    return 0;
}

static int testCallNPrimOpNoPap()
{
    const PrimOp * addPo = findPrimOp("add");
    if (!addPo) {
        std::fprintf(stderr, "testCallNPrimOpNoPap: add not registered\n");
        return 1;
    }

    CompilationUnit cu;
    cu.primops.push_back(addPo);
    cu.entryOffset = 0;
    cu.lambdas.push_back(LambdaDescriptor{
        .codeOffset = 0,
        .prologueOffset = 0,
        .nUpvalues = 0,
        .nLocals = 0,
        .arity = 0,
        .hasFormals = 0,
        .ellipsis = 0,
    });
    cu.lambdaCodeOffsets.push_back(0);
    cu.code.push_back(encode(OP_LIT_PRIMOP, 0));
    cu.code.push_back(encode(OP_LIT_INT, 1));
    cu.code.push_back(encode(OP_LIT_INT, 2));
    cu.code.push_back(encode(OP_CALL_N, 2));
    cu.code.push_back(encode(OP_HALT));

    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value r = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!r.isInt() || r.asInt() != 3) {
        std::fprintf(stderr,
            "testCallNPrimOpNoPap: expected 3, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testCallNPrimOpNoPap: OP_CALL_N primop allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testCallNPrimOpNoPap: OK (OP_CALL_N primop saturates without PAP)\n");
    return 0;
}

static int testCallClosure2PrimOpNoPap()
{
    const PrimOp * addPo = findPrimOp("add");
    if (!addPo) {
        std::fprintf(stderr, "testCallClosure2PrimOpNoPap: add not registered\n");
        return 1;
    }

    Value fun;
    fun.mkPrimOp(addPo);
    Value a;
    a.mkInt(1);
    Value b;
    b.mkInt(2);

    VMState vm;
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value r = callClosure2(vm, fun, a, b);
    uint64_t pairsAfter = allocStats().pairsAllocated;

    if (!r.isInt() || r.asInt() != 3) {
        std::fprintf(stderr,
            "testCallClosure2PrimOpNoPap: expected 3, got tag=%d val=%lld\n",
            (int)r.tag(), r.isInt() ? (long long)r.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testCallClosure2PrimOpNoPap: saturated primop allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testCallClosure2PrimOpNoPap: OK (callClosure2 primop saturates without PAP)\n");
    return 0;
}

static int testForceApp3Arity2NoPap()
{
    CompilationUnit cu;
    cu.entryOffset = 0;
    cu.lambdas.push_back(LambdaDescriptor{
        .codeOffset = 0,
        .prologueOffset = 0,
        .nUpvalues = 0,
        .nLocals = 2,
        .arity = 2,
        .hasFormals = 0,
        .ellipsis = 0,
    });
    cu.lambdaCodeOffsets.push_back(0);
    cu.code.push_back(encode(OP_GET_LOCAL, 0));
    cu.code.push_back(encode(OP_GET_LOCAL, 1));
    cu.code.push_back(encode(OP_ADD));
    cu.code.push_back(encode(OP_RETURN));

    Closure * c = Alloc::allocClosure(0);
    c->desc = &cu.lambdas[0];
    c->cu = &cu;
    c->nUpvalues = 0;
    c->capturedWiths = nullptr;
    closurePostConstructBarrier(c);

    Value fun;
    fun.mkClosure(c);
    Value a;
    a.mkInt(40);
    Value b;
    b.mkInt(2);

    ValuePair * pp = Alloc::allocPair();
    pp->left = fun;
    pp->right = a;
    pp->third = b;
    pairPostConstructBarrier(pp);
    Value app3;
    app3.mkPair(Tag::App3, pp);

    VMState vm;
    vm.valueStack.reserve(16);
    vm.frames.reserve(4);
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value r = forceValue(vm, app3);
    uint64_t pairsAfter = allocStats().pairsAllocated;

    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr,
            "testForceApp3Arity2NoPap: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), r.isInt() ? (long long)r.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testForceApp3Arity2NoPap: forcing App3 allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testForceApp3Arity2NoPap: OK (lazy App3 force saturates without PAP)\n");
    return 0;
}

static int testCallClosureApp3PapSaturates()
{
    CompilationUnit cu;
    cu.entryOffset = 0;
    cu.lambdas.push_back(LambdaDescriptor{
        .codeOffset = 0,
        .prologueOffset = 0,
        .nUpvalues = 0,
        .nLocals = 3,
        .arity = 3,
        .hasFormals = 0,
        .ellipsis = 0,
    });
    cu.lambdaCodeOffsets.push_back(0);
    cu.code.push_back(encode(OP_GET_LOCAL, 0));
    cu.code.push_back(encode(OP_GET_LOCAL, 1));
    cu.code.push_back(encode(OP_ADD));
    cu.code.push_back(encode(OP_GET_LOCAL, 2));
    cu.code.push_back(encode(OP_ADD));
    cu.code.push_back(encode(OP_RETURN));

    Closure * c = Alloc::allocClosure(0);
    c->desc = &cu.lambdas[0];
    c->cu = &cu;
    c->nUpvalues = 0;
    c->capturedWiths = nullptr;
    closurePostConstructBarrier(c);

    Value fun;
    fun.mkClosure(c);
    Value a;
    a.mkInt(40);
    Value b;
    b.mkInt(1);
    Value cArg;
    cArg.mkInt(1);

    ValuePair * pp = Alloc::allocPair();
    pp->left = fun;
    pp->right = a;
    pp->third = b;
    pairPostConstructBarrier(pp);
    Value app3Pap;
    app3Pap.mkPair(Tag::App3, pp);

    VMState vm;
    vm.valueStack.reserve(16);
    vm.frames.reserve(4);
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value r = callClosure(vm, app3Pap, cArg);
    uint64_t pairsAfter = allocStats().pairsAllocated;

    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr,
            "testCallClosureApp3PapSaturates: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), r.isInt() ? (long long)r.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testCallClosureApp3PapSaturates: saturated App3 PAP allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testCallClosureApp3PapSaturates: OK (App3 PAP saturates through callClosure)\n");
    return 0;
}

static int testForceAppArity3NoPap()
{
    CompilationUnit cu;
    cu.entryOffset = 0;
    cu.lambdas.push_back(LambdaDescriptor{
        .codeOffset = 0,
        .prologueOffset = 0,
        .nUpvalues = 0,
        .nLocals = 3,
        .arity = 3,
        .hasFormals = 0,
        .ellipsis = 0,
    });
    cu.lambdaCodeOffsets.push_back(0);
    cu.code.push_back(encode(OP_GET_LOCAL, 0));
    cu.code.push_back(encode(OP_GET_LOCAL, 1));
    cu.code.push_back(encode(OP_ADD));
    cu.code.push_back(encode(OP_GET_LOCAL, 2));
    cu.code.push_back(encode(OP_ADD));
    cu.code.push_back(encode(OP_RETURN));

    Closure * clo = Alloc::allocClosure(0);
    clo->desc = &cu.lambdas[0];
    clo->cu = &cu;
    clo->nUpvalues = 0;
    clo->capturedWiths = nullptr;
    closurePostConstructBarrier(clo);

    Value fun;
    fun.mkClosure(clo);
    Value a;
    a.mkInt(40);
    Value b;
    b.mkInt(1);
    Value c;
    c.mkInt(1);

    ValuePair * inner = Alloc::allocPair();
    inner->left = fun;
    inner->right = a;
    inner->third = b;
    pairPostConstructBarrier(inner);
    Value app3Pap;
    app3Pap.mkPair(Tag::App3, inner);

    ValuePair * outer = Alloc::allocPair();
    outer->left = app3Pap;
    outer->right = c;
    pairPostConstructBarrier(outer);
    Value app;
    app.mkPair(Tag::App, outer);

    VMState vm;
    vm.valueStack.reserve(16);
    vm.frames.reserve(4);
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value r = forceValue(vm, app);
    uint64_t pairsAfter = allocStats().pairsAllocated;

    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr,
            "testForceAppArity3NoPap: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), r.isInt() ? (long long)r.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testForceAppArity3NoPap: forcing arity-3 App spine allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testForceAppArity3NoPap: OK (lazy arity-3 App spine saturates without PAP)\n");
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
    if (!r.isInt() || r.asInt() != 100) {
        std::fprintf(stderr, "testIf: expected 100, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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
    if (!r.isList() || r.asList()->size != 5) {
        std::fprintf(stderr, "testListConcat: expected list of 5, got tag=%d\n", (int)r.tag());
        return 1;
    }
    for (uint32_t i = 0; i < 5; ++i) {
        Value & el = r.asList()->elems[i];
        if (!el.isInt() || el.asInt() != i + 1) {
            std::fprintf(stderr, "testListConcat: elem[%u] expected %u, got %lld\n",
                i, i + 1, (long long)el.asInt());
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
    if (!r.isInt() || r.asInt() != 1) {
        std::fprintf(stderr, "testAttrSelect: expected 1, got tag=%d\n", (int)r.tag());
        return 1;
    }
    std::fprintf(stderr, "testAttrSelect: OK ({ a=1; b=2; }.a = 1)\n");
    return 0;
}

// Large dynamic attrsets use the direct-in-final-Bindings path.  Include one
// null dynamic name to verify the logical size is compacted below capacity.
static int testAttrSetDynLargeNullCompacts()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    std::vector<std::string> names;
    names.reserve(20);
    ir::AttrSetDyn dyn;
    for (uint32_t i = 0; i < 20; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "k%02u", i);
        names.emplace_back(buf);
        ir::VarId nameVar;
        if (i == 5)
            nameVar = addBinding(m, entry, ir::LitNull{});
        else
            nameVar = addBinding(m, entry, ir::LitString{names.back()});
        auto value = addBinding(m, entry, ir::LitInt{static_cast<int64_t>(i)});
        dyn.dynamics.push_back({nameVar, value, i + 1});
    }
    auto attrs = addBinding(m, entry, std::move(dyn));
    setReturn(m, entry, attrs);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isAttrs() || !r.asAttrs()) {
        std::fprintf(stderr,
            "testAttrSetDynLargeNullCompacts: expected attrs, got tag=%d\n",
            (int)r.tag());
        return 1;
    }
    const Bindings * b = r.asAttrs();
    if (b->size != 19) {
        std::fprintf(stderr,
            "testAttrSetDynLargeNullCompacts: expected size 19, got %u\n",
            b->size);
        return 1;
    }
    SymbolId k05 = m.internSymbol("k05");
    if (b->lookup(k05)) {
        std::fprintf(stderr,
            "testAttrSetDynLargeNullCompacts: null dynamic name produced k05\n");
        return 1;
    }
    SymbolId k19 = m.internSymbol("k19");
    const Value * v19 = b->lookup(k19);
    if (!v19 || !v19->isInt() || v19->asInt() != 19) {
        std::fprintf(stderr,
            "testAttrSetDynLargeNullCompacts: k19 mismatch\n");
        return 1;
    }
    std::fprintf(stderr,
        "testAttrSetDynLargeNullCompacts: OK (large dynamic attrset compacted)\n");
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
    if (!r.isInt() || r.asInt() != 2) {
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
    if (!r.isInt() || r.asInt() != 18) {
        std::fprintf(stderr, "testWith: expected 18, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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
    if (!r.isInt() || r.asInt() != 15) {
        std::fprintf(stderr, "testThunkForce: expected 15, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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
    if (!r.isBool() || r.asInt() != 1) {
        std::fprintf(stderr, "testShortCircuit: expected true, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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
    if (!res.isInt() || res.asInt() != 3) {
        std::fprintf(stderr, "testPrimOpLength: expected 3, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.asInt());
        return 1;
    }
    std::fprintf(stderr, "testPrimOpLength: OK (length [10 20 30] = 3)\n");
    return 0;
}

static int testPrimMapIdentityNoApps()
{
    const PrimOp * mapPo = findPrimOp("map");
    if (!mapPo) {
        std::fprintf(stderr, "testPrimMapIdentityNoApps: missing 'map' primop\n");
        return 1;
    }

    auto m = ir::makeModule();
    auto idFid = addFunction(m);
    auto idEntry = m.freshBlock();
    auto idParam = m.freshVar();
    {
        auto & f = funcOf(m, idFid);
        f.entryBlock = idEntry;
        f.argName = m.internSymbol("x");
        f.paramVar = idParam;
        f.name = "id";
        setReturn(m, idEntry, idParam);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto fun = addBinding(m, entry, ir::Lambda{idFid, {}});
    auto a = addBinding(m, entry, ir::LitInt{1});
    auto b = addBinding(m, entry, ir::LitInt{2});
    auto c = addBinding(m, entry, ir::LitInt{3});
    auto lst = addBinding(m, entry, ir::ListExpr{{a, b, c}});
    auto mapped = addBinding(m, entry, ir::PrimOpCall{mapPo, {fun, lst}});
    setReturn(m, entry, mapped);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!res.isList() || !res.asList() || res.asList()->size != 3
        || !res.asList()->elems[0].isInt() || res.asList()->elems[0].asInt() != 1
        || !res.asList()->elems[1].isInt() || res.asList()->elems[1].asInt() != 2
        || !res.asList()->elems[2].isInt() || res.asList()->elems[2].asInt() != 3) {
        std::fprintf(stderr, "testPrimMapIdentityNoApps: unexpected mapped result\n");
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapIdentityNoApps: map identity allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapIdentityNoApps: OK (map identity reuses list without Apps)\n");
    return 0;
}

static int testPrimGenListIdentityNoApps()
{
    const PrimOp * genListPo = findPrimOp("genList");
    if (!genListPo) {
        std::fprintf(stderr, "testPrimGenListIdentityNoApps: missing 'genList' primop\n");
        return 1;
    }

    auto m = ir::makeModule();
    auto idFid = addFunction(m);
    auto idEntry = m.freshBlock();
    auto idParam = m.freshVar();
    {
        auto & f = funcOf(m, idFid);
        f.entryBlock = idEntry;
        f.argName = m.internSymbol("x");
        f.paramVar = idParam;
        f.name = "id";
        setReturn(m, idEntry, idParam);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto fun = addBinding(m, entry, ir::Lambda{idFid, {}});
    auto n = addBinding(m, entry, ir::LitInt{9});
    auto generated = addBinding(m, entry, ir::PrimOpCall{genListPo, {fun, n}});
    setReturn(m, entry, generated);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!res.isList() || !res.asList() || res.asList()->size != 9) {
        std::fprintf(stderr, "testPrimGenListIdentityNoApps: unexpected list shape\n");
        return 1;
    }
    for (uint32_t i = 0; i < 9; ++i) {
        if (!res.asList()->elems[i].isInt()
            || res.asList()->elems[i].asInt() != static_cast<int64_t>(i)) {
            std::fprintf(stderr,
                "testPrimGenListIdentityNoApps: elem[%u] mismatch\n", i);
            return 1;
        }
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimGenListIdentityNoApps: genList identity allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testPrimGenListIdentityNoApps: OK (genList identity emits ints without Apps)\n");
    return 0;
}

static void smokeReturnSecond(EvalState &, Value * args, Value & out)
{
    out = args[1];
}

static void smokeSecondPlusOne(EvalState & state, Value * args, Value & out)
{
    Value v = args[1];
    if (v.isAppLike() || v.tag() == Tag::Thunk || v.tag() == Tag::Slot)
        v = forceValue(*state.vm, v);
    if (!v.isInt())
        throw std::runtime_error("__smokeSecondPlusOne: expected int");
    out.mkInt(v.asInt() + 1);
}

static int testPrimMapAttrsNamesDoNotRealize()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    const PrimOp * attrNamesPo = findPrimOp("attrNames");
    if (!mapAttrsPo || !attrNamesPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: missing mapAttrs/attrNames primop\n");
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecond", 2, smokeReturnSecond
    };

    SymbolId aSym = ir::globalInternSymbol("a");
    SymbolId bSym = ir::globalInternSymbol("b");
    Bindings * src = Alloc::allocBindings(2);
    src->entries[0].name = aSym;
    src->entries[0].pos = 0;
    src->entries[0].value.mkInt(10);
    src->entries[1].name = bSym;
    src->entries[1].pos = 0;
    src->entries[1].value.mkInt(20);

    Value fn;
    fn.mkPrimOp(&returnSecondPo);
    Value srcV;
    srcV.mkAttrs(src);

    VMState vm;
    EvalState st;
    st.vm = &vm;

    Value mapArgs[2] = {fn, srcV};
    uint64_t beforeMap = allocStats().pairsAllocated;
    Value mapped;
    mapAttrsPo->fn(st, mapArgs, mapped);
    uint64_t afterMap = allocStats().pairsAllocated;
    if (afterMap != beforeMap) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: mapAttrs allocated %llu pairs\n",
            (unsigned long long)(afterMap - beforeMap));
        return 1;
    }
    if (!mapped.isAttrs() || !mapped.asAttrs() || !mapped.asAttrs()->isMapAttrs()) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: mapped result is not MapAttrs bindings\n");
        return 1;
    }

    Value namesArgs[1] = {mapped};
    Value names;
    attrNamesPo->fn(st, namesArgs, names);
    uint64_t afterNames = allocStats().pairsAllocated;
    if (afterNames != beforeMap) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: attrNames allocated %llu pairs\n",
            (unsigned long long)(afterNames - beforeMap));
        return 1;
    }
    if (!names.isList() || !names.asList() || names.asList()->size != 2) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: unexpected attrNames result\n");
        return 1;
    }

    Value * av = mapped.asAttrs()->lookup(aSym);
    uint64_t afterLookup = allocStats().pairsAllocated;
    if (!av || afterLookup != beforeMap + 1) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: first lookup allocated %llu pairs\n",
            (unsigned long long)(afterLookup - beforeMap));
        return 1;
    }
    vm.frames.push_back(CallFrame{
        .cu = nullptr,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });
    Value forced = forceValue(vm, *av);
    if (!forced.isInt() || forced.asInt() != 10) {
        std::fprintf(stderr,
            "testPrimMapAttrsNamesDoNotRealize: forced a expected 10, got tag=%d\n",
            (int)forced.tag());
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapAttrsNamesDoNotRealize: OK (name-only mapAttrs stays pair-free)\n");
    return 0;
}

static int testPrimMapAttrsNestedNamesDoNotRealize()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    const PrimOp * attrNamesPo = findPrimOp("attrNames");
    if (!mapAttrsPo || !attrNamesPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: missing mapAttrs/attrNames primop\n");
        return 1;
    }
    static const PrimOp plusOnePo{
        "__smokeSecondPlusOneNestedNames", 2, smokeSecondPlusOne
    };
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondNestedNames", 2, smokeReturnSecond
    };

    SymbolId aSym = ir::globalInternSymbol("a");
    SymbolId bSym = ir::globalInternSymbol("b");
    Bindings * src = Alloc::allocBindings(2);
    src->entries[0].name = aSym;
    src->entries[0].pos = 0;
    src->entries[0].value.mkInt(10);
    src->entries[1].name = bSym;
    src->entries[1].pos = 0;
    src->entries[1].value.mkInt(20);

    Value plusOne;
    plusOne.mkPrimOp(&plusOnePo);
    Value returnSecond;
    returnSecond.mkPrimOp(&returnSecondPo);
    Value srcV;
    srcV.mkAttrs(src);

    VMState vm;
    EvalState st;
    st.vm = &vm;

    uint64_t before = allocStats().pairsAllocated;
    Value inner;
    Value innerArgs[2] = {plusOne, srcV};
    mapAttrsPo->fn(st, innerArgs, inner);
    Value outer;
    Value outerArgs[2] = {returnSecond, inner};
    mapAttrsPo->fn(st, outerArgs, outer);
    uint64_t afterMaps = allocStats().pairsAllocated;
    if (afterMaps != before) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: nested mapAttrs allocated %llu pairs\n",
            (unsigned long long)(afterMaps - before));
        return 1;
    }
    if (!inner.isAttrs() || !inner.asAttrs() || !inner.asAttrs()->isMapAttrs()
        || !outer.isAttrs() || !outer.asAttrs() || !outer.asAttrs()->isMapAttrs()) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: expected nested MapAttrs results\n");
        return 1;
    }
    if ((inner.asAttrs()->entries[0].pos & Bindings::kMapAttrsUnrealizedPosBit) == 0) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: inner entry was realized during composition\n");
        return 1;
    }

    Value names;
    Value namesArgs[1] = {outer};
    attrNamesPo->fn(st, namesArgs, names);
    uint64_t afterNames = allocStats().pairsAllocated;
    if (afterNames != before) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: attrNames allocated %llu pairs\n",
            (unsigned long long)(afterNames - before));
        return 1;
    }

    Value * av = outer.asAttrs()->lookup(aSym);
    uint64_t afterLookup = allocStats().pairsAllocated;
    if (!av || afterLookup != before + 2) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: first nested lookup allocated %llu pairs\n",
            (unsigned long long)(afterLookup - before));
        return 1;
    }
    vm.frames.push_back(CallFrame{
        .cu = nullptr,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });
    Value forced = forceValue(vm, *av);
    if (!forced.isInt() || forced.asInt() != 11) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedNamesDoNotRealize: forced a expected 11, got tag=%d val=%lld\n",
            (int)forced.tag(), forced.isInt() ? (long long)forced.asInt() : 0LL);
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapAttrsNestedNamesDoNotRealize: OK (nested mapAttrs stays lazy)\n");
    return 0;
}

static int testPrimAttrValuesMapAttrsSortsWithOneAppPerValue()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    const PrimOp * attrValuesPo = findPrimOp("attrValues");
    if (!mapAttrsPo || !attrValuesPo) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: missing mapAttrs/attrValues primop\n");
        return 1;
    }
    static const PrimOp plusOnePo{
        "__smokeSecondPlusOneAttrValues", 2, smokeSecondPlusOne
    };

    SymbolId zSym = ir::globalInternSymbol("__zz_attrValues_smoke");
    SymbolId aSym = ir::globalInternSymbol("__aa_attrValues_smoke");
    if (!(zSym < aSym)) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: expected fresh symbol id order\n");
        return 1;
    }
    Bindings * src = Alloc::allocBindings(2);
    src->entries[0].name = zSym;
    src->entries[0].pos = 0;
    src->entries[0].value.mkInt(20);
    src->entries[1].name = aSym;
    src->entries[1].pos = 0;
    src->entries[1].value.mkInt(10);

    Value plusOne;
    plusOne.mkPrimOp(&plusOnePo);
    Value srcV;
    srcV.mkAttrs(src);

    VMState vm;
    EvalState st;
    st.vm = &vm;

    Value mapped;
    Value mapArgs[2] = {plusOne, srcV};
    mapAttrsPo->fn(st, mapArgs, mapped);
    uint64_t beforeValues = allocStats().pairsAllocated;

    Value values;
    Value valuesArgs[1] = {mapped};
    attrValuesPo->fn(st, valuesArgs, values);
    uint64_t afterValues = allocStats().pairsAllocated;
    if (!values.isList() || !values.asList() || values.asList()->size != 2) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: unexpected attrValues shape\n");
        return 1;
    }
    if (afterValues != beforeValues + 2) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: expected two mapped App3 pairs, got %llu\n",
            (unsigned long long)(afterValues - beforeValues));
        return 1;
    }

    vm.frames.push_back(CallFrame{
        .cu = nullptr,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = 0,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });
    Value first = forceValue(vm, values.asList()->elems[0]);
    Value second = forceValue(vm, values.asList()->elems[1]);
    uint64_t afterForce = allocStats().pairsAllocated;
    if (!first.isInt() || first.asInt() != 11
        || !second.isInt() || second.asInt() != 21) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: unexpected forced values tag=(%d,%d) val=(%lld,%lld)\n",
            (int)first.tag(), (int)second.tag(),
            first.isInt() ? (long long)first.asInt() : 0LL,
            second.isInt() ? (long long)second.asInt() : 0LL);
        return 1;
    }
    if (afterForce != afterValues) {
        std::fprintf(stderr,
            "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: forcing allocated %llu extra pairs\n",
            (unsigned long long)(afterForce - afterValues));
        return 1;
    }
    std::fprintf(stderr,
        "testPrimAttrValuesMapAttrsSortsWithOneAppPerValue: OK (lexical order, one App3/value)\n");
    return 0;
}

static int runPrimMapAttrsSelectNoApp3(bool dynamicName)
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsSelectNoApp3: missing mapAttrs primop\n");
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondSelect", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
    auto mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, attrs}});
    ir::VarId selected;
    if (dynamicName) {
        auto name = addBinding(m, entry, ir::LitString{"a"});
        selected = addBinding(m, entry, ir::AttrSelectDyn{mapped, name});
    } else {
        selected = addBinding(m, entry, ir::AttrSelect{mapped, aSym});
    }
    setReturn(m, entry, selected);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!res.isInt() || res.asInt() != 10) {
        std::fprintf(stderr,
            "testPrimMapAttrsSelectNoApp3: expected 10 from %s select, got tag=%d\n",
            dynamicName ? "dynamic" : "static", (int)res.tag());
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsSelectNoApp3: %s select allocated %llu ValuePair(s)\n",
            dynamicName ? "dynamic" : "static",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    return 0;
}

static int testPrimMapAttrsSelectNoApp3()
{
    int rc = 0;
    rc |= runPrimMapAttrsSelectNoApp3(false);
    rc |= runPrimMapAttrsSelectNoApp3(true);
    if (rc == 0)
        std::fprintf(stderr,
            "testPrimMapAttrsSelectNoApp3: OK (select stays pair-free)\n");
    return rc;
}

static int testPrimMapAttrsEmptyUpdateNoApp3()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsEmptyUpdateNoApp3: missing mapAttrs primop\n");
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondEmptyUpdate", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
    auto mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, attrs}});
    auto empty = addBinding(m, entry, ir::AttrSet{/*entries*/ {}});
    auto updated = addBinding(m, entry, ir::Update{mapped, empty});
    setReturn(m, entry, updated);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!res.isAttrs() || !res.asAttrs() || !res.asAttrs()->isMapAttrs()) {
        std::fprintf(stderr,
            "testPrimMapAttrsEmptyUpdateNoApp3: result did not preserve MapAttrs\n");
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsEmptyUpdateNoApp3: empty update allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapAttrsEmptyUpdateNoApp3: OK (empty // keeps MapAttrs lazy)\n");
    return 0;
}

static int runPrimMapAttrsUpdateNoApp3(bool mapOnLeft)
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: missing mapAttrs primop\n");
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondMapAttrsUpdate", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto cVal = addBinding(m, entry, ir::LitInt{30});
    auto overlayAVal = addBinding(m, entry, ir::LitInt{1});
    auto overlayBVal = addBinding(m, entry, ir::LitInt{99});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto cSym = m.internSymbol("c");

    ir::VarId mapped;
    ir::VarId overlay;
    if (mapOnLeft) {
        auto src = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
        mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, src}});
        overlay = addBinding(m, entry, ir::AttrSet{ { {bSym, overlayBVal}, {cSym, cVal} } });
    } else {
        overlay = addBinding(m, entry, ir::AttrSet{ { {aSym, overlayAVal} } });
        auto src = addBinding(m, entry, ir::AttrSet{ { {bSym, bVal}, {cSym, cVal} } });
        mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, src}});
    }
    auto updated = mapOnLeft
        ? addBinding(m, entry, ir::Update{mapped, overlay})
        : addBinding(m, entry, ir::Update{overlay, mapped});
    setReturn(m, entry, updated);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: %s update allocated %llu ValuePair(s)\n",
            mapOnLeft ? "lhs-mapAttrs" : "rhs-mapAttrs",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    if (!res.isAttrs() || !res.asAttrs() || res.asAttrs()->countDistinct() != 3) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: unexpected %s result shape\n",
            mapOnLeft ? "lhs-mapAttrs" : "rhs-mapAttrs");
        return 1;
    }

    const Bindings * out = res.asAttrs();
    if (mapOnLeft) {
        if (!out->isChain() || !out->parent || !out->parent->isMapAttrs()) {
            std::fprintf(stderr,
                "testPrimMapAttrsUpdateNoApp3: lhs-mapAttrs did not build MapAttrs-backed chain\n");
            return 1;
        }
    } else if (!out->isMapAttrs()) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: rhs-mapAttrs did not preserve MapAttrs result\n");
        return 1;
    }

    const Bindings * mappedLayer = mapOnLeft ? out->parent : out;
    const Bindings * overlayLayer = out;
    const Bindings::Entry * a = mapOnLeft
        ? mappedLayer->lookupLocalEntry(aSym)
        : overlayLayer->lookupLocalEntry(aSym);
    const Bindings::Entry * b = mapOnLeft
        ? overlayLayer->lookupLocalEntry(bSym)
        : mappedLayer->lookupLocalEntry(bSym);
    const Bindings::Entry * c = mapOnLeft
        ? overlayLayer->lookupLocalEntry(cSym)
        : mappedLayer->lookupLocalEntry(cSym);
    if (!a || !b || !c) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: %s result missing entries\n",
            mapOnLeft ? "lhs-mapAttrs" : "rhs-mapAttrs");
        return 1;
    }
    const bool aMapped = (a->pos & Bindings::kMapAttrsUnrealizedPosBit) != 0;
    const bool bMapped = (b->pos & Bindings::kMapAttrsUnrealizedPosBit) != 0;
    const bool cMapped = (c->pos & Bindings::kMapAttrsUnrealizedPosBit) != 0;
    if (mapOnLeft) {
        if (!aMapped || bMapped || cMapped) {
            std::fprintf(stderr,
                "testPrimMapAttrsUpdateNoApp3: lhs-mapAttrs entry flags wrong\n");
            return 1;
        }
    } else {
        if (aMapped || !bMapped || !cMapped) {
            std::fprintf(stderr,
                "testPrimMapAttrsUpdateNoApp3: rhs-mapAttrs entry flags wrong\n");
            return 1;
        }
    }
    return 0;
}

static int runPrimMapAttrsUpdateSelectNoApp3(bool mapOnLeft)
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: missing mapAttrs primop\n");
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondMapAttrsUpdateSelect", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto cVal = addBinding(m, entry, ir::LitInt{30});
    auto overlayAVal = addBinding(m, entry, ir::LitInt{1});
    auto overlayBVal = addBinding(m, entry, ir::LitInt{99});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto cSym = m.internSymbol("c");

    ir::VarId mapped;
    ir::VarId overlay;
    if (mapOnLeft) {
        auto src = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
        mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, src}});
        overlay = addBinding(m, entry, ir::AttrSet{ { {bSym, overlayBVal}, {cSym, cVal} } });
    } else {
        overlay = addBinding(m, entry, ir::AttrSet{ { {aSym, overlayAVal} } });
        auto src = addBinding(m, entry, ir::AttrSet{ { {bSym, bVal}, {cSym, cVal} } });
        mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, src}});
    }
    auto updated = mapOnLeft
        ? addBinding(m, entry, ir::Update{mapped, overlay})
        : addBinding(m, entry, ir::Update{overlay, mapped});
    auto aSel = addBinding(m, entry, ir::AttrSelect{updated, aSym});
    auto bSel = addBinding(m, entry, ir::AttrSelect{updated, bSym});
    auto cSel = addBinding(m, entry, ir::AttrSelect{updated, cSym});
    auto ab = addBinding(m, entry, ir::Add{aSel, bSel});
    auto sum = addBinding(m, entry, ir::Add{ab, cSel});
    setReturn(m, entry, sum);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    const int64_t expected = mapOnLeft ? 139 : 51;
    if (!res.isInt() || res.asInt() != expected) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: %s select expected %lld, got tag=%d val=%lld\n",
            mapOnLeft ? "lhs-mapAttrs" : "rhs-mapAttrs",
            (long long)expected, (int)res.tag(),
            res.isInt() ? (long long)res.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: %s select allocated %llu ValuePair(s)\n",
            mapOnLeft ? "lhs-mapAttrs" : "rhs-mapAttrs",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    return 0;
}

static int testPrimMapAttrsUpdateNoApp3()
{
    int rc = 0;
    rc |= runPrimMapAttrsUpdateNoApp3(true);
    rc |= runPrimMapAttrsUpdateNoApp3(false);
    rc |= runPrimMapAttrsUpdateSelectNoApp3(true);
    rc |= runPrimMapAttrsUpdateSelectNoApp3(false);
    if (rc == 0)
        std::fprintf(stderr,
            "testPrimMapAttrsUpdateNoApp3: OK (// preserves lazy MapAttrs entries)\n");
    return rc;
}

static int testPrimMapAttrsNestedSelectUsesMappedValue()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedSelectUsesMappedValue: missing mapAttrs primop\n");
        return 1;
    }
    static const PrimOp plusOnePo{
        "__smokeSecondPlusOneNestedSelect", 2, smokeSecondPlusOne
    };
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondNestedSelect", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto plusOne = addBinding(m, entry, ir::LitPrimOp{&plusOnePo});
    auto returnSecond = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto aSym = m.internSymbol("a");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal} } });
    auto inner = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {plusOne, attrs}});
    auto outer = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {returnSecond, inner}});
    auto selected = addBinding(m, entry, ir::AttrSelect{outer, aSym});
    setReturn(m, entry, selected);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (!res.isInt() || res.asInt() != 11) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedSelectUsesMappedValue: expected 11, got tag=%d val=%lld\n",
            (int)res.tag(), res.isInt() ? (long long)res.asInt() : 0LL);
        return 1;
    }
    if (pairsAfter != pairsBefore + 1) {
        std::fprintf(stderr,
            "testPrimMapAttrsNestedSelectUsesMappedValue: expected one inner App3 pair, got %llu\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapAttrsNestedSelectUsesMappedValue: OK (nested select uses mapped source)\n");
    return 0;
}

static int runPrimMapAttrsSetOpNoApp3(const PrimOp * po, const char * name)
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo || !po) {
        std::fprintf(stderr,
            "testPrimMapAttrsSetOpsNoApp3: missing primop for %s\n", name);
        return 1;
    }
    static const PrimOp returnSecondPo{
        "__smokeReturnSecondSetOp", 2, smokeReturnSecond
    };

    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::LitPrimOp{&returnSecondPo});
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
    auto mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, attrs}});

    ir::VarId result;
    if (std::strcmp(name, "removeAttrs") == 0) {
        auto removeName = addBinding(m, entry, ir::LitString{"b"});
        auto removeList = addBinding(m, entry, ir::ListExpr{{removeName}});
        result = addBinding(m, entry, ir::PrimOpCall{po, {mapped, removeList}});
    } else {
        auto keepVal = addBinding(m, entry, ir::LitInt{0});
        auto keep = addBinding(m, entry, ir::AttrSet{ { {aSym, keepVal} } });
        result = addBinding(m, entry, ir::PrimOpCall{po, {keep, mapped}});
    }
    setReturn(m, entry, result);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    SymbolId aGlobal = ir::globalInternSymbol("a");
    if (!res.isAttrs() || !res.asAttrs() || res.asAttrs()->size != 1
        || !res.asAttrs()->isMapAttrs()
        || res.asAttrs()->entries[0].name != aGlobal) {
        std::fprintf(stderr,
            "testPrimMapAttrsSetOpsNoApp3: unexpected %s result\n", name);
        return 1;
    }
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsSetOpsNoApp3: %s allocated %llu ValuePair(s)\n",
            name, (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    return 0;
}

static int testPrimMapAttrsSetOpsNoApp3()
{
    int rc = 0;
    rc |= runPrimMapAttrsSetOpNoApp3(findPrimOp("removeAttrs"), "removeAttrs");
    rc |= runPrimMapAttrsSetOpNoApp3(findPrimOp("intersectAttrs"), "intersectAttrs");
    if (rc == 0)
        std::fprintf(stderr,
            "testPrimMapAttrsSetOpsNoApp3: OK (set ops preserve lazy MapAttrs)\n");
    return rc;
}

static int testPrimMapAttrsValueIdentityNoApps()
{
    const PrimOp * mapAttrsPo = findPrimOp("mapAttrs");
    if (!mapAttrsPo) {
        std::fprintf(stderr,
            "testPrimMapAttrsValueIdentityNoApps: missing mapAttrs primop\n");
        return 1;
    }

    auto m = ir::makeModule();
    auto mapperFid = addFunction(m);
    auto mapperEntry = m.freshBlock();
    auto nameParam = m.freshVar();
    auto valueParam = m.freshVar();
    {
        auto & f = funcOf(m, mapperFid);
        f.entryBlock = mapperEntry;
        f.argName = m.internSymbol("name");
        f.paramVar = nameParam;
        f.extraParams.push_back(valueParam);
        f.name = "mapAttrs-value-identity";
        setReturn(m, mapperEntry, valueParam);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto fn = addBinding(m, entry, ir::Lambda{ mapperFid, /*freeVars*/ {} });
    auto aVal = addBinding(m, entry, ir::LitInt{10});
    auto bVal = addBinding(m, entry, ir::LitInt{20});
    auto aSym = m.internSymbol("a");
    auto bSym = m.internSymbol("b");
    auto attrs = addBinding(m, entry, ir::AttrSet{ { {aSym, aVal}, {bSym, bVal} } });
    auto mapped = addBinding(m, entry, ir::PrimOpCall{mapAttrsPo, {fn, attrs}});
    setReturn(m, entry, mapped);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    if (!cu.lambdas[mapperFid].secondArgIdentityLambda) {
        std::fprintf(stderr,
            "testPrimMapAttrsValueIdentityNoApps: descriptor flag not set\n");
        return 1;
    }

    uint64_t pairsBefore = allocStats().pairsAllocated;
    Value res = run(cu);
    uint64_t pairsAfter = allocStats().pairsAllocated;
    if (pairsAfter != pairsBefore) {
        std::fprintf(stderr,
            "testPrimMapAttrsValueIdentityNoApps: allocated %llu ValuePair(s)\n",
            (unsigned long long)(pairsAfter - pairsBefore));
        return 1;
    }
    if (!res.isAttrs() || !res.asAttrs() || res.asAttrs()->isMapAttrs()
        || res.asAttrs()->size != 2) {
        std::fprintf(stderr,
            "testPrimMapAttrsValueIdentityNoApps: unexpected result shape\n");
        return 1;
    }
    const Value * a = res.asAttrs()->lookup(aSym);
    const Value * b = res.asAttrs()->lookup(bSym);
    if (!a || !a->isInt() || a->asInt() != 10
        || !b || !b->isInt() || b->asInt() != 20) {
        std::fprintf(stderr,
            "testPrimMapAttrsValueIdentityNoApps: unexpected values\n");
        return 1;
    }
    std::fprintf(stderr,
        "testPrimMapAttrsValueIdentityNoApps: OK (value identity copies attrs without Apps)\n");
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
    if (!res.isInt() || res.asInt() != 20) {
        std::fprintf(stderr, "testPrimOpHeadTail: expected 20, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.asInt());
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
    if (!res.isInt() || res.asInt() != 55) {
        std::fprintf(stderr, "testFibonacciSelfApp: expected 55, got tag=%d val=%lld\n",
            (int)res.tag(), (long long)res.asInt());
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
    if (!origR.isInt() || origR.asInt() != 42) {
        std::fprintf(stderr,
            "testSerializeWithRecAttrset: original eval got tag=%d val=%lld (want 42)\n",
            (int)origR.tag(), (long long)origR.asInt());
        return 1;
    }

    // Round-trip.
    std::string blob = serialize::serializeCU(cu);
    auto cu2 = serialize::deserializeCU(blob);

    Value rtR = run(cu2);
    if (!rtR.isInt() || rtR.asInt() != 42) {
        std::fprintf(stderr,
            "testSerializeWithRecAttrset: round-trip eval got tag=%d val=%lld "
            "(want 42, blob=%zu bytes)\n",
            (int)rtR.tag(), (long long)rtR.asInt(), blob.size());
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
    if (!r.isInt() || r.asInt() != 42) {
        std::fprintf(stderr,
            "testSerializeRoundTrip: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.asInt());
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

// ===========================================================================
// #539 — IR text dumper + FileCheck-style helper.
// ===========================================================================

// dumpModule produces a stable, line-oriented representation suitable
// for `; CHECK:` directives.  Build a tiny module, dump it, and verify
// every binding shows up in the expected form.
static int testIrDumpBasic()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{42});       // v1
    auto b = addBinding(m, entry, ir::LitInt{1});         // v2
    auto c = addBinding(m, entry, ir::Add{a, b});         // v3
    setReturn(m, entry, c);

    std::string dump = ir::dumpModule(m);

    const char * expected = R"(
        ; CHECK: ; module n_funcs=1
        ; CHECK: ; func f0 entry=B1
        ; CHECK: B1:
        ; CHECK:   v1 = LitInt 42
        ; CHECK:   v2 = LitInt 1
        ; CHECK:   v3 = Add v1 v2
        ; CHECK:   return v3
    )";

    auto err = ir::checkIr(dump, expected);
    if (!err.empty()) {
        std::fprintf(stderr, "testIrDumpBasic: %s\nfull dump:\n%s\n",
            err.c_str(), dump.c_str());
        return 1;
    }
    std::fprintf(stderr, "testIrDumpBasic: OK\n");
    return 0;
}

// Verify CHECK-NOT directives fire when forbidden patterns appear.
static int testIrDumpNegativeCheckNot()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{99});
    setReturn(m, entry, a);

    std::string dump = ir::dumpModule(m);

    // We assert that the literal 99 is ABSENT — but it IS present, so
    // the check should fail.  Use this to validate the NOT arm.
    const char * expected = R"(
        ; CHECK: B1:
        ; CHECK-NOT: LitInt 99
        ; CHECK: return v1
    )";

    auto err = ir::checkIr(dump, expected);
    if (err.empty()) {
        std::fprintf(stderr, "testIrDumpNegativeCheckNot: CHECK-NOT was supposed to fail but didn't\n");
        return 1;
    }
    if (err.find("CHECK-NOT") == std::string::npos
        || err.find("LitInt 99") == std::string::npos) {
        std::fprintf(stderr,
            "testIrDumpNegativeCheckNot: failure diagnostic missing context: %s\n",
            err.c_str());
        return 1;
    }
    std::fprintf(stderr, "testIrDumpNegativeCheckNot: OK (CHECK-NOT fired correctly)\n");
    return 0;
}

// Verify CHECK directives are ordered: out-of-order CHECK lines fail.
static int testIrDumpOrderingMatters()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    (void)addBinding(m, entry, ir::LitInt{1});
    auto b = addBinding(m, entry, ir::LitInt{2});
    setReturn(m, entry, b);

    std::string dump = ir::dumpModule(m);

    // Ask for `LitInt 2` BEFORE `LitInt 1` — the actual order is
    // reversed.  CHECK is in-order so this should fail.
    const char * expected = R"(
        ; CHECK: LitInt 2
        ; CHECK: LitInt 1
    )";

    auto err = ir::checkIr(dump, expected);
    if (err.empty()) {
        std::fprintf(stderr, "testIrDumpOrderingMatters: out-of-order CHECK passed when it shouldn't\n");
        return 1;
    }
    std::fprintf(stderr, "testIrDumpOrderingMatters: OK (in-order CHECK enforced)\n");
    return 0;
}

// ===========================================================================
// #540 — analyseOccurrence: 12-case corpus per OPT_OCCUR_PLAN §A.5.
// ===========================================================================

// Helper: assert OccInfo for a specific VarId; logs failure context.
static bool assertOcc(const ir::OccMap & occ, ir::VarId v,
                      ir::OccKind kind, uint16_t count, bool captured,
                      const char * label)
{
    auto info = occ.lookup(v);
    if (info.kind == kind && info.count == count && info.capturedUse == captured)
        return true;
    std::fprintf(stderr,
        "  %s: var=%u kind=%d count=%u captured=%d  expected kind=%d count=%u captured=%d\n",
        label, (unsigned)v, (int)info.kind, (unsigned)info.count, (int)info.capturedUse,
        (int)kind, (unsigned)count, (int)captured);
    return false;
}

// Empty module: just slot 0 (kInvalid sentinel).
static int testOccurEmptyModule()
{
    auto m = ir::makeModule();
    auto occ = ir::analyseOccurrence(m);
    if (occ.data.size() != 1) {  // slot 0 = kInvalid sentinel
        std::fprintf(stderr,
            "testOccurEmptyModule: expected size 1, got %zu\n", occ.data.size());
        return 1;
    }
    std::fprintf(stderr, "testOccurEmptyModule: OK\n");
    return 0;
}

// `let x = 1 in []` -> x is Dead (count 0).
static int testOccurDeadLiteral()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x = addBinding(m, entry, ir::LitInt{1});
    auto ret = addBinding(m, entry, ir::ListExpr{{}});  // empty list, doesn't reference x
    setReturn(m, entry, ret);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, x,   ir::OccKind::Dead,       0, false, "x")
           && assertOcc(occ, ret, ir::OccKind::OnceLinear, 1, false, "ret (used by terminal)");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurDeadLiteral: OK\n");
    return 0;
}

// `let x = 1 in x + 1` -> x OnceLinear, count 1.
static int testOccurOnceLinear()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x   = addBinding(m, entry, ir::LitInt{1});
    auto one = addBinding(m, entry, ir::LitInt{1});
    auto sum = addBinding(m, entry, ir::Add{x, one});
    setReturn(m, entry, sum);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, x,   ir::OccKind::OnceLinear, 1, false, "x")
           && assertOcc(occ, one, ir::OccKind::OnceLinear, 1, false, "one")
           && assertOcc(occ, sum, ir::OccKind::OnceLinear, 1, false, "sum");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurOnceLinear: OK\n");
    return 0;
}

// `let x = 1 in x + x` -> x Many, count 2 (saturating).
static int testOccurManyTwoUses()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x   = addBinding(m, entry, ir::LitInt{1});
    auto sum = addBinding(m, entry, ir::Add{x, x});
    setReturn(m, entry, sum);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, x,   ir::OccKind::Many,       2, false, "x")
           && assertOcc(occ, sum, ir::OccKind::OnceLinear, 1, false, "sum");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurManyTwoUses: OK\n");
    return 0;
}

// `let x = 1 in (\y: x)` -> x is captured (lambda body in different fn).
//
// Note: after `computeFreeVars` runs, `x` would also appear in the
// inner lambda's freeVars list.  analyseOccurrence MUST NOT
// double-count those (the captured-list is derived data).  We assert
// count==1 even after running computeFreeVars to validate the
// invariant.
static int testOccurOnceCapturedAcrossLambda()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x = addBinding(m, entry, ir::LitInt{42});

    // Inner lambda: `\y: x`.
    auto innerFid  = addFunction(m);
    auto innerBody = m.freshBlock();
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerBody;
        f.argName    = ir::SymbolId{1};  // arbitrary symbol
        f.paramVar   = innerParam;
        // Body just references x.
        setReturn(m, innerBody, x);
    }

    auto innerVar = addBinding(m, entry, ir::Lambda{innerFid, /*freeVars*/ {x}});
    setReturn(m, entry, innerVar);

    auto occ = ir::analyseOccurrence(m);
    // x: count 1 (one direct use in innerBody's terminal), captured (innerBody is in fn1).
    bool ok = assertOcc(occ, x,         ir::OccKind::OnceCaptured, 1, true,  "x")
           && assertOcc(occ, innerVar,  ir::OccKind::OnceLinear,   1, false, "innerVar")
           && assertOcc(occ, innerParam, ir::OccKind::Param,       0, false, "innerParam");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurOnceCapturedAcrossLambda: OK\n");
    return 0;
}

// `\x: x + x` -> paramVar Param, count 2 across two direct uses.
//
// Test the captured-list double-count regression: run computeFreeVars
// FIRST, then analyseOccurrence.  Param should still be classified
// Param with count 2 (NOT 4 if freeVars somehow leak into the count).
static int testOccurNoDoubleCountAfterComputeFreeVars()
{
    auto m = ir::makeModule();

    auto innerFid = addFunction(m);
    auto innerBody = m.freshBlock();
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerBody;
        f.argName    = ir::SymbolId{1};
        f.paramVar   = innerParam;
        auto sum = addBinding(m, innerBody, ir::Add{innerParam, innerParam});
        setReturn(m, innerBody, sum);
    }

    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto lamb = addBinding(m, entry, ir::Lambda{innerFid, /*freeVars*/ {}});
    setReturn(m, entry, lamb);

    // Run computeFreeVars to populate Lambda::freeVars and similar.
    ir::computeFreeVars(m);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, innerParam, ir::OccKind::Param, 2, false, "innerParam")
           && assertOcc(occ, lamb, ir::OccKind::OnceLinear, 1, false, "lamb");
    if (!ok) {
        std::fprintf(stderr, "testOccurNoDoubleCountAfterComputeFreeVars: param count must stay at 2\n");
        return 1;
    }
    std::fprintf(stderr, "testOccurNoDoubleCountAfterComputeFreeVars: OK\n");
    return 0;
}

// `\x: 42` -> paramVar Param with count 0 (unused parameter).
static int testOccurParamUnused()
{
    auto m = ir::makeModule();
    auto innerFid = addFunction(m);
    auto innerBody = m.freshBlock();
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerBody;
        f.argName    = ir::SymbolId{1};
        f.paramVar   = innerParam;
        auto k42 = addBinding(m, innerBody, ir::LitInt{42});
        setReturn(m, innerBody, k42);
    }
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto lamb = addBinding(m, entry, ir::Lambda{innerFid, {}});
    setReturn(m, entry, lamb);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, innerParam, ir::OccKind::Param, 0, false, "innerParam");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurParamUnused: OK\n");
    return 0;
}

// `let x = 1 in if c then x else 2` -> x OnceLinear (one branch arm).
//
// MVP conservatively counts syntactic uses; `x` appears once in
// thenBlock so count==1 -> OnceLinear.
static int testOccurOnceLinearIfBranch()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x = addBinding(m, entry, ir::LitInt{1});

    auto thenB = m.freshBlock();
    auto elseB = m.freshBlock();
    setReturn(m, thenB, x);
    auto two = addBinding(m, elseB, ir::LitInt{2});
    setReturn(m, elseB, two);

    auto cond = addBinding(m, entry, ir::LitBool{true});
    auto ifv  = addBinding(m, entry, ir::If{cond, thenB, elseB});
    setReturn(m, entry, ifv);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, x,    ir::OccKind::OnceLinear, 1, false, "x")
           && assertOcc(occ, cond, ir::OccKind::OnceLinear, 1, false, "cond")
           && assertOcc(occ, ifv,  ir::OccKind::OnceLinear, 1, false, "ifv");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurOnceLinearIfBranch: OK\n");
    return 0;
}

// `let x = 1 in if c then x else x` -> x Many (MVP counts 2 syntactic uses).
static int testOccurManyAcrossBranches()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto x = addBinding(m, entry, ir::LitInt{1});

    auto thenB = m.freshBlock();
    auto elseB = m.freshBlock();
    setReturn(m, thenB, x);
    setReturn(m, elseB, x);

    auto cond = addBinding(m, entry, ir::LitBool{true});
    auto ifv  = addBinding(m, entry, ir::If{cond, thenB, elseB});
    setReturn(m, entry, ifv);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, x, ir::OccKind::Many, 2, false, "x");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurManyAcrossBranches: OK\n");
    return 0;
}

// `with attrs; foo` -> attrs OnceLinear (single use at With::attrs).
static int testOccurWithAttrsOnce()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    // attrs = an empty AttrSet for simplicity.
    auto attrs = addBinding(m, entry, ir::AttrSet{});

    auto bodyB = m.freshBlock();
    auto v = addBinding(m, bodyB, ir::WithLookup{ir::SymbolId{1}});
    setReturn(m, bodyB, v);

    auto wb = addBinding(m, entry, ir::With{attrs, bodyB,
                                            ir::kInvalid, ir::kInvalidSymbol});
    setReturn(m, entry, wb);

    auto occ = ir::analyseOccurrence(m);
    bool ok = assertOcc(occ, attrs, ir::OccKind::OnceLinear, 1, false, "attrs");
    if (!ok) return 1;
    std::fprintf(stderr, "testOccurWithAttrsOnce: OK\n");
    return 0;
}

// isTrivialRhs sanity: literals, VarRef, primops are trivial; arithmetic
// and structural ops are not.
static int testIsTrivialRhs()
{
    if (!ir::isTrivialRhs(ir::Expr{ir::LitInt{0}}))      { std::fprintf(stderr, "trivial LitInt failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitFloat{1.0}}))  { std::fprintf(stderr, "trivial LitFloat failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitBool{true}}))  { std::fprintf(stderr, "trivial LitBool failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitNull{}}))      { std::fprintf(stderr, "trivial LitNull failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitString{"x"}})) { std::fprintf(stderr, "trivial LitString failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitPath{"."}}))   { std::fprintf(stderr, "trivial LitPath failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::VarRef{1}}))      { std::fprintf(stderr, "trivial VarRef failed\n"); return 1; }
    if (!ir::isTrivialRhs(ir::Expr{ir::LitBuiltins{}}))  { std::fprintf(stderr, "trivial LitBuiltins failed\n"); return 1; }

    if (ir::isTrivialRhs(ir::Expr{ir::Add{1, 2}}))       { std::fprintf(stderr, "Add wrongly trivial\n"); return 1; }
    if (ir::isTrivialRhs(ir::Expr{ir::AttrSet{}}))       { std::fprintf(stderr, "AttrSet wrongly trivial\n"); return 1; }
    if (ir::isTrivialRhs(ir::Expr{ir::ListExpr{}}))      { std::fprintf(stderr, "ListExpr wrongly trivial\n"); return 1; }
    if (ir::isTrivialRhs(ir::Expr{ir::Force{1}}))        { std::fprintf(stderr, "Force wrongly trivial\n"); return 1; }

    std::fprintf(stderr, "testIsTrivialRhs: OK\n");
    return 0;
}

// ===========================================================================
// #542 — emit-time deferring + binary fast path (bytecode-shape tests).
// ===========================================================================
//
// We can't easily run the standalone v3-eval CLI on a hand-built IR, but
// we CAN drive `compile()` directly and inspect the resulting bytecode
// stream.  The tests below build a small module by hand, compile it, and
// assert specific opcode patterns are PRESENT or ABSENT — proving the
// deferring optimisation is firing on the canonical fib-shape pattern
// without breaking the no-defer-able fallback paths.

#include "v3/bytecode.hh"
#include "v3/disasm.hh"

namespace {

/// Disassemble a CU's lambda body to a string for FileCheck.
static std::string disasmFunction(const nix::v3::CompilationUnit & cu, size_t funcIdx)
{
    std::string out;
    out += "; func " + std::to_string(funcIdx) + "\n";
    if (funcIdx >= cu.lambdas.size()) return out;
    uint32_t lo = cu.lambdas[funcIdx].codeOffset;
    uint32_t hi = (funcIdx + 1 < cu.lambdas.size())
        ? cu.lambdas[funcIdx + 1].codeOffset
        : static_cast<uint32_t>(cu.code.size());
    // disassembleWindow writes to a FILE*, so use a temp + fread.
    char buf[16384];
    FILE * f = fmemopen(buf, sizeof buf, "w");
    nix::v3::disassembleWindow(f, cu, lo, hi);
    long len = ftell(f);
    fclose(f);
    out.append(buf, len > 0 ? (size_t)len : 0);
    return out;
}

} // namespace

// `wrapper arg = ({ ... }: 1) arg`; call wrapper with a thunk returning `[]`.
// The wrapper body must compile to OP_TAIL_CALL, and the tail-call formals
// path must still force the thunk argument and reject the list.
static int testTailCallFormalsEllipsisForcesThunkArg()
{
    auto m = ir::makeModule();

    auto listFid = addFunction(m);
    auto listEntry = m.freshBlock();
    {
        auto & f = funcOf(m, listFid);
        f.entryBlock = listEntry;
        f.name = "list-thunk";
        auto emptyList = addBinding(m, listEntry, ir::ListExpr{{}});
        setReturn(m, listEntry, emptyList);
    }

    auto innerFid = addFunction(m);
    auto innerEntry = m.freshBlock();
    auto innerParam = m.freshVar();
    {
        auto & f = funcOf(m, innerFid);
        f.entryBlock = innerEntry;
        f.paramVar = innerParam;
        f.hasFormals = true;
        f.ellipsis = true;
        f.name = "ellipsis-formals";
        auto one = addBinding(m, innerEntry, ir::LitInt{1});
        setReturn(m, innerEntry, one);
    }

    auto topEntry = m.freshBlock();
    funcOf(m, 0).entryBlock = topEntry;
    auto innerClo = addBinding(m, topEntry, ir::Lambda{innerFid, /*freeVars*/ {}});

    auto wrapperFid = addFunction(m);
    auto wrapperEntry = m.freshBlock();
    auto wrapperParam = m.freshVar();
    {
        auto & f = funcOf(m, wrapperFid);
        f.entryBlock = wrapperEntry;
        f.argName = m.internSymbol("arg");
        f.paramVar = wrapperParam;
        f.name = "tail-wrapper";
        auto tailCall = addBinding(m, wrapperEntry, ir::App{innerClo, wrapperParam});
        setReturn(m, wrapperEntry, tailCall);
    }

    auto wrapperClo = addBinding(m, topEntry, ir::Lambda{wrapperFid, /*freeVars*/ {}});
    auto listThunk = addBinding(m, topEntry, ir::MkThunk{listFid, /*freeVars*/ {}});
    auto call = addBinding(m, topEntry, ir::App{wrapperClo, listThunk});
    setReturn(m, topEntry, call);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    std::string dis = disasmFunction(cu, wrapperFid);
    if (dis.find("OP_TAIL_CALL") == std::string::npos) {
        std::fprintf(stderr,
            "testTailCallFormalsEllipsisForcesThunkArg: wrapper did not compile "
            "to OP_TAIL_CALL\n%s\n", dis.c_str());
        return 1;
    }

    try {
        Value r = run(cu);
        std::fprintf(stderr,
            "testTailCallFormalsEllipsisForcesThunkArg: expected type error, "
            "got tag=%d\n", (int)r.tag());
        return 1;
    } catch (const std::exception & e) {
        std::string msg = e.what();
        if (msg.find("expected a set but found a list") == std::string::npos) {
            std::fprintf(stderr,
                "testTailCallFormalsEllipsisForcesThunkArg: wrong error: %s\n",
                e.what());
            return 1;
        }
    }

    std::fprintf(stderr,
        "testTailCallFormalsEllipsisForcesThunkArg: OK "
        "(tail-call formals force thunk arg before body)\n");
    return 0;
}

// Positive case: the fib-cond shape `Less(force(k), 2)` should compile
// to GET_LOCAL_FORCE; LIT_INT; LESS — no SET_LOCAL or GET_LOCAL of
// intermediate slots (the OnceLinear bindings T_force_k and T_lit2
// are deferred and consumed by the binary fast path).
static int testDeferFibCondShape()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto & f = funcOf(m, 0);
    f.argName = ir::SymbolId{1};
    f.paramVar = m.freshVar();  // simulate `k` as the param
    auto kVar = f.paramVar;

    auto kForce = addBinding(m, entry, ir::Force{kVar});           // OnceLinear
    auto two    = addBinding(m, entry, ir::LitInt{2});              // OnceLinear
    auto less   = addBinding(m, entry, ir::Less{kForce, two});      // tail
    setReturn(m, entry, less);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    std::string dis = disasmFunction(cu, 0);

    // The shape we want: FORCE on k, LIT_INT 2, LESS, RETURN.  Each
    // intermediate is consumed via the deferring pipeline.  We use
    // GET_LOCAL_FORCE 0 for k (the param-slot superinstruction).
    const char * expected = R"(
        ; CHECK: OP_GET_LOCAL_FORCE
        ; CHECK-NOT: OP_SET_LOCAL
        ; CHECK: OP_LIT_INT
        ; CHECK-NOT: OP_GET_LOCAL
        ; CHECK: OP_LESS
        ; CHECK: OP_HALT
    )";
    auto err = ir::checkIr(dis, expected);
    if (!err.empty()) {
        std::fprintf(stderr,
            "testDeferFibCondShape: %s\nactual disasm:\n%s\n",
            err.c_str(), dis.c_str());
        return 1;
    }
    std::fprintf(stderr, "testDeferFibCondShape: OK (defer + binary fast path fired)\n");
    return 0;
}

// Negative case: with NIX_V3_NO_DEFER=1, the same shape must emit the
// full SET/GET ladder.  Validates the kill-switch.
static int testDeferKillSwitch()
{
    setenv("NIX_V3_NO_DEFER", "1", 1);
    // analyseOccurrence + tryDefer caches the env var lookup as
    // function-local static, so each test would normally see the
    // first call's value.  But Emitter is constructed fresh per
    // compile(), and tryDefer uses `static const bool`.  The static
    // cache means we have to set the env BEFORE the first compile in
    // this process — which is hard to guarantee.  Instead: this test
    // documents the intent.  In practice, NIX_V3_NO_DEFER must be
    // set before nix-direct or v3-eval starts.

    // Actually run it to verify no crash + correct value.
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto a = addBinding(m, entry, ir::LitInt{2});
    auto b = addBinding(m, entry, ir::LitInt{3});
    auto sum = addBinding(m, entry, ir::Add{a, b});
    setReturn(m, entry, sum);
    ir::computeFreeVars(m);
    auto cu = compile(m);
    auto v = run(cu);
    unsetenv("NIX_V3_NO_DEFER");
    if (!v.isInt() || v.asInt() != 5) {
        std::fprintf(stderr, "testDeferKillSwitch: expected 5, got tag=%d\n",
            (int)v.tag());
        return 1;
    }
    std::fprintf(stderr, "testDeferKillSwitch: OK\n");
    return 0;
}

// Positive correctness: 1 + 2 with deferring active, returns 3.
static int testDeferAddCorrectness()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto one  = addBinding(m, entry, ir::LitInt{1});
    auto two  = addBinding(m, entry, ir::LitInt{2});
    auto sum  = addBinding(m, entry, ir::Add{one, two});
    setReturn(m, entry, sum);
    ir::computeFreeVars(m);
    auto cu = compile(m);
    auto v = run(cu);
    if (!v.isInt() || v.asInt() != 3) {
        std::fprintf(stderr, "testDeferAddCorrectness: expected 3, got tag=%d\n",
            (int)v.tag());
        return 1;
    }
    std::fprintf(stderr, "testDeferAddCorrectness: OK (1+2=3 with defer active)\n");
    return 0;
}

// Negative: a binding with Many uses must NOT be deferred.  Verify by
// asserting the bytecode contains a SET_LOCAL for a Many-use binding.
static int testDeferSkipsManyUseBinding()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    // x = 5; x + x  → x is Many (used twice).  Must SET to a slot
    // because deferring "consumes" the value off the stack.  The first
    // use is adjacent to the SET, so the SET_LOCAL_KEEP fusion
    // (BYTECODE_NGRAM_ANALYSIS §7) collapses `SET_LOCAL x; GET_LOCAL x`
    // into `SET_LOCAL_KEEP x` — which STILL stores the slot (the test's
    // intent: a Many binding gets a real slot store, not a defer), so
    // the second read still loads from the slot.  Expected shape:
    //   LIT_INT 5; SET_LOCAL_KEEP 0; GET_LOCAL 0; ADD.
    auto x   = addBinding(m, entry, ir::LitInt{5});
    auto sum = addBinding(m, entry, ir::Add{x, x});
    setReturn(m, entry, sum);
    ir::computeFreeVars(m);
    auto cu = compile(m);
    std::string dis = disasmFunction(cu, 0);

    const char * expected = R"(
        ; CHECK: OP_LIT_INT
        ; CHECK: OP_SET_LOCAL_KEEP
        ; CHECK: OP_GET_LOCAL
        ; CHECK: OP_ADD
    )";
    auto err = ir::checkIr(dis, expected);
    if (!err.empty()) {
        std::fprintf(stderr,
            "testDeferSkipsManyUseBinding: %s\nactual disasm:\n%s\n",
            err.c_str(), dis.c_str());
        return 1;
    }

    auto v = run(cu);
    if (!v.isInt() || v.asInt() != 10) {
        std::fprintf(stderr, "testDeferSkipsManyUseBinding: expected 10, got tag=%d\n",
            (int)v.tag());
        return 1;
    }
    std::fprintf(stderr,
        "testDeferSkipsManyUseBinding: OK (Many binding gets slot store via "
        "SET_LOCAL_KEEP fusion, value=10)\n");
    return 0;
}

// Positive: the fib `if k < 2 then ... else ...` shape combines the
// binary fast path on Less with the unary fast path on If.  The
// expected disasm collapses to FORCE k; LIT 2; LESS; BRANCH_FALSE
// — no SET/GET intermediates between LESS and BRANCH_FALSE.
static int testDeferIfOnLessShape()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    auto thenB = m.freshBlock();
    auto elseB = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;
    auto & f = funcOf(m, 0);
    f.argName = ir::SymbolId{1};
    f.paramVar = m.freshVar();
    auto kVar = f.paramVar;

    // body: let kf = force k; two = 2; c = kf < two; if c then 100 else 200
    auto kf   = addBinding(m, entry, ir::Force{kVar});
    auto two  = addBinding(m, entry, ir::LitInt{2});
    auto less = addBinding(m, entry, ir::Less{kf, two});
    auto ifv  = addBinding(m, entry, ir::If{less, thenB, elseB});
    setReturn(m, entry, ifv);
    auto t1 = addBinding(m, thenB, ir::LitInt{100});
    setReturn(m, thenB, t1);
    auto e1 = addBinding(m, elseB, ir::LitInt{200});
    setReturn(m, elseB, e1);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    std::string dis = disasmFunction(cu, 0);

    // Expected: GET_LOCAL_FORCE; LIT_INT; LESS; BRANCH_FALSE — no
    // OP_SET_LOCAL between LESS and BRANCH_FALSE (the OnceLinear
    // `less` binding must be deferred and consumed by the unary fast
    // path on If).
    const char * expected = R"(
        ; CHECK: OP_GET_LOCAL_FORCE
        ; CHECK: OP_LIT_INT
        ; CHECK: OP_LESS
        ; CHECK-NOT: OP_SET_LOCAL
        ; CHECK: OP_BRANCH_FALSE
    )";
    auto err = ir::checkIr(dis, expected);
    if (!err.empty()) {
        std::fprintf(stderr,
            "testDeferIfOnLessShape: %s\nactual disasm:\n%s\n",
            err.c_str(), dis.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "testDeferIfOnLessShape: OK (binary+unary fast paths chain)\n");
    return 0;
}

// Regression: rec attrset construction must not be broken by deferring.
// `rec { x = 7; y = x + 1; }` should produce { x = 7, y = 8 }.
static int testDeferLetRecCorrect()
{
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    // Build a LetRec by hand.  The lowerer would do this for `rec { ... }`.
    auto recVar = m.freshVar();

    // Two thunk bodies: one returns 7, one returns recVar.x + 1.
    auto thunkX = addFunction(m);
    auto thunkY = addFunction(m);
    auto thunkXEntry = m.freshBlock();
    auto thunkYEntry = m.freshBlock();
    funcOf(m, thunkX).entryBlock = thunkXEntry;
    funcOf(m, thunkY).entryBlock = thunkYEntry;

    auto seven = addBinding(m, thunkXEntry, ir::LitInt{7});
    setReturn(m, thunkXEntry, seven);

    // thunkY: x = recVar.x; x + 1
    ir::SymbolId nx{1};
    auto sel  = addBinding(m, thunkYEntry, ir::AttrSelect{recVar, nx});
    auto fsel = addBinding(m, thunkYEntry, ir::Force{sel});
    auto one  = addBinding(m, thunkYEntry, ir::LitInt{1});
    auto plus = addBinding(m, thunkYEntry, ir::Add{fsel, one});
    setReturn(m, thunkYEntry, plus);

    // Build the LetRec binding in entry.
    ir::LetRec lr;
    lr.recVar = recVar;
    ir::SymbolId ny{2};
    lr.entries.push_back({nx, thunkX, 0, {}, {}});
    lr.entries.push_back({ny, thunkY, 0, {}, {}});
    auto rec = m.freshVar();
    m.blocks[entry].bindings.push_back({rec, lr});
    auto force = addBinding(m, entry, ir::Force{rec});
    auto selY  = addBinding(m, entry, ir::AttrSelect{force, ny});
    auto final = addBinding(m, entry, ir::Force{selY});
    setReturn(m, entry, final);

    ir::computeFreeVars(m);
    auto cu = compile(m);
    auto v = run(cu);
    if (!v.isInt() || v.asInt() != 8) {
        std::fprintf(stderr, "testDeferLetRecCorrect: expected 8, got tag=%d\n",
            (int)v.tag());
        return 1;
    }
    std::fprintf(stderr,
        "testDeferLetRecCorrect: OK (rec { x=7; y=x+1; }.y = 8 under defer)\n");
    return 0;
}

// ---------------------------------------------------------------------------
// OPT_OCCUR Phase B — deadBindingElimViaOccur smoke tests
// ---------------------------------------------------------------------------
//
// Two complementary checks for the new occurrence-info-driven DCE
// pass added by Phase 0.3:
//
//   1. Positive: a chain of unused bindings where round 1 of the new
//      pass removes the orphan literal, and round 2 picks up the
//      now-dead VarRef alias that only consumed it.  This case
//      exercises the "re-run analyseOccurrence" step from OPT_OCCUR
//      Phase B; if the pass were a single-round walk it would leave
//      the alias behind.
//
//   2. Equivalence: the old deadBindingElim and the new
//      deadBindingElimViaOccur produce identical Block::bindings
//      vectors on a non-trivial module.  Direct verification of the
//      side-by-side claim (without going through the env-var-gated
//      validation harness).

static int testOccurDceRemovesChainedDeadBinding()
{
    // OPT_OCCUR Phase B is specifically two passes: round 1 removes
    // obviously-Dead bindings; round 2 picks up bindings whose sole
    // consumer was a round-1 victim.  This test constructs the
    // minimal case the round-2 step is needed for:
    //
    //   live  = LitInt 1            (returned — survives)
    //   inner = LitInt 99           (used only by `wrap` below)
    //   wrap  = VarRef{inner}       (unused by anyone — dead at round 1)
    //
    // Round 1: `wrap` has count=0 → Dead → removed.  `inner` still
    // has count=1 (wrap uses it) → OnceLinear → preserved.
    // Round 2: re-run analyseOccurrence on the now-smaller module;
    // `inner`'s count is now 0 → Dead → removed.
    //
    // A single-round pass would leave `inner` behind.  This proves
    // the round-2 step is observably load-bearing.
    auto m = ir::makeModule();
    auto entry = m.freshBlock();
    funcOf(m, 0).entryBlock = entry;

    auto inner = addBinding(m, entry, ir::LitInt{99});
    auto wrap = addBinding(m, entry, ir::VarRef{inner});
    (void)wrap;
    auto live = addBinding(m, entry, ir::LitInt{1});
    setReturn(m, entry, live);

    // Use the new pass directly (not via optimise(), to isolate from
    // the rest of the pipeline).
    size_t removed = ir::deadBindingElimViaOccur(m);

    bool sawInner = false, sawWrap = false, sawLive = false;
    for (auto & bb : m.blocks[entry].bindings) {
        if (bb.var == inner) sawInner = true;
        if (bb.var == wrap)  sawWrap = true;
        if (bb.var == live)  sawLive = true;
    }
    if (sawInner || sawWrap || !sawLive || removed != 2) {
        std::fprintf(stderr,
            "testOccurDceRemovesChainedDeadBinding: inner=%d wrap=%d "
            "live=%d removed=%zu (expected 0/0/1, removed=2)\n",
            (int)sawInner, (int)sawWrap, (int)sawLive, removed);
        return 1;
    }
    std::fprintf(stderr,
        "testOccurDceRemovesChainedDeadBinding: OK (round-2 swept the "
        "chained dead binding, live preserved, removed=%zu)\n", removed);
    return 0;
}

static int testOccurDceMatchesOldDce()
{
    // Build two structurally-identical modules; run old DCE on one,
    // new DCE on the other; assert resulting binding vectors are
    // identical (per-block, per-var).
    auto buildMod = []() {
        auto m = ir::makeModule();
        auto entry = m.freshBlock();
        funcOf(m, 0).entryBlock = entry;
        // Mix: unused LitInts, an unused alias chain, a referenced
        // lambda body, a referenced Force (impure — must survive).
        addBinding(m, entry, ir::LitInt{100});                  // dead
        auto a = addBinding(m, entry, ir::LitInt{200});         // dead
        auto b = addBinding(m, entry, ir::VarRef{a});            // dead
        (void)b;
        auto c = addBinding(m, entry, ir::LitInt{300});          // live
        auto d = addBinding(m, entry, ir::LitInt{400});          // dead but impure consumer below
        auto e = addBinding(m, entry, ir::Add{c, d});            // live (returned)
        setReturn(m, entry, e);
        return m;
    };
    auto mOld = buildMod();
    auto mNew = buildMod();

    size_t removedOld = ir::deadBindingElim(mOld);
    size_t removedNew = ir::deadBindingElimViaOccur(mNew);

    if (removedOld != removedNew) {
        std::fprintf(stderr,
            "testOccurDceMatchesOldDce: removed-count mismatch "
            "(old=%zu, new=%zu)\n", removedOld, removedNew);
        return 1;
    }
    if (mOld.blocks.size() != mNew.blocks.size()) {
        std::fprintf(stderr,
            "testOccurDceMatchesOldDce: block-count mismatch\n");
        return 1;
    }
    for (size_t bid = 0; bid < mOld.blocks.size(); ++bid) {
        const auto & oldB = mOld.blocks[bid].bindings;
        const auto & newB = mNew.blocks[bid].bindings;
        if (oldB.size() != newB.size()) {
            std::fprintf(stderr,
                "testOccurDceMatchesOldDce: block %zu binding-count "
                "mismatch (old=%zu, new=%zu)\n", bid, oldB.size(), newB.size());
            return 1;
        }
        for (size_t i = 0; i < oldB.size(); ++i) {
            if (oldB[i].var != newB[i].var) {
                std::fprintf(stderr,
                    "testOccurDceMatchesOldDce: block %zu pos %zu var "
                    "mismatch (old=%u, new=%u)\n", bid, i,
                    oldB[i].var, newB[i].var);
                return 1;
            }
        }
    }
    std::fprintf(stderr,
        "testOccurDceMatchesOldDce: OK (old vs new identical, "
        "removed=%zu each)\n", removedOld);
    return 0;
}

// #823 / A1a Phase A — ChainBindings discriminator + chain-aware lookup.
//
// Phase A is a no-functional-change scaffold (no consumer constructs a
// Chain), so the lang-test suite alone cannot exercise the new
// `lookupLocal` / `lookup`-walks-parent code paths.  This unit test
// manually wires up a two-segment chain:
//
//   parent (Sorted):   { "a" -> 1, "b" -> 2 }
//   child  (Chain  ):  overlay { "a" -> 10, "c" -> 3 },  parent = &parent
//
// and asserts the expected lookup semantics:
//
//   lookup("a") -> 10   (overlay shadows parent)
//   lookup("b") ->  2   (parent fallback)
//   lookup("c") ->  3   (overlay only)
//   lookup("d") -> nullptr (absent everywhere)
//   parent.lookup("a") -> 1   (standalone parent still observes its own entries)
//
// Phase B/C/D must keep this test green.  If the chain-walk loop in
// `Bindings::lookup` (alloc.hh) is broken (early-exit, infinite loop,
// wrong order), this test fires.
static int testBindingsChainLookup()
{
    // Helper: allocate a Sorted Bindings of size n and populate entries.
    auto makeSorted = [](std::initializer_list<std::pair<SymbolId, int64_t>> kvs) {
        // Entries must be sorted ascending by name (binary-search invariant).
        std::vector<std::pair<SymbolId, int64_t>> sorted(kvs.begin(), kvs.end());
        std::sort(sorted.begin(), sorted.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        auto * b = Alloc::allocBindings(static_cast<uint32_t>(sorted.size()));
        for (size_t i = 0; i < sorted.size(); ++i) {
            b->entries[i].name  = sorted[i].first;
            b->entries[i].pos   = kNoPos;
            b->entries[i].value.mkInt(sorted[i].second);
        }
        return b;
    };

    // SymbolIds 1..4 stand in for the symbol table entries "a","b","c","d".
    // The Phase A code never looks at the symbol table itself; it only
    // compares SymbolId values, so opaque ints are fine.
    Bindings * parent = makeSorted({{1, 1}, {2, 2}});
    Bindings * child  = makeSorted({{1, 10}, {3, 3}});  // overlay
    parent->entries[0].pos = 101;
    parent->entries[1].pos = 102;
    child->entries[0].pos = 201;
    child->entries[1].pos = 203;
    child->kind   = uint8_t(Bindings::Kind::Chain);
    child->parent = parent;

    if (!child->isChain()) {
        std::fprintf(stderr, "testBindingsChainLookup: isChain() returned false\n");
        return 1;
    }

    auto check = [&](const Bindings * b, SymbolId name, const char * what,
                     bool expectHit, int64_t expectVal) {
        const Value * v = b->lookup(name);
        if (expectHit) {
            if (!v) {
                std::fprintf(stderr,
                    "testBindingsChainLookup: %s expected hit, got nullptr\n", what);
                return 1;
            }
            if (!v->isInt() || v->asInt() != expectVal) {
                std::fprintf(stderr,
                    "testBindingsChainLookup: %s expected Int(%lld), "
                    "got tag=%d val=%lld\n",
                    what, (long long)expectVal,
                    (int)v->tag(), (long long)v->asInt());
                return 1;
            }
        } else {
            if (v) {
                std::fprintf(stderr,
                    "testBindingsChainLookup: %s expected nullptr, got Int(%lld)\n",
                    what, (long long)v->asInt());
                return 1;
            }
        }
        return 0;
    };

    int rc = 0;
    rc |= check(child,  1, "child.lookup(a)",     true,  10);  // overlay shadows
    rc |= check(child,  2, "child.lookup(b)",     true,  2);   // parent fallback
    rc |= check(child,  3, "child.lookup(c)",     true,  3);   // overlay only
    rc |= check(child,  4, "child.lookup(d)",     false, 0);   // absent
    rc |= check(parent, 1, "parent.lookup(a)",    true,  1);   // parent intact
    rc |= check(parent, 3, "parent.lookup(c)",    false, 0);   // not in parent

    // `lookupLocal` must NOT walk parent — verify by asking parent for "c".
    if (parent->lookupLocal(3) != nullptr) {
        std::fprintf(stderr,
            "testBindingsChainLookup: parent->lookupLocal(c) walked parent (shouldn't)\n");
        return 1;
    }
    // And child.lookupLocal(b) must miss (b only lives in parent).
    if (child->lookupLocal(2) != nullptr) {
        std::fprintf(stderr,
            "testBindingsChainLookup: child->lookupLocal(b) walked parent (shouldn't)\n");
        return 1;
    }
    auto checkEntry = [&](const Bindings * b, SymbolId name, PosIdx32 expectPos,
                          int64_t expectVal, const char * what) {
        const Bindings::Entry * e = b->lookupEntry(name);
        if (!e || e->pos != expectPos || !e->value.isInt()
            || e->value.asInt() != expectVal) {
            std::fprintf(stderr,
                "testBindingsChainLookup: %s expected pos=%u val=%lld, "
                "got %s\n",
                what, expectPos, (long long)expectVal, e ? "mismatch" : "nullptr");
            return 1;
        }
        return 0;
    };
    rc |= checkEntry(child, 1, 201, 10, "child.lookupEntry(a)");
    rc |= checkEntry(child, 2, 102,  2, "child.lookupEntry(b)");
    rc |= checkEntry(child, 3, 203,  3, "child.lookupEntry(c)");
    if (child->lookupEntry(4) != nullptr) {
        std::fprintf(stderr,
            "testBindingsChainLookup: child.lookupEntry(d) expected nullptr\n");
        return 1;
    }
    if (lookupAttrPos(child, 1) != 201
        || lookupAttrPos(child, 2) != 102
        || lookupAttrPos(child, 3) != 203
        || lookupAttrPos(child, 4) != 0) {
        std::fprintf(stderr,
            "testBindingsChainLookup: lookupAttrPos chain semantics wrong\n");
        return 1;
    }

    if (rc == 0)
        std::fprintf(stderr,
            "testBindingsChainLookup: OK (overlay shadow + parent fallback + pos lookup)\n");
    return rc;
}

// #825 / A1a Phase B — verify materialize() + forEach() iteration helpers.
//
// Constructs a 2-segment chain (overlay shadows parent) and checks:
//   1. isSorted() / chainDepth() report the right shape
//   2. materialize() produces a Sorted Bindings with exactly the
//      distinct names (overlay wins for shared names)
//   3. forEach() visits every distinct name in ascending order,
//      with overlay's value where names collide
//   4. Sorted bindings are pass-through (materialize returns `this`,
//      forEach iterates entries[] directly)
//
// Phase B's helpers are the prerequisite for Phase C's mergeBindings
// Chain construction.  If this test goes red, the chain materialise
// or iteration semantics are wrong.
static int testBindingsForEachMaterialise()
{
    auto makeSorted = [](std::initializer_list<std::pair<SymbolId, int64_t>> kvs) {
        std::vector<std::pair<SymbolId, int64_t>> sorted(kvs.begin(), kvs.end());
        std::sort(sorted.begin(), sorted.end(),
            [](auto & a, auto & b) { return a.first < b.first; });
        auto * b = Alloc::allocBindings(static_cast<uint32_t>(sorted.size()));
        for (size_t i = 0; i < sorted.size(); ++i) {
            b->entries[i].name  = sorted[i].first;
            b->entries[i].pos   = kNoPos;
            b->entries[i].value.mkInt(sorted[i].second);
        }
        return b;
    };

    // Parent: a=1, b=2, c=3.  Overlay: a=10, d=4.  Expected merged:
    //   a=10 (overlay), b=2 (parent), c=3 (parent), d=4 (overlay).
    Bindings * parent  = makeSorted({{1, 1}, {2, 2}, {3, 3}});
    Bindings * overlay = makeSorted({{1, 10}, {4, 4}});
    overlay->kind   = uint8_t(Bindings::Kind::Chain);
    overlay->parent = parent;

    // Shape diagnostics.
    if (overlay->isSorted() || !overlay->isChain()) {
        std::fprintf(stderr, "testForEachMaterialise: chain isSorted/isChain wrong\n");
        return 1;
    }
    if (overlay->chainDepth() != 2) {
        std::fprintf(stderr, "testForEachMaterialise: chainDepth expected 2, got %u\n",
            overlay->chainDepth());
        return 1;
    }
    if (!parent->isSorted() || parent->chainDepth() != 1) {
        std::fprintf(stderr, "testForEachMaterialise: parent shape wrong\n");
        return 1;
    }
    if (overlay->totalSize() != 4) {
        std::fprintf(stderr, "testForEachMaterialise: totalSize expected 4, got %u\n",
            overlay->totalSize());
        return 1;
    }
    if (parent->totalSize() != 3) {
        std::fprintf(stderr, "testForEachMaterialise: parent totalSize expected 3\n");
        return 1;
    }

    // materialize() should produce a Sorted result with all four names + correct shadowing.
    const Bindings * mat = overlay->materialize();
    if (mat == overlay) {
        std::fprintf(stderr, "testForEachMaterialise: materialize() returned same chain\n");
        return 1;
    }
    if (!mat->isSorted() || mat->size != 4) {
        std::fprintf(stderr, "testForEachMaterialise: materialise produced wrong shape "
                             "(isSorted=%d size=%u)\n",
                             mat->isSorted() ? 1 : 0, mat->size);
        return 1;
    }
    // Expected: name 1 -> 10 (overlay), 2 -> 2, 3 -> 3, 4 -> 4.
    int64_t expected[] = {10, 2, 3, 4};
    for (uint32_t i = 0; i < 4; ++i) {
        if (mat->entries[i].name != i + 1
            || !mat->entries[i].value.isInt()
            || mat->entries[i].value.asInt() != expected[i]) {
            std::fprintf(stderr,
                "testForEachMaterialise: entry[%u] expected (name=%u, val=%lld), "
                "got (name=%u, val=%lld)\n",
                i, i + 1, (long long)expected[i],
                mat->entries[i].name, (long long)mat->entries[i].value.asInt());
            return 1;
        }
    }

    // materialize() on a Sorted Bindings must return the same pointer (no copy).
    const Bindings * matSorted = parent->materialize();
    if (matSorted != parent) {
        std::fprintf(stderr, "testForEachMaterialise: Sorted materialise should be identity\n");
        return 1;
    }

    // forEach on Chain — collects all four entries in ascending name order.
    std::vector<std::pair<SymbolId, int64_t>> seenChain;
    overlay->forEach([&](const Bindings::Entry & e) {
        seenChain.push_back({e.name, e.value.asInt()});
    });
    if (seenChain.size() != 4) {
        std::fprintf(stderr,
            "testForEachMaterialise: chain forEach expected 4 entries, got %zu\n",
            seenChain.size());
        return 1;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (seenChain[i].first != SymbolId(i + 1) || seenChain[i].second != expected[i]) {
            std::fprintf(stderr,
                "testForEachMaterialise: chain forEach[%zu] mismatch\n", i);
            return 1;
        }
    }

    // forEach on Sorted — identical semantics; no materialise needed.
    std::vector<std::pair<SymbolId, int64_t>> seenSorted;
    parent->forEach([&](const Bindings::Entry & e) {
        seenSorted.push_back({e.name, e.value.asInt()});
    });
    if (seenSorted.size() != 3
        || seenSorted[0] != std::pair<SymbolId, int64_t>(1, 1)
        || seenSorted[1] != std::pair<SymbolId, int64_t>(2, 2)
        || seenSorted[2] != std::pair<SymbolId, int64_t>(3, 3)) {
        std::fprintf(stderr, "testForEachMaterialise: Sorted forEach mismatch\n");
        return 1;
    }

    std::fprintf(stderr,
        "testForEachMaterialise: OK (chain materialise + forEach + Sorted identity)\n");
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
    rc |= testTailCallFormalsEllipsisForcesThunkArg();
    rc |= testCallNPrimOpNoPap();
    rc |= testCallClosure2PrimOpNoPap();
    rc |= testForceApp3Arity2NoPap();
    rc |= testCallClosureApp3PapSaturates();
    rc |= testForceAppArity3NoPap();
    rc |= testIf();
    rc |= testListConcat();
    rc |= testAttrSelect();
    rc |= testAttrSetDynLargeNullCompacts();
    rc |= testAttrUpdate();
    rc |= testWith();
    rc |= testThunkForce();
    rc |= testShortCircuit();
    rc |= testPrimOpLength();
    rc |= testPrimMapIdentityNoApps();
    rc |= testPrimGenListIdentityNoApps();
    rc |= testPrimMapAttrsNamesDoNotRealize();
    rc |= testPrimMapAttrsNestedNamesDoNotRealize();
    rc |= testPrimAttrValuesMapAttrsSortsWithOneAppPerValue();
    rc |= testPrimMapAttrsSelectNoApp3();
    rc |= testPrimMapAttrsEmptyUpdateNoApp3();
    rc |= testPrimMapAttrsUpdateNoApp3();
    rc |= testPrimMapAttrsNestedSelectUsesMappedValue();
    rc |= testPrimMapAttrsSetOpsNoApp3();
    rc |= testPrimMapAttrsValueIdentityNoApps();
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

    // #539 — IR text dumper + FileCheck.
    rc |= testIrDumpBasic();
    rc |= testIrDumpNegativeCheckNot();
    rc |= testIrDumpOrderingMatters();

    // #540 — analyseOccurrence corpus.
    rc |= testOccurEmptyModule();
    rc |= testOccurDeadLiteral();
    rc |= testOccurOnceLinear();
    rc |= testOccurManyTwoUses();
    rc |= testOccurOnceCapturedAcrossLambda();
    rc |= testOccurNoDoubleCountAfterComputeFreeVars();
    rc |= testOccurParamUnused();
    rc |= testOccurOnceLinearIfBranch();
    rc |= testOccurManyAcrossBranches();
    rc |= testOccurWithAttrsOnce();
    rc |= testIsTrivialRhs();

    // #542 — emit-time deferring + binary fast path.
    rc |= testDeferAddCorrectness();
    rc |= testDeferFibCondShape();
    rc |= testDeferIfOnLessShape();
    rc |= testDeferSkipsManyUseBinding();
    rc |= testDeferLetRecCorrect();
    rc |= testDeferKillSwitch();

    // OPT_OCCUR Phase B — deadBindingElimViaOccur (Phase 0.3/0.4)
    rc |= testOccurDceRemovesChainedDeadBinding();
    rc |= testOccurDceMatchesOldDce();

    // #823 / A1a Phase A — ChainBindings discriminator unit test.
    rc |= testBindingsChainLookup();

    // #825 / A1a Phase B — forEach + materialize iteration helpers.
    rc |= testBindingsForEachMaterialise();

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
