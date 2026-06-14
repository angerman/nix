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

// Match alloc.hh's NIX_USE_BOEHMGC sourcing (via the v3-owned
// gc-config header — no direct TW include).  Done unconditionally so the
// gate macro is defined before any -Wundef-sensitive use site.  Currently
// the nursery doesn't depend on Boehm directly (the buffer is plain
// malloc'd); the include is for forward-compatibility with Phase D's
// optional GC integration.
#include "v3/gc-config.hh"
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
        // #738 Phase E v0.1 (2026-05-21) survival telemetry.
        // Aggregated across the process lifetime by `gc.cc`.
        //   survivedBytes = cumulative bytes copied nursery -> tenured
        //                   across all scavenges.
        //   diedBytes     = cumulative bytes that were in the nursery
        //                   pre-scavenge but were NOT copied out
        //                   (= reclaimed on `resetBumpAfterScavenge`).
        // ratio diedBytes / (diedBytes + survivedBytes) = young-gen
        // mortality rate — the headline Rule 0 input for Phase E
        // v0.2's two-region survivor-pool design.  When this rate is
        // low (most allocs survive), Phase E won't help; when it's
        // high (most allocs die in their first cycle), Phase E will
        // recover those bytes.
        uint64_t survivedBytes = 0;
        uint64_t diedBytes     = 0;
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
            survivedBytes,
            diedBytes,
        };
    }
    /// Phase E v0.1 — record survival bytes.  Called by gc.cc's
    /// Scavenger after each scavenge run completes.  `surv` is the
    /// number of bytes copied from nursery to tenured during this
    /// scavenge; `pre.used` minus `surv` is the bytes that died.
    void recordSurvival(uint64_t surv, uint64_t preUsed) noexcept
    {
        survivedBytes += surv;
        // Defensive: preUsed should always be >= surv (you can't
        // survive more bytes than were allocated), but clamp at 0
        // to be safe under unusual measurement conditions.
        diedBytes += (preUsed > surv) ? (preUsed - surv) : 0;
    }

    /// Phase E v0.2 — record per-region promotion bytes.  Called
    /// once per scavenge by gc.cc::scavengeNursery, after
    /// Scavenger::run completes and before resetting bumpers.
    /// All three are cumulative across the process lifetime.
    ///
    ///   yToS    — bytes promoted Y → S (age-1 survivors retained).
    ///   sToT    — bytes promoted S → T (age-2 = tenured).
    ///   yToTOvf — bytes promoted Y → T directly because survivor
    ///             buffer was full (Phase E) OR because Phase E is
    ///             disabled (legacy single-region path).
    void recordPhaseEBytes(uint64_t yToS, uint64_t sToT,
                           uint64_t yToTOvf) noexcept
    {
        bytesYToS    += yToS;
        bytesSToT    += sToT;
        bytesYToTOvf += yToTOvf;
    }
    /// Accessors for run.cc's NIX_VM_STATS banner.
    uint64_t getBytesYToS()    const noexcept { return bytesYToS; }
    uint64_t getBytesSToT()    const noexcept { return bytesSToT; }
    uint64_t getBytesYToTOvf() const noexcept { return bytesYToTOvf; }

    /// True iff `p` lies inside this nursery's young buffer (the
    /// bump-allocator-managed region where fresh allocations go).
    /// O(1) range check.
    bool inYoung(const void * p) const noexcept
    {
        return p && static_cast<const char *>(p) >= base
               && static_cast<const char *>(p) < end;
    }

    /// #738 Phase E v0.2 (2026-05-21) — true iff `p` lies inside
    /// the currently-ACTIVE survivor buffer.  The active survivor
    /// holds objects that survived a prior scavenge; they are this
    /// scavenge's promotion candidates (active-S → tenured).
    bool inActiveSurvivor(const void * p) const noexcept
    {
        if (!p || !phaseEEnabled) return false;
        const char * cp = static_cast<const char *>(p);
        return survActiveIdx == 0
            ? (cp >= survBaseA && cp < survEndA)
            : (cp >= survBaseB && cp < survEndB);
    }

    /// True iff `p` lies inside the INACTIVE survivor buffer (the
    /// destination for this scavenge's Y survivors).  Useful during
    /// scavenge for sanity checks; not on the hot path.
    bool inInactiveSurvivor(const void * p) const noexcept
    {
        if (!p || !phaseEEnabled) return false;
        const char * cp = static_cast<const char *>(p);
        return survActiveIdx == 0
            ? (cp >= survBaseB && cp < survEndB)
            : (cp >= survBaseA && cp < survEndA);
    }

    /// True iff `p` lies inside ANY nursery-managed region (young,
    /// or either survivor buffer when Phase E is active).  This is
    /// the API surface that Phase D barriers (in include/v3/barrier.hh)
    /// consult via `isNurseryPayload` to decide whether a write
    /// crosses the inter-gen boundary.  Phase E extends the semantics:
    /// any survivor buffer also counts as "nursery", so barriers
    /// track tenured → survivor edges transparently.
    bool contains(const void * p) const noexcept
    {
        if (!p) return false;
        const char * cp = static_cast<const char *>(p);
        if (cp >= base && cp < end) return true;
        if (!phaseEEnabled) return false;
        if (cp >= survBaseA && cp < survEndA) return true;
        if (cp >= survBaseB && cp < survEndB) return true;
        return false;
    }

    /// #738 Phase E v0.2 — bump-allocate `bytes` from the INACTIVE
    /// survivor buffer (= destination for this scavenge's Y
    /// survivors).  Returns nullptr if survivor buffer overflows or
    /// Phase E is disabled; caller falls back to tenured (promotion
    /// path).  Called only from `Scavenger::fwdX` during scavenge.
    void * targetSurvivorAlloc(size_t bytes) noexcept
    {
        if (!phaseEEnabled) return nullptr;
        bytes = (bytes + 15) & ~size_t{15};
        if (survActiveIdx == 0) {
            if (!survBaseB || survNextB + bytes > survEndB) return nullptr;
            void * p = survNextB;
            survNextB += bytes;
            return p;
        } else {
            if (!survBaseA || survNextA + bytes > survEndA) return nullptr;
            void * p = survNextA;
            survNextA += bytes;
            return p;
        }
    }

    /// True iff Phase E mode is active (NIX_V3_PHASE_E=1 + nursery
    /// itself enabled).  Used by gc.cc's Scavenger to choose
    /// between single-region (Phase D) and two-region (Phase E)
    /// destination dispatch.
    bool isPhaseEActive() const noexcept { return phaseEEnabled; }

    /// Phase C: heuristic check — true when the nursery is past
    /// the fill threshold and a scavenge should run.  Cheap branch
    /// in the dispatch hot path; the actual scavenge cost is paid
    /// only when this fires.
    ///
    /// Threshold default: 75 % fill.  Per `PHASE_E_V02_DAY2_
    /// FALSIFIED_2026-05-27.md` Path A + B: the 75 % threshold
    /// allows opcode bodies that allocate-many-objects-in-succession
    /// to overflow within a single opcode, bypassing the scavenger
    /// trigger (which only fires at dispatch-loop top, between
    /// opcodes).  Path B measured hit rate 19-31 % on HNE / hello —
    /// the architectural safe-point model + 75 % threshold leaves
    /// 81 % / 69 % of allocations bypassing the nursery.
    ///
    /// `NIX_V3_NURSERY_TRIGGER_PCT=N` (1..99) overrides the default
    /// to fire scavenge at N % fill.  Lower N → more frequent scavenge
    /// → less per-opcode overflow → higher hit rate.  But each
    /// scavenge has fixed cost (root walk + copy survivors); more
    /// scavenges = more cumulative wall cost.  Pre-committed for
    /// the Path A iteration:
    ///   N = 75 (default): hit rate 19-31 %, RSS regression 100+ MB
    ///   N = 50: expected hit rate ~50 %, wall regression ?
    ///   N = 25: expected hit rate ~75 %, more wall cost ?
    /// Measure each step against the SHIP gate (RSS Δ ≤ 50 MB).
    bool shouldScavenge() const noexcept
    {
        if (!enabled || !base) return false;
        // `triggerPct` is set in initLazy from
        // NIX_V3_NURSERY_TRIGGER_PCT (default 75).  Threshold =
        // sizeBytes * triggerPct / 100.  size_t arithmetic; the
        // intermediate sizeBytes * 100 doesn't overflow for any
        // sane nursery size (< 100 MB), and the multiply-then-
        // divide avoids floating-point on the hot path.
        size_t threshold = (sizeBytes * triggerPct) / 100;
        return size_t(next - base) >= threshold;
    }

    /// Phase C: scavenge driver.  Triggered between opcodes from
    /// the dispatch loop.  Returns true iff a scavenge ran (caller
    /// must then re-read frame locals from `vm.frames.back()`).
    /// Implementation in `gc.cc`.
    bool maybeScavenge(VMState & vm) noexcept;

    /// Stage 3 prereq (action plan Phase 1.7) — force a scavenge
    /// unconditionally, bypassing both `scavengeEnabled` and
    /// `shouldScavenge()`'s threshold check.  Used by the
    /// `V3_DBG_GC_STRESS=N` debug gate to fire scavenge every N
    /// opcodes regardless of nursery occupancy — surfaces
    /// missed-root bugs that natural scavenge frequency would
    /// otherwise hide.
    ///
    /// Pre-conditions identical to `maybeScavenge`'s caller:
    ///   - Called only at `exitDepth == 0` (per
    ///     `feedback_v3_nursery_cstack_safety.md`).
    ///   - `activeVMStack` contains exactly the caller's `vm`
    ///     (no nested VMState; deferral logic handled by caller).
    ///
    /// Returns true iff a scavenge ran (i.e. the nursery itself
    /// was enabled).  When NIX_V3_NURSERY=0 the nursery is a no-op
    /// allocator and `forceScavenge` returns false — STRESS does
    /// nothing in that case because there's no nursery to clear.
    bool forceScavenge(VMState & vm) noexcept;

    /// #738 Phase E v0.2 — swap active survivor index + reset both
    /// young and the (now-promoted-to-tenured) old-active survivor.
    /// Called by `gc.cc::scavengeNursery` at the end of a scavenge
    /// when Phase E is active, in lieu of `resetBumpAfterScavenge`.
    ///
    /// After this call:
    ///   - young bump pointer reset to base (Y is empty).
    ///   - The buffer that USED to be active (whose contents we
    ///     just promoted to tenured) has its bump pointer reset
    ///     (so future scavenges can reuse it).
    ///   - `survActiveIdx` is flipped so the buffer that NOW holds
    ///     this-cycle's Y-survivors becomes the new active S.
    ///
    /// The #705 NO_RESET diagnostic gate is honoured for both Y and
    /// the demoted-active survivor.
    void swapAndResetAfterScavenge() noexcept
    {
        if (!phaseEEnabled) {
            // Fall back to legacy single-region reset.
            resetBumpAfterScavenge();
            return;
        }
        static const bool s_noReset =
            std::getenv("NIX_V3_NURSERY_NO_RESET") != nullptr;
        if (base) {
            if (!s_noReset) std::memset(base, 0, size_t(next - base));
            next = base;
        }
        // Reset the OLD-active survivor (it just got promoted to
        // tenured; its bytes are now reclaimable).
        if (survActiveIdx == 0) {
            if (survBaseA) {
                if (!s_noReset)
                    std::memset(survBaseA, 0, size_t(survNextA - survBaseA));
                survNextA = survBaseA;
            }
        } else {
            if (survBaseB) {
                if (!s_noReset)
                    std::memset(survBaseB, 0, size_t(survNextB - survBaseB));
                survNextB = survBaseB;
            }
        }
        // Flip active idx: the buffer we just filled with Y
        // survivors becomes the new active S.
        survActiveIdx = 1 - survActiveIdx;
        ++scavengeCount;
    }

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
            //
            // #705 diagnostic gate: `NIX_V3_NURSERY_NO_RESET=1` keeps
            // the buffer's bytes intact so a stale pointer dereference
            // hits VALID old data rather than zeros.  Lets us isolate
            // "memset wiped a missed root" from a downstream logic
            // bug — if the eval completes with NO_RESET=1, the bug is
            // purely "missed root."  Production must always reset.
            static const bool s_noReset =
                std::getenv("NIX_V3_NURSERY_NO_RESET") != nullptr;
            if (!s_noReset) std::memset(base, 0, size_t(next - base));
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
        // #829 / B2 (2026-05-26) — nursery stays opt-in.
        //
        // Attempted default-on flip in this session.  Measurement on
        // hello.drvPath + firefox.drvPath (n=5 hyperfine):
        //
        //   peak_rss (ON vs OFF, mean):  689.0 vs 689.1 MB (hello),
        //                                1459.6 vs 1460.5 MB (firefox)
        //   wall     (ON vs OFF, mean):  906.9 ± 37.2 vs 886.3 ± 7.0 ms
        //                                (ON 2.3 % slower, within σ)
        //   v3_arena reclaim:            ~34 MB consistently
        //   tests:                       12/12 v3-core + 15/15 brute
        //                                PASS under default-on
        //
        // The nursery's 32 MB buffer offsets the ~34 MB arena reclaim
        // exactly, leaving peak_rss flat and wall slightly negative
        // (cache locality benefit < eviction + scavenge overhead).
        //
        // Per measure-twice-cut-once (ship if ≥50 MB peak_rss OR
        // ≥2 % wall reduction): REVERT — no meaningful benefit on
        // these workloads.  Kept opt-in (`NIX_V3_NURSERY=1`)
        // until HNE-class measurement (3 GB peak) justifies the
        // flip; HNE-class workloads may yet show enough absolute
        // benefit to clear the threshold.
        // FLIP (2026-06-15): nursery default-ON; disabled ONLY by explicit
        // NIX_V3_NURSERY=0.  RETIREMENT: drop the opt-out once default-on soaks.
        const char * gate = std::getenv("NIX_V3_NURSERY");
        if (gate && gate[0] == '0') {
            enabled = false;
            return;
        }
        enabled = true;
        // Phase C: scavenge default-ON under the flip (the nursery is a
        // generational GC, not allocate-only); disabled ONLY by explicit
        // NIX_V3_NURSERY_SCAVENGE=0.  RETIREMENT: drop the opt-out once soaked.
        const char * sg = std::getenv("NIX_V3_NURSERY_SCAVENGE");
        scavengeEnabled = !(sg && sg[0] == '0');
        const char * sz = std::getenv("NIX_V3_NURSERY_SIZE");
        size_t mb = 32;
        if (sz) {
            long v = std::strtol(sz, nullptr, 10);
            if (v > 0 && v < 4096) mb = static_cast<size_t>(v);
        }
        sizeBytes = mb * (size_t(1) << 20);
        // Path A (2026-05-27) trigger-percent env-override.  Clamped
        // to [1, 99] to keep the shouldScavenge() arithmetic sane.
        if (const char * tp = std::getenv("NIX_V3_NURSERY_TRIGGER_PCT")) {
            long v = std::strtol(tp, nullptr, 10);
            if (v >= 1 && v <= 99) triggerPct = static_cast<uint32_t>(v);
        }
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
        // #738 Phase E v0.2 (2026-05-21) survivor-pool init.  Gated
        // by NIX_V3_PHASE_E=1.  Allocates two equal-sized buffers
        // for the double-buffered survivor area.  Default survivor
        // buffer = same size as young (~32 MB default).  Total
        // nursery footprint under Phase E ≈ 3 × NIX_V3_NURSERY_SIZE.
        // Tunable via NIX_V3_PHASE_E_SURV_MB.
        const char * pe = std::getenv("NIX_V3_PHASE_E");
        phaseEEnabled = pe && pe[0] != '0';
        if (phaseEEnabled) {
            const char * survSz = std::getenv("NIX_V3_PHASE_E_SURV_MB");
            size_t survMb = mb;  // default: match young
            if (survSz) {
                long v = std::strtol(survSz, nullptr, 10);
                if (v > 0 && v < 4096) survMb = static_cast<size_t>(v);
            }
            survSizeBytes = survMb * (size_t(1) << 20);
            survBaseA = static_cast<char *>(std::calloc(1, survSizeBytes));
            survBaseB = static_cast<char *>(std::calloc(1, survSizeBytes));
            if (!survBaseA || !survBaseB) {
                // Survivor allocation failed; degrade to Phase D
                // single-region behaviour.  Phase E is opt-in so a
                // failed init shouldn't take the whole eval down.
                if (survBaseA) std::free(survBaseA);
                if (survBaseB) std::free(survBaseB);
                survBaseA = survBaseB = nullptr;
                phaseEEnabled = false;
            } else {
                survNextA = survBaseA;
                survNextB = survBaseB;
                survEndA  = survBaseA + survSizeBytes;
                survEndB  = survBaseB + survSizeBytes;
                survActiveIdx = 0;
            }
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
        // Phase E v0.2: register the survivor buffers as Boehm roots
        // too.  Same rationale as the young buffer: survivor cells
        // may hold Bridge thunks whose `bridgeSrc` points at TW
        // values; without Boehm's scan covering survivor memory the
        // bridge contents become invisible across libc++abi unwind.
        if (phaseEEnabled && survBaseA && survBaseB) {
            GC_add_roots(survBaseA, survBaseA + survSizeBytes);
            GC_add_roots(survBaseB, survBaseB + survSizeBytes);
        }
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
    // Path A (2026-05-27, per PHASE_E_V02_DAY2_FALSIFIED_2026-05-27):
    // scavenge-fire threshold as percent of nursery size.
    // Default 75 % (legacy behaviour).  Env-overridable via
    // `NIX_V3_NURSERY_TRIGGER_PCT=N` for N in [1, 99].  Lower N →
    // scavenge fires sooner → higher hit rate at cost of more
    // scavenge cycles.  Set in initLazy().
    uint32_t triggerPct = 75;
    uint64_t allocCount    = 0;
    uint64_t allocBytes    = 0;
    uint64_t overflowCount = 0;
    uint64_t scavengeCount = 0;
    // #738 Phase E v0.1 (2026-05-21) lifetime survival accumulators.
    // See `recordSurvival` for the bump protocol; reported by
    // `stats()` to run.cc's NIX_VM_STATS banner.
    uint64_t survivedBytes = 0;
    uint64_t diedBytes     = 0;

    // #738 Phase E v0.2 (2026-05-21) two-region survivor pool.
    //
    // Architecture:
    //   - young region (this struct's `base..end`): bump-allocator
    //     for fresh objects.  Same role as Phase A/C/D.
    //   - survivor A / survivor B: double-buffered survivor pool.
    //     One is "active" (holds prior-scavenge survivors — this
    //     scavenge's promotion candidates); the other is the
    //     "inactive" target for THIS scavenge's Y survivors.
    //
    // Per scavenge under Phase E:
    //   1. Walk roots + dirty list.
    //   2. fwdX dispatch: Y-source → copy to inactive S (age 1).
    //                     active-S-source → copy to tenured (age 2 = promote).
    //   3. Drain queue: forward interior pointers.
    //   4. `swapAndResetAfterScavenge`: reset Y bump, reset old-active S,
    //      flip `survActiveIdx`.  Now-active S holds Y survivors;
    //      now-inactive S is empty awaiting next-cycle Y survivors.
    //
    // Phase D barriers transparently extend: `contains(p)` returns
    // true for any of the three buffers, so any tenured cell holding
    // a survivor-S pointer is recorded in dirtyContainers when the
    // mutator writes it, AND post-walk in the scavenger for edges
    // the scavenger itself created.
    //
    // Gated by `NIX_V3_PHASE_E=1`.  Default OFF — Phase D single-
    // region behaviour preserved.
    bool   phaseEEnabled = false;
    char * survBaseA = nullptr, * survEndA = nullptr, * survNextA = nullptr;
    char * survBaseB = nullptr, * survEndB = nullptr, * survNextB = nullptr;
    uint8_t survActiveIdx = 0;
    size_t  survSizeBytes = 0;  // per-buffer size; A and B are equal.

    // #738 Phase E v0.2 per-region byte counters (cumulative across
    // process lifetime).  Reported by run.cc's NIX_VM_STATS banner.
    // youngDied is the "true mortality" — bytes that died in Y
    // without ever reaching the survivor pool, the win that Phase E
    // delivers vs Phase D.  survDied is bytes that survived once
    // (Y→S) but died before age 2 — recovered when running.
    uint64_t bytesYToS    = 0;  // Y survivors promoted to S
    uint64_t bytesSToT    = 0;  // S survivors promoted to T
    uint64_t bytesYToTOvf = 0;  // Y survivors promoted directly to T (S full)
};

// GC_AUDIT_ROUND_2 N5 (LATENT, documented 2026-05-21):
// `thread_local Nursery n` has no destructor — base/end/etc. are
// trivially destructible POD.  Today single-threaded so process
// exit reclaims everything.  When multi-threaded eval lands, threads
// that complete during runtime will leak their nursery buffer AND
// their `GC_add_roots(base, base+sizeBytes)` registration; Boehm
// would continue scanning freed/recycled memory as if it held v3
// objects.  Pre-multi-thread checklist: add a Nursery destructor
// that `GC_remove_roots(base, base+size); free(base);` before the
// thread_local goes out of scope.
inline Nursery & threadNursery() noexcept
{
    thread_local Nursery n;
    return n;
}

} // namespace nix::v3
