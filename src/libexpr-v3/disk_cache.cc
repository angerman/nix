/// @file
/// SQLite-backed disk cache for v3 CompilationUnit blobs.
///
/// Single SQLite DB at `$XDG_CACHE_HOME/nix/v3-bytecode-v3.sqlite`.
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

/// 2026-05-25 #814: Composite (key, schema) PRIMARY KEY — schema-bumped
/// rows must coexist with their old-schema predecessors at the same
/// content hash, otherwise INSERT OR IGNORE silently drops the new
/// rows and post-bump hit_rate stays at 0%.  See computeDbPath() for
/// the matching v1→v2 file-name bump.
constexpr const char * kSchema = R"sql(
create table if not exists CompilationUnits (
    key       blob not null,
    blob      blob not null,
    schema    integer not null,
    last_used integer not null,
    size      integer not null,
    primary key (key, schema)
);
create index if not exists idx_lru on CompilationUnits(last_used);
-- #741 Phase 5: eval-result cache (drvPath-keyed result attrset blobs).
-- Mirrors the CompilationUnits shape but uses kEvalResultSchemaVersion
-- so format changes to one cache don't invalidate the other.
create table if not exists EvalResults (
    key       blob not null,
    blob      blob not null,
    schema    integer not null,
    last_used integer not null,
    size      integer not null,
    primary key (key, schema)
);
create index if not exists idx_eval_lru on EvalResults(last_used);
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
    // #741 Phase 5: parallel statements for the EvalResults table.
    nix::SQLiteStmt evalInsert;
    nix::SQLiteStmt evalLookup;
    nix::SQLiteStmt evalUpdateLastUsed;
    bool initialised = false;
    // #741 Phase 5b: nest-counted transaction scope.  Tracks
    // beginEvalResultBatch() / commitEvalResultBatch() pairs.  The
    // OUTERMOST begin issues a real SQLite BEGIN; the OUTERMOST
    // commit issues the COMMIT.  Inner calls just bump/decrement
    // the depth — matters for recursive `runRootExpr` invocations
    // (e.g. `import` re-entry from inside a primop).
    bool inEvalBatch = false;
    uint32_t evalBatchDepth = 0;
    // #741 Phase 5b' — sub-batched commit support.  Tracks the number
    // of inserts accumulated into the CURRENT open transaction.  When
    // it reaches `evalBatchSize()`, the transaction is committed and
    // a fresh one is begun.  See disk_cache.hh comment block.
    uint32_t evalBatchInsertsThisTxn = 0;
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

/// Compute the database path.  Honours NIX_V3_CACHE_DIR for tests /
/// benchmarks; falls back to `getCacheDir() / v3-bytecode-v3.sqlite`
/// (XDG-compliant).
///
/// File-name bumped v1→v2→v3 in #814 (2026-05-25) to force fresh
/// DBs after the kSchema composite-PK fix and the schema-12/13
/// deserialize round-trip work.  Pre-existing v1 + v2 files
/// remain on disk as orphans — v3 has no on-disk format consumers
/// yet (initial development phase), so backwards-compatibility
/// isn't a concern; users can `rm ~/.cache/nix/v3-bytecode-v{1,2}.sqlite`
/// to reclaim disk.  SQLite cannot ALTER a PRIMARY KEY on an
/// existing table; the v3 path forces a fresh DB so the
/// composite-(key, schema) PK and the schema-13 wire-format take
/// effect cleanly.
std::filesystem::path computeDbPath()
{
    if (const char * override = std::getenv("NIX_V3_CACHE_DIR");
        override && *override)
        return std::filesystem::path(override) / "v3-bytecode-v3.sqlite";
    return std::filesystem::path(nix::getCacheDir()) / "v3-bytecode-v3.sqlite";
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
        // #741 Phase 5: EvalResults statements.
        state->evalInsert.create(
            state->db,
            "insert or ignore into EvalResults "
            "(key, blob, schema, last_used, size) "
            "values (?, ?, ?, unixepoch(), ?)");
        state->evalLookup.create(
            state->db,
            "select blob from EvalResults where key = ? and schema = ?");
        state->evalUpdateLastUsed.create(
            state->db,
            "update EvalResults set last_used = unixepoch() where key = ?");
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

// ---------------------------------------------------------------------------
// #741 Phase 5 — EvalResults table.  Symmetric implementation to
// lookup/insert above, parametrised on the parallel prepared
// statements and the independent schema-version constant.
// ---------------------------------------------------------------------------

std::optional<std::string> lookupEvalResult(const CacheKey & key)
{
    auto & st = stats();
    if (key.empty()) return std::nullopt;
    if (!ensureOpen()) { st.evalMisses++; return std::nullopt; }
    st.evalLookups++;
    auto & h = dbHandle();
    try {
        auto state = h.state.lock();
        sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->evalLookup);
        sqlite3_reset(raw);
        sqlite3_bind_blob(raw, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
        sqlite3_bind_int64(raw, 2,
            static_cast<int64_t>(kEvalResultSchemaVersion));
        int rc = sqlite3_step(raw);
        if (rc != SQLITE_ROW) { st.evalMisses++; return std::nullopt; }
        const void * data = sqlite3_column_blob(raw, 0);
        int len = sqlite3_column_bytes(raw, 0);
        std::string blob(static_cast<const char *>(data),
                         static_cast<size_t>(len));
        try {
            sqlite3_stmt * up = static_cast<sqlite3_stmt *>(state->evalUpdateLastUsed);
            sqlite3_reset(up);
            sqlite3_bind_blob(up, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
            sqlite3_step(up);
        } catch (...) { /* advisory */ }
        st.evalHits++;
        return blob;
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        st.evalMisses++;
        return std::nullopt;
    }
}

void insertEvalResult(const CacheKey & key, std::string_view blob)
{
    auto & st = stats();
    if (key.empty() || blob.empty()) return;
    if (!ensureOpen()) { st.evalInsertFailures++; return; }
    auto & h = dbHandle();
    try {
        auto state = h.state.lock();
        sqlite3_stmt * raw = static_cast<sqlite3_stmt *>(state->evalInsert);
        sqlite3_reset(raw);
        sqlite3_bind_blob(raw, 1, key.bytes, sizeof key.bytes, SQLITE_TRANSIENT);
        sqlite3_bind_blob64(raw, 2, blob.data(),
            static_cast<sqlite3_uint64>(blob.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(raw, 3,
            static_cast<int64_t>(kEvalResultSchemaVersion));
        sqlite3_bind_int64(raw, 4, static_cast<int64_t>(blob.size()));
        int rc = sqlite3_step(raw);
        if (rc != SQLITE_DONE) { st.evalInsertFailures++; return; }
        st.evalInserts++;
        // #741 Phase 5b' — sub-batched commit.  Inside an open batch,
        // count inserts; on threshold, COMMIT + BEGIN (state lock is
        // already held).  We MUST be at the outermost depth — nested
        // depths mean a callee opened a sub-batch we shouldn't break.
        if (state->inEvalBatch && state->evalBatchDepth == 1) {
            ++state->evalBatchInsertsThisTxn;
            uint32_t target = evalBatchSize();
            if (target > 0 && state->evalBatchInsertsThisTxn >= target) {
                try {
                    state->db.exec("COMMIT");
                    state->db.exec("BEGIN");
                    state->evalBatchInsertsThisTxn = 0;
                    ++st.evalBatchFlushes;
                } catch (...) {
                    // On COMMIT/BEGIN failure, exit batch mode for the
                    // rest of this eval.  Subsequent inserts will run
                    // as implicit per-call transactions (pre-batch
                    // behaviour) — slower but correct.
                    state->inEvalBatch = false;
                    state->evalBatchDepth = 0;
                    state->evalBatchInsertsThisTxn = 0;
                }
            }
        }
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        st.evalInsertFailures++;
    }
}

// #741 Phase 5b — batched-insert transaction control.

void beginEvalResultBatch() noexcept
{
    auto & h = dbHandle();
    if (h.failed.load(std::memory_order_relaxed)) return;
    if (!ensureOpen()) return;
    try {
        auto state = h.state.lock();
        // Re-entrancy: nested begins just bump the depth counter.
        // Only the outermost begin issues a real SQLite BEGIN.
        if (state->inEvalBatch) {
            ++state->evalBatchDepth;
            return;
        }
        // synchronous=OFF + WAL keeps BEGIN cheap; the win is in
        // amortising the per-insert commit's fsync/sync replacement
        // (~1 ms) across all inserts of the eval scope.
        state->db.exec("BEGIN");
        state->inEvalBatch = true;
        state->evalBatchDepth = 1;
        state->evalBatchInsertsThisTxn = 0;
    } catch (...) {
        // Don't poison h.failed; cache is advisory.  A failed BEGIN
        // just means subsequent inserts each commit individually
        // (the pre-batch behaviour).
    }
}

void commitEvalResultBatch() noexcept
{
    auto & h = dbHandle();
    if (h.failed.load(std::memory_order_relaxed)) return;
    auto state = h.state.lock();
    if (!state->initialised) return;
    if (!state->inEvalBatch) return;
    // Nested commit: just decrement; the outermost commit issues
    // the real COMMIT.
    if (state->evalBatchDepth > 1) {
        --state->evalBatchDepth;
        return;
    }
    try {
        state->db.exec("COMMIT");
        state->inEvalBatch = false;
        state->evalBatchDepth = 0;
        state->evalBatchInsertsThisTxn = 0;
    } catch (...) {
        // If COMMIT fails, the transaction stays open until
        // connection close (which rolls it back).  Flag the batch
        // as closed so subsequent calls don't loop on a poisoned
        // transaction.
        state->inEvalBatch = false;
        state->evalBatchDepth = 0;
        state->evalBatchInsertsThisTxn = 0;
    }
}

// #741 Phase 5b' — sub-batched commit threshold.
//
// Cached once on first read; subsequent calls return the cached
// value.  Honours `NIX_V3_DRV_HASH_CACHE_DISK_BATCH=N`.  N=0 (or
// unset) disables sub-batching — each insert commits individually
// (Phase 5 default).  N>0 enables: commit + re-begin every N
// inserts within an open transaction.
//
// Recommended range based on Phase 5 cold-cache count (~517 inserts
// on hello.drvPath):
//   N=50  → ~10 commits/eval, each over ~25 KB WAL
//   N=100 → ~5 commits/eval, each over ~50 KB WAL
//   N=200 → ~3 commits/eval, each over ~100 KB WAL
// Smaller N approaches per-insert commit cost; larger N approaches
// full-batch checkpoint-storm behaviour.
uint32_t evalBatchSize() noexcept
{
    static const uint32_t cached = []() -> uint32_t {
        const char * env = std::getenv("NIX_V3_DRV_HASH_CACHE_DISK_BATCH");
        if (!env || !*env) return 0;
        char * endp = nullptr;
        unsigned long v = std::strtoul(env, &endp, 10);
        if (endp == env) return 0;
        if (v > 100000u) return 100000u;  // sanity cap
        return static_cast<uint32_t>(v);
    }();
    return cached;
}

// Public helper to flush the current sub-batch if it has crossed
// the threshold.  Currently a no-op stub — flushing happens inline
// in `insertEvalResult` to avoid an extra mutex acquisition.  Kept
// in the interface for future callers (e.g. an explicit flush at
// the end of a derivation-batch phase).
void flushEvalResultBatchEvery() noexcept
{
    // Intentional no-op; see commentary above.
}

} // namespace nix::v3::disk_cache
