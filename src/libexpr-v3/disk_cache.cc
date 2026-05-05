/// @file
/// SQLite-backed disk cache for v3 CompilationUnit blobs.
///
/// Single SQLite DB at `$XDG_CACHE_HOME/nix/v3-bytecode-v1.sqlite`.
/// Schema mirrors src/libexpr/bytecode-disk-cache.cc and the rest of
/// nix's caches (libfetchers, nar-info-disk-cache, eval-cache):
///
///   key       BLOB PRIMARY KEY     -- 32-byte SHA-256 of source text
///   blob      BLOB                 -- serialize::serializeCU output
///   schema    INTEGER              -- serialize::kSchemaVersion gate
///   last_used INTEGER              -- unix-epoch (LRU eviction)
///   size      INTEGER              -- blob size for total-size queries
///
/// WAL mode + isCache() pragmas (`-PRAGMA synchronous=OFF;
/// -PRAGMA journal_mode=TRUNCATE`) — same trade-off the eval-cache and
/// fetcher caches make: a crash mid-write may lose recent inserts but
/// never corrupts older entries, and the cache is fully advisory.
///
/// Concurrency: SQLite WAL allows one writer + N readers simultaneously
/// across processes.  `INSERT OR IGNORE` makes racing inserts safe;
/// the first writer's blob wins (same content-hash key → same blob, so
/// the loser silently retries on its next lookup).
///
/// Replaces the prior file-per-key layout under `nix/v3-bc-v1/<hex64>`
/// (~1 file per CU) with one DB file.  Migration: there is none — the
/// file layout was opt-in (NIX_V3_DISK_CACHE) and never relied on by
/// any released code.  Delete the old directory manually if it exists.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/disk_cache.hh"
#include "v3/serialize.hh"
#include "nix/util/hash.hh"
#include "nix/util/sync.hh"
#include "nix/util/users.hh"
#include "nix/util/file-system.hh"
#include "nix/store/sqlite.hh"

#include <sqlite3.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nix::v3::disk_cache {

bool CacheKey::empty() const noexcept
{
    for (auto b : bytes) if (b) return false;
    return true;
}

std::string CacheKey::hex() const
{
    static const char hexChars[] = "0123456789abcdef";
    std::string s;
    s.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        s[i * 2]     = hexChars[bytes[i] >> 4];
        s[i * 2 + 1] = hexChars[bytes[i] & 0xF];
    }
    return s;
}

namespace {

void sha256(std::string_view data, CacheKey & out)
{
    nix::Hash h = nix::hashString(nix::HashAlgorithm::SHA256, data);
    static_assert(sizeof(out.bytes) == 32, "CacheKey expects 32-byte SHA-256");
    std::memcpy(out.bytes, h.hash, 32);
}

constexpr const char * kSchema = R"sql(
create table if not exists CompilationUnits (
    key       blob primary key,
    blob      blob not null,
    schema    integer not null,
    last_used integer not null,
    size      integer not null
);
create index if not exists idx_lru on CompilationUnits(last_used);
)sql";

/// Lazily-opened SQLite handle.  Wrapped in Sync<> so any thread can
/// call lookup/insert; the SQLite handle itself is single-threaded.
/// On any open/exec failure we set `failed=true` and silently no-op
/// every subsequent call — the cache is purely advisory.
struct DbState
{
    nix::SQLite db;
    nix::SQLiteStmt insert;
    nix::SQLiteStmt lookup;
    nix::SQLiteStmt updateLastUsed;
    bool initialised = false;
};

struct DbHandle
{
    std::atomic<bool> failed{false};
    nix::Sync<DbState> state;
};

DbHandle & dbHandle()
{
    static DbHandle h;
    return h;
}

/// Compute the database path.  Honours NIX_V3_CACHE_DIR (legacy env
/// var name from the file-per-key cache) for tests / benchmarks; falls
/// back to `getCacheDir() / v3-bytecode-v1.sqlite` (XDG-compliant).
std::filesystem::path computeDbPath()
{
    if (const char * override = std::getenv("NIX_V3_CACHE_DIR");
        override && *override)
        return std::filesystem::path(override) / "v3-bytecode-v1.sqlite";
    return std::filesystem::path(nix::getCacheDir()) / "v3-bytecode-v1.sqlite";
}

/// Open + populate prepared statements on first use.  Returns false
/// on irrecoverable failure (caller treats as cache disabled).
bool ensureOpen()
{
    auto & h = dbHandle();
    if (h.failed.load(std::memory_order_relaxed)) return false;
    auto state = h.state.lock();
    if (state->initialised) return true;
    try {
        auto path = computeDbPath();
        nix::createDirs(path.parent_path());
        state->db = nix::SQLite(path, {.useWAL = true});
        state->db.isCache();
        state->db.exec(kSchema);
        state->insert.create(
            state->db,
            "insert or ignore into CompilationUnits "
            "(key, blob, schema, last_used, size) "
            "values (?, ?, ?, unixepoch(), ?)");
        state->lookup.create(
            state->db,
            "select blob from CompilationUnits where key = ? and schema = ?");
        state->updateLastUsed.create(
            state->db,
            "update CompilationUnits set last_used = unixepoch() where key = ?");
        state->initialised = true;
        return true;
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        return false;
    }
}

} // namespace

CacheKey computeKeyForFile(const std::string & path)
{
    CacheKey k{};
    try {
        // Read the file into a buffer and hash.  On failure leave the
        // key empty so callers no-op the cache.
        std::string content = nix::readFile(path);
        sha256(content, k);
    } catch (...) {
        // Empty key signals "uncacheable".
    }
    return k;
}

CacheKey computeKeyForString(std::string_view content)
{
    CacheKey k{};
    sha256(content, k);
    return k;
}

std::optional<std::string> lookup(const CacheKey & key)
{
    auto & st = stats();
    if (key.empty()) return std::nullopt;
    if (!ensureOpen()) { st.misses++; return std::nullopt; }
    st.lookups++;
    auto & h = dbHandle();
    try {
        auto state = h.state.lock();
        // Use the raw sqlite3 API for blob bind + blob fetch; the
        // SQLiteStmt::Use helper only handles TEXT/INT (its getStr
        // path goes through column_text which truncates at NUL).
        sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->lookup);
        sqlite3_reset(raw);
        sqlite3_bind_blob(raw, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
        sqlite3_bind_int64(raw, 2,
            static_cast<int64_t>(serialize::kSchemaVersion));
        int rc = sqlite3_step(raw);
        if (rc != SQLITE_ROW) { st.misses++; return std::nullopt; }
        const void * data = sqlite3_column_blob(raw, 0);
        int len = sqlite3_column_bytes(raw, 0);
        std::string blob(static_cast<const char *>(data),
                         static_cast<size_t>(len));
        // Best-effort LRU bump.  Failures are silent.
        try {
            sqlite3_stmt * up = static_cast<sqlite3_stmt *>(state->updateLastUsed);
            sqlite3_reset(up);
            sqlite3_bind_blob(up, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
            sqlite3_step(up);
        } catch (...) { /* advisory */ }
        st.hits++;
        return blob;
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        st.misses++;
        return std::nullopt;
    }
}

void insert(const CacheKey & key, std::string_view blob)
{
    auto & st = stats();
    if (key.empty() || blob.empty()) return;
    if (!ensureOpen()) { st.insertFailures++; return; }
    auto & h = dbHandle();
    try {
        auto state = h.state.lock();
        sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->insert);
        sqlite3_reset(raw);
        sqlite3_bind_blob(raw, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
        // REVIEW §3: bind_blob takes int length -- a CU >2GB would
        // truncate.  Use bind_blob64 which takes sqlite3_uint64.  In
        // practice a v3 CU is small (10-200 KB), but the silent
        // truncation has been a real foot-gun in other libs.
        sqlite3_bind_blob64(raw, 2, blob.data(),
            static_cast<sqlite3_uint64>(blob.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(raw, 3,
            static_cast<int64_t>(serialize::kSchemaVersion));
        sqlite3_bind_int64(raw, 4, static_cast<int64_t>(blob.size()));
        int rc = sqlite3_step(raw);
        if (rc != SQLITE_DONE) { st.insertFailures++; return; }
        st.inserts++;
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        st.insertFailures++;
    }
}

Stats & stats() noexcept
{
    static Stats s;
    return s;
}

} // namespace nix::v3::disk_cache
