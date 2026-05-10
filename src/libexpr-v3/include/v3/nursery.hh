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
#include <cstdio>
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

struct VMState;

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
/// Nursery memory is malloc'd AND registered with Boehm via
/// `GC_add_roots` at `initLazy` time.  Earlier design assumed
/// Boehm's conservative C-stack scan would suffice: nursery
/// objects' contents (Bridge `bridgeSrc` pointing at TW
/// `nix::Value*`, Closure upvalues holding TW Values, etc.)
/// would be reached through VMState's C-stack references.
/// That assumption was wrong for cross-library exception paths —
/// the `__cxa_rethrow` -> `_Unwind_RaiseException` chain in
/// libc++abi can run while v3 nursery memory holds the only
/// references to TW values, and Boehm reclaiming those values
/// would leave dangling pointers that surface as SIGTRAP at
/// rethrow boundaries.  Registering the nursery as a Boehm root
/// closes that gap at zero perf cost (nursery memory never
/// holds objects Boehm itself manages, so the GC scan over the
/// nursery just walks zero-filled / aligned-payload bytes).
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
        // Lazy init runs unconditionally on first access — sets
        // `enabled` from the env var.  Was previously gated behind
        // `if (!enabled) return nullptr` BEFORE initLazy(), which
        // meant initLazy could never run (latent Phase A bug —
        // tests passed only because fall-back-to-tenured always
        // works).  `inited` separates "we've seen this nursery"
        // from "this nursery is enabled".
        if (!inited) initLazy();
        if (!enabled) return nullptr;
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

    /// True iff `p` lies inside this nursery's backing buffer.
    /// Compared as plain pointer arithmetic on the malloc'd block.
    /// O(1) range check.
    bool contains(const void * p) const noexcept
    {
        return p && static_cast<const char *>(p) >= base
               && static_cast<const char *>(p) < end;
    }

    /// Phase C: heuristic check — true when the nursery is past
    /// the fill threshold and a scavenge should run.  Cheap branch
    /// in the dispatch hot path; the actual scavenge cost is paid
    /// only when this fires.
    bool shouldScavenge() const noexcept
    {
        if (!enabled || !base) return false;
        // 75% fill: leaves headroom so a single opcode that
        // allocates several objects in succession doesn't get
        // half-way through and overflow before the next loop top
        // can call maybeScavenge.
        size_t threshold = sizeBytes - (sizeBytes >> 2);
        return size_t(next - base) >= threshold;
    }

    /// Phase C: scavenge driver.  Triggered between opcodes from
    /// the dispatch loop.  Returns true iff a scavenge ran (caller
    /// must then re-read frame locals from `vm.frames.back()`).
    /// Implementation in `gc.cc`.
    bool maybeScavenge(VMState & vm) noexcept;

    /// Reset the bump pointer; called by `gc.cc` at the end of
    /// `scavengeNursery`.  The buffer keeps its backing memory.
    void resetBumpAfterScavenge() noexcept
    {
        if (base) {
            // Zero only the used region — Boehm's conservative scan
            // is currently triggered through other code paths
            // (`GC_add_roots` on the tenured arena), so the nursery
            // doesn't need a clean slate, but a memset hides any
            // accidentally-retained pointer-shaped bit pattern from
            // future allocators that read uninitialized bytes.
            // Cheap relative to the work we just did.
            std::memset(base, 0, size_t(next - base));
            next = base;
        }
        ++scavengeCount;
    }

    /// Allocator helper for `gc.cc`: bump-pointer allocate `bytes`
    /// from this nursery, ignoring the tenured fall-through path.
    /// Used to implement to-space when (in a future phase) we move
    /// from one half of the nursery to the other.  Currently
    /// unused — scavenge always copies to tenured.
    [[gnu::always_inline]] inline void * tryAllocLocal(size_t bytes) noexcept
    {
        if (!enabled || !base) return nullptr;
        bytes = (bytes + 15) & ~size_t{15};
        if (next + bytes > end) return nullptr;
        void * p = next;
        next += bytes;
        return p;
    }

    /// Toggle from env var on first access.  Default: disabled
    /// (Phase A is purely instrumentation; gates the routing
    /// without correctness risk).
    bool isEnabled() const noexcept { return enabled; }

    /// Phase C: env-var-gated toggle for the actual scavenge.
    /// `NIX_V3_NURSERY_SCAVENGE=1` (default OFF for safe rollout).
    /// Independent of `NIX_V3_NURSERY` so we can route allocations
    /// to the nursery (Phase A) without enabling reclamation
    /// (Phase C) until validated.
    bool isScavengeEnabled() const noexcept { return scavengeEnabled; }

private:
    void initLazy() noexcept
    {
        // Read env vars exactly once.  `inited` is set FIRST so a
        // re-entry from inside getenv (extremely unlikely but
        // defensible) doesn't recurse.
        inited = true;
        const char * gate = std::getenv("NIX_V3_NURSERY");
        if (!gate || gate[0] == '0') {
            enabled = false;
            return;
        }
        enabled = true;
        // Phase C: independent gate for scavenge.  Lets us route
        // allocations to the nursery (Phase A) without reclaiming
        // them (Phase C) until validation completes.
        const char * sg = std::getenv("NIX_V3_NURSERY_SCAVENGE");
        scavengeEnabled = sg && sg[0] != '0';
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
#if NIX_USE_BOEHMGC
        // CORRECTNESS CRITICAL: register the nursery as a Boehm root.
        //
        // The original Phase A design assumed Boehm's conservative
        // C-stack scan would keep nursery objects' contents alive
        // because all v3 roots transitively touch the C stack.
        // That assumption is WRONG for libsForQt5-bypass + v3-direct:
        // nursery `Closure::upvalues[]` and `Thunk::tail[]` cells
        // hold `Tag::Thunk{Bridge}` Values pointing at TW
        // (Boehm-managed) `nix::Value*`s, and the Bridge thunk
        // header itself sits in nursery memory.  When Boehm runs
        // its own collection (it can fire any time during the
        // primop callback chain that bypass triggers), it walks
        // the GC roots looking for live TW values.  The nursery
        // is NOT a Boehm root, so the bridge's `bridgeSrc` ->
        // `nix::Value*` chain is invisible; Boehm reclaims TW
        // values that v3 still references.  Subsequent dereference
        // through the Bridge segfaults / SIGTRAPs.
        //
        // Why the regression suite missed this: synthetic tests
        // produce TW values only via known-rooted paths (TW eval
        // owns the values; Boehm has its own roots into them).
        // The libsForQt5 bypass + nixpkgs eval routes
        // treeWalkerToV3 through chains where the only path to a
        // TW value is via a v3 nursery cell.
        //
        // Why a 512 MB nursery still crashes (no scavenge fires):
        // the bug is in the ROUTING (nursery memory invisible to
        // Boehm), not the SCAVENGING.  Bigger nursery just gives
        // Boehm more time to find the unrooted references and
        // reclaim them mid-eval.
        GC_add_roots(base, base + sizeBytes);
#endif
        next = base;
        end  = base + sizeBytes;
    }

    bool     inited          = false;
    bool     enabled         = false;
    bool     scavengeEnabled = false;
    char *   base    = nullptr;
    char *   next    = nullptr;
    char *   end     = nullptr;
    size_t   sizeBytes = 0;
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
