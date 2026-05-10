// Cheney nursery — young-generation copying GC for short-lived
// thunk/closure/bindings/list allocations.  Background and design:
// `lode/CHENEY_NURSERY_DESIGN.md`.
//
// Phase A (this file's initial form): bump-pointer allocator with
// fall-back-to-tenured-on-overflow.  No scavenge yet.  Gated by
// `NIX_V3_NURSERY=1`; default OFF.
//
// Per-thread (`thread_local`) instance, mirroring `threadArena()`.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
// Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Match alloc.hh's NIX_USE_BOEHMGC sourcing (it pulls
// nix/expr/config.hh).  Done unconditionally so the gate macro is
// defined before any -Wundef-sensitive use site.  Currently the
// nursery doesn't depend on Boehm directly (the buffer is plain
// malloc'd); the include is for forward-compatibility with
// Phase D's optional GC integration.
#include "nix/expr/config.hh"
#if NIX_USE_BOEHMGC
#include <gc/gc.h>
#endif

namespace nix::v3 {

/// Nursery — fixed-size bump-pointer young generation.
///
/// Lifetime: per-thread (thread_local), zero-initialised on first
/// access.  The backing buffer is malloc'd lazily on first
/// allocation request.  Default size 32 MB; tunable via
/// `NIX_V3_NURSERY_SIZE` (megabytes, integer).
///
/// Allocation: bumps `next`; on overflow, Phase A falls back to
/// the tenured arena (so the existing alloc semantics are
/// preserved).  Phase C will add a scavenge pass that copies live
/// nursery objects to the tenured arena and resets `next`.
///
/// Nursery memory is malloc'd, NOT registered with Boehm via
/// `GC_add_roots`.  This is intentional: nursery objects are
/// short-lived; their roots are reachable through `VMState`'s C++
/// references which are visible to Boehm's conservative C-stack
/// scan.  Once we add scavenge, all surviving objects move to
/// tenured (which IS Boehm-rooted), and the nursery itself never
/// needs to be reclaimed by Boehm — we reuse the same buffer
/// across scavenges.
class Nursery
{
public:
    /// Bump-pointer allocate `bytes` (already 16-byte aligned).
    /// Returns `nullptr` if the nursery is disabled, or if the
    /// allocation doesn't fit AND fallback isn't available; the
    /// caller is expected to fall through to the tenured arena
    /// in that case.  Phase A: any time we'd overflow, return
    /// nullptr immediately so the caller routes to tenured.
    /// Phase C: scavenge first, retry once.
    void * tryAlloc(size_t bytes) noexcept
    {
        if (!enabled) return nullptr;
        if (!base) initLazy();
        // Caller already aligned to 16, but defensively re-align.
        bytes = (bytes + 15) & ~size_t{15};
        if (next + bytes > end) {
            // Phase A: no scavenge yet — caller must fall back.
            ++overflowCount;
            return nullptr;
        }
        void * p = next;
        next += bytes;
        ++allocCount;
        allocBytes += bytes;
        return p;
    }

    /// Stats accessor — used by V3_DBG_FORCES progress and atexit
    /// dump to expose nursery health (allocations, overflows).
    struct Stats
    {
        size_t   sizeBytes;
        size_t   used;        // bytes consumed since last reset
        uint64_t allocCount;  // lifetime alloc count
        uint64_t allocBytes;  // lifetime alloc bytes
        uint64_t overflowCount;
        uint64_t scavengeCount;
    };
    Stats stats() const noexcept
    {
        return Stats{
            sizeBytes,
            base ? size_t(next - base) : 0,
            allocCount,
            allocBytes,
            overflowCount,
            scavengeCount,
        };
    }

    /// Phase B/C entry point — copies live nursery objects to
    /// tenured and resets `next`.  Phase A: stub that does nothing
    /// (nursery already drained back to base via fall-through).
    void scavengeStub() noexcept
    {
        // Phase A intentionally a no-op — fall-throughs in
        // tryAlloc keep tenured allocations correct without
        // resetting the nursery.  Resetting prematurely would
        // free still-live nursery objects.
        //
        // Phase C will replace this with the real Cheney pass.
    }

    /// Toggle from env var on first access.  Default: disabled
    /// (Phase A is purely instrumentation; gates the routing
    /// without correctness risk).
    bool isEnabled() const noexcept { return enabled; }

private:
    void initLazy() noexcept
    {
        // Read env vars exactly once.
        const char * gate = std::getenv("NIX_V3_NURSERY");
        if (!gate || gate[0] == '0') {
            enabled = false;
            return;
        }
        enabled = true;
        const char * sz = std::getenv("NIX_V3_NURSERY_SIZE");
        size_t mb = 32;
        if (sz) {
            long v = std::strtol(sz, nullptr, 10);
            if (v > 0 && v < 4096) mb = static_cast<size_t>(v);
        }
        sizeBytes = mb * (size_t(1) << 20);
        // calloc gives zero-filled pages; matches arena behaviour
        // (zero-fill avoids stale ptr-shaped bytes that would
        // confuse Boehm's conservative scan if the nursery's
        // address ever ended up on the C stack).
        base = static_cast<char *>(std::calloc(1, sizeBytes));
        if (!base) {
            // Allocation failed; treat as disabled so the alloc
            // path falls through to tenured.
            enabled = false;
            sizeBytes = 0;
            return;
        }
        next = base;
        end  = base + sizeBytes;
    }

    bool   enabled = false;
    char * base    = nullptr;
    char * next    = nullptr;
    char * end     = nullptr;
    size_t sizeBytes = 0;
    uint64_t allocCount    = 0;
    uint64_t allocBytes    = 0;
    uint64_t overflowCount = 0;
    uint64_t scavengeCount = 0;
};

inline Nursery & threadNursery() noexcept
{
    thread_local Nursery n;
    return n;
}

} // namespace nix::v3
