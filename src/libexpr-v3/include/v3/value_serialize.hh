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

} // namespace nix::v3::value_serialize
