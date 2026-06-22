// v3 JIT — J1: aarch64 instruction encoder + executable-memory manager.
//
// Stage J1 of lode/JIT_DESIGN_2026-06-19.md (J0 feasibility PROVEN — MAP_JIT +
// W^X toggle works on this host).  This is the FOUNDATION only: a minimal,
// correct aarch64 encoder for the subset a copy-patch body-JIT (J2) emits
// (mov-imm, base+offset load/store, integer ALU, compare, branch, call/ret) plus
// a `JitArena` that mmaps MAP_JIT pages, batches writes under one W^X toggle, and
// flushes the icache.  NO VM integration yet (J2) and NO GC safepoints yet (J3,
// the UAF-risk crux) — a body that allocates is NOT safe to JIT until J3.
//
// Header-only + inline so the VM can include it at J2 without a new TU; until
// then nothing includes it, so it cannot affect the production build.  Validated
// standalone by research/jit_encoder_test.cc (encodes + RUNS generated code).
//
// Encoding references are the ARM Architecture Reference Manual (A64); each
// emitter is unit-checked against a known-good word AND exercised end-to-end.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
// Group.  SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#if defined(__APPLE__)
#  include <pthread.h>
#  include <libkern/OSCacheControl.h>
#endif

namespace nix::v3::jit {

// --------------------------------------------------------------------------
// Aarch64 register names (X = 64-bit, the only width the body-JIT needs since
// v3 Values are 64-bit NaN-boxed words).  W-forms are X-form with sf=0.
// --------------------------------------------------------------------------
enum Reg : uint32_t {
    X0=0,  X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15,
    X16, X17, X18, X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, X29, X30,
    SP=31,            // also ZR depending on instruction class
};
inline constexpr Reg ZR = static_cast<Reg>(31);   // zero register (non-SP class)
enum class Cond : uint32_t {   // condition codes for B.cond / CSET
    EQ=0x0, NE=0x1, CS=0x2, CC=0x3, MI=0x4, PL=0x5, VS=0x6, VC=0x7,
    HI=0x8, LS=0x9, GE=0xA, LT=0xB, GT=0xC, LE=0xD, AL=0xE,
};

// --------------------------------------------------------------------------
// Aarch64Emitter — appends 32-bit little-endian instruction words to a buffer.
// Each emitter returns the byte offset of the emitted instruction so callers
// can patch branch targets (label fixups) afterwards.
// --------------------------------------------------------------------------
class Aarch64Emitter {
public:
    std::vector<uint32_t> code;

    size_t pos() const noexcept { return code.size(); }       // in words
    size_t emit(uint32_t w) { code.push_back(w); return code.size() - 1; }

    // MOVZ Xd, #imm16, LSL #(shift*16)  — zero-extend a 16-bit immediate.
    //   sf=1 opc=10 100101 hw imm16 Rd
    size_t movz(Reg d, uint16_t imm16, unsigned shift = 0) {
        return emit(0xD2800000u | (uint32_t(shift & 3) << 21)
                    | (uint32_t(imm16) << 5) | (d & 31));
    }
    // MOVK Xd, #imm16, LSL #(shift*16)  — keep other bits, set this 16-bit slice.
    size_t movk(Reg d, uint16_t imm16, unsigned shift = 0) {
        return emit(0xF2800000u | (uint32_t(shift & 3) << 21)
                    | (uint32_t(imm16) << 5) | (d & 31));
    }
    // Materialise a full 64-bit constant (1-4 instructions).
    void movImm64(Reg d, uint64_t v) {
        movz(d, uint16_t(v & 0xFFFF), 0);
        if (v >> 16)  movk(d, uint16_t((v >> 16) & 0xFFFF), 1);
        if (v >> 32)  movk(d, uint16_t((v >> 32) & 0xFFFF), 2);
        if (v >> 48)  movk(d, uint16_t((v >> 48) & 0xFFFF), 3);
    }
    // MOV Xd, Xn  (alias for ORR Xd, XZR, Xn).
    size_t mov(Reg d, Reg n) {
        return emit(0xAA0003E0u | (uint32_t(n) << 16) | (d & 31));
    }
    // LDR Xt, [Xn, #imm]  — unsigned offset (imm is in BYTES, must be 8-aligned).
    size_t ldr(Reg t, Reg n, uint32_t byteOff) {
        uint32_t imm12 = (byteOff >> 3) & 0xFFF;
        return emit(0xF9400000u | (imm12 << 10) | (uint32_t(n) << 5) | (t & 31));
    }
    // STR Xt, [Xn, #imm]  — unsigned offset (bytes, 8-aligned).
    size_t str(Reg t, Reg n, uint32_t byteOff) {
        uint32_t imm12 = (byteOff >> 3) & 0xFFF;
        return emit(0xF9000000u | (imm12 << 10) | (uint32_t(n) << 5) | (t & 31));
    }
    // ADD Xd, Xn, Xm   (shifted-register, shift 0).
    size_t add(Reg d, Reg n, Reg m) {
        return emit(0x8B000000u | (uint32_t(m) << 16) | (uint32_t(n) << 5) | (d & 31));
    }
    // SUB Xd, Xn, Xm.
    size_t sub(Reg d, Reg n, Reg m) {
        return emit(0xCB000000u | (uint32_t(m) << 16) | (uint32_t(n) << 5) | (d & 31));
    }
    // MUL Xd, Xn, Xm   (alias for MADD Xd, Xn, Xm, XZR).
    size_t mul(Reg d, Reg n, Reg m) {
        return emit(0x9B000000u | (uint32_t(m) << 16) | (31u << 10)
                    | (uint32_t(n) << 5) | (d & 31));
    }
    // ADD Xd, Xn, #imm12.
    size_t addImm(Reg d, Reg n, uint32_t imm12) {
        return emit(0x91000000u | ((imm12 & 0xFFF) << 10) | (uint32_t(n) << 5) | (d & 31));
    }
    // SUB Xd, Xn, #imm12.
    size_t subImm(Reg d, Reg n, uint32_t imm12) {
        return emit(0xD1000000u | ((imm12 & 0xFFF) << 10) | (uint32_t(n) << 5) | (d & 31));
    }
    // CMP Xn, Xm  (alias for SUBS XZR, Xn, Xm).
    size_t cmp(Reg n, Reg m) {
        return emit(0xEB00001Fu | (uint32_t(m) << 16) | (uint32_t(n) << 5));
    }
    // B <label>  — emit with a placeholder; patch via patchBranch(at, target).
    size_t b() { return emit(0x14000000u); }
    // B.<cond> <label> — placeholder; patch via patchCondBranch.
    size_t bcond(Cond c) { return emit(0x54000000u | uint32_t(c)); }
    // BLR Xn  — call register.
    size_t blr(Reg n) { return emit(0xD63F0000u | (uint32_t(n) << 5)); }
    // RET (X30).
    size_t ret() { return emit(0xD65F03C0u); }

    // Patch an unconditional B at word index `at` to jump to word index `target`.
    void patchBranch(size_t at, size_t target) {
        int32_t off = int32_t(target) - int32_t(at);       // in words (imm26)
        code[at] = 0x14000000u | (uint32_t(off) & 0x03FFFFFF);
    }
    // Patch a B.cond at word index `at` to target word index `target`.
    void patchCondBranch(size_t at, size_t target) {
        int32_t off = int32_t(target) - int32_t(at);       // imm19
        code[at] = (code[at] & 0xFF00001Fu) | ((uint32_t(off) & 0x7FFFF) << 5);
    }
};

// --------------------------------------------------------------------------
// JitArena — executable-memory manager (J1).  Extracted from the proven
// feasibility spike: mmap MAP_JIT pages, copy code under a single W^X toggle,
// flush the icache, hand back a callable function pointer.
// --------------------------------------------------------------------------
class JitArena {
public:
    // Materialise `emitter.code` as executable machine code; returns a callable
    // pointer (nullptr on mmap failure).  Each call gets its own page(s) for
    // simplicity in J1; J2 will sub-allocate within a larger arena.
    void * finalize(const Aarch64Emitter & e) noexcept {
        const size_t len = e.code.size() * sizeof(uint32_t);
        if (len == 0) return nullptr;
        const size_t pages = ((len + 4095) / 4096) * 4096;
        int prot  = PROT_READ | PROT_WRITE | PROT_EXEC;
        int flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__)
        flags |= MAP_JIT;
#endif
        void * page = mmap(nullptr, pages, prot, flags, -1, 0);
        if (page == MAP_FAILED) return nullptr;
#if defined(__APPLE__)
        pthread_jit_write_protect_np(0);              // writable
        std::memcpy(page, e.code.data(), len);
        pthread_jit_write_protect_np(1);              // executable
        sys_icache_invalidate(page, len);
#else
        std::memcpy(page, e.code.data(), len);
        __builtin___clear_cache(reinterpret_cast<char *>(page),
                                reinterpret_cast<char *>(page) + len);
#endif
        regions_.push_back({page, pages});
        return page;
    }

    ~JitArena() {
        for (auto & r : regions_) munmap(r.first, r.second);
    }

private:
    std::vector<std::pair<void *, size_t>> regions_;
};

} // namespace nix::v3::jit
