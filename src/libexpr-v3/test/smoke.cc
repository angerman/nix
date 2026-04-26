/// @file
/// v3 bring-up smoke test.
///
/// Hand-build the IR for `let f = x: x + 1; in f 41` and assert that
/// running it through the v3 pipeline yields 42.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/vm.hh"
#include "v3/alloc.hh"

#include <cassert>
#include <cstdio>
#include <stdexcept>

using namespace nix::v3;

static int testLitInt()
{
    // Top-level: just `42`.
    ir::Module m;
    ir::Function top;
    top.name = "top";
    top.bindings.push_back({1, ir::LitInt{42}});
    top.returnVar = 1;
    m.functions.push_back(std::move(top));

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 42) {
        std::fprintf(stderr, "testLitInt: expected 42, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testLitInt: OK (42)\n");
    return 0;
}

static int testAdd()
{
    // Top-level: `1 + 2`.
    ir::Module m;
    ir::Function top;
    top.name = "top";
    top.bindings.push_back({1, ir::LitInt{1}});
    top.bindings.push_back({2, ir::LitInt{2}});
    top.bindings.push_back({3, ir::Add{1, 2}});
    top.returnVar = 3;
    m.functions.push_back(std::move(top));

    ir::computeFreeVars(m);
    auto cu = compile(m);
    Value r = run(cu);
    if (!r.isInt() || r.payload.i != 3) {
        std::fprintf(stderr, "testAdd: expected 3, got tag=%d val=%lld\n",
            (int)r.tag(), (long long)r.payload.i);
        return 1;
    }
    std::fprintf(stderr, "testAdd: OK (1+2=3)\n");
    return 0;
}

static int testLambdaCall()
{
    // Equivalent to `let f = x: x + 1; in f 41` → 42.
    //
    //   Module:
    //     functions[0] = top:
    //       v1 = Lambda { freeVars=[], funcIdx=1 }   -- f
    //       v2 = LitInt 41                           -- 41
    //       v3 = App { fun=v1, arg=v2 }              -- f 41
    //       return v3
    //     functions[1] = inner:
    //       param = 100                              -- x (any unique VarId)
    //       v101 = LitInt 1
    //       v102 = Add { lhs=100, rhs=101 }
    //       return v102

    ir::Module m;

    // functions[0] (top)
    ir::Function top;
    top.name = "top";
    top.bindings.push_back({1, ir::Lambda{ /*freeVars*/{}, /*funcIdx*/1 }});
    top.bindings.push_back({2, ir::LitInt{41}});
    top.bindings.push_back({3, ir::App{1, 2}});
    top.returnVar = 3;
    m.functions.push_back(std::move(top));

    // functions[1] (inner)
    ir::Function inner;
    inner.name = "f";
    inner.param = 100;
    inner.bindings.push_back({101, ir::LitInt{1}});
    inner.bindings.push_back({102, ir::Add{100, 101}});
    inner.returnVar = 102;
    m.functions.push_back(std::move(inner));

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

static int testClosureCapture()
{
    // Equivalent to `let n = 10; f = x: x + n; in f 32` → 42.
    //
    //   Module:
    //     functions[0] = top:
    //       v1 = LitInt 10                           -- n
    //       v2 = Lambda { freeVars=[v1], funcIdx=1 } -- f (captures n)
    //       v3 = LitInt 32                           -- 32
    //       v4 = App { v2, v3 }
    //       return v4
    //     functions[1] = inner:
    //       param = 100                              -- x
    //       freeVars in IR will contain v1 (n)
    //       body: v101 = Add(100, v1); return v101
    //
    //   Inside the inner function, references to v1 are upvalues
    //   (free vars).  computeFreeVars + emit translate to OP_GET_UPVALUE.

    ir::Module m;

    ir::Function top;
    top.name = "top";
    top.bindings.push_back({1, ir::LitInt{10}});
    top.bindings.push_back({2, ir::Lambda{ /*freeVars*/{}, /*funcIdx*/1 }});
    top.bindings.push_back({3, ir::LitInt{32}});
    top.bindings.push_back({4, ir::App{2, 3}});
    top.returnVar = 4;
    m.functions.push_back(std::move(top));

    ir::Function inner;
    inner.name = "f";
    inner.param = 100;
    inner.bindings.push_back({101, ir::Add{100, 1}}); // refers to outer v1
    inner.returnVar = 101;
    m.functions.push_back(std::move(inner));

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

int main()
{
    int rc = 0;
    rc |= testLitInt();
    rc |= testAdd();
    rc |= testLambdaCall();
    rc |= testClosureCapture();

    std::fprintf(stderr, "\nv3 alloc stats: values=%llu closures=%llu thunks=%llu envs=%llu\n",
        (unsigned long long)allocStats().valuesAllocated,
        (unsigned long long)allocStats().closuresAllocated,
        (unsigned long long)allocStats().thunksAllocated,
        (unsigned long long)allocStats().envsAllocated);

    if (rc == 0) {
        std::fprintf(stderr, "\nALL v3 SMOKE TESTS PASSED\n");
    } else {
        std::fprintf(stderr, "\nSOME v3 TESTS FAILED\n");
    }
    return rc;
}
