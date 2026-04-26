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
///     bits 1-2 = 01  →  reserved (was: bool — replaced by static singletons)
///     bits 1-2 = 10  →  reserved (was: null — replaced by static singleton)
///     bits 1-2 = 11  →  reserved
///
/// Note: tagged bool and tagged null encodings were removed because the
/// VM uses static singletons (&Value::vTrue/vFalse/vNull) for these.
/// The tags would only have helped if arithmetic produced tagged bools
/// directly to be consumed inline by JUMP_IF_*, but the actual code
/// paths produce singletons instead.
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
// 0b011, 0b101, 0b111 reserved — tagged bool/null were removed since
// the VM uses static singletons (&Value::vTrue/vFalse/vNull).

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

/// True if w is any tagged scalar (currently only tagged ints).
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

} // namespace nix::nanbox
