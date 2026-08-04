/// @file
/// v3 VM interning subsystems — extracted from vm.cc (step 2 of the vm.cc
/// split).  PURE MOVE: these definitions are byte-for-byte the ones that used
/// to live in vm.cc; only the linkage of the cross-TU entry points changed
/// (internal → external, declared in v3/vm_internal.hh) so vm.cc's dispatch /
/// creation paths + the GC scavenger / gen-major safepoint can reach them.
///
/// Two subsystems:
///   1. Env-tuple interning (envintern::*, maybeInternUpvalueEnvFromStack,
///      clearEnvInternTable) — a weak epoch-local table sharing byte-identical
///      upvalue-capture tuples across closures/thunks.  DEFAULT-DISABLED
///      (shareAfter returns UINT32_MAX), cleared at the major-GC safepoint.
///   2. Captured-withs singleton interning (internOrAllocSingletonCapWiths,
///      snapshotCurrentWiths, clearCapWithsCache, refreshCapWithsCacheAfter-
///      Scavenge, get*CapWiths* stats) — a 4096-bucket cache sharing 1-element
///      capturedWiths ListVecs.  Its slots are minor-GC roots
///      (singletonCapturedWithsRegistry); gc.cc forwards them then calls
///      refreshCapWithsCacheAfterScavenge to rekey; gen-major clears it.
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
#include "v3/alloc.hh"    // Alloc::allocEnv, Alloc::allocList
#include "v3/barrier.hh"  // envPostConstructBarrier, listPostConstructBarrier, singletonCapturedWithsRegistry

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace nix::v3 {

// Env tuple interning -------------------------------------------------------
//
// Env-sharing moved closure/thunk captures out of inline FAM tails, but the
// first implementation still allocated one fresh Env per runtime object.  This
// weak, epoch-local table shares byte-identical capture tuples across closures
// and thunks, which recovers the common "many lazy siblings close over the same
// lexical state" shape without changing the public object model.
//
// The table is deliberately NOT a GC root.  It is cleared at the outermost
// major-GC safepoint before mark/sweep, just like the Bindings materialize memo,
// so no stale Env* survives a collection.  Minor scavenges may rewrite values
// inside an Env, which can make a bucket miss later; that only loses sharing
// until the next equal tuple is inserted, not correctness.
namespace envintern {
struct Entry {
    Env * env = nullptr;
    uint32_t observations = 0;
    uint16_t nUp = 0;
};

using Bucket = std::vector<Entry>;

inline bool enabled() noexcept
{
    static const bool s_enabled = [] {
        if (std::getenv("NIX_V3_NO_ENV_INTERN")) return false;
        const char * e = std::getenv("NIX_V3_ENV_INTERN");
        if (e) return e[0] != '0';
        return true;
    }();
    return s_enabled;
}

inline std::unordered_map<uint64_t, Bucket> & table()
{
    static thread_local std::unordered_map<uint64_t, Bucket> t;
    return t;
}

inline uint32_t shareAfter(uint16_t nUp) noexcept
{
    static const uint32_t s_override = [] {
        const char * e = std::getenv("NIX_V3_ENV_SHARE_AFTER");
        if (!e || !*e) return 0u;
        char * end = nullptr;
        unsigned long v = std::strtoul(e, &end, 10);
        return end != e ? static_cast<uint32_t>(v) : 0u;
    }();
    if (s_override) return s_override;

    // P0.C (BEAT_TW_V3_PLAN §3.1, 2026-07-03): RETIRED — env-share interning is
    // a structural no-op that never earned its keep, so the DEFAULT never
    // interns (always UINT32_MAX ⇒ maybeInternFromStack returns nullptr ⇒ inline
    // FAM).  FALSIFIER (Rule 0): the heuristic only interned nUp>8 capture tuples
    // reused ≥2×, but the Phase-1 nUp histogram measured avg nUp 1.88–2.10 across
    // hello/firefox/git/M5 (and firefox `envs=0.1 MB` of 677 MB RSS) — i.e. it
    // shared ~nothing while adding a dead 8 B upvalEnv branch on the #1 opcode
    // family + a call per creation.  DEV: the mechanism stays testable via
    // NIX_V3_ENV_SHARE_AFTER=N (the s_override above) for any future A/B.  KEEP
    // the plumbing (Env / upvalEnv / closureUpvalue / walkEnv / ENV_SHARED).
    // (The env-pointer-capture trial that also reused this plumbing was KILLed
    // at Gate C 2026-07-04 and deleted; branch 8eebbe25b preserves it.)
    return UINT32_MAX;
}

template <typename Stack>
inline uint64_t hashStackTuple(const Stack & stack,
                               size_t base,
                               uint16_t nUp) noexcept
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(nUp) * 0xC2B2AE3D27D4EB4Full);
    for (uint16_t i = 0; i < nUp; ++i) {
        uint64_t x = stack[base + i].rawWord();
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        h ^= x; h *= 0x100000001B3ull;
    }
    return h;
}

template <typename Stack>
inline bool sameTuple(const Env * env,
                      const Stack & stack,
                      size_t base,
                      uint16_t nUp) noexcept
{
    if (!env || env->nValues != nUp) return false;
    for (uint16_t i = 0; i < nUp; ++i)
        if (env->values[i].rawWord() != stack[base + i].rawWord())
            return false;
    return true;
}

Env * maybeInternFromStack(VMState & vm, uint16_t nUp)
{
    assert(nUp > 0);
    assert(vm.valueStack.size() >= nUp);
    const size_t base = vm.valueStack.size() - nUp;
    const uint32_t threshold = shareAfter(nUp);
    if (threshold == UINT32_MAX)
        return nullptr;

    if (__builtin_expect(enabled(), 1)) {
        uint64_t h = hashStackTuple(vm.valueStack, base, nUp);
        Bucket & b = table()[h];
        Entry * seed = nullptr;
        for (Entry & e : b) {
            if (e.env && sameTuple(e.env, vm.valueStack, base, nUp)) {
                ++e.observations;
                vm.valueStack.resize(base);
                return e.env;
            }
            if (!e.env && e.nUp == nUp && !seed)
                seed = &e;
        }

        if (!seed) {
            b.push_back(Entry{nullptr, 0, nUp});
            seed = &b.back();
        }
        ++seed->observations;
        if (seed->observations < threshold)
            return nullptr;

        Env * env = Alloc::allocEnv(nUp);
        for (uint16_t i = 0; i < nUp; ++i)
            env->values[i] = vm.valueStack[base + i];
        // P0.A-4 (DEFECT_REVIEW_2026-07-03 §1.9): the interned Env is TENURED but
        // its values[] copy nursery cells off the value stack; register it as a
        // barriered root source at creation.  Previously covered only
        // TRANSITIVELY via each consumer's closure/thunk post-construct scan —
        // one new frame-Env consumer away from a missed root.  One line
        // closes it.
        envPostConstructBarrier(env);
        vm.valueStack.resize(base);
        seed->env = env;
        return env;
    }

    return nullptr;
}

inline void clear() noexcept
{
    table().clear();
}
} // namespace envintern

Env * maybeInternUpvalueEnvFromStack(VMState & vm, uint16_t nUp)
{
    return envintern::maybeInternFromStack(vm, nUp);
}

void clearEnvInternTable() noexcept
{
    envintern::clear();
}

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
// Gate: NIX_V3_NO_CAPWITHS_INTERN=1 reverts every call to a fresh
// allocList for A/B measurement.
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
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_CAPWITHS_INTERN") != nullptr;
    if (__builtin_expect(s_disabled, 0)) {
        ListVec * lws = Alloc::allocList(1);
        lws->elems[0] = v;
        listPostConstructBarrier(lws);
        return lws;
    }
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
