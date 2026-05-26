#pragma once
/// @file
/// File-based persistent cache for v3 CompilationUnit blobs.
///
/// Cache layout: `$XDG_CACHE_HOME/nix/v3-bc-v1/<hex64>` where each
/// file is a serialized `CompilationUnit` blob keyed by the
/// SHA-256 of the source-file content.
///
///   - One file per cached CU; atomic writes via temp + rename.
///   - No SQLite (keeps the implementation small + dependency-free
///     beyond the existing nix util headers).
///   - No automatic eviction yet — the cache grows; a future
///     `maybeEvict()` can sweep oldest-mtime files when the dir
///     exceeds a configurable size.
///
/// Cacheability: only sources reachable from a real filesystem path
/// (SourcePath that resolves to an absolute file) are cacheable.
/// String-evaluated expressions (`nix-instantiate --eval --expr`),
/// stdin, and virtual filesystem entries return an empty key and
/// skip the cache.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nix::v3::disk_cache {

/// 32-byte cache key.  An empty key means "this source is not
/// cacheable" and lookups/inserts no-op.
struct CacheKey {
    uint8_t bytes[32];
    bool empty() const noexcept;
    /// Hex string for use as a filename (`<64 hex chars>`).
    std::string hex() const;
};

/// Compute a cache key for a file's content.  Returns an empty
/// key on read failure.
CacheKey computeKeyForFile(const std::string & path);

/// Compute a cache key for an in-memory blob.  Used for
/// string-evaluated expressions when the caller has already
/// captured the source text.
CacheKey computeKeyForString(std::string_view content);

/// Look up a cached blob by key.  Returns the serialized bytes on
/// hit (one read from disk), `std::nullopt` on miss or read error.
std::optional<std::string> lookup(const CacheKey & key);

/// Insert a serialized blob under `key`.  Best-effort: on write
/// failure (full disk, permission denied, etc.) silently noop —
/// the cache is purely advisory and never the source of truth.
void insert(const CacheKey & key, std::string_view blob);

/// Process-wide stats counters.  Always populated; printed via
/// NIX_VM_STATS so users can confirm the cache is firing.
struct Stats {
    // CompilationUnits table (the existing CU disk cache).
    uint64_t lookups = 0;
    uint64_t hits    = 0;
    uint64_t misses  = 0;
    uint64_t inserts = 0;
    uint64_t insertFailures = 0;
    // EvalResults table (added by #741 Phase 5).
    uint64_t evalLookups        = 0;
    uint64_t evalHits           = 0;
    uint64_t evalMisses         = 0;
    uint64_t evalInserts        = 0;
    uint64_t evalInsertFailures = 0;
    // #741 Phase 5b' (2026-05-26) — sub-batched commit telemetry.
    // Incremented when the in-progress transaction crosses the
    // `NIX_V3_DRV_HASH_CACHE_DISK_BATCH=N` threshold and is committed
    // + re-opened.  0 when sub-batching is disabled.
    uint64_t evalBatchFlushes   = 0;
};
Stats & stats() noexcept;

// ---------------------------------------------------------------------------
// #741 Phase 5 (2026-05-23) — disk-persisted eval-result cache.
//
// New table `EvalResults` in the same SQLite database.  Schema mirrors
// CompilationUnits but uses an independent schema-version constant
// (`kEvalResultSchemaVersion`) so format changes to one cache don't
// invalidate the other.
//
// Cache key: SHA-256 over the drvPath string.  (drvPath IS the
// canonical content hash of the constructed `drv` per libstore.)
// Cache value: serialized v3 Value of the derivation result attrset,
// produced by `value_serialize::serialize()`.
//
// Usage from `value_serialize::drvHashCache*`:
//   * `drvHashCacheLookup` calls `lookupEvalResult` on in-memory miss
//   * `drvHashCacheInsert` calls `insertEvalResult` after in-memory insert
//
// Phase 5 SHADOW (this commit): always run primop body even on hit;
// just verify cached vs computed.  Phase 5 ACTIVE skip-on-hit
// requires modulo-hash caching for `drvHashes` replay (next session).
// ---------------------------------------------------------------------------

/// Bumped on any breaking change to the EvalResults table's blob format
/// (i.e. the value_serialize V3VR encoding).  Schema mismatches on
/// load are silent misses; insertions overwrite via `insert or ignore`.
constexpr uint32_t kEvalResultSchemaVersion = 1;

std::optional<std::string> lookupEvalResult(const CacheKey & key);
void insertEvalResult(const CacheKey & key, std::string_view blob);

// ---------------------------------------------------------------------------
// #741 Phase 5b — batched-insert support.
//
// Without batching, each `insertEvalResult` runs as its own implicit
// SQLite transaction (BEGIN → step → COMMIT).  Even with
// synchronous=OFF + WAL, the per-commit overhead is ~1 ms.  On a
// cold-cache eval of hello.drvPath this means 517 inserts × 1.1 ms
// ≈ 569 ms wall — the dominant component of Phase 5's COLD +71%
// slowdown finding.
//
// `beginEvalResultBatch` opens an explicit transaction; subsequent
// `insertEvalResult` calls accumulate INTO the open transaction (no
// per-call commit).  `commitEvalResultBatch` closes the transaction
// with a single commit.  Amortises the per-insert sync cost.
//
// Re-entrancy: nested batch begins are no-ops (the outermost
// brackets the entire batch).  Commit of a never-opened batch is
// a no-op.  On exception or commit failure, the transaction is
// rolled back implicitly when the connection closes.
//
// Hook site: typically at the start/end of `runRootExpr` — the
// natural eval-scope boundary.  Each top-level eval gets its own
// batch.
// ---------------------------------------------------------------------------

void beginEvalResultBatch() noexcept;
void commitEvalResultBatch() noexcept;

// #741 Phase 5b' (2026-05-26) — sub-batched commit.
//
// Hyperfine on hello.drvPath showed FULL-eval-scope batching
// regressed wall + ballooned variance 24× (commit bff1f670f):
//
//   COLD ACTIVE+DISK unbatched : 1429 ms ± 31 ms
//   COLD ACTIVE+DISK full-batch: 1666 ms ± 764 ms
//
// Hypothesis: a long-held transaction with ~256 KB accumulated WAL
// triggers SQLite's deferred checkpoint at COMMIT, interacting with
// APFS / page-cache flushes non-deterministically.  The cure is to
// commit periodically — keeping each transaction small enough that
// COMMIT stays cheap, while still amortising the per-insert cost
// across N inserts.
//
// `flushEvalResultBatchEvery` commits the current transaction and
// opens a fresh one when `evalBatchDepth == 1` AND `N` inserts
// have accumulated since the last open.  Called from
// `insertEvalResult` after a successful step.  N is read from
// `NIX_V3_DRV_HASH_CACHE_DISK_BATCH=N` once at process init;
// 0 disables sub-batching (per-insert commits — current default).
//
// Hook site: same as `beginEvalResultBatch` / `commitEvalResultBatch`
// — `runRootExpr` start/end.  Only the outermost begin opens the
// transaction; nested begins inherit it.
void flushEvalResultBatchEvery() noexcept;

// Return the configured sub-batch size (0 = sub-batching disabled).
uint32_t evalBatchSize() noexcept;

} // namespace nix::v3::disk_cache
