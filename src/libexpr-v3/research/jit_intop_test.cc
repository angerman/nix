// J2 codegen proof — JIT a NaN-box-aware integer ADD over v3 Values, with the
// byte-identity BAIL contract, and validate by EXECUTING it against the v8nan
// reference codec.  This is the hard part of J2: a JIT'd body must read/produce
// the interpreter's exact NaN-boxed Value bits, or bail to the interpreter.
//
// v3 inline-int Value (value.hh / v8nan): Tag::Int=1 -> codeOf=2 ->
//   INT_HEADER = box(2,0) = 0x7FF2000000000000 ; payload = bits[0..47];
//   inline range [-2^47, 2^47-1]; out-of-range -> a heap-boxed pointer (BOXEDINT).
// The JIT'd add handles the inline case bit-identically and BAILS (returns a
// non-int sentinel) on: non-inline-int operand OR 48-bit overflow — exactly the
// cases the interpreter handles differently (heap box / other tags).
//
// Build/run:  make -C src/libexpr-v3/research jit-intop-test
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
// Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/jit.hh"
#include <cstdio>
#include <cstdint>
#include <cinttypes>

using namespace nix::v3::jit;

// --- v8nan inline-int reference codec (mirrors value.hh) ---
static constexpr uint64_t INT_HEADER = 0x7FF2000000000000ull; // box(codeOf(Int=1)=2, 0)
static constexpr uint64_t TOPMASK    = 0xFFFF000000000000ull; // non-payload bits
static constexpr uint64_t PAY        = 0x0000FFFFFFFFFFFFull; // bits[0..47]
static constexpr int64_t  INT_MIN48  = -(1LL << 47);
static constexpr int64_t  INT_MAX48  =  (1LL << 47) - 1;
static bool     isInlineInt(uint64_t w) { return (w & TOPMASK) == INT_HEADER; }
static uint64_t refMkInt(int64_t n)     { return INT_HEADER | (uint64_t(n) & PAY); }
static int64_t  refAsInt(uint64_t w)    { // sign-extend 48-bit payload
    int64_t p = int64_t(w & PAY); return (p << 16) >> 16;
}
// A non-int Value for the bail test: a plain double 3.14 reinterpreted.
static uint64_t floatVal() { double d = 3.14; uint64_t w; __builtin_memcpy(&w,&d,8); return w; }

static int failures = 0;
static void check(const char * name, bool ok, const char * detail = "") {
    std::printf("  %-4s %-26s %s\n", ok ? "OK" : "FAIL", name, detail);
    if (!ok) ++failures;
}

int main() {
#if !defined(__aarch64__)
    std::fprintf(stderr, "jit-intop-test: aarch64 only.\n"); return 2;
#else
    JitArena arena;
    // Emit: uint64_t jitAdd(uint64_t a /*X0*/, uint64_t b /*X1*/)
    //   inline-int(a) && inline-int(b) && !overflow ? boxed-int(a+b) : 0 (bail).
    Aarch64Emitter e;
    e.movImm64(X9,  INT_HEADER);
    e.movImm64(X10, TOPMASK);
    e.andReg(X11, X0, X10); e.cmp(X11, X9); size_t bA = e.bcond(Cond::NE);
    e.andReg(X11, X1, X10); e.cmp(X11, X9); size_t bB = e.bcond(Cond::NE);
    e.sbfx(X2, X0, 0, 48);                 // a := sext48(a)
    e.sbfx(X3, X1, 0, 48);                 // b := sext48(b)
    e.add(X4, X2, X3);                     // s := a + b
    e.sbfx(X5, X4, 0, 48); e.cmp(X4, X5); size_t bOvf = e.bcond(Cond::NE); // overflow if sext48(s)!=s
    e.movImm64(X10, PAY);
    e.andReg(X4, X4, X10);                 // s &= PAY
    e.orrReg(X0, X9, X4);                  // result := INT_HEADER | s
    e.ret();
    size_t bail = e.pos();
    e.movImm64(X0, 0);                     // bail sentinel (a Float 0.0, not an Int)
    e.ret();
    e.patchCondBranch(bA, bail);
    e.patchCondBranch(bB, bail);
    e.patchCondBranch(bOvf, bail);

    auto jitAdd = reinterpret_cast<uint64_t(*)(uint64_t, uint64_t)>(arena.finalize(e));
    if (!jitAdd) { std::fprintf(stderr, "finalize failed\n"); return 1; }

    // Cases that should compute (small inline ints, no overflow) — byte-id vs ref.
    struct { int64_t a, b; } ok[] = {
        {1000, 337}, {-5, 9}, {0, 0}, {INT_MAX48 - 1, 1}, {INT_MIN48 + 1, -1},
        {123456789, 987654321}, {-1, -1},
    };
    for (auto & c : ok) {
        uint64_t got  = jitAdd(refMkInt(c.a), refMkInt(c.b));
        uint64_t want = refMkInt(c.a + c.b);   // in-range by construction
        char d[96]; std::snprintf(d, sizeof d, "%" PRId64 "+%" PRId64 "=%" PRId64,
                                  c.a, c.b, refAsInt(got));
        check("int-add byte-id", isInlineInt(got) && got == want, d);
    }
    // Cases that MUST bail (interpreter handles differently): overflow + non-int.
    check("bail: 48-bit overflow",
          jitAdd(refMkInt(INT_MAX48), refMkInt(1)) == 0 /* sentinel, not inline-int */);
    check("bail: overflow negative",
          jitAdd(refMkInt(INT_MIN48), refMkInt(-1)) == 0);
    check("bail: non-int (float) lhs", !isInlineInt(jitAdd(floatVal(), refMkInt(1))));
    check("bail: non-int (float) rhs", !isInlineInt(jitAdd(refMkInt(1), floatVal())));

    std::printf("jit-intop-test: %s (%d failures)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
#endif
}
