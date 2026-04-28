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
    uint64_t lookups = 0;
    uint64_t hits    = 0;
    uint64_t misses  = 0;
    uint64_t inserts = 0;
    uint64_t insertFailures = 0;
};
Stats & stats() noexcept;

} // namespace nix::v3::disk_cache
