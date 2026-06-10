// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
// SPDX-License-Identifier: Apache-2.0
//
// Lever B L1 encoding spike (lode/LEVER_B_IMPL_PLAN_2026-06-10.md).
// STANDALONE — mirrors v3's Tag enum + a NaN-boxed 8-byte Value encoding and
// round-trips every case.  NOT wired into the VM; this de-risks the crux 8B
// encoding (does the scheme faithfully hold all 17 tags + 48-bit ints w/ box
// overflow + mixed-alignment pointers + floats incl. ±0/±inf/NaN?) before the
// pervasive migration.  Build+run:
//   c++ -std=c++20 -O2 -o /tmp/v8spike value8-encoding-spike.cc && /tmp/v8spike
//
// Encoding (NaN-box): a float is the raw double, EXCEPT exp=0x7FF & mantissa!=0
// patterns, which are "boxed" values: tag = sign[63] ‖ bits[48..51] (5 bits, 32
// values), payload = bits[0..47] (48-bit pointer or signed immediate).  +-inf
// (exp=0x7FF, mantissa==0) decode as Float; Nix NaN canonicalises to a reserved
// FLOATNAN boxed tag.  Ints outside 48-bit signed are boxed to a heap cell.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <bit>
#include <limits>
#include <initializer_list>

namespace {

// --- mirror of v3 nix::v3::Tag (value.hh) — 18 tags, 0..17 incl. App3=17 ---
enum class Tag : uint8_t {
    Uninitialized=0, Int=1, Float=2, Bool=3, Null=4, String=5, Path=6,
    Attrs=7, List=8, Closure=9, Thunk=10, PrimOp=11, PrimOpApp=12, App=13,
    Blackhole=14, External=15, Slot=16, App3=17,
};

// 5-bit tag CODES.  A boxed value must stay a NaN (exp=0x7FF, mantissa!=0), never
// ±inf — so a code with a zero payload must keep the mantissa nonzero via its tag
// bits.  Mantissa tag bits = the LOW NIBBLE (bits[48..51]); the 5th tag bit is the
// sign, which is NOT a mantissa bit.  ⇒ any code whose low nibble is 0 (codes 0 and
// 16) is forbidden (a zero-payload boxed value there would be ±inf).  Map Tag t ->
// 1.. skipping 16: tags 0..14 → codes 1..15, then 15/16/17 → 17/18/19.  FLOATNAN is
// the reserved float-NaN code; it MUST avoid every tag code — with App3=17 the tag
// codes run up to 19, so FLOATNAN=20 (low-nibble 4, nonzero, ≤31).  (The earlier
// FLOATNAN=19 was correct ONLY for the 17-tag enum without App3 — codeOf(App3)=19
// collides; this is the corrected value for the real 18-tag enum.)
constexpr uint8_t  codeOf(Tag t)    { uint8_t c = (uint8_t)t + 1; return c >= 16 ? c + 1 : c; }
constexpr Tag      tagFromCode(uint8_t c) { return (Tag)(c > 16 ? c - 2 : c - 1); }
constexpr uint8_t  FLOATNAN = 20;

constexpr uint64_t EXP   = 0x7FFULL << 52;            // exponent all ones
constexpr uint64_t MANT  = (1ULL << 52) - 1;          // mantissa bits [0..51]
constexpr uint64_t PAY   = (1ULL << 48) - 1;          // payload bits [0..47]
constexpr int64_t  INT_MIN48 = -(1LL << 47);
constexpr int64_t  INT_MAX48 =  (1LL << 47) - 1;

// Box a (tagCode, 48-bit payload) into the NaN region.
inline uint64_t box(uint8_t code5, uint64_t payload48) {
    uint64_t sign = (uint64_t)(code5 >> 4) & 1;
    uint64_t lo4  = (uint64_t)(code5 & 0xF);
    return (sign << 63) | EXP | (lo4 << 48) | (payload48 & PAY);
}
inline bool     isBoxed(uint64_t w) { return ((w >> 52) & 0x7FF) == 0x7FF && (w & MANT) != 0; }
inline uint8_t  boxCode(uint64_t w) { return (uint8_t)(((w >> 63) & 1) << 4 | ((w >> 48) & 0xF)); }
inline uint64_t boxPay(uint64_t w)  { return w & PAY; }

// --- encoders ---
inline uint64_t encFloat(double d) {
    if (std::isnan(d)) return box(FLOATNAN, 0);           // canonicalise NaN
    return std::bit_cast<uint64_t>(d);                    // normal/±0/±inf raw
}
// Returns false if n must be boxed to a heap cell (out of 48-bit signed range).
inline bool encIntInline(int64_t n, uint64_t & out) {
    if (n < INT_MIN48 || n > INT_MAX48) return false;
    out = box(codeOf(Tag::Int), (uint64_t)n & PAY);
    return true;
}
inline uint64_t encPtr(Tag t, const void * p) {
    return box(codeOf(t), (uint64_t)(uintptr_t)p);
}
inline uint64_t encConst(Tag t) { return box(codeOf(t), 0); } // Bool/Null/Uninit/Blackhole

// --- decoders ---
inline Tag decTag(uint64_t w) {
    if (!isBoxed(w)) return Tag::Float;                   // normal double / ±inf
    uint8_t c = boxCode(w);
    if (c == FLOATNAN) return Tag::Float;
    return tagFromCode(c);
}
inline double  decFloat(uint64_t w) {
    if (isBoxed(w) && boxCode(w) == FLOATNAN) return std::numeric_limits<double>::quiet_NaN();
    return std::bit_cast<double>(w);
}
inline int64_t decIntInline(uint64_t w) {
    uint64_t p = boxPay(w);
    // sign-extend 48-bit
    if (p & (1ULL << 47)) p |= ~PAY;
    return (int64_t)p;
}
inline void * decPtr(uint64_t w) { return (void *)(uintptr_t)boxPay(w); }

// --- test harness ---
int failures = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); ++failures; } } while(0)

void testTags() {
    // pointer tags: distinct tag, pointer round-trips (use 16-aligned + 8-aligned + odd)
    const Tag ptrTags[] = { Tag::String, Tag::Path, Tag::Attrs, Tag::List, Tag::Closure,
                            Tag::Thunk, Tag::PrimOp, Tag::PrimOpApp, Tag::App, Tag::App3,
                            Tag::External, Tag::Slot };
    // a few representative 48-bit pointers incl. non-16-aligned (PrimOp) and odd (char*)
    const uintptr_t ptrs[] = { 0x10, 0x1000, 0x7ffeed00, 0xabcdef012340ULL, 0x123ULL /*8-unaligned*/, 0x1ULL };
    for (Tag t : ptrTags) {
        for (uintptr_t pv : ptrs) {
            uint64_t w = encPtr(t, (void*)pv);
            CHECK(decTag(w) == t, "ptr tag roundtrip");
            CHECK((uintptr_t)decPtr(w) == (pv & PAY), "ptr payload roundtrip");
        }
    }
    // App3 = a 17th tag (Slot is code 17); ensure App3-equivalent (we used App/Slot/External)
    // constants
    for (Tag t : { Tag::Bool, Tag::Null, Tag::Uninitialized, Tag::Blackhole }) {
        uint64_t w = encConst(t);
        CHECK(decTag(w) == t, "const tag roundtrip");
    }
}

void testInts() {
    int64_t vals[] = { 0, 1, -1, 2, -2, 1000, -1000, INT_MAX48, INT_MIN48,
                       (1LL<<46), -(1LL<<46), 0x7fffffffLL, -0x80000000LL };
    for (int64_t n : vals) {
        uint64_t w; bool inl = encIntInline(n, w);
        CHECK(inl, "int fits 48-bit inline");
        CHECK(decTag(w) == Tag::Int, "int tag");
        CHECK(decIntInline(w) == n, "int value roundtrip");
    }
    // overflow → must box (signal false)
    int64_t big[] = { (1LL<<47), -(1LL<<47)-1, std::numeric_limits<int64_t>::max(),
                      std::numeric_limits<int64_t>::min() };
    for (int64_t n : big) { uint64_t w; CHECK(!encIntInline(n, w), "big int must box"); }
}

void testFloats() {
    double vals[] = { 0.0, -0.0, 1.0, -1.0, 3.14159, 1e300, -1e300, 1e-300,
                      std::numeric_limits<double>::infinity(),
                      -std::numeric_limits<double>::infinity() };
    for (double d : vals) {
        uint64_t w = encFloat(d);
        CHECK(decTag(w) == Tag::Float, "float tag");
        double r = decFloat(w);
        CHECK(std::bit_cast<uint64_t>(r) == std::bit_cast<uint64_t>(d), "float bits roundtrip");
    }
    // NaN
    double nan = std::numeric_limits<double>::quiet_NaN();
    uint64_t w = encFloat(nan);
    CHECK(decTag(w) == Tag::Float, "NaN decodes as Float tag");
    CHECK(std::isnan(decFloat(w)), "NaN roundtrips to NaN");
    // a "raw" signalling NaN that v3 might produce must NOT be mistaken for a boxed tag:
    // we canonicalise on encode, so the only NaN we ever STORE is FLOATNAN — verified above.
}

void testNoCollision() {
    // Every tag code we ACTUALLY use must, with a zero payload, still be a NaN
    // (exp=0x7FF, mantissa!=0) — never ±inf.  Iterate the real codes (all 18
    // Tags 0..17 incl. App3 + FLOATNAN), worst case payload==0.  This also
    // proves no tag code collides with FLOATNAN.
    int seen[64] = {0};
    for (int ti = 0; ti <= 17; ++ti) {
        uint8_t c = codeOf((Tag)ti);
        CHECK(c != FLOATNAN, "tag code must not collide with FLOATNAN");
        CHECK(!seen[c], "tag codes must be distinct");
        seen[c] = 1;
    }
    for (int ti = 0; ti <= 17; ++ti) {
        uint8_t c = codeOf((Tag)ti);
        CHECK((c & 0xF) != 0, "tag code low-nibble nonzero (zero-payload stays NaN)");
        CHECK(isBoxed(box(c, 0)), "boxed-zero-payload stays NaN (not inf)");
        CHECK(tagFromCode(c) == (Tag)ti, "code<->tag bijection");
    }
    CHECK((FLOATNAN & 0xF) != 0, "FLOATNAN low-nibble nonzero");
    CHECK(isBoxed(box(FLOATNAN, 0)), "FLOATNAN-zero-payload stays NaN");
    CHECK(sizeof(uint64_t) == 8, "8-byte word");
}

} // namespace

int main() {
    testTags(); testInts(); testFloats(); testNoCollision();
    if (failures == 0) { printf("value8 encoding spike: ALL PASS (NaN-box, 5-bit tag, 48-bit payload)\n"); return 0; }
    printf("value8 encoding spike: %d FAILURES\n", failures); return 1;
}
