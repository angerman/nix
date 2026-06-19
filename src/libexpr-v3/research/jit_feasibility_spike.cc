// JIT feasibility spike (JIT-0, goal item 3 first step).
//
// A full JIT of hot lib bodies is a multi-week compiler backend (machine-code
// encoder + register allocation + VM calling convention + GC safepoints +
// byte-identity bail).  Before any of that, the #1 platform-feasibility risk on
// this host (macOS aarch64 / Apple Silicon) must be answered with RUNNABLE code,
// not a design doc: can we allocate executable memory and run generated machine
// code at all?  Apple Silicon enforces W^X — a page is either writable or
// executable, never both — and JIT requires either the `com.apple.security.cs.
// allow-jit` entitlement or the MAP_JIT mapping + pthread_jit_write_protect_np()
// toggling.  If THIS fails, the whole JIT track needs a different strategy
// (e.g. an out-of-process compile, or AOT-only).  So this spike is the genuine
// gate.
//
// What it does: mmap an executable page (MAP_JIT on Darwin), emit a trivial
// aarch64 function `int f() { return 42; }` as raw machine code, flush the
// icache, call it, and verify it returns 42.  Exit 0 = JIT mechanism works on
// this host; nonzero = blocked (prints why).
//
// Build/run:  nix develop -c make -C src/libexpr-v3/research jit-spike
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

#if defined(__APPLE__)
#  include <pthread.h>          // pthread_jit_write_protect_np
#  include <libkern/OSCacheControl.h>  // sys_icache_invalidate
#endif

int main()
{
#if !defined(__aarch64__)
    std::fprintf(stderr, "jit-spike: not aarch64 — this spike encodes aarch64 only.\n");
    return 2;
#else
    // aarch64 machine code for: int f() { return 42; }
    //   movz w0, #42      -> 0x52800540   (MOVZ Wd=0, imm16=42)
    //   ret               -> 0xD65F03C0
    const uint32_t code[] = { 0x52800540u, 0xD65F03C0u };
    const size_t   len    = sizeof(code);

    // Allocate an executable page.  On Darwin/Apple Silicon the kernel enforces
    // W^X, so we map with MAP_JIT and toggle write-protect around the write.
    int prot  = PROT_READ | PROT_WRITE | PROT_EXEC;
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
    flags |= MAP_JIT;
#endif
    void * page = mmap(nullptr, 4096, prot, flags, -1, 0);
    if (page == MAP_FAILED) {
        std::perror("jit-spike: mmap(PROT_EXEC, MAP_JIT) FAILED");
        std::fprintf(stderr,
            "jit-spike: VERDICT = JIT BLOCKED on this host (no executable mmap).\n");
        return 1;
    }

#if defined(__APPLE__)
    // Enter write mode (page becomes writable, not executable), copy, then leave
    // write mode (page becomes executable, not writable).  Per-thread on Darwin.
    pthread_jit_write_protect_np(0);   // 0 = writable
#endif
    std::memcpy(page, code, len);
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);   // 1 = executable
    sys_icache_invalidate(page, len);  // generated code must be visible to I-cache
#else
    __builtin___clear_cache(reinterpret_cast<char *>(page),
                            reinterpret_cast<char *>(page) + len);
#endif

    using Fn = int (*)();
    Fn f = reinterpret_cast<Fn>(page);
    int got = f();
    munmap(page, 4096);

    if (got == 42) {
        std::printf("jit-spike: generated code returned %d (expected 42)\n", got);
        std::printf("jit-spike: VERDICT = JIT MECHANISM WORKS on this host "
                    "(MAP_JIT + W^X toggle + icache flush + call OK).\n");
        return 0;
    }
    std::fprintf(stderr,
        "jit-spike: generated code returned %d (expected 42) — encoding/call wrong.\n",
        got);
    return 1;
#endif
}
