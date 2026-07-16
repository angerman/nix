/// @file
/// AOT cache mmap reader implementation.  See include/v3/aot_cache.hh
/// for the file format + API.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/aot_cache.hh"
#include "v3/serialize.hh"   // WS5-B2: readSparseTables for the canonical table
#include "v3/ir.hh"          // WS5-B2: globalSeedSymbol / reserveSymbolCapacity
#include "v3/alloc.hh"       // WS5-B2: seedPosSnapshotAt / reservePosCapacity

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace nix::v3::aot_cache {

namespace {

// Format constants — must mirror bench/build-aot-cache.py exactly.
constexpr const char kMagic[8] = {'V','3','A','O','T','C','0','1'};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kHeaderSize = 16;       // magic + fmt + count
constexpr size_t kEntrySize  = 56;       // key + tbl + pad + off + len

// Reader state — singleton.  Lazy init on first lookup() or eager
// via init().
struct Reader {
    // Atomic flags so concurrent lookups can early-exit without
    // taking the mutex.
    std::atomic<bool> initStarted{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> enabled{false};

    // Mutex protects the init path.  Held only during init; after
    // init, all reads are lock-free (the mmap'd region is read-only
    // and the entry table doesn't change).
    std::mutex initMutex;

    // Mapped file.
    void *      map      = nullptr;
    size_t      mapBytes = 0;
    int         fd       = -1;

    // Pointers into map (post-init).
    const uint8_t * entries = nullptr;   // pointer to first entry header
    uint32_t        nEntries = 0;
};

Reader & reader() noexcept
{
    static Reader r;
    return r;
}

Stats & mutableStats() noexcept
{
    static Stats s;
    return s;
}

// Read little-endian u32 / u64 from a byte pointer.  Mmap'd file is
// little-endian per the format spec.
uint32_t loadU32(const uint8_t * p) noexcept
{
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
uint64_t loadU64(const uint8_t * p) noexcept
{
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// Compare a key embedded at entry index `i` against `target`.
// Returns negative, zero, positive (memcmp-style).
int compareKey(const uint8_t * entries, size_t i,
               const disk_cache::CacheKey & target) noexcept
{
    return std::memcmp(entries + i * kEntrySize, target.bytes, 32);
}

// Attempt initialisation.  Returns true on success.  Mutex held.
bool tryInitLocked()
{
    auto & r = reader();
    if (r.ready.load(std::memory_order_acquire)) return r.enabled.load();

    const char * path = std::getenv("NIX_V3_AOT_CACHE_FILE");
    if (!path || !*path) {
        r.ready.store(true, std::memory_order_release);
        return false;
    }

    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: open('%s') failed (errno=%d); "
            "AOT cache disabled for this process\n", path, errno);
        r.ready.store(true, std::memory_order_release);
        return false;
    }

    struct stat st {};
    if (::fstat(fd, &st) < 0 || st.st_size < (off_t)kHeaderSize) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: fstat / size check failed on '%s'\n",
            path);
        ::close(fd);
        r.ready.store(true, std::memory_order_release);
        return false;
    }
    const size_t fileSize = static_cast<size_t>(st.st_size);

    void * m = ::mmap(nullptr, fileSize, PROT_READ,
                       MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: mmap('%s', %zu) failed (errno=%d)\n",
            path, fileSize, errno);
        ::close(fd);
        r.ready.store(true, std::memory_order_release);
        return false;
    }

    const uint8_t * base = static_cast<const uint8_t *>(m);

    // Validate magic + format version.
    if (std::memcmp(base, kMagic, sizeof kMagic) != 0) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: '%s' bad magic; expected V3AOTC01\n",
            path);
        ::munmap(m, fileSize);
        ::close(fd);
        r.ready.store(true, std::memory_order_release);
        return false;
    }
    uint32_t fmt = loadU32(base + 8);
    uint32_t n   = loadU32(base + 12);
    if (fmt != kFormatVersion) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: '%s' format=%u, expected %u; "
            "AOT cache disabled (rebuild via bench/build-aot-cache.py)\n",
            path, fmt, kFormatVersion);
        ::munmap(m, fileSize);
        ::close(fd);
        r.ready.store(true, std::memory_order_release);
        return false;
    }
    // Sanity: entry table must fit within mapped file.
    const size_t need = kHeaderSize + (size_t)n * kEntrySize;
    if (need > fileSize) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: '%s' truncated (need >= %zu, have %zu)\n",
            path, need, fileSize);
        ::munmap(m, fileSize);
        ::close(fd);
        r.ready.store(true, std::memory_order_release);
        return false;
    }

    // Advisory: split the hint to match the actual access pattern.
    //
    // Initial implementation used MADV_RANDOM for the whole file.
    // That disables read-ahead, which hurts blob reads — each blob
    // is ~70 KB on HNE workloads and the deserializer scans it
    // sequentially.  MADV_RANDOM forces a page-fault round-trip for
    // every 4 KB page within each blob.
    //
    // New split:
    //   * Entry table (kHeaderSize .. blobStart): MADV_WILLNEED.
    //     Binary search hits log2(N) entries scattered over a small
    //     region; pre-fetching the whole table is cheap (~196 KB
    //     for 3.5 K entries) and avoids the per-comparison fault.
    //   * Blob region (blobStart .. fileSize): default MADV_NORMAL.
    //     The OS's adaptive read-ahead works well for the "skip
    //     between random offsets + sequential within each blob"
    //     pattern.  Wasted prefetch on adjacent unrelated blobs is
    //     small compared to the cost of full per-page faults.
    //
    // Measured benefit (HNE warm, n=20 hyperfine): MADV_RANDOM ran
    // at 6.83 ± 0.27 s; this split runs at <SEE measurement>.
    const size_t entryTableEnd = kHeaderSize + (size_t)n * kEntrySize;
    ::madvise(m, entryTableEnd, MADV_WILLNEED);
    if (entryTableEnd < fileSize) {
        ::madvise(static_cast<uint8_t *>(m) + entryTableEnd,
                  fileSize - entryTableEnd, MADV_NORMAL);
    }

    // Success.
    r.map      = m;
    r.mapBytes = fileSize;
    r.fd       = fd;
    r.entries  = base + kHeaderSize;
    r.nEntries = n;

    // WS5-B2 — adopt the writer's CANONICAL symbol/pos id assignment WHOLESALE
    // before any symbol is interned.  D2a only *reserved* the writer's id RANGE
    // (fresh interns appended above it); it could NOT realign the LOW base ids
    // (builtins / common attr names) that the reader interns at startup — those
    // collided with the writer's baked low ids, so most borrowed CUs needed a
    // per-process `code` remap → owned copy (7.6 % borrow, was the D2a blocker).
    //
    // The fix: reconstruct the writer's canonical table from the UNION of every
    // CU blob's sparse (id→name) / (id→pos) tables — all CU blobs in one AOT
    // file come from a single writer process, so they agree on every id — and
    // SEED it into this reader's global symbol table + pos pool right here,
    // BEFORE the root expr is lowered (aot_cache::init runs eagerly at the top
    // of runRootExprFromString).  With writer and reader agreeing on ALL ids
    // (incl. low ones), a borrowed CU's read-only bytecode needs only the
    // IDENTITY remap → it borrows in place (Shared_Clean) → ~100 % code-borrow.
    //
    // Seeding is a pure HINT: `globalSeedSymbol`/`seedPosSnapshotAt` never move
    // an already-interned symbol, and the per-CU deserializeCUBorrowed path
    // still independently seeds + verifies identity + falls back to own+remap
    // on any conflict.  So correctness (byte-identical drvPath) is unconditional
    // regardless of how completely the seeding succeeds; it only moves the
    // borrow RATE.
    {
        std::unordered_map<uint32_t, std::string_view>          symById;
        std::unordered_map<uint32_t, serialize::BlobPosEntry>   posById;
        uint32_t maxSym = 0, maxPos = 0;
        std::vector<serialize::BlobSymEntry> syms;
        std::vector<serialize::BlobPosEntry> poss;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t * e = base + kHeaderSize + (size_t)i * kEntrySize;
            if (loadU32(e + 32) != static_cast<uint32_t>(TBL_CU)) continue;
            uint64_t off = loadU64(e + 40);
            uint64_t len = loadU64(e + 48);
            if (off + len > fileSize) continue;
            if (!serialize::readSparseTables(
                    std::string_view(reinterpret_cast<const char *>(base + off),
                                     static_cast<size_t>(len)),
                    syms, poss))
                continue;
            // First writer id wins (all blobs agree, so identical anyway).
            for (const auto & s : syms) {
                symById.emplace(s.id, s.name);
                if (s.id > maxSym) maxSym = s.id;
            }
            for (const auto & p : poss) {
                posById.emplace(p.id, p);
                if (p.id > maxPos) maxPos = p.id;
            }
        }
        // Pre-grow both tables to the writer's max id (holes for unreferenced
        // ids) so each seed below just fills a hole — no repeated resizes if
        // the map iterates ids out of order.  Fresh interns then append above.
        if (maxSym) ir::reserveSymbolCapacity(maxSym);
        if (maxPos) reservePosCapacity(maxPos);
        // Seed each canonical entry at the writer's id.  Order-independent: a
        // name maps to exactly one id in the writer, so there are no intra-map
        // conflicts; each entry seeds to the identity in the (fresh) reader.
        for (const auto & [id, name] : symById)
            ir::globalSeedSymbol(id, name);
        for (const auto & [id, p] : posById)
            seedPosSnapshotAt(id, PosSnapshot{std::string(p.file), p.line, p.column});
    }

    r.enabled.store(true, std::memory_order_relaxed);
    r.ready.store(true, std::memory_order_release);

    auto & st2 = mutableStats();
    st2.enabled     = true;
    st2.entries     = n;
    st2.mappedBytes = fileSize;

    // Diagnostic-friendly init line.  Suppress when NIX_V3_AOT_QUIET
    // is set so production runs aren't noisy.
    if (!std::getenv("NIX_V3_AOT_QUIET")) {
        std::fprintf(stderr,
            "v3 NIX_V3_AOT_CACHE_FILE: mmap'd '%s' (%zu bytes, %u entries)\n",
            path, fileSize, n);
    }
    return true;
}

} // namespace

bool init() noexcept
{
    auto & r = reader();
    if (r.ready.load(std::memory_order_acquire)) return r.enabled.load();
    std::lock_guard<std::mutex> g(r.initMutex);
    return tryInitLocked();
}

std::optional<std::string_view>
lookup(const disk_cache::CacheKey & key, TableId table) noexcept
{
    auto & r = reader();
    if (!r.ready.load(std::memory_order_acquire)) {
        // Lazy init on first call.
        std::lock_guard<std::mutex> g(r.initMutex);
        if (!tryInitLocked()) return std::nullopt;
    }
    if (!r.enabled.load(std::memory_order_relaxed)) return std::nullopt;

    auto & st = mutableStats();
    ++st.lookups;

    // Binary search the entry table by key (32 bytes).  Entries are
    // sorted ascending per the writer (bench/build-aot-cache.py).
    if (r.nEntries == 0) { ++st.misses; return std::nullopt; }

    size_t lo = 0;
    size_t hi = r.nEntries;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = compareKey(r.entries, mid, key);
        if (c < 0) {
            lo = mid + 1;
        } else if (c > 0) {
            hi = mid;
        } else {
            // Key matched.  Need ALSO table_id match (we may have
            // entries for both CU + EvalResults under different
            // tables but the same key would be possible if a CU and
            // an eval-result key happened to collide.  Tables differ
            // so the entry tells us which table it serves).
            //
            // The current writer dedups by (table_id, key) pair, so
            // (table != stored table) means this isn't the right
            // entry — we should NOT continue searching because the
            // key matches but the entries[mid] is for the wrong
            // table; the SAME (key, table) might not exist in the
            // file.  Return miss.
            const uint8_t * e = r.entries + mid * kEntrySize;
            uint32_t storedTable = loadU32(e + 32);
            if (storedTable != static_cast<uint32_t>(table)) {
                // Key collides with a different table — for the AOT
                // distribution this is rare (SHA-256 keys differ by
                // construction across tables), but the format
                // permits it.  Walk a small neighbourhood for the
                // matching table_id.
                // Look left.
                for (size_t j = mid; j > 0; --j) {
                    if (compareKey(r.entries, j - 1, key) != 0) break;
                    uint32_t t = loadU32(r.entries + (j - 1) * kEntrySize + 32);
                    if (t == static_cast<uint32_t>(table)) { mid = j - 1; goto found; }
                }
                // Look right.
                for (size_t j = mid + 1; j < r.nEntries; ++j) {
                    if (compareKey(r.entries, j, key) != 0) break;
                    uint32_t t = loadU32(r.entries + j * kEntrySize + 32);
                    if (t == static_cast<uint32_t>(table)) { mid = j; goto found; }
                }
                ++st.misses;
                return std::nullopt;
            }
          found:
            // Read offset + length and bounds-check against mapped
            // region.
            const uint8_t * e2 = r.entries + mid * kEntrySize;
            uint64_t off = loadU64(e2 + 40);
            uint64_t len = loadU64(e2 + 48);
            if (off + len > r.mapBytes) {
                ++st.errors;
                return std::nullopt;
            }
            ++st.hits;
            const uint8_t * base = static_cast<const uint8_t *>(r.map);
            return std::string_view(
                reinterpret_cast<const char *>(base + off),
                static_cast<size_t>(len));
        }
    }
    ++st.misses;
    return std::nullopt;
}

Stats & stats() noexcept
{
    return mutableStats();
}

} // namespace nix::v3::aot_cache
