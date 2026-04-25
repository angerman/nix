#pragma once
/// @file
/// NaN-boxing / pointer-tagging support for the Nix bytecode VM.
///
/// Encodes small immediate values (ints, bools, null) directly into
/// 64-bit words alongside heap-allocated Value* pointers, avoiding
/// allocation for the most common values.
///
/// Encoding scheme (low-bit tagged — Boehm GC compatible):
///
///   bit 0 = 0:  Untagged Value* pointer (8-byte aligned, low bit naturally 0)
///               Boehm sees these as pointers — no GC issues.
///
///   bit 0 = 1:  Tagged immediate value
///     bits 1-2 = 00  →  60-bit signed int (shifted left 3, low bits are 001)
///     bits 1-2 = 01  →  bool (bit 3 = value)
///     bits 1-2 = 10  →  null (specific value 0b101)
///     bits 1-2 = 11  →  reserved
///
/// Choice: bit 0 = 1 means "immediate", because heap pointers from
/// GC_MALLOC are always 8-byte aligned (bit 0..2 all zero).  This way
/// Boehm's conservative scanner never mistakes a tagged immediate for
/// a pointer (the low bit set makes it look like a misaligned pointer
/// which Boehm ignores), and all real pointers are visible to the GC
/// in their natural form.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace nix {
struct Value;
} // namespace nix

namespace nix::nanbox {

/// Tagged value: either a Value* (low bit 0) or a tagged immediate (low bit 1).
/// We store as Value* for compatibility with the existing VM stack type
/// (which is Value**).  The VM treats this uniformly; opcodes that read
/// values check isTagged() before dereferencing.
using NixWord = ::nix::Value *;

// Tag bits (encoded in low 3 bits when bit 0 = 1):
constexpr uintptr_t kTagMask        = 0b111;
constexpr uintptr_t kTagPointer     = 0b000;  // bit 0 = 0
constexpr uintptr_t kTagInt         = 0b001;  // bits 0-2 = 001
constexpr uintptr_t kTagBool        = 0b011;  // bits 0-2 = 011
constexpr uintptr_t kTagNull        = 0b101;  // bits 0-2 = 101 (singleton)
// 0b111 reserved for future tags

/// True if w is a tagged immediate (any low-bit-1 value).
[[gnu::always_inline]]
inline bool isTagged(NixWord w) noexcept
{
    return (reinterpret_cast<uintptr_t>(w) & 1) != 0;
}

/// True if w is an untagged Value* pointer (low bit 0).
[[gnu::always_inline]]
inline bool isPointer(NixWord w) noexcept
{
    return (reinterpret_cast<uintptr_t>(w) & 1) == 0;
}

/// True if w is a tagged int.
[[gnu::always_inline]]
inline bool isTaggedInt(NixWord w) noexcept
{
    return (reinterpret_cast<uintptr_t>(w) & kTagMask) == kTagInt;
}

/// True if w is a tagged bool.
[[gnu::always_inline]]
inline bool isTaggedBool(NixWord w) noexcept
{
    return (reinterpret_cast<uintptr_t>(w) & kTagMask) == kTagBool;
}

/// True if w is the tagged null singleton.
[[gnu::always_inline]]
inline bool isTaggedNull(NixWord w) noexcept
{
    return reinterpret_cast<uintptr_t>(w) == kTagNull;
}

/// True if w is any tagged scalar (int, bool, null).
/// These values are "already forced" — no thunk to evaluate.
[[gnu::always_inline]]
inline bool isTaggedScalar(NixWord w) noexcept
{
    return isTagged(w);
}

// ---- Encode ----

/// Encode a 60-bit signed integer as a tagged immediate.
/// Caller must ensure n fits in 60 bits (range -2^59 .. 2^59-1).
/// Values outside this range must be heap-boxed.
[[gnu::always_inline]]
inline NixWord encodeInt(int64_t n) noexcept
{
    // Shift left by 3 to make room for tag bits, then OR in the tag.
    // The shift preserves the sign bit (arithmetic shift on signed types).
    uintptr_t bits = (static_cast<uintptr_t>(n) << 3) | kTagInt;
    return reinterpret_cast<NixWord>(bits);
}

/// True if `n` fits in a tagged 60-bit int.
[[gnu::always_inline]]
inline bool intFitsTagged(int64_t n) noexcept
{
    // Range: [-2^59, 2^59 - 1]
    constexpr int64_t kMin = -(static_cast<int64_t>(1) << 59);
    constexpr int64_t kMax = (static_cast<int64_t>(1) << 59) - 1;
    return n >= kMin && n <= kMax;
}

/// Encode a bool as a tagged immediate.
[[gnu::always_inline]]
inline NixWord encodeBool(bool b) noexcept
{
    // bit 3 holds the bool value, bits 0-2 = 011.
    uintptr_t bits = (static_cast<uintptr_t>(b ? 1 : 0) << 3) | kTagBool;
    return reinterpret_cast<NixWord>(bits);
}

/// Encode null as the tagged null singleton.
[[gnu::always_inline]]
inline NixWord encodeNull() noexcept
{
    return reinterpret_cast<NixWord>(kTagNull);
}

// ---- Decode ----

/// Decode a tagged int back to int64_t.
/// Caller must verify isTaggedInt(w) first.
[[gnu::always_inline]]
inline int64_t decodeInt(NixWord w) noexcept
{
    // Arithmetic right shift to sign-extend (cast through intptr_t).
    return static_cast<int64_t>(
        static_cast<intptr_t>(reinterpret_cast<uintptr_t>(w)) >> 3);
}

/// Decode a tagged bool.
/// Caller must verify isTaggedBool(w) first.
[[gnu::always_inline]]
inline bool decodeBool(NixWord w) noexcept
{
    return ((reinterpret_cast<uintptr_t>(w) >> 3) & 1) != 0;
}

} // namespace nix::nanbox
