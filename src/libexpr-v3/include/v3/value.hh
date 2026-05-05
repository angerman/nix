#pragma once
/// @file
/// Nix v3 evaluator: 16-byte tagged Value.
///
/// Design choices (per doc/v3-design/v3-design.md §3):
///   - 5-bit tag in low byte of `tag_payload` word; rest of word is currently
///     unused (room for future small-int / inline-string optimisations).
///   - 8-byte payload union is the second word; carries either an immediate
///     scalar (int, float, bool — though Bool is a singleton) or a pointer
///     to a heap object.
///   - Singletons for Bool/Null/Blackhole/EmptyList/EmptyAttrs — the tag
///     plus a known global address.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstddef>
#include <cassert>

namespace nix::v3 {

struct Closure;
struct Thunk;
struct Bindings;
struct ListVec;
struct PrimOp;
struct Value;
struct ValuePair; // pair of Values for App / PrimOpApp; defined after Value

/// 5-bit tag.  Stored in the low 8 bits of Value::tag_payload.
enum class Tag : uint8_t {
    Uninitialized = 0,
    Int           = 1,
    Float         = 2,
    Bool          = 3,
    Null          = 4,
    String        = 5,
    Path          = 6,
    Attrs         = 7,
    List          = 8,
    Closure       = 9,
    Thunk         = 10,
    PrimOp        = 11,
    PrimOpApp     = 12,
    App           = 13,
    Blackhole     = 14,
    External      = 15,
    /// Slot pointer — a stable pointer to another Value living in
    /// heap-allocated storage (let-rec env).  WC-38 / SECD-style
    /// DUM/RAP: when `with E;` source resolves to a let-rec slot,
    /// or when an upvalue captures a let-rec binding, the value
    /// stored is `Tag::Slot` with payload = `Value*`.  Forcing a
    /// Tag::Slot dereferences the pointer and forces *that* value;
    /// since the pointed-to slot is mutated in-place when the
    /// let-rec body completes (mkAttrs equivalent), sub-thunks
    /// observing the slot at use time see the up-to-date value
    /// rather than a stale snapshot.
    Slot          = 16,
};

/// Two-word Value (16 bytes on 64-bit).
struct Value
{
    /// Low byte: Tag.  Upper 56 bits reserved (e.g., for inline-string size,
    /// small-bool encoding, future tagged-immediate flags).
    uint64_t tag_payload;

    /// 8-byte payload union.  Interpretation depends on Tag.
    /// PrimOpApp / App carry a heap-allocated pair (ValuePair*) — payload is
    /// the pointer; we don't pack two pointers inline since that would bloat
    /// every Value to 24 bytes.
    union Payload {
        int64_t        i;          // Tag::Int
        double         f;          // Tag::Float
        const char *   str;        // Tag::String
        const char *   path;       // Tag::Path (with separate accessor table)
        Bindings *     bindings;   // Tag::Attrs
        ListVec *      list;       // Tag::List
        Closure *      closure;    // Tag::Closure
        Thunk *        thunk;      // Tag::Thunk
        const PrimOp * primop;     // Tag::PrimOp
        Value *        next;       // Tag::Thunk Blackhole chain (transient)
        ValuePair *    pair;       // PrimOpApp / App (allocated)
        Value *        slot;       // Tag::Slot — stable pointer to another Value
        void *         raw;        // External / generic
    } payload;

    [[gnu::always_inline]] inline Tag tag() const noexcept
    {
        return static_cast<Tag>(tag_payload & 0xFF);
    }

    [[gnu::always_inline]] inline bool isInt()      const noexcept { return tag() == Tag::Int; }
    [[gnu::always_inline]] inline bool isFloat()    const noexcept { return tag() == Tag::Float; }
    [[gnu::always_inline]] inline bool isBool()     const noexcept { return tag() == Tag::Bool; }
    [[gnu::always_inline]] inline bool isNull()     const noexcept { return tag() == Tag::Null; }
    [[gnu::always_inline]] inline bool isString()   const noexcept { return tag() == Tag::String; }
    [[gnu::always_inline]] inline bool isPath()     const noexcept { return tag() == Tag::Path; }
    [[gnu::always_inline]] inline bool isAttrs()    const noexcept { return tag() == Tag::Attrs; }
    [[gnu::always_inline]] inline bool isList()     const noexcept { return tag() == Tag::List; }
    [[gnu::always_inline]] inline bool isClosure()  const noexcept { return tag() == Tag::Closure; }
    [[gnu::always_inline]] inline bool isThunk()    const noexcept { return tag() == Tag::Thunk; }
    [[gnu::always_inline]] inline bool isPrimOp()   const noexcept { return tag() == Tag::PrimOp; }
    [[gnu::always_inline]] inline bool isBlackhole()const noexcept { return tag() == Tag::Blackhole; }
    // isApp / isSlot helpers removed -- 0 callers, dispatch sites all
    // use `tag() == Tag::App` / `Tag::Slot` directly so the explicit
    // tag check is closer to the dispatch in vm.cc and forceValue.

    /// Forced = not a thunk, not an unevaluated app, not a slot indirection.
    [[gnu::always_inline]] inline bool isForced() const noexcept
    {
        Tag t = tag();
        return t != Tag::Thunk && t != Tag::App && t != Tag::Slot;
    }

    /// In-place initialisers (no allocation).
    inline void mkInt(int64_t n) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Int);
        payload.i = n;
    }
    inline void mkFloat(double d) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Float);
        payload.f = d;
    }
    inline void mkBool(bool b) noexcept;       // sets to vTrue/vFalse singleton
    inline void mkNull() noexcept;             // sets to vNull singleton
    inline void mkBlackhole() noexcept;        // sets to Blackhole tag
    inline void mkClosure(Closure * c) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Closure);
        payload.closure = c;
    }
    inline void mkThunk(Thunk * t) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Thunk);
        payload.thunk = t;
    }
    inline void mkAttrs(Bindings * b) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Attrs);
        payload.bindings = b;
    }
    inline void mkString(const char * s) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::String);
        payload.str = s;
    }
    inline void mkSlot(Value * p) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Slot);
        payload.slot = p;
    }

    /// Singletons (defined in value.cc).
    static Value vTrue;
    static Value vFalse;
    static Value vNull;
    static Value vBlackhole;
    static Value vEmptyList;
    static Value vEmptyAttrs;
};

/// Pair of Values for App / PrimOpApp.  Heap allocated; pointer kept in the
/// payload of the parent Value to keep the Value itself at 16 bytes.
struct ValuePair { Value left; Value right; };

static_assert(sizeof(Value) == 16, "v3 Value must be exactly 16 bytes");

inline void Value::mkBool(bool b) noexcept
{
    *this = b ? vTrue : vFalse;
}

inline void Value::mkNull() noexcept
{
    *this = vNull;
}

inline void Value::mkBlackhole() noexcept
{
    tag_payload = static_cast<uint64_t>(Tag::Blackhole);
    payload.raw = nullptr;
}

} // namespace nix::v3
