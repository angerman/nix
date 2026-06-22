// J3 safepoint proof — the central design question for a shippable JIT.
//
// The v3 nursery + gen-major collectors MOVE objects.  The scavenger walks
// vm.valueStack / vm.withStack (gc.cc:846) and visitValue() (gc.cc:571)
// REWRITES each pointer Value IN PLACE to its forwarded address.  So a JIT'd
// body that holds a v3 pointer in a register across an allocation would keep a
// STALE from-space pointer after a collection (a use-after-free — the PhD-6
// class).  The J3 calling-convention contract that avoids this:
//
//   * Before any allocation (a safepoint), spill every live v3-pointer Value to
//     the GC-visible value stack (vm.valueStack); keep NO v3 pointer live in a
//     register across the safepoint.
//   * After the allocation, RELOAD the pointer from its value-stack slot — which
//     the scavenger has rewritten in place to the forwarded address.
//
// This spike PROVES that contract standalone (no VM integration), exactly as
// J1 proved the encoder and J2 proved the NaN-box codegen.  It JITs two bodies
// over a toy moving collector whose "safepoint" mirrors visitValue's in-place
// root rewrite:
//   * spilled (CORRECT): reload from the value-stack slot  -> forwarded object.
//   * register-kept (WRONG control): deref the cached register -> the poisoned
//     from-space object, proving the discipline is load-bearing.
//
// Build/run:  nix develop -c make -C src/libexpr-v3/research jit-safepoint-test
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
// Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/jit.hh"
#include <cstdio>
#include <cstdint>
#include <cinttypes>

using namespace nix::v3::jit;

// --- toy boxed-pointer Value (mirrors v8nan: tag in top 16 bits, ptr in PAY) ---
// aarch64 user pointers are <= 48 bits, so a heap/static address fits in PAY —
// exactly why v3 boxes Closure*/Thunk*/etc. into the NaN-box payload.
static constexpr uint64_t PTR_HEADER = 0x7FF4000000000000ull; // a pointer-tag code
static constexpr uint64_t PAY        = 0x0000FFFFFFFFFFFFull; // bits[0..47]
static uint64_t boxPtr(const void * p) { return PTR_HEADER | (uint64_t(p) & PAY); }

// A toy heap object.  `magic` is the integrity witness we read back through the
// JIT'd code after the (possibly moving) safepoint.
struct Obj { uint64_t magic; };
static Obj g_fromSpace;
static Obj g_toSpace;

static constexpr uint64_t kGoodMagic = 0x0BADF00DCAFEBEEFull;
static constexpr uint64_t kPoison    = 0xDEADBEEFDEADBEEFull;

// The "safepoint": a C routine the JIT'd body calls (BLR) before using its
// pointer again.  It models a MOVING collection — it copies the object that the
// value-stack root references from from-space to to-space, REWRITES the root in
// place (mirrors gc.cc:571 visitValue: v.mkClosure(fwdClosure(...))), then
// POISONS the vacated from-space cell so that any stale (un-forwarded) pointer
// read is detectable.  `extern "C"` so the symbol is BLR-callable with the
// AAPCS64 arg in X0.
extern "C" void j3_safepoint(uint64_t * vstack) {
    uint64_t boxed = vstack[0];                 // the GC root, as the JIT spilled it
    Obj * from = reinterpret_cast<Obj *>(boxed & PAY);
    g_toSpace.magic = from->magic;              // "allocate + copy" survivor to to-space
    vstack[0] = boxPtr(&g_toSpace);             // forward the root IN PLACE
    from->magic = kPoison;                      // poison the vacated from-space cell
}

// Emit the shared prologue/epilogue (J2 trampoline ABI): a 16-byte frame holding
// the caller's LR (X30) and X19 (we use X19 as a callee-saved scratch the BLR'd
// C function must preserve).  16-byte frame keeps SP 16-aligned.
static void prologue(Aarch64Emitter & e) {
    e.subImm(SP, SP, 16);
    e.str(X30, SP, 0);     // save return address across the BLR
    e.str(X19, SP, 8);     // save caller's X19
}
static void epilogue(Aarch64Emitter & e) {
    e.ldr(X19, SP, 8);
    e.ldr(X30, SP, 0);
    e.addImm(SP, SP, 16);
    e.ret();
}

int main() {
#if !defined(__aarch64__)
    std::fprintf(stderr, "jit-safepoint-test: aarch64 only.\n"); return 2;
#else
    // Static addresses must fit the 48-bit payload (they always do on aarch64,
    // but assert so the proof can't silently degrade).
    if ((uint64_t(&g_toSpace) >> 48) || (uint64_t(&g_fromSpace) >> 48)) {
        std::fprintf(stderr, "address exceeds 48-bit payload; spike assumption broken\n");
        return 2;
    }

    JitArena arena;
    int failures = 0;
    uint64_t vstack[4];   // the toy value stack (slot 0 = our live root)

    // Body signature: uint64_t body(uint64_t boxedPtr /*X0*/,
    //                               uint64_t * vstack  /*X1*/,
    //                               void * safepointFn /*X2*/)
    // Returns the `magic` it reads back through its pointer after the safepoint.

    // ---- CORRECT body: spill to vstack[0], call safepoint, RELOAD from vstack[0]. ----
    {
        Aarch64Emitter e;
        prologue(e);
        e.mov(X19, X1);          // X19 = vstack base (callee-saved -> survives the BLR)
        e.str(X0, X19, 0);       // SPILL the live root to vstack[0]  (GC-visible)
        e.mov(X0, X19);          // arg0 = vstack for the safepoint
        e.blr(X2);               // safepoint: move + forward-in-place + poison
        e.ldr(X0, X19, 0);       // RELOAD forwarded pointer from the slot
        e.movImm64(X10, PAY);
        e.andReg(X0, X0, X10);   // unbox
        e.ldr(X0, X0, 0);        // deref -> magic of the FORWARDED (to-space) object
        epilogue(e);

        auto body = reinterpret_cast<uint64_t(*)(uint64_t, uint64_t *, void *)>(arena.finalize(e));
        g_fromSpace.magic = kGoodMagic; g_toSpace.magic = 0;
        vstack[0] = 0;
        uint64_t got = body(boxPtr(&g_fromSpace), vstack, reinterpret_cast<void *>(&j3_safepoint));
        bool ok = (got == kGoodMagic)
               && ((vstack[0] & PAY) == (uint64_t(&g_toSpace) & PAY));  // root was forwarded
        std::printf("  %-4s spilled-reload survives move   magic=0x%016" PRIx64 " (want 0x%016" PRIx64 ")\n",
                    ok ? "OK" : "FAIL", got, kGoodMagic);
        if (!ok) ++failures;
    }

    // ---- WRONG control: keep the pointer in a register across the safepoint. ----
    // Still spills to vstack[0] so the safepoint has a root to move, but the body
    // then dereferences its CACHED register copy (the un-forwarded from-space
    // pointer) instead of reloading -> reads the poison.  Proves the spill/reload
    // discipline is load-bearing, not ceremony.
    {
        Aarch64Emitter e;
        prologue(e);
        e.mov(X19, X0);          // KEEP boxedPtr in callee-saved X19 across the call (WRONG)
        e.str(X0, X1, 0);        // (still give the safepoint a root in vstack[0])
        e.mov(X0, X1);           // arg0 = vstack
        e.blr(X2);               // safepoint moves the object + poisons from-space
        e.mov(X0, X19);          // use the STALE register copy (no reload)
        e.movImm64(X10, PAY);
        e.andReg(X0, X0, X10);   // unbox
        e.ldr(X0, X0, 0);        // deref -> magic of the POISONED from-space object
        epilogue(e);

        auto body = reinterpret_cast<uint64_t(*)(uint64_t, uint64_t *, void *)>(arena.finalize(e));
        g_fromSpace.magic = kGoodMagic; g_toSpace.magic = 0;
        vstack[0] = 0;
        uint64_t got = body(boxPtr(&g_fromSpace), vstack, reinterpret_cast<void *>(&j3_safepoint));
        // The WRONG discipline MUST read the poison — that is the UAF this proof
        // exists to demonstrate.  "OK" here means "the hazard reproduced".
        bool ok = (got == kPoison);
        std::printf("  %-4s register-kept reads stale (UAF) magic=0x%016" PRIx64 " (poison 0x%016" PRIx64 ")\n",
                    ok ? "OK" : "FAIL", got, kPoison);
        if (!ok) ++failures;
    }

    std::printf("jit-safepoint-test: %s (%d failures)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
#endif
}
