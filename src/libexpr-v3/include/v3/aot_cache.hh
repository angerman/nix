#pragma once
/// @file
/// R8a Phase 1 Day 10-12 — client-side mmap reader for the AOT cache
/// flat file produced by bench/build-aot-cache.py (Day 4-6).
///
/// File format v1 (see lode/AOT_PHASE1_DAY4-6_2026-05-26.md):
///
///   Offset  Size   Field
///   0       8      Magic: "V3AOTC01"
///   8       4      Format version (u32 = 1)
///   12      4      Entry count (u32 N)
///   16      56*N   Entry table (sorted-by-key)
///                    key (32):      SHA-256 cache key
///                    table_id (4):  1=CompilationUnits, 2=EvalResults
///                    _pad (4):      zero
///                    blob_offset (8): absolute u64
///                    blob_length (8): u64
///   16+56*N var    Blob data
///
/// Activation: env `NIX_V3_AOT_CACHE_FILE=<path>` set at process
/// start.  Init is lazy — first lookup triggers it.  On failure
/// (missing file, bad magic, bad format) the reader stays disabled
/// and lookups return nullopt; the SQLite disk_cache continues to
/// handle all queries.
///
/// Composition with disk_cache: aot_cache is consulted BEFORE the
/// SQLite disk_cache in `disk_cache::lookup()` and
/// `disk_cache::lookupEvalResult()`.  This is L3 in the cache
/// hierarchy from lode/AOT_DISTRIBUTION_2026-05-26.md §3.5:
///
///   L1: in-process memory cache (T1.1 instrumentation surface)
///   L2: SQLite disk_cache  (process-local mutable; #777)
///   L3: AOT mmap region    (cross-process shared snapshot)
///   L4: compile from source
///
/// Lifecycle: AOT region is mmap'd MAP_PRIVATE READ-ONLY at init;
/// process exit unmaps via OS.  No explicit teardown — the reader
/// is single-init / single-load / read-only / never mutates.
///
/// Retirement criterion (per AOT_DISTRIBUTION_2026-05-26.md §7.2):
/// Day 13-15 measurement on haskell-nix-example must show ≥30 %
/// warm-eval improvement.  Below 15 % triggers REVERT WITH DATA —
/// delete this file + the disk_cache.cc recorder + the Python
/// builder.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/disk_cache.hh"

#include <cstdint>
#include <optional>
#include <string_view>

namespace nix::v3::aot_cache {

/// Table ID enum mirroring the flat-file format.  Must match the
/// constants written by bench/build-aot-cache.py.
enum TableId : uint32_t {
    TBL_CU           = 1,
    TBL_EVAL_RESULT  = 2,
};

/// Look up a (key, table) pair in the mmap'd AOT cache.  Returns a
/// string_view into the mmap'd region on hit (caller must NOT
/// retain past process exit; for current callers this is safe
/// because they immediately copy into a std::string or deserialize
/// into owned values).
///
/// Lazy-initialises on first call.  If NIX_V3_AOT_CACHE_FILE is
/// unset or initialisation fails, every call returns nullopt and
/// is a near-no-op (one atomic-bool load).
std::optional<std::string_view> lookup(const disk_cache::CacheKey & key,
                                        TableId table) noexcept;

/// Process-wide stats.  Always populated when the reader is
/// enabled; printed via NIX_VM_STATS so users can confirm AOT
/// cache hits are firing.
struct Stats {
    uint64_t lookups = 0;
    uint64_t hits    = 0;
    uint64_t misses  = 0;       // key not in AOT index
    uint64_t errors  = 0;       // mmap region read failures (should be 0)
    // Set at init.  Zero when reader is disabled / failed.
    size_t   mappedBytes = 0;
    uint32_t entries     = 0;
    bool     enabled     = false;
};
Stats & stats() noexcept;

/// Best-effort eager init.  Returns true on success, false if env
/// var unset or init failed.  Most callers don't need to call this
/// — `lookup()` does lazy init on first call.  Provided for
/// callers that want to control timing (e.g. early in process
/// startup, before forking workers).
bool init() noexcept;

} // namespace nix::v3::aot_cache
