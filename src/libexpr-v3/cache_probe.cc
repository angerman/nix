/// @file
/// v3 cache-hook per-call-site instrumentation — see
/// `include/v3/cache_probe.hh` for the design.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group. SPDX-License-Identifier: Apache-2.0

#include "v3/cache_probe.hh"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string_view>

namespace nix::v3 {

std::vector<CacheHookCallSite *> & cacheHookSites() noexcept
{
    static std::vector<CacheHookCallSite *> v;
    return v;
}

CacheHookCallSite::CacheHookCallSite(const char * n, const char * p) noexcept
    : name(n), pos(p)
{
    // Function-static instances run this constructor exactly once,
    // the first time the enclosing scope is entered.  Registration
    // is append-only; the global registry's backing vector lives
    // for the lifetime of the process.
    cacheHookSites().push_back(this);
}

const bool g_cacheHookActive = [] {
    const char * v = std::getenv("NIX_VM_CACHE_SITES");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}();

uint64_t CacheHookTimer::activeStart() noexcept
{
    if (__builtin_expect(cacheHookActive(), 0)) [[unlikely]]
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    return 0;
}

uint64_t CacheHookTimer::elapsed(uint64_t start) noexcept
{
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return now > start ? (now - start) : 0;
}

namespace {
/// Format `nsInHook` as a human-readable amount with a one-char
/// magnitude suffix.  Truncates to ~3 significant figures so the
/// dump stays in fixed-width columns.
void formatNs(char * buf, size_t bufsz, uint64_t ns) noexcept
{
    if (ns >= 1'000'000'000ull)
        std::snprintf(buf, bufsz, "%6.2fs", double(ns) / 1e9);
    else if (ns >= 1'000'000ull)
        std::snprintf(buf, bufsz, "%6.2fM", double(ns) / 1e6);
    else if (ns >= 1'000ull)
        std::snprintf(buf, bufsz, "%6.2fK", double(ns) / 1e3);
    else
        std::snprintf(buf, bufsz, "%6lluN", static_cast<unsigned long long>(ns));
}

/// Format byte count with K/M/G suffix.  Same fixed-width convention
/// as `formatNs`.
void formatBytes(char * buf, size_t bufsz, uint64_t bytes) noexcept
{
    if (bytes >= (1ull << 30))
        std::snprintf(buf, bufsz, "%6.2fG", double(bytes) / double(1ull << 30));
    else if (bytes >= (1ull << 20))
        std::snprintf(buf, bufsz, "%6.2fM", double(bytes) / double(1ull << 20));
    else if (bytes >= 1024ull)
        std::snprintf(buf, bufsz, "%6.2fK", double(bytes) / 1024.0);
    else
        std::snprintf(buf, bufsz, "%6lluB", static_cast<unsigned long long>(bytes));
}
} // anonymous

void dumpCacheHookSites(std::FILE * out) noexcept
{
    auto & sites = cacheHookSites();
    // Collect non-zero sites first; skip the dump entirely if nothing
    // fired (avoids noise in routine `NIX_VM_STATS=1` output that
    // doesn't enable the cache-site probe).
    std::vector<CacheHookCallSite *> active;
    active.reserve(sites.size());
    for (auto * s : sites)
        if (s->fires > 0) active.push_back(s);
    if (active.empty()) return;

    // Sort by fires descending, then by name for tie-breaking
    // (reproducible output across runs).
    std::sort(active.begin(), active.end(),
        [](CacheHookCallSite * a, CacheHookCallSite * b) {
            if (a->fires != b->fires) return a->fires > b->fires;
            return std::string_view(a->name) < std::string_view(b->name);
        });

    std::fprintf(out,
        "v3 cache-hook call sites (NIX_VM_CACHE_SITES=1):\n");
    std::fprintf(out,
        "  %-36s %12s %12s %12s %12s %7s %7s  %s\n",
        "site", "fires", "hits", "misses", "inserts",
        "bytes", "ns", "pos");
    for (auto * s : active) {
        char nsBuf[16];
        char bytesBuf[16];
        formatNs(nsBuf, sizeof nsBuf, s->nsInHook);
        formatBytes(bytesBuf, sizeof bytesBuf, s->bytesWritten);
        std::fprintf(out,
            "  %-36s %12llu %12llu %12llu %12llu %7s %7s  %s\n",
            s->name ? s->name : "<unnamed>",
            static_cast<unsigned long long>(s->fires),
            static_cast<unsigned long long>(s->hits),
            static_cast<unsigned long long>(s->misses),
            static_cast<unsigned long long>(s->inserts),
            bytesBuf,
            nsBuf,
            s->pos ? s->pos : "?");
    }
    std::fflush(out);
}

} // namespace nix::v3
