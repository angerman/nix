// J1 validation — exercises the aarch64 encoder + JitArena (include/v3/jit.hh)
// by EMITTING small functions, mapping them executable, CALLING them, and
// asserting the results.  Running generated code is the real correctness gate
// for the instruction encodings (a wrong bit pattern → wrong result or crash).
//
// Build/run:  nix develop -c make -C src/libexpr-v3/research jit-encoder-test
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
// Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/jit.hh"
#include <cstdio>
#include <cstdint>
#include <cinttypes>

using namespace nix::v3::jit;

static int failures = 0;
static void check(const char * name, int64_t got, int64_t want) {
    if (got == want) {
        std::printf("  OK   %-22s = %" PRId64 "\n", name, got);
    } else {
        std::printf("  FAIL %-22s got %" PRId64 " want %" PRId64 "\n", name, got, want);
        ++failures;
    }
}

int main() {
#if !defined(__aarch64__)
    std::fprintf(stderr, "jit-encoder-test: aarch64 only.\n");
    return 2;
#else
    JitArena arena;

    // t1: 64-bit constant materialisation (movz + 3x movk) + ret.
    {
        Aarch64Emitter e;
        const uint64_t K = 0x123456789ABCDEF0ull;
        e.movImm64(X0, K);
        e.ret();
        auto f = reinterpret_cast<uint64_t(*)()>(arena.finalize(e));
        check("movImm64", (int64_t)(f ? f() : 1), (int64_t)K);
    }
    // t2: load two i64 from [X0], add, return  (ldr + ldr + add + mov + ret).
    {
        Aarch64Emitter e;
        e.ldr(X1, X0, 0);
        e.ldr(X2, X0, 8);
        e.add(X0, X1, X2);
        e.ret();
        auto f = reinterpret_cast<int64_t(*)(int64_t*)>(arena.finalize(e));
        int64_t a[2] = { 1000, 337 };
        check("ldr+add", f ? f(a) : 0, 1337);
    }
    // t3: store X1 into [X0,#8], then load it back from [X0,#8] and return.
    {
        Aarch64Emitter e;
        e.str(X1, X0, 8);
        e.ldr(X0, X0, 8);
        e.ret();
        auto f = reinterpret_cast<int64_t(*)(int64_t*, int64_t)>(arena.finalize(e));
        int64_t a[2] = { 0, 0 };
        check("str+ldr", f ? f(a, 424242) : 0, 424242);
    }
    // t4: max(a,b) via cmp + conditional branch  (X0=a, X1=b).
    {
        Aarch64Emitter e;
        e.cmp(X0, X1);                 // a - b
        size_t br = e.bcond(Cond::GE); // if a>=b, skip the mov
        e.mov(X0, X1);                 // X0 = b  (a<b case)
        size_t end = e.pos();
        e.ret();
        e.patchCondBranch(br, end);    // GE -> ret with X0=a
        auto f = reinterpret_cast<int64_t(*)(int64_t,int64_t)>(arena.finalize(e));
        check("max(7,3)",  f ? f(7,3)  : 0, 7);
        check("max(3,9)",  f ? f(3,9)  : 0, 9);
    }
    // t5: integer (a + b) * c - imm  (add + mul + subImm) — ALU coverage.
    {
        Aarch64Emitter e;
        e.add(X3, X0, X1);             // X3 = a + b
        e.mul(X3, X3, X2);             // X3 = (a+b) * c
        e.subImm(X0, X3, 5);           // X0 = ((a+b)*c) - 5
        e.ret();
        auto f = reinterpret_cast<int64_t(*)(int64_t,int64_t,int64_t)>(arena.finalize(e));
        check("(a+b)*c-5", f ? f(2,3,10) : 0, 45);
    }
    // t6: unconditional B forward over a poison value.
    {
        Aarch64Emitter e;
        e.movImm64(X0, 111);
        size_t j = e.b();
        e.movImm64(X0, 999);           // skipped
        size_t tgt = e.pos();
        e.ret();
        e.patchBranch(j, tgt);
        auto f = reinterpret_cast<int64_t(*)()>(arena.finalize(e));
        check("B-forward", f ? f() : 0, 111);
    }
    // NOTE: a BLR cross-call test belongs to J2 — it needs the calling
    // convention to SAVE/RESTORE X30 (LR) around the call (else the JIT'd
    // function's own RET is corrupted).  That's a J2 trampoline-ABI concern,
    // not a J1 encoder-correctness check; the BLR *encoding* itself is emitted
    // by `blr()` and exercised once the J2 ABI exists.

    std::printf("jit-encoder-test: %s (%d failures)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
#endif
}
