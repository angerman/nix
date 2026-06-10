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

// Lever B L4 (2026-06-10): the tagged 8-byte Value (NaN-box) is now the DEFAULT
// layout — lode/LEVER_B_IMPL_PLAN_2026-06-10.md. Gates met: −18–24% peak RSS
// (synthetic attrset-of-attrsets + the real firefox.drvPath), wall ≤5% on darwin-4
// (+1–4%), byte-identical (v3 lang 142/143; firefox.drvPath 16B↔8B identical), and
// #455 fixed so v3-direct completes real nixpkgs. Opt OUT to the legacy 16-byte
// layout during the soak window with -DV3_VALUE_16B_LEGACY; the 16B `#else` path and
// this valve retire once 8B has baked on the cutover-parity corpus + M5/HNE.
#if !defined(V3_VALUE_8B) && !defined(V3_VALUE_16B_LEGACY)
#  define V3_VALUE_8B 1
#endif

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
    /// 3-arg deferred application.  Reuses ValuePair: left = fn,
    /// right = arg1, evaluated = arg2 (NOT a memo cache — App3 is
    /// not memoized).  Created by mapAttrs / zipAttrsWith / similar
    /// "build a curried lazy entry per attribute" patterns to save
    /// one ValuePair allocation per entry (1 pair vs current 2).
    ///
    /// Forcing a Tag::App3: applies fn to arg1, then applies the
    /// result to arg2.  No memoization — single-reference use cases
    /// only.  Sharing an App3 pair across multiple Value slots is
    /// safe but each Force recomputes; for shared apps prefer the
    /// 2-pair Tag::App pattern with its `evaluated` memo.
    ///
    /// EXIT_GC_SPIRAL Week 1 Day 9-11 (2026-05-29) — per
    /// `EXIT_DAY3-5_DECISION §3.2`, ~50 MB savings on HNE.
    App3          = 17,
};

#ifdef V3_VALUE_8B
/// Lever B (lode/LEVER_B_IMPL_PLAN_2026-06-10.md §"L1 encoding") — NaN-box codec
/// for the tagged 8-byte Value; proven in test/value8-encoding-spike.cc over all
/// 18 tags + 48-bit ints (+ box overflow) + pointer-per-kind + float(±0/±inf/NaN).
///
/// A Value is one `uint64_t w`.  A Float is the raw IEEE-754 double EXCEPT the
/// boxed-NaN region (exp[52..62]=0x7FF AND mantissa[0..51]!=0): there, tag =
/// sign[63]‖bits[48..51] (5 bits → 32 values) and payload = bits[0..47] (a 48-bit
/// canonical user-space pointer OR a 48-bit signed immediate).  ±inf has
/// mantissa==0 → decodes as Float; a Nix NaN canonicalises to the FLOATNAN box.
/// Ints outside ±2^47 box into a heap int64 cell (BOXEDINT code).
namespace v8nan {
    inline constexpr uint64_t EXP       = 0x7FFULL << 52;     // exponent all-ones
    inline constexpr uint64_t MANT      = (1ULL << 52) - 1;   // mantissa [0..51]
    inline constexpr uint64_t PAY       = (1ULL << 48) - 1;   // payload  [0..47]
    inline constexpr int64_t  INT_MIN48 = -(1LL << 47);
    inline constexpr int64_t  INT_MAX48 =  (1LL << 47) - 1;
    inline constexpr uint8_t  FLOATNAN  = 20;  // ∉ tag codes {1..15,17,18,19}, low-nibble!=0
    inline constexpr uint8_t  BOXEDINT  = 21;  // 64-bit int that overflowed the 48-bit inline range
    /// Tag t → 5-bit code, skipping code 16 (low-nibble 0 → ±inf at payload 0).
    inline constexpr uint8_t  codeOf(Tag t)         { uint8_t c = static_cast<uint8_t>(t) + 1; return c >= 16 ? c + 1 : c; }
    inline constexpr Tag      tagFromCode(uint8_t c){ return static_cast<Tag>(c > 16 ? c - 2 : c - 1); }
    [[gnu::always_inline]] inline uint64_t box(uint8_t code5, uint64_t pay48) noexcept {
        return ((static_cast<uint64_t>(code5) >> 4) & 1) << 63 | EXP
             | ((static_cast<uint64_t>(code5) & 0xF) << 48) | (pay48 & PAY);
    }
    [[gnu::always_inline]] inline bool     isBoxed(uint64_t w) noexcept { return ((w >> 52) & 0x7FF) == 0x7FF && (w & MANT) != 0; }
    [[gnu::always_inline]] inline uint8_t  boxCode(uint64_t w) noexcept { return static_cast<uint8_t>((((w >> 63) & 1) << 4) | ((w >> 48) & 0xF)); }
    [[gnu::always_inline]] inline uint64_t boxPay (uint64_t w) noexcept { return w & PAY; }
    /// Overflow-int box/unbox (defined in value.cc so value.hh stays allocator-free):
    /// boxInt64 allocates a heap int64 cell (GC keeps it live as a leaf), returns its
    /// address; unboxInt64 derefs it.
    const void * boxInt64(int64_t n);
    int64_t      unboxInt64(const void * cell) noexcept;
}
#endif

/// Value: 16 bytes today (8 B tag word + 8 B payload union); under V3_VALUE_8B a
/// single NaN-boxed 8 B word (see the v8nan codec above).
struct Value
{
#ifdef V3_VALUE_8B
    uint64_t w;   ///< NaN-boxed tagged word.

    [[gnu::always_inline]] inline Tag tag() const noexcept
    {
        if (!v8nan::isBoxed(w)) return Tag::Float;        // normal double / ±inf
        uint8_t c = v8nan::boxCode(w);
        if (c == v8nan::FLOATNAN) return Tag::Float;      // canonicalised Nix NaN
        if (c == v8nan::BOXEDINT) return Tag::Int;        // overflow int in a heap cell
        return v8nan::tagFromCode(c);
    }
#else
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
#endif

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
    //
    // EXIT_GC_SPIRAL Week 1 Day 9-11 (2026-05-29): re-introducing
    // `isAppLike()` because Tag::App3 (the new 3-arg variant)
    // behaves identically to Tag::App in almost every dispatch
    // outside `forceValue`'s force handler.  Touch sites change
    // from `tag() == Tag::App` to `isAppLike()` when they want to
    // treat 2-arg and 3-arg apps the same.
    [[gnu::always_inline]] inline bool isAppLike() const noexcept
    {
        Tag t = tag();
        return t == Tag::App || t == Tag::App3;
    }

    /// Forced = not a thunk, not an unevaluated app, not a slot indirection.
    /// Tag::App3 added 2026-05-29 (EXIT Week 1 Day 9-11) — same
    /// unforced semantics as Tag::App.
    [[gnu::always_inline]] inline bool isForced() const noexcept
    {
        Tag t = tag();
        return t != Tag::Thunk && t != Tag::App && t != Tag::App3 && t != Tag::Slot;
    }

    /// L0 (LEVER_B_IMPL_PLAN_2026-06-10) — read accessors mirroring the mkX
    /// writers.  TODAY they just return the payload field.  When Lever B flips
    /// `Value` to a tagged 8-byte word, ONLY these accessors + the mkX writers
    /// change (decode/encode the tagged word); every caller routed through them
    /// stays correct without edits.  Migrating direct `.payload.X` reads to
    /// these is the (staged, byte-identical) L0 work that localizes the flip.
#ifdef V3_VALUE_8B
    // 8B: pointers are the low 48 bits; int sign-extends 48-bit (or derefs a
    // BOXEDINT cell); float reconstructs the double (NaN canonicalised back).
private:
    [[gnu::always_inline]] inline void * ptrBits() const noexcept
    { return reinterpret_cast<void *>(static_cast<uintptr_t>(v8nan::boxPay(w))); }
public:
    [[gnu::always_inline]] inline int64_t        asInt()     const noexcept {
        if (v8nan::boxCode(w) == v8nan::BOXEDINT) return v8nan::unboxInt64(ptrBits());
        uint64_t p = v8nan::boxPay(w);
        if (p & (1ULL << 47)) p |= ~v8nan::PAY;       // sign-extend 48-bit
        return static_cast<int64_t>(p);
    }
    [[gnu::always_inline]] inline double         asFloat()   const noexcept {
        if (v8nan::isBoxed(w) && v8nan::boxCode(w) == v8nan::FLOATNAN) return __builtin_nan("");
        double d; __builtin_memcpy(&d, &w, 8); return d;
    }
    [[gnu::always_inline]] inline const char *   asString()  const noexcept { return reinterpret_cast<const char *>(ptrBits()); }
    [[gnu::always_inline]] inline const char *   asPath()    const noexcept { return reinterpret_cast<const char *>(ptrBits()); }
    [[gnu::always_inline]] inline Bindings *      asAttrs()   const noexcept { return reinterpret_cast<Bindings *>(ptrBits()); }
    [[gnu::always_inline]] inline ListVec *       asList()    const noexcept { return reinterpret_cast<ListVec *>(ptrBits()); }
    [[gnu::always_inline]] inline Closure *       asClosure() const noexcept { return reinterpret_cast<Closure *>(ptrBits()); }
    [[gnu::always_inline]] inline Thunk *         asThunk()   const noexcept { return reinterpret_cast<Thunk *>(ptrBits()); }
    [[gnu::always_inline]] inline const PrimOp *  asPrimOp()  const noexcept { return reinterpret_cast<const PrimOp *>(ptrBits()); }
    [[gnu::always_inline]] inline ValuePair *     asPair()    const noexcept { return reinterpret_cast<ValuePair *>(ptrBits()); }
    [[gnu::always_inline]] inline Value *         asSlot()    const noexcept { return reinterpret_cast<Value *>(ptrBits()); }
    [[gnu::always_inline]] inline void *          asRaw()     const noexcept { return ptrBits(); }
#else
    [[gnu::always_inline]] inline int64_t        asInt()     const noexcept { return payload.i; }
    [[gnu::always_inline]] inline double         asFloat()   const noexcept { return payload.f; }
    [[gnu::always_inline]] inline const char *   asString()  const noexcept { return payload.str; }
    [[gnu::always_inline]] inline const char *   asPath()    const noexcept { return payload.path; }
    [[gnu::always_inline]] inline Bindings *      asAttrs()   const noexcept { return payload.bindings; }
    [[gnu::always_inline]] inline ListVec *       asList()    const noexcept { return payload.list; }
    [[gnu::always_inline]] inline Closure *       asClosure() const noexcept { return payload.closure; }
    [[gnu::always_inline]] inline Thunk *         asThunk()   const noexcept { return payload.thunk; }
    [[gnu::always_inline]] inline const PrimOp *  asPrimOp()  const noexcept { return payload.primop; }
    [[gnu::always_inline]] inline ValuePair *     asPair()    const noexcept { return payload.pair; }
    [[gnu::always_inline]] inline Value *         asSlot()    const noexcept { return payload.slot; }
    [[gnu::always_inline]] inline void *          asRaw()     const noexcept { return payload.raw; }
#endif

    /// L0: raw identity word — used by the CU-cache key + thunk fingerprint to
    /// compare Values by bits.  Today this is the tag word only (the 16B layout
    /// keys on the tag); under the tagged 8B layout it becomes the full encoded
    /// word (tag ‖ immediate/pointer) — strictly more precise, still a valid
    /// equality key.  Callers must treat it as opaque bits.
#ifdef V3_VALUE_8B
    [[gnu::always_inline]] inline uint64_t        rawWord()   const noexcept { return w; }
    [[gnu::always_inline]] inline void            mkUninitialized() noexcept { w = v8nan::box(v8nan::codeOf(Tag::Uninitialized), 0); }
    /// 8B: the serializer wants the SEMANTIC double's bits; reconstruct via asFloat
    /// (a canonicalised NaN round-trips back to a NaN, which is fine).
    [[gnu::always_inline]] inline uint64_t        floatBits() const noexcept { double d = asFloat(); uint64_t b; __builtin_memcpy(&b, &d, 8); return b; }
    [[gnu::always_inline]] inline void            setFloatBits(uint64_t b) noexcept { double d; __builtin_memcpy(&d, &b, 8); mkFloat(d); }
#else
    [[gnu::always_inline]] inline uint64_t        rawWord()   const noexcept { return tag_payload; }
    /// L0: clear to Uninitialized (replaces `v.tag_payload = 0` / `= Tag::Uninitialized`).
    [[gnu::always_inline]] inline void            mkUninitialized() noexcept { tag_payload = static_cast<uint64_t>(Tag::Uninitialized); payload.raw = nullptr; }
    /// L0: raw float bits — for the serializer's bit-exact read/write of a
    /// Float (replaces `memcpy(&…, &v.payload.f, 8)` / `memcmp(&a.payload.f,…)`).
    /// Under the tagged 8B layout these become the float (de)encoders.
    [[gnu::always_inline]] inline uint64_t        floatBits() const noexcept { uint64_t b; __builtin_memcpy(&b, &payload.f, 8); return b; }
    [[gnu::always_inline]] inline void            setFloatBits(uint64_t b) noexcept { tag_payload = static_cast<uint64_t>(Tag::Float); __builtin_memcpy(&payload.f, &b, 8); }
#endif

    inline void mkBool(bool b) noexcept;       // sets to vTrue/vFalse singleton
    inline void mkNull() noexcept;             // sets to vNull singleton
    inline void mkBlackhole() noexcept;        // sets to Blackhole tag

    /// In-place initialisers (no allocation, except mkInt's rare boxed-overflow).
#ifdef V3_VALUE_8B
private:
    [[gnu::always_inline]] inline void setPtr(Tag t, const void * p) noexcept
    { w = v8nan::box(v8nan::codeOf(t), static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p))); }
public:
    inline void mkInt(int64_t n) noexcept {
        if (n >= v8nan::INT_MIN48 && n <= v8nan::INT_MAX48)
            w = v8nan::box(v8nan::codeOf(Tag::Int), static_cast<uint64_t>(n) & v8nan::PAY);
        else
            w = v8nan::box(v8nan::BOXEDINT, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(v8nan::boxInt64(n))));
    }
    inline void mkFloat(double d) noexcept {
        if (__builtin_isnan(d)) { w = v8nan::box(v8nan::FLOATNAN, 0); return; }
        __builtin_memcpy(&w, &d, 8);
    }
    inline void mkClosure(Closure * c)     noexcept { setPtr(Tag::Closure, c); }
    inline void mkThunk  (Thunk * t)       noexcept { setPtr(Tag::Thunk, t); }
    inline void mkAttrs  (Bindings * b)    noexcept { setPtr(Tag::Attrs, b); }
    inline void mkString (const char * s)  noexcept { setPtr(Tag::String, s); }
    inline void mkSlot   (Value * p)       noexcept { setPtr(Tag::Slot, p); }
    inline void mkList   (ListVec * l)     noexcept { setPtr(Tag::List, l); }
    inline void mkPath   (const char * p)  noexcept { setPtr(Tag::Path, p); }
    inline void mkPrimOp (const PrimOp * p)noexcept { setPtr(Tag::PrimOp, p); }
    inline void mkExternal(void * p)       noexcept { setPtr(Tag::External, p); }
    /// App / App3 / PrimOpApp share the ValuePair* payload; caller picks the tag.
    inline void mkPair   (Tag t, ValuePair * p) noexcept { setPtr(t, p); }
#else
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
    // L0: remaining pointer/immediate writers (write targets for the
    // `.payload.X = …` → `mkX(…)` migration).  Each sets tag + payload as a
    // unit; under the tagged 8B layout they become the encoders.
    inline void mkList(ListVec * l) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::List);
        payload.list = l;
    }
    inline void mkPath(const char * p) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::Path);
        payload.path = p;
    }
    inline void mkPrimOp(const PrimOp * p) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::PrimOp);
        payload.primop = p;
    }
    inline void mkExternal(void * p) noexcept
    {
        tag_payload = static_cast<uint64_t>(Tag::External);
        payload.raw = p;
    }
    /// App / App3 / PrimOpApp share the ValuePair* payload; the caller picks
    /// the tag (they are distinct dispatch tags).  `t` must be one of
    /// Tag::App / Tag::App3 / Tag::PrimOpApp.
    inline void mkPair(Tag t, ValuePair * p) noexcept
    {
        tag_payload = static_cast<uint64_t>(t);
        payload.pair = p;
    }
#endif

    /// Singletons (defined in value.cc).
    static Value vTrue;
    static Value vFalse;
    static Value vNull;
    static Value vBlackhole;
    static Value vEmptyList;
    static Value vEmptyAttrs;
};

// L0 (LEVER_B_IMPL_PLAN_2026-06-10) canary: `Value` is 16 B today (8 B tag word
// + 8 B payload).  Lever B shrinks it to a tagged 8 B word (pointer tagging +
// 61-bit inline ints, box overflow), projected ≈ −28% arena (Bindings entry
// 24→16, ValuePair 64→32, ListVec elem 16→8, thunk Value field halves).  When
// that lands, flip this assert to ==8 together with the accessors/mkX encoders.
#ifdef V3_VALUE_8B
static_assert(sizeof(Value) == 8,
              "V3_VALUE_8B: Value must be exactly 8 bytes (NaN-boxed word).");
#else
static_assert(sizeof(Value) == 16,
              "Value is 16B today; Lever B (lode/LEVER_B_IMPL_PLAN_2026-06-10.md) "
              "targets a tagged 8B Value — change this assert + the as*/mk* "
              "accessors together when the encoding lands.");
#endif

/// Pair of Values for App / PrimOpApp.  Heap allocated; pointer kept in the
/// payload of the parent Value to keep the Value itself at 16 bytes.
///
/// 2026-05-18: `evaluated` field added for App-result memoization.  When
/// forceValue resolves a Tag::App, it stores the WHNF result in
/// `evaluated` (initially Tag::Uninitialized).  Subsequent forces of the
/// same App short-circuit by reading `evaluated.tag() != Uninitialized`.
/// Without this, lazy entries built by genList/map (e.g.
/// extendDerivation's `outputsList = map (...)` lambda body) re-execute
/// the lambda on every access — observed as 32%+ of forces hitting a
/// single thunk on nixpkgs hello.drvPath.  Cost: 16 bytes per ValuePair
/// (32 → 48), but each lazy entry needs only one alloc total.
///
/// PrimOpApp doesn't use `evaluated` (the App-arg chain is consumed by
/// callClosure / OP_CALL's primop branch which reads left/right then
/// invokes; no force happens on PrimOpApp itself).  The extra field is
/// inert for PrimOpApp instances — small per-instance waste.
///
/// 2026-05-30 (EXIT_GC_SPIRAL Day 4 option A): added `third` slot for
/// Tag::App3 to carry its second argument WITHOUT overloading the
/// memoization slot.  The Day 9-11 attempt overloaded `evaluated` for
/// arg2 and lost App-result memoization (#696 regression — re-eval
/// every dispatch on hot mapAttrs entries).  This version keeps
/// `evaluated` separate so Tag::App3 dispatches memoize identically
/// to Tag::App.  Cost: +16 B per ValuePair across all tags.  Net on
/// HNE: -29 MB (-35 MB mapAttrs savings minus +6 MB other-App tax).
/// `third` is meaningful ONLY when Value's tag == Tag::App3; for
/// Tag::App / Tag::PrimOpApp instances it stays Uninitialized.
///
/// Field order chosen so Tag::App's hot path (left, right, evaluated)
/// keeps the same first-3-slot layout as the prior 48-byte struct.
/// `third` lives at offset 48 — Tag::App ignores it, Tag::App3 reads
/// it after the hot fields.
struct ValuePair { Value left; Value right; Value evaluated; Value third; };

#ifdef V3_VALUE_8B
static_assert(sizeof(Value) == 8, "v3 Value must be exactly 8 bytes under V3_VALUE_8B");
static_assert(sizeof(ValuePair) == 32, "ValuePair is 4×8B = 32B under V3_VALUE_8B (T3)");
#else
static_assert(sizeof(Value) == 16, "v3 Value must be exactly 16 bytes");
#endif

/// Tag classification for precise-root scanning.
///
/// **The single source of truth** for "does this Tag's payload hold a
/// v3-heap pointer the GC must trace?"  Every walker (nursery scavenger,
/// auditor, BRUTE scanner, future precise-root infrastructure) MUST
/// agree on this classification — without that agreement, missed
/// pointers cause silent corruption (Phase D / Phase E missed-root
/// bugs all traced to walker-vs-allocator-vs-emitter disagreement).
///
/// Tags producing v3-heap pointers in `payload`:
///   Closure   → payload.closure   (Closure*)
///   Thunk     → payload.thunk     (Thunk*)
///   Attrs     → payload.bindings  (Bindings*)
///   List      → payload.list      (ListVec*)
///   App       → payload.pair      (ValuePair*)
///   PrimOpApp → payload.pair      (ValuePair*)
///   Slot      → payload.slot      (Value*; pointer into a tenured cell)
///
/// Tags with payload that is NOT a v3-heap pointer (scalar OR external
/// pointer that the GC does NOT manage):
///   Uninitialized / Int / Float / Bool / Null   — scalar or empty
///   String / Path                                — const char* into
///                                                  arena-allocated text
///                                                  (immutable; tenured by
///                                                  construction)
///   PrimOp                                       — const PrimOp* to
///                                                  static registration
///   Blackhole                                    — transient marker
///   External                                     — opaque void*
///
/// Codified 2026-05-27 as the foundation for the "ditch Boehm" precise-
/// root infrastructure (`GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md`).
/// Replaces ad-hoc Tag dispatch tables previously duplicated in:
///   gc.cc Scavenger::visitValue (the nursery walker)
///   gc.cc postScavengeAudit
///   gc.cc postScavengeBruteScan
///   ir_dump.cc value-printing dispatch
[[nodiscard]] constexpr bool tagIsPointer(Tag t) noexcept
{
    switch (t) {
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::Attrs:
    case Tag::List:
    case Tag::App:
    case Tag::App3:
    case Tag::PrimOpApp:
    case Tag::Slot:
        return true;
    case Tag::Uninitialized:
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::String:
    case Tag::Path:
    case Tag::PrimOp:
    case Tag::Blackhole:
    case Tag::External:
        return false;
    }
    // -Werror=switch-enum should catch any missing case at compile
    // time; the unreachable() here is defense-in-depth in case the
    // compiler treats the switch as exhaustive (gcc) vs partial (other).
    return false;
}

/// Convenience: same predicate against a Value.
[[nodiscard]] inline bool valueHoldsPointer(const Value & v) noexcept
{
    return tagIsPointer(v.tag());
}

// Compile-time correctness gate for the classification — if any Tag's
// payload semantic changes (e.g. Tag::PrimOp becomes a fully GC-managed
// pointer instead of static), exactly one of these static_asserts will
// fail and force a code review of the dispatch.  Cheap insurance
// against silent walker drift.
static_assert(!tagIsPointer(Tag::Uninitialized));
static_assert(!tagIsPointer(Tag::Int));
static_assert(!tagIsPointer(Tag::Float));
static_assert(!tagIsPointer(Tag::Bool));
static_assert(!tagIsPointer(Tag::Null));
static_assert(!tagIsPointer(Tag::String));
static_assert(!tagIsPointer(Tag::Path));
static_assert( tagIsPointer(Tag::Attrs));
static_assert( tagIsPointer(Tag::List));
static_assert( tagIsPointer(Tag::Closure));
static_assert( tagIsPointer(Tag::Thunk));
static_assert(!tagIsPointer(Tag::PrimOp));
static_assert( tagIsPointer(Tag::PrimOpApp));
static_assert( tagIsPointer(Tag::App));
static_assert( tagIsPointer(Tag::App3));
static_assert(!tagIsPointer(Tag::Blackhole));
static_assert(!tagIsPointer(Tag::External));
static_assert( tagIsPointer(Tag::Slot));

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
#ifdef V3_VALUE_8B
    w = v8nan::box(v8nan::codeOf(Tag::Blackhole), 0);
#else
    tag_payload = static_cast<uint64_t>(Tag::Blackhole);
    payload.raw = nullptr;
#endif
}

} // namespace nix::v3
