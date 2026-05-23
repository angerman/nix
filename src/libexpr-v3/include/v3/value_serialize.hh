#pragma once
/// @file
/// #741 Phase 1 spike — Value-subset serialiser for derivation-result
/// shapes (Attrs / List / String-with-context / Int / Bool / Null /
/// Path / Float).  Round-trippable; deterministic.
///
/// This is the foundational falsifier for #741 IFD content-addressed
/// eval-result cache.  If we can round-trip derivation-result Values
/// byte-identically with bounded overhead, the broader cache
/// architecture is viable.  If not, kill #741.
///
/// Format (binary, little-endian):
///
///   Magic (4 bytes):   "V3VR"  (V3 Value Roundtrip)
///   Schema (1 byte):   1
///   Body:              one serialised Value, recursively encoded
///
/// Per-Value encoding:
///   'I' 0x49  i64 (8 LE)                              -- Int
///   'F' 0x46  f64 (8 IEEE LE)                         -- Float
///   'B' 0x42  u8 0|1                                  -- Bool
///   'N' 0x4E  (no payload)                            -- Null
///   'S' 0x53  u32 strLen, bytes,                      -- String:
///             u32 ctxCount,                           --  ctx entries are
///             (u32 entryLen, bytes)*                  --  stringContextSideTable
///                                                     --  format (already encoded
///                                                     --  via to_string()).
///   'P' 0x50  u32 pathLen, bytes                      -- Path
///   'L' 0x4C  u32 elemCount, value*                   -- List
///   'A' 0x41  u32 entryCount,                         -- Attrs (sorted by name)
///             (u32 nameLen, bytes, value)*
///
/// String context entries are stored in the same encoded form used
/// in `stringContextSideTable` (`!<output>!<drvPath>` / `=<drvPath>`
/// / `<path>`).  No re-canonicalisation needed.
///
/// Tags NOT supported in Phase 1 (caller checks Tag before invoking):
///   Closure, Thunk, PrimOp, PrimOpApp, App, Blackhole, External, Slot
///   — these never appear in WHNF derivation results.  Trying to
///   serialise one throws SerializeError.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nix::v3::value_serialize {

class SerializeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Serialise the WHNF Value `v` into `out`.  Throws SerializeError
/// on unsupported Tag (Closure/Thunk/etc.) or oversized input.
/// `out` is APPENDED to, not cleared.
void serialize(const Value & v, std::string & out);

/// Deserialise a Value previously emitted by `serialize`.  Allocates
/// via Alloc::allocBindings / allocList / allocChars and respects
/// Phase D write barriers.  Throws SerializeError on bad magic,
/// schema mismatch, truncated input, or unknown tag byte.
Value deserialize(std::string_view in);

/// Structural value equality for the subset we serialise.  Bool/Null
/// compare by tag only (singletons); Int/Float by payload; String by
/// byte-equal + context-vector-equal; Path by byte-equal; List + Attrs
/// recursive.  Other tags compare unequal.
bool valuesEqual(const Value & a, const Value & b) noexcept;

/// Round-trip diagnostics.  Bumped from `runRoundTripTest` (called by
/// `buildAndWriteDrvNative` when test mode is enabled).
struct RoundTripStats {
    uint64_t attempts        = 0;  // total runRoundTripTest calls
    uint64_t successes       = 0;  // round-trip + valuesEqual passed
    uint64_t mismatches      = 0;  // round-trip OK but valuesEqual failed
    uint64_t serErrors       = 0;  // serialize threw
    uint64_t deserErrors     = 0;  // deserialize threw
    uint64_t totalBytes      = 0;  // sum of serialised blob sizes
    uint64_t totalSerNs      = 0;  // wall ns in serialize()
    uint64_t totalDeserNs    = 0;  // wall ns in deserialize()
    uint64_t totalCompareNs  = 0;  // wall ns in valuesEqual()
};

RoundTripStats & roundTripStats() noexcept;

/// Read NIX_V3_TEST_DRV_RESULT_SERIALIZE once at process start; cache
/// the bool so primDerivationStrict's per-call check is one load.
bool testModeEnabled() noexcept;

/// Run serialise → deserialise → valuesEqual on `result` and bump
/// roundTripStats accordingly.  No-op (cheap) when testModeEnabled()
/// is false.
void runRoundTripTest(const Value & result) noexcept;

/// Format the stats as a human-readable summary line(s).  Called
/// from run.cc under NIX_VM_STATS when testModeEnabled() is true.
void dumpStats(std::FILE * out);

// ---------------------------------------------------------------------------
// #741 Phase 2 — canonical Value hash (cross-process determinism gate).
//
// canonicalHash(v) := SHA-256(serialize(v))
//
// serialize() already emits attr names sorted by NAME-STRING (not
// SymbolId), and string-context entries are stored in a sorted vector
// (NixStringContext is a `std::set`; v3's `encodeStringContext`
// preserves set-iteration order).  Positions / GC addresses / SymbolIds
// are not part of the encoded form.  Therefore serialize()'s output
// is a deterministic function of the Value's structural content.
// canonicalHash() is just SHA-256 over those bytes.
//
// Phase 2 falsifier: two processes computing canonicalHash() on the
// same logical input must produce byte-identical 32-byte digests for
// ≥99.9% of inputs.  Gate: NIX_V3_TEST_CANONICAL_HASH=1 emits a
// `V3-VAL-HASH: <64-hex>` stderr line per derivation result.  Sort
// + diff across two process invocations.
// ---------------------------------------------------------------------------

/// Compute the canonical 32-byte SHA-256 digest of `v`'s structural
/// content.  Throws SerializeError on unsupported tags (Closure /
/// Thunk / etc.) — caller's responsibility to pass a WHNF Value.
void canonicalHash(const Value & v, uint8_t out[32]);

/// Lowercase-hex form of canonicalHash (64 chars).
std::string canonicalHashHex(const Value & v);

bool canonicalHashTestModeEnabled() noexcept;

/// When canonicalHashTestModeEnabled(), compute + emit
/// `V3-VAL-HASH: <64-hex>` to stderr.  Cheap no-op otherwise.
void dumpCanonicalHashLine(const Value & v) noexcept;

// ---------------------------------------------------------------------------
// #741 Phase 3a — in-memory SHADOW eval-result cache (intra-process).
//
// SHADOW mode: the primop body ALWAYS runs.  The cache lookup result
// is used ONLY to verify the just-computed output matches the cached
// one.  No primop body is skipped; correctness is preserved by
// construction.
//
// Rationale: `buildAndWriteDrvNative` has a non-replayable side effect
// (writes the .drv file via `store.writeDerivation`).  An active
// skip-on-hit cache needs a side-effect-replay path for the .drv file
// (and for `nix::drvHashes` memoisation, though that one is
// recoverable via `pathDerivationModulo`'s lazy read fallback).  That
// replay infrastructure is Phase 3b; Phase 3a validates the cache
// pipeline architecture safely first.
//
// Phase 3a flow per primop call:
//   - At entry: deep-force args[0], canonical-hash → key, look up.
//     Stash (hit-flag, cachedValue) in the primop's locals.
//   - The primop body ALWAYS runs.
//   - At exit:
//       * If hit: structurally compare cachedValue to `out`.  Mismatch
//         bumps a counter (a Rule-0 falsifier — indicates a hash
//         collision or determinism bug).
//       * If miss: serialise(out) and insert (key, blob) for later
//         calls in the same process.
//
// Gate: NIX_V3_EVAL_RESULT_CACHE=1 (independent of Phase 1/2 gates).
//
// Falsifier (Phase 3a): on hello.drvPath + gcc + python3, hit rate
// should be ≥ 30% (matching the Phase 2 duplicate finding) and
// `mismatchHits` should be 0 across all three workloads.
// ---------------------------------------------------------------------------

bool evalResultCacheEnabled() noexcept;

struct EvalResultCacheStats {
    uint64_t lookups          = 0;  // entry attempts (deep-force + hash + look up)
    uint64_t hits             = 0;  // key found in cache; result deserialised
    uint64_t misses           = 0;  // key not found; primop body must run + insert
    uint64_t inserts          = 0;  // successful inserts on miss
    uint64_t mismatchHits     = 0;  // hit's cached output ≠ just-computed — falsifier!
    uint64_t deserErrors      = 0;  // deserialise threw on hit; fallback to compute
    uint64_t hashErrors       = 0;  // canonicalHash threw; cache skipped
    uint64_t bytesCached      = 0;  // sum of inserted blob sizes
    uint64_t bytesDelivered   = 0;  // sum of blob sizes returned via hit
    uint64_t totalHashNs      = 0;  // wall time in input-hash compute
    uint64_t totalSerNs       = 0;  // wall time in insert-serialize
    uint64_t totalLookupNs    = 0;  // wall time in unordered_map lookup
    uint64_t totalDeserNs     = 0;  // wall time in deserialise-on-hit
};

EvalResultCacheStats & evalResultCacheStats() noexcept;

/// Look up cached result for `input`.  Returns true if hit (and writes
/// deserialised result into `outResult`).  Returns false on miss
/// (caller continues normal primop; should later call
/// `evalResultCacheInsert(input, computedResult)`).
///
/// `outKey` is filled with the hash key, used by the matching insert
/// call to avoid recomputing the hash.  Empty `outKey` (size 0) means
/// the cache is disabled or the key computation failed; the matching
/// insert MUST be a no-op in that case.
bool evalResultCacheLookup(const Value & input,
                            Value & outResult,
                            std::string & outKey) noexcept;

/// Insert (key, serialise(result)) into the cache.  No-op when the
/// cache is disabled, when `key` is empty, or when serialise fails.
void evalResultCacheInsert(const std::string & key,
                            const Value & result) noexcept;

/// Dump cache stats line(s) to `out`.  Silent when cache is disabled
/// or no lookups happened.
void dumpEvalResultCacheStats(std::FILE * out);

// ---------------------------------------------------------------------------
// #741 Phase 3e — mid-body drv-hash SHADOW cache.
//
// At the call site in `buildAndWriteDrvNative`, the constructed
// `nix::Derivation drv` has all the effective inputs flattened to
// canonical libstore form (drv.env strings, inputDrvs / inputSrcs
// derived from string context).  The drvPath (computed via
// `computeStorePath`) IS the SHA-256 of `drv.unparse()` + store
// metadata — i.e. the canonical content hash of all effective
// inputs.
//
// Phase 3e uses the drvPath string as the cache key.  Cache value
// is the result attrset (the v3 Value `out`).  SHADOW mode: body
// ALWAYS runs; on hit, compare cached vs current.  Falsifier:
// mismatch count must stay 0 (drvPath is a content hash; same
// drvPath → same result by libstore semantics).
//
// Gate: NIX_V3_DRV_HASH_CACHE=1 (independent of Phase 3a's
// NIX_V3_EVAL_RESULT_CACHE).  Stats kept separately.
// ---------------------------------------------------------------------------

bool drvHashCacheEnabled() noexcept;

struct DrvHashCacheStats {
    uint64_t lookups          = 0;
    uint64_t hits             = 0;
    uint64_t misses           = 0;
    uint64_t inserts          = 0;
    uint64_t mismatchHits     = 0;
    uint64_t deserErrors      = 0;
    uint64_t bytesCached      = 0;
    uint64_t bytesDelivered   = 0;
    uint64_t totalSerNs       = 0;
    uint64_t totalDeserNs     = 0;
    uint64_t totalLookupNs    = 0;
    // Phase 3e ACTIVE: incremented on each skip-on-hit (where the
    // primop body's libstore tail was bypassed because the cache
    // had the result).  Equals the wall-saving event count.
    uint64_t activeSkips      = 0;
};
DrvHashCacheStats & drvHashCacheStats() noexcept;

/// ACTIVE mode: on hit, skip the rest of `buildAndWriteDrvNative`
/// (hashDerivationModulo + drvHashes insert + result attrset
/// construction) and use the cached result directly.  In-process
/// safe because cache hits always follow the populating miss
/// (the miss populated drvHashes; subsequent libstore consumers see
/// the entry).  In `nix eval --impure` (readOnlyMode), writeDerivation
/// was a no-op anyway so no .drv-file concern.
///
/// Gate: NIX_V3_DRV_HASH_CACHE_ACTIVE=1.  ACTIVE implies SHADOW
/// (lookups still go through the same machinery).
bool drvHashCacheActiveEnabled() noexcept;

/// SHADOW-mode lookup by external key (e.g. drvPath string).  On hit,
/// fills outResult with the deserialised cached value.  Caller is
/// expected to verify outResult against just-computed in shadow mode.
bool drvHashCacheLookup(const std::string & key, Value & outResult) noexcept;

/// Insert (key, serialise(result)) into the drv-hash cache.
void drvHashCacheInsert(const std::string & key, const Value & result) noexcept;

void dumpDrvHashCacheStats(std::FILE * out);

} // namespace nix::v3::value_serialize
