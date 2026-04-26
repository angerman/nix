/// @file
/// SQLite-backed persistent cache implementation (Phase 3.2-6).
///
/// Mirrors src/libexpr/eval-cache.cc's WAL+isCache pattern but for
/// CU blobs instead of evaluated attribute trees.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode-disk-cache.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/util.hh"
#include "nix/util/file-system.hh"
#include "nix/util/users.hh"

#include <sqlite3.h>
#include <atomic>

namespace nix::bytecode {

namespace {
constexpr const char * kCacheSchema = R"sql(
create table if not exists CompilationUnits (
    key       blob primary key,
    blob      blob not null,
    schema    integer not null,
    last_used integer not null,
    size      integer not null,
    src       text
);
create index if not exists idx_lru on CompilationUnits(last_used);
)sql";
} // anonymous namespace

class DiskCacheImpl
{
public:
    std::atomic<bool> failed{false};

    struct State
    {
        SQLite db;
        SQLiteStmt insert;
        SQLiteStmt lookup;
        SQLiteStmt updateLastUsed;
        SQLiteStmt deleteOldest;
        SQLiteStmt totalSize;
    };

    std::unique_ptr<Sync<State>> _state;

    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> inserts{0};
    std::atomic<uint64_t> evictionCallCount{0};

    uint64_t evictInterval = 1000;
    uint64_t cacheLimitBytes = 1ull * 1024 * 1024 * 1024; // 1 GiB

    explicit DiskCacheImpl(std::optional<std::filesystem::path> dbPath)
        : _state(std::make_unique<Sync<State>>())
    {
        if (auto interval = ::getenv("NIX_BYTECODE_CACHE_EVICT_INTERVAL")) {
            try { evictInterval = std::stoull(interval); }
            catch (...) {}
        }
        if (auto limit = ::getenv("NIX_BYTECODE_CACHE_LIMIT")) {
            try { cacheLimitBytes = std::stoull(limit); }
            catch (...) {}
        }
        try {
            // NIX_BYTECODE_CACHE_DIR overrides for benchmarks/tests
            // without disturbing the broader XDG_CACHE_HOME setup
            // (flake cache, eval-cache, fetcher cache all live there).
            std::filesystem::path path;
            if (dbPath) {
                path = *dbPath;
            } else if (auto envOverride = ::getenv("NIX_BYTECODE_CACHE_DIR")) {
                path = std::filesystem::path(envOverride) /
                    "bytecode-cache-v1.sqlite";
            } else {
                path = std::filesystem::path(getCacheDir()) /
                    "bytecode-cache-v1.sqlite";
            }
            createDirs(path.parent_path());

            auto state(_state->lock());
            state->db = SQLite(path, {.useWAL = true});
            state->db.isCache();
            state->db.exec(kCacheSchema);

            state->insert.create(
                state->db,
                "insert or ignore into CompilationUnits "
                "(key, blob, schema, last_used, size, src) "
                "values (?, ?, ?, unixepoch(), ?, ?)");
            state->lookup.create(
                state->db,
                "select blob from CompilationUnits where key = ? and schema = ?");
            state->updateLastUsed.create(
                state->db,
                "update CompilationUnits set last_used = unixepoch() where key = ?");
            state->deleteOldest.create(
                state->db,
                "delete from CompilationUnits "
                "where key in (select key from CompilationUnits "
                "              order by last_used asc limit ?)");
            state->totalSize.create(
                state->db,
                "select coalesce(sum(size), 0) from CompilationUnits");
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
        }
    }

    std::optional<std::string> lookup(const CacheKey & key)
    {
        if (failed) { misses++; return std::nullopt; }
        try {
            auto state(_state->lock());
            // We need to read the BLOB column with sqlite3_column_blob
            // (SQLiteStmt::Use::getStr converts via column_text which
            // truncates at embedded NUL bytes).
            sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->lookup);
            sqlite3_reset(raw);
            sqlite3_bind_blob(raw, 1, key.hash.hash, key.hash.hashSize,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(raw, 2, kBytecodeSerializeSchemaVersion);
            int rc = sqlite3_step(raw);
            if (rc != SQLITE_ROW) {
                misses++;
                return std::nullopt;
            }
            const void * data = sqlite3_column_blob(raw, 0);
            int len = sqlite3_column_bytes(raw, 0);
            std::string blob(static_cast<const char *>(data),
                             static_cast<size_t>(len));
            // Best-effort LRU bump.
            try {
                state->updateLastUsed.use()(
                    key.hash.hash, key.hash.hashSize).exec();
            } catch (...) {
                ignoreExceptionExceptInterrupt();
            }
            hits++;
            return blob;
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
            misses++;
            return std::nullopt;
        }
    }

    /// M8: zero-copy lookup — invoke consumer with the SQLite blob view
    /// while the lock is still held.  Saves the std::string copy in the
    /// hot warm-cache path.
    bool lookupView(
        const CacheKey & key,
        const std::function<void(std::string_view)> & consume)
    {
        if (failed) { misses++; return false; }
        try {
            auto state(_state->lock());
            sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->lookup);
            sqlite3_reset(raw);
            sqlite3_bind_blob(raw, 1, key.hash.hash, key.hash.hashSize,
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(raw, 2, kBytecodeSerializeSchemaVersion);
            int rc = sqlite3_step(raw);
            if (rc != SQLITE_ROW) {
                misses++;
                return false;
            }
            const void * data = sqlite3_column_blob(raw, 0);
            int len = sqlite3_column_bytes(raw, 0);
            std::string_view blob(
                static_cast<const char *>(data),
                static_cast<size_t>(len));
            consume(blob);
            // Best-effort LRU bump.
            try {
                state->updateLastUsed.use()(
                    key.hash.hash, key.hash.hashSize).exec();
            } catch (...) {
                ignoreExceptionExceptInterrupt();
            }
            hits++;
            return true;
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
            misses++;
            return false;
        }
    }

    void insert(const CacheKey & key, std::string_view blob, std::string_view src)
    {
        if (failed) return;
        if (key.hash.hashSize == 0) return;
        try {
            auto state(_state->lock());
            state->insert.use()(
                key.hash.hash, key.hash.hashSize)(
                reinterpret_cast<const unsigned char *>(blob.data()), blob.size())(
                static_cast<int64_t>(kBytecodeSerializeSchemaVersion))(
                static_cast<int64_t>(blob.size()))(
                src).exec();
            inserts++;
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
        }
    }

    uint64_t totalSize()
    {
        if (failed) return 0;
        try {
            auto state(_state->lock());
            auto stmt = state->totalSize.use();
            if (!stmt.next()) return 0;
            return static_cast<uint64_t>(stmt.getInt(0));
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
            return 0;
        }
    }

    uint64_t evictTo(uint64_t targetBytes)
    {
        if (failed) return 0;
        try {
            auto cur = totalSize();
            if (cur <= targetBytes) return 0;
            // Estimate: assume average blob is ~1MB; delete in
            // batches of 16 oldest entries until we're under target.
            uint64_t removed = 0;
            uint64_t toRemove = cur - targetBytes;
            while (removed < toRemove) {
                auto state(_state->lock());
                state->deleteOldest.use()(static_cast<int64_t>(16)).exec();
                uint64_t newSize = 0;
                {
                    auto stmt = state->totalSize.use();
                    if (stmt.next())
                        newSize = static_cast<uint64_t>(stmt.getInt(0));
                }
                if (newSize >= cur) break;  // nothing changed; bail.
                removed += (cur - newSize);
                cur = newSize;
            }
            return removed;
        } catch (...) {
            ignoreExceptionExceptInterrupt();
            failed = true;
            return 0;
        }
    }
};

BytecodeDiskCache::BytecodeDiskCache(std::optional<std::filesystem::path> dbPath)
    : impl(std::make_unique<DiskCacheImpl>(dbPath))
{
}

BytecodeDiskCache::~BytecodeDiskCache() = default;

std::optional<std::string> BytecodeDiskCache::lookup(const CacheKey & key)
{
    return impl->lookup(key);
}

bool BytecodeDiskCache::lookupView(
    const CacheKey & key,
    const std::function<void(std::string_view)> & consume)
{
    return impl->lookupView(key, consume);
}

void BytecodeDiskCache::insert(const CacheKey & key, std::string_view blob,
                                std::string_view srcPath)
{
    impl->insert(key, blob, srcPath);
}

uint64_t BytecodeDiskCache::evictTo(uint64_t targetBytes)
{
    return impl->evictTo(targetBytes);
}

uint64_t BytecodeDiskCache::maybeEvict()
{
    auto count = ++impl->evictionCallCount;
    if (count % impl->evictInterval != 0)
        return 0;
    return impl->evictTo(impl->cacheLimitBytes);
}

uint64_t BytecodeDiskCache::totalSize() const
{
    return impl->totalSize();
}

uint64_t BytecodeDiskCache::nrHits() const     { return impl->hits.load(); }
uint64_t BytecodeDiskCache::nrMisses() const   { return impl->misses.load(); }
uint64_t BytecodeDiskCache::nrInserts() const  { return impl->inserts.load(); }

} // namespace nix::bytecode
