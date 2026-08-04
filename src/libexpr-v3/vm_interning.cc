/// @file
/// v3 VM captured-withs singleton interning — extracted from vm.cc (step 2 of the
/// vm.cc split).  PURE MOVE: these definitions are byte-for-byte the ones that
/// used to live in vm.cc; only the linkage of the cross-TU entry points changed
/// (internal → external, declared in v3/vm_internal.hh) so vm.cc's dispatch /
/// creation paths + the GC scavenger / gen-major safepoint can reach them.
///
/// Captured-withs singleton interning (internOrAllocSingletonCapWiths,
/// snapshotCurrentWiths, clearCapWithsCache, refreshCapWithsCacheAfterScavenge,
/// get*CapWiths* stats) — a 4096-bucket cache sharing 1-element capturedWiths
/// ListVecs.  Its slots are minor-GC roots (singletonCapturedWithsRegistry);
/// gc.cc forwards them then calls refreshCapWithsCacheAfterScavenge to rekey;
/// gen-major clears it.
///
/// (The Env-tuple interning subsystem — envintern::*,
/// maybeInternUpvalueEnvFromStack, clearEnvInternTable — that also lived here was
/// RETIRED 2026-08: it was default-disabled (shareAfter returned UINT32_MAX ⇒
/// always nullptr ⇒ inline-FAM), interned ~nothing on real workloads, and its
/// only opt-in was an A/B knob.  Purged with the dead Closure::upvalEnv /
/// THUNK_ENV_SHARED plumbing it fed.)
///
/// pushCapturedWiths (the ~20-call-site hot loop reading a capturedWiths ListVec
/// onto vm.withStack at every apply) deliberately stays `inline` in vm.cc —
/// out-of-lining a 3-line push_back loop on the call hot path is an unjustified
/// perf risk.  It is NOT part of this extraction.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm_internal.hh"
#include "v3/vm.hh"       // VMState, Env, ListVec, Value, Tag
#include "v3/alloc.hh"    // Alloc::allocList
#include "v3/barrier.hh"  // listPostConstructBarrier, singletonCapturedWithsRegistry

#include <cstdint>
#include <vector>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// EXIT_GC_SPIRAL Week 2 Day 13-15 (2026-05-29): singleton interning pool
// for 1-element capturedWiths ListVecs.
//
// T1_3_PAIRS_LISTS_ATTR_2026-05-27 measured 544 K MAKE_THUNK allocs on HNE
// with avg-size 1.04, max 2 — overwhelmingly nWiths==1.  After the 8-byte
// Value flip, each 1-element ListVec is 16 B before allocator rounding.
// Interning shares one ListVec across all Thunks/Closures capturing the same
// with-target, eliminating ~95% of the per-call alloc cost.
//
// Correctness model:
//   * ListVec is set-once at MAKE time and read-only thereafter.
//     pushCapturedWiths only READS via ->size / ->elems[i].  Sharing
//     is therefore safe — no inter-thunk mutation.
//   * Allocated through Alloc::allocList(), so the ListVec itself may live in
//     the moving nursery.  Cache bucket slots are registered as minor-GC roots;
//     scavenge forwards them in place before resetting the nursery.
//   * Phase D / nursery generational correctness: first miss installs the
//     ListVec and calls listPostConstructBarrier().  If the ListVec is tenured
//     and its element is young, the dirty list makes the next scavenge walk it.
//   * Cache keys are refreshed after every scavenge from the forwarded
//     ListVec::elems[0].  This avoids stale young-address keys false-hitting
//     after the nursery reuses an old address for a different object.
//
// Memory footprint of the cache itself: 4096 buckets * 24 B = 96 KB.
//
// The intern is unconditional (the NIX_V3_NO_CAPWITHS_INTERN A/B opt-out,
// which reverted every call to a fresh allocList, was retired).
//
// Retirement criterion (Rule 0): retire the cache when (a) Phase E
// v0.2 default-on makes nursery-allocated ListVecs cheap enough to
// drop the per-call cost, OR (b) Stage 6 production GC reclaims
// per-call ListVecs unaided.  Until then, keep the cache.

namespace {

constexpr size_t kCapWithsCacheBuckets = 4096;

struct CapWithsCacheEntry {
    uint64_t key_tag_payload;
    uint64_t key_payload_raw;
    ListVec * value;  // nullptr → empty entry
};

static CapWithsCacheEntry s_capWithsCache[kCapWithsCacheBuckets] = {};
static uint64_t s_capWithsHits     = 0;
static uint64_t s_capWithsMisses   = 0;
static uint64_t s_capWithsEvicts   = 0;

inline size_t hashCapWithsKey(uint64_t tp, uint64_t pr) noexcept
{
    uint64_t h = tp * 0x9e3779b97f4a7c15ull;
    h ^= pr * 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    return static_cast<size_t>(h) & (kCapWithsCacheBuckets - 1);
}

inline void registerCapWithsCacheSlotsOnce() noexcept
{
    static const bool s_registered = [] {
        auto & roots = singletonCapturedWithsRegistry();
        roots.reserve(roots.size() + kCapWithsCacheBuckets);
        for (CapWithsCacheEntry & e : s_capWithsCache)
            roots.push_back(&e.value);
        return true;
    }();
    (void)s_registered;
}

inline void internalRefreshCapWithsCacheAfterScavenge() noexcept
{
    for (CapWithsCacheEntry & e : s_capWithsCache) {
        if (!e.value) {
            e.key_tag_payload = 0;
            e.key_payload_raw = 0;
            continue;
        }
        if (e.value->size != 1) {
            e.key_tag_payload = 0;
            e.key_payload_raw = 0;
            e.value = nullptr;
            continue;
        }
        const Value & v = e.value->elems[0];
        e.key_tag_payload = v.rawWord();
        e.key_payload_raw = reinterpret_cast<uint64_t>(v.asRaw());
    }
}

// File-scope accessors require external linkage so run.cc can call
// them.  The statics they read live in the unnamed inner namespace
// above (internal linkage, but visible within this TU).
inline uint64_t internalCapWithsHits()    noexcept { return s_capWithsHits; }
inline uint64_t internalCapWithsMisses()  noexcept { return s_capWithsMisses; }
inline uint64_t internalCapWithsEvicts()  noexcept { return s_capWithsEvicts; }

} // anonymous

void clearCapWithsCache() noexcept
{
    for (CapWithsCacheEntry & e : s_capWithsCache) {
        e.key_tag_payload = 0;
        e.key_payload_raw = 0;
        e.value = nullptr;
    }
}

/// Intern-or-allocate a 1-element ListVec capturing `v`.  Hit returns
/// the existing arena pointer in O(1); miss allocates fresh, installs
/// (overwriting any colliding entry — collisions are cheaper than
/// chaining at this scale).
ListVec * internOrAllocSingletonCapWiths(const Value & v) noexcept
{
    // Under the moving nursery, the cache's static slots are explicit scavenge
    // roots: gc.cc forwards each ListVec* and then asks vm.cc to refresh the key
    // from the forwarded element.  That keeps the cache sound without falling
    // back to one ListVec allocation per captured `with`.
    registerCapWithsCacheSlotsOnce();
    const uint64_t tp = v.rawWord();
    const uint64_t pr = reinterpret_cast<uint64_t>(v.asRaw());
    const size_t idx = hashCapWithsKey(tp, pr);
    CapWithsCacheEntry & e = s_capWithsCache[idx];
    if (e.value
        && e.key_tag_payload == tp
        && e.key_payload_raw == pr)
    {
        ++s_capWithsHits;
        return e.value;
    }
    ++s_capWithsMisses;
    if (e.value) ++s_capWithsEvicts;  // collision: prior entry replaced
    ListVec * lws = Alloc::allocList(1);
    lws->elems[0] = v;
    listPostConstructBarrier(lws);
    e.key_tag_payload = tp;
    e.key_payload_raw = pr;
    e.value = lws;
    return lws;
}

/// Snapshot the current frame's visible with-stack (entries from
/// `withStackBase` to top) into a fresh ListVec.  Returns nullptr when
/// no withs are currently in scope (cheap fast-path for the common case
/// of no enclosing `with`).
ListVec * snapshotCurrentWiths(VMState & vm)
{
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    size_t top  = vm.withStack.size();
    if (top <= base) return nullptr;
    uint32_t n = static_cast<uint32_t>(top - base);
    // Day 13-15 (2026-05-29): intern the 1-element case via the
    // singleton pool — same correctness model as the OP_MAKE_THUNK
    // path (see internOrAllocSingletonCapWiths above).  Larger
    // sizes fall through to per-call alloc.  snapshotCurrentWiths
    // is the secondary T1.3 site (vm.cc:6991 in the 2026-05-27
    // numbering, 5.13 MB on HNE, avg-size 1.38 max 339): some
    // fraction will hit the size-1 fast path.
    if (n == 1) {
        return internOrAllocSingletonCapWiths(vm.withStack[base]);
    }
    ListVec * out = Alloc::allocList(n);
    for (uint32_t i = 0; i < n; ++i)
        out->elems[i] = vm.withStack[base + i];
    listPostConstructBarrier(out);  // Phase D coverage
    return out;
}

// EXIT_GC_SPIRAL Day 13-15 (2026-05-29): external-linkage wrappers around the
// internal capWiths stats + scavenge refresh.  Declared in v3/vm.hh so run.cc's
// NIX_VM_STATS dump can read the counters and gc.cc's post-scavenge hook can
// rekey the cache across the TU boundary.
uint64_t getCapWithsHits()   noexcept { return internalCapWithsHits(); }
uint64_t getCapWithsMisses() noexcept { return internalCapWithsMisses(); }
uint64_t getCapWithsEvicts() noexcept { return internalCapWithsEvicts(); }
void refreshCapWithsCacheAfterScavenge() noexcept
{
    internalRefreshCapWithsCacheAfterScavenge();
}

} // namespace nix::v3
