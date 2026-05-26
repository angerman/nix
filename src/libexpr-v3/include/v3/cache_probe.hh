#pragma once
/// @file
/// v3 cache-hook per-call-site instrumentation (#827 / A3 / T1.1).
///
/// Generalises the per-site-counter pattern that #795 Phase A1
/// (v3ToTwBySite) and #2103cdddb (ifdProbeWithCtx[16]) independently
/// proved out, into reusable infrastructure for any cache-hook call
/// site (CU disk cache, EvalResults Phase 4b, AttrSelect IC, ...).
///
/// Motivation.  Three independent investigations in three days
/// (Phase 4b cache scope `35564703f`, CU-disk-cache cold-tax artifact
/// `fe678273a`, disk_cache PK collision `9e09a7e4c`) each spent hours
/// because the existing aggregate cache stats (`disk_cache::Stats`)
/// don't tell us WHICH call site fired which cache hook with which
/// hit/miss/insert profile.  A per-call-site breakdown surfaces the
/// blind spot directly: if `primImport-cu-disk @ primops.cc:8074`
/// fires on nixpkgs-internal paths (e.g. `<nixpkgs>/lib/strings.nix`)
/// when the cache scope was meant to cover only IFD-class imports,
/// the dump tells you immediately.
///
/// Design.
///   * One `CacheHookCallSite` struct per instrumented site.
///     Function-static so each call site auto-instances once at
///     first entry.  Self-registers into the global site list via
///     the constructor.
///   * `CACHE_HOOK_DEFINE_SITE(VAR, NAME)` declares a function-static
///     site (run-once construction).  Use at the top of a function or
///     scope that owns the cache check.
///   * Per-event helpers (`cacheHookHit`, `cacheHookMiss`,
///     `cacheHookInsert`, `cacheHookBytesWritten`) bump the
///     corresponding counter.  Each helper is one branch when the
///     `NIX_VM_CACHE_SITES` env-var gate is off (the cached bool
///     `cacheHookActive()` returns false; counters skipped).
///   * `CacheHookTimer` RAII wraps a `++fires` + elapsed-ns
///     accumulator.  Use at the entry to the cache-checked path:
///     `CacheHookTimer t(_site);` (only when active).
///   * Dump under `NIX_VM_CACHE_SITES=1` via
///     `dumpCacheHookSites(stderr)`; sorted by `fires` descending.
///
/// Cost when env-var off: one branch test per cache-hook event
/// (predicted not-taken; ~0.5 cycle).  Cost when on: ~5-10 cycles
/// per event (one cache-line write per counter bump).  Per-site nsec
/// timing adds ~10 ns per cache-checked operation (steady_clock read).
///
/// AR1 compliance (per `NEXT_STEPS_2026-05-25.md` §8.5): this is
/// diagnostic-on-demand instrumentation.  It is NOT stripped under
/// V3_RELEASE (the cached-bool branch costs ~0 when off; the struct
/// pointers + global registry are tiny static data).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group. SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstdio>
#include <vector>

namespace nix::v3 {

/// One per-call-site counter struct.  Function-static instances
/// auto-register into the global site list via the constructor on
/// first call.
struct CacheHookCallSite
{
    /// Stable identifier for the dump (e.g. "primImport-cu-disk").
    /// Caller's job to ensure uniqueness across sites.
    const char * name = nullptr;
    /// Compile-time source position (`__FILE__ ":" V3_STR(__LINE__)`).
    const char * pos = nullptr;

    /// Number of times the cache-checked code path was entered.
    uint64_t fires = 0;
    /// Cache-lookup hits (entry returned without recomputation).
    uint64_t hits = 0;
    /// Cache-lookup misses (entry not found; fell through to compute).
    uint64_t misses = 0;
    /// New entries added to the cache (after a miss + compute).
    uint64_t inserts = 0;
    /// Sum of `bytes` written across `inserts`.  Caller passes the
    /// blob size on insert.  Useful for "is this cache growing
    /// unbounded?" investigations.
    uint64_t bytesWritten = 0;
    /// Total nanoseconds spent inside the cache-checked path
    /// (accumulated by `CacheHookTimer` RAII).  Only valid when
    /// `NIX_VM_CACHE_SITES=1` was set at the call.
    uint64_t nsInHook = 0;

    /// Constructor self-registers into the global site list so
    /// `dumpCacheHookSites` can iterate every instrumented site.
    /// Use only as the initialiser of a function-static instance —
    /// other use cases (heap, locals) would leak the registry.
    CacheHookCallSite(const char * n, const char * p) noexcept;
};

/// Append-only global site registry.  Constructors push; never
/// remove.  Backing storage is a function-static `std::vector`
/// initialised on first call.
std::vector<CacheHookCallSite *> & cacheHookSites() noexcept;

/// Cached check for `NIX_VM_CACHE_SITES=1`.  Initialised on first
/// call.  Inline so the branch predicts well: when off the helper
/// short-circuits with a load + test + jne.
[[gnu::always_inline]] inline bool cacheHookActive() noexcept;

extern const bool g_cacheHookActive;

inline bool cacheHookActive() noexcept { return g_cacheHookActive; }

/// Per-event helpers.  Each is one branch when the gate is off.
/// `s` is a `CacheHookCallSite &` (typically a function-static
/// declared via `CACHE_HOOK_DEFINE_SITE`).

[[gnu::always_inline]] inline void
cacheHookFire(CacheHookCallSite & s) noexcept
{
    if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]] ++s.fires;
}

[[gnu::always_inline]] inline void
cacheHookHit(CacheHookCallSite & s) noexcept
{
    if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]] ++s.hits;
}

[[gnu::always_inline]] inline void
cacheHookMiss(CacheHookCallSite & s) noexcept
{
    if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]] ++s.misses;
}

[[gnu::always_inline]] inline void
cacheHookInsert(CacheHookCallSite & s, uint64_t bytes) noexcept
{
    if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]] {
        ++s.inserts;
        s.bytesWritten += bytes;
    }
}

/// RAII timer.  Constructor calls `cacheHookFire`; destructor adds
/// elapsed ns to `nsInHook` when active.  Use at the top of the
/// cache-checked function/scope:
///
///   CACHE_HOOK_DEFINE_SITE(siteImport, "primImport-cu-disk");
///   CacheHookTimer t(siteImport);
///   if (auto v = cache.lookup(key)) {
///       cacheHookHit(siteImport);
///       return *v;
///   }
///   cacheHookMiss(siteImport);
///   auto v = compute(...);
///   cache.insert(key, v);
///   cacheHookInsert(siteImport, v.size());
///   return v;
class CacheHookTimer
{
public:
    [[gnu::always_inline]] explicit CacheHookTimer(CacheHookCallSite & s) noexcept
        : site(s), start(activeStart())
    {
        cacheHookFire(s);
    }

    [[gnu::always_inline]] ~CacheHookTimer() noexcept
    {
        if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]]
            site.nsInHook += elapsed(start);
    }

    CacheHookTimer(const CacheHookTimer &)             = delete;
    CacheHookTimer(CacheHookTimer &&)                  = delete;
    CacheHookTimer & operator=(const CacheHookTimer &) = delete;
    CacheHookTimer & operator=(CacheHookTimer &&)      = delete;

private:
    CacheHookCallSite & site;
    uint64_t start;

    static uint64_t activeStart() noexcept;
    static uint64_t elapsed(uint64_t start) noexcept;
};

/// Stringify helper for `CACHE_HOOK_DEFINE_SITE` (handles
/// `__LINE__` macro expansion).
#define V3_CACHE_HOOK_STRINGIFY2(x) #x
#define V3_CACHE_HOOK_STRINGIFY(x)  V3_CACHE_HOOK_STRINGIFY2(x)

/// Declare a function-static `CacheHookCallSite`.  Use once at the
/// top of a function or scope that owns the cache check.  `VAR`
/// is the local name; `NAME` is the string identifier used in the
/// dump.  Captures `__FILE__:__LINE__` automatically.
///
///   void primImport(...) {
///       CACHE_HOOK_DEFINE_SITE(siteCu, "primImport-cu-disk");
///       CacheHookTimer t(siteCu);
///       ...
///   }
#define CACHE_HOOK_DEFINE_SITE(VAR, NAME) \
    static ::nix::v3::CacheHookCallSite VAR{ \
        NAME, __FILE__ ":" V3_CACHE_HOOK_STRINGIFY(__LINE__)}

/// Dump every registered site to `out` (sorted by `fires`
/// descending, then by name).  Skipped entirely when no site
/// has fired.  Caller is responsible for the env-var gate
/// (NIX_VM_CACHE_SITES=1).
void dumpCacheHookSites(std::FILE * out) noexcept;

} // namespace nix::v3
