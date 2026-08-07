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
#include "v3/aot_cache.hh"
#include "v3/serialize.hh"
#include "nix/util/hash.hh"
#include "nix/util/sync.hh"
#include "nix/util/users.hh"
#include "nix/util/file-system.hh"
#include "nix/store/sqlite.hh"

#include <sqlite3.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nix::v3::disk_cache {

// ---------------------------------------------------------------------------
// R8a Phase 1 — AOT BUILD MODE manifest recorder (per
// lode/AOT_DISTRIBUTION_2026-05-26.md §7.1 + lode/
// DIRECTION_NOTE_2026-05-26.md §6 action #5).
//
// When NIX_V3_AOT_BUILD_MODE=<path> is set, every successful
// disk_cache insert (CompilationUnits and EvalResults tables) appends
// one line to <path>:
//
//   <unix_ts> <table> <key_hex> <blob_size>
//
// The manifest enumerates everything the AOT distribution snapshot
// needs to include: this lets a downstream tool dump matching blobs
// to an mmap'd flat file (per EVAL_CACHE_ARCHITECTURE §4.3 + §7).
//
// Cost when disabled: one TLS magic-static-flag check per insert
// (negligible).  Cost when enabled: one fprintf per insert, no
// fsync — best-effort logging.
//
// Retirement criterion: this manifest is the Phase 1 Day 1-3
// deliverable.  Phase 1 success (≥30 % warm-eval improvement on
// haskell-nix-example with the mmap'd cache) promotes this to a
// permanent infrastructure; Phase 1 failure (<15 %) retires both
// this and the manifest format.  See AOT_DISTRIBUTION §7.2 for
// the kill criterion.
// ---------------------------------------------------------------------------

namespace {

// Manifest FILE* (lazily opened on first record).  Lifetime: process
// lifetime; closed at exit via atexit handler installed alongside open.
FILE * & aotManifest() noexcept
{
    static FILE * f = nullptr;
    return f;
}

// Open the manifest if NIX_V3_AOT_BUILD_MODE is set and we haven't
// opened it yet.  Returns the FILE* or nullptr if disabled / open
// failed.  Magic-static gate keeps the env-var check off the hot
// path after the first call.
FILE * openAotManifestIfEnabled()
{
    static FILE * f = []() -> FILE * {
        const char * path = std::getenv("NIX_V3_AOT_BUILD_MODE");
        if (!path || !*path) return nullptr;
        FILE * h = std::fopen(path, "a");
        if (!h) {
            // Failed to open — log to stderr once and disable.
            std::fprintf(stderr,
                "v3 NIX_V3_AOT_BUILD_MODE: failed to open '%s' (errno=%d); "
                "AOT build manifest disabled for this process\n",
                path, errno);
            return nullptr;
        }
        // Header line so consumers can identify the file format
        // version + know what columns to expect.  '#' prefix keeps
        // it skippable by a downstream consumer that ignores
        // comments.
        std::fprintf(h,
            "# v3 NIX_V3_AOT_BUILD_MODE manifest format v1\n"
            "# columns: unix_ts table key_hex blob_size_bytes\n"
            "# tables: CompilationUnits, EvalResults\n");
        std::fflush(h);
        // Install atexit to close the manifest cleanly.  We don't
        // use the unsynchronised aotManifest() global here because
        // we control the closure capture directly.
        std::atexit([]() {
            FILE * h = aotManifest();
            if (h) {
                std::fflush(h);
                std::fclose(h);
                aotManifest() = nullptr;
            }
        });
        aotManifest() = h;
        return h;
    }();
    return f;
}

// Record a single insert.  No-op when AOT build mode is disabled.
void aotRecord(const char * table, const CacheKey & key, size_t blobSize) noexcept
{
    FILE * f = openAotManifestIfEnabled();
    if (!f) return;
    // Format: <unix_ts> <table> <key_hex> <size_bytes>\n
    // We use std::time() not steady_clock — the manifest is
    // human-debuggable and consumed by external tooling that wants
    // wall-clock timestamps.
    std::time_t now = std::time(nullptr);
    try {
        std::fprintf(f, "%lld %s %s %zu\n",
            static_cast<long long>(now), table,
            key.hex().c_str(), blobSize);
    } catch (...) {
        // Best-effort logging; never propagate.
    }
}

} // namespace

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

uint64_t approxResidentBytes() noexcept
{
    // Best-effort: 0 when the DB was never opened (e.g. a string-eval
    // run with no cacheable imports), so this never forces the cache
    // open just to measure it.  `dbHandle()` / `DbState` live in the
    // anon namespace above but are TU-visible here.
    auto & h = dbHandle();
    if (h.failed.load(std::memory_order_relaxed)) return 0;
    auto state = h.state.lock();
    if (!state->initialised) return 0;
    sqlite3 * db = state->db;   // nix::SQLite::operator sqlite3 *()
    if (!db) return 0;

    uint64_t total = 0;
    int cur = 0, hi = 0;
    // The three connection-local memory pools SQLite reports.  CACHE_USED
    // is the page cache (dominant); SCHEMA_USED holds parsed schema;
    // STMT_USED is the prepared-statement working memory.  All are
    // current (not high-water) bytes for THIS connection only — so this
    // excludes libstore's separate store DB connection.
    if (sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_USED, &cur, &hi, 0) == SQLITE_OK && cur > 0)
        total += static_cast<uint64_t>(cur);
    if (sqlite3_db_status(db, SQLITE_DBSTATUS_SCHEMA_USED, &cur, &hi, 0) == SQLITE_OK && cur > 0)
        total += static_cast<uint64_t>(cur);
    if (sqlite3_db_status(db, SQLITE_DBSTATUS_STMT_USED, &cur, &hi, 0) == SQLITE_OK && cur > 0)
        total += static_cast<uint64_t>(cur);
    return total;
}

std::optional<std::string_view> lookupCuBorrow(const CacheKey & key)
{
    // WS5-D2a — return the AOT mmap view directly (borrowable in place by
    // deserializeCUBorrowed) so the CU-bytecode pages stay Shared_Clean
    // across processes.  AOT only: on a miss the caller falls back to the
    // copying `lookup()` (SQLite).  Bumps the same disk_cache stats an AOT
    // hit bumps in `lookup()`, so accounting is unchanged.
    if (key.empty()) return std::nullopt;
    if (auto sv = aot_cache::lookup(key, aot_cache::TBL_CU)) {
        auto & st = stats();
        st.lookups++;
        st.hits++;
        return sv;   // view into the process-lifetime mmap — do NOT copy
    }
    return std::nullopt;
}

std::optional<std::string> lookup(const CacheKey & key)
{
    auto & st = stats();
    if (key.empty()) return std::nullopt;
    // R8a Phase 1 Day 10-12: consult the AOT mmap reader first
    // (L3 in the cache hierarchy).  AOT cache is read-only, mmap'd
    // once at init; lookups are O(log N) binary search and skip
    // SQLite query overhead entirely.  Lazy init on first call when
    // NIX_V3_AOT_CACHE_FILE is set.
    if (auto sv = aot_cache::lookup(key, aot_cache::TBL_CU)) {
        // Hit — increment the existing disk_cache stats so users
        // see this as a cache hit (the AOT cache keeps its own
        // internal sub-stats for the breakdown).
        st.lookups++;
        st.hits++;
        return std::string(*sv);
    }
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
        // P-12 / M-12 (CODEBASE_REVIEW_2026-06-11): the per-hit LRU `last_used`
        // bump is GONE.  It was a SQLite UPDATE (a write) on the warm-cache READ
        // path, maintaining an index that NOTHING consumes — there is no
        // eviction / DELETE anywhere in this file, so last_used was write-only.
        // Dropping it removes a write per cache hit.  If LRU eviction is ever
        // added, re-introduce the bump together with the DELETE that reads it.
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
        return;
    }
    // R8a Phase 1 manifest recorder — append AFTER successful insert
    // so cancelled / failing inserts don't pollute the manifest.
    aotRecord("CompilationUnits", key, blob.size());
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
    // R8a Phase 1 Day 10-12: AOT mmap reader fast-path.  See lookup()
    // above for the rationale.  The AOT cache may contain both
    // CompilationUnits + EvalResults entries (different table_id);
    // here we ask only for EvalResults.
    if (auto sv = aot_cache::lookup(key, aot_cache::TBL_EVAL_RESULT)) {
        st.evalLookups++;
        st.evalHits++;
        return std::string(*sv);
    }
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
        // P-12 / M-12: per-hit LRU bump removed (no eviction consumes it; it
        // was a write on the warm-cache read path).  See lookup() above.
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
    } catch (...) {
        h.failed.store(true, std::memory_order_relaxed);
        st.evalInsertFailures++;
        return;
    }
    // R8a Phase 1 manifest recorder — same as insert() above; record
    // only on successful EvalResults insert.
    aotRecord("EvalResults", key, blob.size());
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
    } catch (...) {
        // If COMMIT fails, the transaction stays open until
        // connection close (which rolls it back).  Flag the batch
        // as closed so subsequent calls don't loop on a poisoned
        // transaction.
        state->inEvalBatch = false;
        state->evalBatchDepth = 0;
    }
}

} // namespace nix::v3::disk_cache
