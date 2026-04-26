#pragma once
/// @file
/// SQLite-backed persistent cache for bytecode CompilationUnits.
///
/// Single SQLite DB at `$XDG_CACHE_HOME/nix/bytecode-cache-v1.sqlite`.
/// Schema:
///   key     BLOB PRIMARY KEY     -- 32-byte SHA-256
///   blob    BLOB                 -- bytecode-serialize.cc payload
///   schema  INTEGER              -- kBytecodeSerializeSchemaVersion
///   last_used INTEGER            -- unix-epoch seconds (LRU sweeps)
///   size      INTEGER            -- length of blob (LRU eviction)
///   src       TEXT                -- canonical source path (debugging)
///
/// WAL mode + isCache() pragmas (mirrors src/libexpr/eval-cache.cc).
/// Concurrency: SQLite WAL allows one writer + N readers; INSERT OR
/// IGNORE makes racing inserts safe.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode-serialize.hh"
#include "nix/util/sync.hh"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nix::bytecode {

class DiskCacheImpl;

class BytecodeDiskCache
{
public:
    /// Open or create the disk cache at the default location.
    /// `dbPath` overrides the default for testing.  If construction
    /// fails (read-only filesystem, corruption, ...) the cache enters
    /// a permanently-failed state and all subsequent lookups/inserts
    /// silently no-op.
    explicit BytecodeDiskCache(std::optional<std::filesystem::path> dbPath = std::nullopt);
    ~BytecodeDiskCache();

    BytecodeDiskCache(const BytecodeDiskCache &) = delete;
    BytecodeDiskCache & operator=(const BytecodeDiskCache &) = delete;

    /// Look up a CU blob by key.  Returns the serialized blob on hit,
    /// std::nullopt on miss or schema mismatch.  Updates last_used.
    std::optional<std::string> lookup(const CacheKey & key);

    /// Insert a CU blob.  No-op on key collision (caller's previous
    /// blob wins).  No-op on internal failure.
    void insert(const CacheKey & key, std::string_view blob,
                std::string_view srcPath);

    /// Delete cached entries to bring DB size below `targetBytes`.
    /// LRU order (oldest last_used first).  Returns bytes removed.
    uint64_t evictTo(uint64_t targetBytes);

    /// Total stored bytes; 0 on failure.
    uint64_t totalSize() const;

    /// Hit / miss counters for instrumentation.
    uint64_t nrHits() const;
    uint64_t nrMisses() const;
    uint64_t nrInserts() const;

private:
    std::unique_ptr<DiskCacheImpl> impl;
};

} // namespace nix::bytecode
