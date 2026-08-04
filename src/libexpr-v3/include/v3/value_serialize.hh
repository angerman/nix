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
// canonicalHash() is just SHA-256 over those bytes.  It is the memo-key
// digest for the live applied-import cache (vm_applied_cache.cc).
// ---------------------------------------------------------------------------

/// Compute the canonical 32-byte SHA-256 digest of `v`'s structural
/// content.  Throws SerializeError on unsupported tags (Closure /
/// Thunk / etc.) — caller's responsibility to pass a WHNF Value.
void canonicalHash(const Value & v, uint8_t out[32]);

/// Lowercase-hex form of canonicalHash (64 chars).
std::string canonicalHashHex(const Value & v);

} // namespace nix::v3::value_serialize
