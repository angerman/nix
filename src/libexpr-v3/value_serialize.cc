/// @file
/// #741 Phase 1 spike — implementation of value_serialize.hh.
///
/// See header for the binary format spec.  This file holds:
///   - serialize() / deserialize() recursive encoders
///   - valuesEqual() structural comparator
///   - runRoundTripTest() instrumented per-call gate
///   - dumpStats() summary writer
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value_serialize.hh"

#include "v3/alloc.hh"
#include "v3/barrier.hh"
#include "v3/closure.hh"  // Thunk + ThunkState (for chaseToWHNF)
#include "v3/disk_cache.hh"  // Phase 5: lookupEvalResult / insertEvalResult
#include "v3/cache_probe.hh"  // #828 / A3 / B1 per-call-site cache-hook
#include "v3/ir.hh"
#include "v3/value.hh"
#include "v3/ffi.hh"  // nix::Hash / HashAlgorithm via the FFI surface (no direct TW include)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace nix::v3::value_serialize {

// ---------------------------------------------------------------------------
// Format constants.
// ---------------------------------------------------------------------------

namespace {

constexpr char kMagic[4] = {'V', '3', 'V', 'R'};
constexpr uint8_t kSchema = 1;

constexpr uint8_t kTagInt    = 'I';
constexpr uint8_t kTagFloat  = 'F';
constexpr uint8_t kTagBool   = 'B';
constexpr uint8_t kTagNull   = 'N';
constexpr uint8_t kTagString = 'S';
constexpr uint8_t kTagPath   = 'P';
constexpr uint8_t kTagList   = 'L';
constexpr uint8_t kTagAttrs  = 'A';

// ---------------------------------------------------------------------------
// LE encoders / decoders.  Bytewise so we don't rely on host endianness;
// hello.drvPath / x86_64-darwin is LE-native but staying portable
// costs nothing.
// ---------------------------------------------------------------------------

void writeU8(std::string & out, uint8_t v) { out.push_back(static_cast<char>(v)); }

void writeU32(std::string & out, uint32_t v)
{
    char buf[4];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    buf[2] = static_cast<char>((v >> 16) & 0xFF);
    buf[3] = static_cast<char>((v >> 24) & 0xFF);
    out.append(buf, 4);
}

void writeU64(std::string & out, uint64_t v)
{
    char buf[8];
    for (int i = 0; i < 8; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
    out.append(buf, 8);
}

void writeBytes(std::string & out, std::string_view s)
{
    out.append(s.data(), s.size());
}

// ---------------------------------------------------------------------------

class Reader {
public:
    Reader(std::string_view s) : data(s.data()), end(s.data() + s.size()) {}

    uint8_t u8()
    {
        if (data >= end) throw SerializeError("truncated input (u8)");
        return static_cast<uint8_t>(*data++);
    }

    uint32_t u32()
    {
        if (end - data < 4) throw SerializeError("truncated input (u32)");
        uint32_t v = static_cast<uint8_t>(data[0])
                  | (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8)
                  | (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16)
                  | (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
        data += 4;
        return v;
    }

    uint64_t u64()
    {
        if (end - data < 8) throw SerializeError("truncated input (u64)");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (8 * i);
        data += 8;
        return v;
    }

    std::string_view bytes(size_t n)
    {
        if (static_cast<size_t>(end - data) < n)
            throw SerializeError("truncated input (bytes)");
        std::string_view r(data, n);
        data += n;
        return r;
    }

    bool atEnd() const { return data >= end; }

private:
    const char * data;
    const char * end;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Thunk / App / Slot chasing — read evaluated state without mutating.
//
// #741 Phase 3b (2026-05-23): serializing INPUTS to derivation primops
// requires walking past the Tag::Thunk / Tag::App / Tag::Slot
// indirections that wrap evaluated values.  forceDeep MUTATES Bindings
// via bindingsSetValue, which broke nixpkgs hello.drvPath (RCA in
// Phase 3a commit).  This helper READS but never writes: follows
// Thunk::evaluated when state == Evaluated, ValuePair::evaluated
// when set, Slot pointer transitively.  Throws if the chain ends on
// a non-WHNF (still-Suspended thunk, un-evaluated App, etc.) — caller
// (canonicalHash via serializeOne) catches and bypasses the cache.
// ---------------------------------------------------------------------------

namespace {

const Value & chaseToWHNF(const Value & v, int maxHops = 32)
{
    const Value * cur = &v;
    for (int i = 0; i < maxHops; ++i) {
        Tag t = cur->tag();
        if (t == Tag::Thunk) {
            Thunk * th = cur->asThunk();
            if (!th || th->state != ThunkState::Evaluated)
                throw SerializeError("Thunk not Evaluated; cannot canonical-hash");
            cur = &th->evaluated;
            continue;
        }
        if (t == Tag::App) {
            ValuePair * p = cur->asPair();
            if (!p || p->evaluated.tag() == Tag::Uninitialized)
                throw SerializeError("App not yet evaluated");
            cur = &p->evaluated;
            continue;
        }
        if (t == Tag::App3) {
            // 2026-05-30: Tag::App3 now has a separate `third` slot for
            // arg2 and `evaluated` is preserved as memoization sink
            // (same shape as Tag::App).  Chase through evaluated when
            // populated.
            ValuePair * p = cur->asPair();
            if (!p || p->evaluated.tag() == Tag::Uninitialized)
                throw SerializeError("App3 not yet evaluated; force before serialise");
            cur = &p->evaluated;
            continue;
        }
        if (t == Tag::Slot) {
            if (!cur->asSlot())
                throw SerializeError("Slot null");
            cur = cur->asSlot();
            continue;
        }
        return *cur;  // WHNF
    }
    throw SerializeError("chase chain exceeded max hops");
}

} // anonymous

// ---------------------------------------------------------------------------
// Serialise.
// ---------------------------------------------------------------------------

static void serializeOne(const Value & vIn, std::string & out);

// M-6 (CODEBASE_REVIEW_2026-06-11): bound serialisation recursion.  serializeOne
// → serializeList/serializeAttrs → serializeOne is unbounded; a deeply-nested or
// cyclic Value (which can reach the IFD / drvHash eval-result caches) would
// overflow the C stack — an uncatchable crash — instead of a catchable
// SerializeError.  A depth cap is sufficient to convert the overflow into an
// error (cyclic values hit the cap and throw).
namespace { thread_local int s_serializeDepth = 0; }
namespace { constexpr int kMaxSerializeDepth = 10000; }
struct SerializeDepthGuard {
    SerializeDepthGuard() {
        if (++s_serializeDepth > kMaxSerializeDepth) {
            --s_serializeDepth;
            throw SerializeError(
                "value too deeply nested (or cyclic) to serialise");
        }
    }
    ~SerializeDepthGuard() { --s_serializeDepth; }
};

static void serializeString(const Value & v, std::string & out)
{
    writeU8(out, kTagString);
    const char * buf = v.asString();
    size_t n = buf ? std::strlen(buf) : 0;
    if (n > 0x7FFFFFFFu) throw SerializeError("string too large to serialise");
    writeU32(out, static_cast<uint32_t>(n));
    if (n) writeBytes(out, std::string_view(buf, n));
    // Context entries — already in encoded form (`!o!p` / `=p` / `p`).
    const std::vector<std::string> * ctx =
        buf ? lookupStringContextEntries(buf) : nullptr;
    uint32_t ctxCount = ctx ? static_cast<uint32_t>(ctx->size()) : 0;
    writeU32(out, ctxCount);
    if (ctx) {
        // T-6 (CODEBASE_REVIEW_2026-06-11): the string-context side-table
        // stores entries in INSERTION order, not sorted (the "std::set backing
        // NixStringContext" claim on canonicalHash is inaccurate — it is a
        // std::vector).  Serialising in insertion order makes a Value's
        // canonical hash depend on the order context was accumulated, so two
        // structurally-identical values hash differently → spurious
        // cross-process eval-result cache MISSES.  Sort here so the serialised
        // form (and thus canonicalHash) is order-independent — matching
        // valuesEqual, which already compares sorted copies.
        std::vector<std::string> sortedCtx(*ctx);
        std::sort(sortedCtx.begin(), sortedCtx.end());
        for (const auto & e : sortedCtx) {
            if (e.size() > 0x7FFFFFFFu)
                throw SerializeError("context entry too large to serialise");
            writeU32(out, static_cast<uint32_t>(e.size()));
            writeBytes(out, e);
        }
    }
}

static void serializeAttrs(const Value & v, std::string & out)
{
    writeU8(out, kTagAttrs);
    const Bindings * b = v.asAttrs();
    uint32_t n = b ? b->countDistinct() : 0;
    writeU32(out, n);
    if (!b) return;
    // Bindings::entries are sorted ascending by SymbolId.  Determinism
    // requires sorting by NAME-string instead, since SymbolId numbering
    // varies across processes (interning order).  Stream visible chain
    // entries and sort name->value pairs once before emitting.
    const auto & gst = ir::globalSymbolTable();
    struct NameValue { std::string_view name; Value value; };
    std::vector<NameValue> ordered;
    ordered.reserve(n);
    b->forEach([&](const Bindings::Entry & e) {
        SymbolId sid = e.name;
        std::string_view nm = sid < gst.size()
            ? std::string_view(gst[sid])
            : std::string_view{};
        ordered.push_back({nm, e.value});
    });
    std::sort(ordered.begin(), ordered.end(),
        [](const NameValue & a, const NameValue & b) { return a.name < b.name; });
    for (const auto & e : ordered) {
        if (e.name.size() > 0x7FFFFFFFu)
            throw SerializeError("attr name too large to serialise");
        writeU32(out, static_cast<uint32_t>(e.name.size()));
        writeBytes(out, e.name);
        serializeOne(e.value, out);
    }
}

static void serializeList(const Value & v, std::string & out)
{
    writeU8(out, kTagList);
    const ListVec * lv = v.asList();
    uint32_t n = lv ? lv->size : 0;
    writeU32(out, n);
    if (!lv) return;
    for (uint32_t i = 0; i < n; ++i)
        serializeOne(lv->elems[i], out);
}

static void serializeOne(const Value & vIn, std::string & out)
{
    SerializeDepthGuard depthGuard;  // M-6: bound recursion (cycle/deep-nest)
    // Phase 3b: chase Thunk/App/Slot to WHNF.  No-op on already-WHNF
    // inputs (so Phase 1 round-trip + Phase 2 hash semantics are
    // unchanged on derivation-result Values).
    const Value & v = chaseToWHNF(vIn);
    Tag t = v.tag();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (t) {
    case Tag::Int:
        writeU8(out, kTagInt);
        writeU64(out, static_cast<uint64_t>(v.asInt()));
        return;
    case Tag::Float: {
        writeU8(out, kTagFloat);
        writeU64(out, v.floatBits());
        return;
    }
    case Tag::Bool:
        writeU8(out, kTagBool);
        // vTrue/vFalse: asInt() is the bool encoded as 0|1.
        writeU8(out, v.asInt() ? 1 : 0);
        return;
    case Tag::Null:
        writeU8(out, kTagNull);
        return;
    case Tag::String:
        serializeString(v, out);
        return;
    case Tag::Path: {
        writeU8(out, kTagPath);
        const char * p = v.asPath();
        size_t n = p ? std::strlen(p) : 0;
        if (n > 0x7FFFFFFFu) throw SerializeError("path too large to serialise");
        writeU32(out, static_cast<uint32_t>(n));
        if (n) writeBytes(out, std::string_view(p, n));
        return;
    }
    case Tag::List:
        serializeList(v, out);
        return;
    case Tag::Attrs:
        serializeAttrs(v, out);
        return;
    default:
        throw SerializeError(std::string("unsupported tag in serialise: ")
            + std::to_string(static_cast<int>(t)));
    }
#pragma clang diagnostic pop
}

void serialize(const Value & v, std::string & out)
{
    out.append(kMagic, 4);
    writeU8(out, kSchema);
    serializeOne(v, out);
}

// ---------------------------------------------------------------------------
// Deserialise.
// ---------------------------------------------------------------------------

static Value deserializeOne(Reader & r);

static Value deserializeString(Reader & r)
{
    uint32_t strLen = r.u32();
    auto bytes = r.bytes(strLen);
    // Use arena-backed buffer so the lifetime matches a v3-native
    // String.  Add the null terminator manually (strlen-based callers
    // expect it; see mkStringValueOwned at primops.cc:601).
    char * buf = Alloc::allocChars(strLen + 1);
    if (strLen) std::memcpy(buf, bytes.data(), strLen);
    buf[strLen] = '\0';
    Value v;
    v.mkString(buf);
    uint32_t ctxCount = r.u32();
    if (ctxCount > 0) {
        std::vector<std::string> entries;
        entries.reserve(ctxCount);
        for (uint32_t i = 0; i < ctxCount; ++i) {
            uint32_t entryLen = r.u32();
            auto e = r.bytes(entryLen);
            entries.emplace_back(e.data(), entryLen);
        }
        setStringContextEntries(buf, std::move(entries));
    }
    return v;
}

static Value deserializePath(Reader & r)
{
    uint32_t pathLen = r.u32();
    auto bytes = r.bytes(pathLen);
    char * buf = Alloc::allocChars(pathLen + 1);
    if (pathLen) std::memcpy(buf, bytes.data(), pathLen);
    buf[pathLen] = '\0';
    Value v;
    v.mkPath(buf);
    return v;
}

static Value deserializeList(Reader & r)
{
    uint32_t n = r.u32();
    Value v;
    if (n == 0) {
        v = Value::vEmptyList;
        return v;
    }
    ListVec * lv = Alloc::allocList(n);
    V3_STATS_INC(listsAllocated);
    for (uint32_t i = 0; i < n; ++i)
        lv->elems[i] = deserializeOne(r);
    listPostConstructBarrier(lv);  // Phase D batch barrier.
    v.mkList(lv);
    return v;
}

static Value deserializeAttrs(Reader & r)
{
    uint32_t n = r.u32();
    Value v;
    if (n == 0) {
        v = Value::vEmptyAttrs;
        return v;
    }
    // We need to (1) read entries in serialised (name-sorted) order,
    // (2) intern names to SymbolIds, (3) re-sort by SymbolId because
    // Bindings::entries is SymbolId-sorted (binary search invariant).
    struct Tmp { SymbolId sid; Value val; };
    std::vector<Tmp> tmp;
    tmp.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t nameLen = r.u32();
        auto nm = r.bytes(nameLen);
        SymbolId sid = ir::globalInternSymbol(std::string_view(nm.data(), nameLen));
        Value child = deserializeOne(r);
        tmp.push_back({sid, child});
    }
    std::sort(tmp.begin(), tmp.end(),
        [](const Tmp & a, const Tmp & b) { return a.sid < b.sid; });
    Bindings * b = Alloc::allocBindings(n);
    V3_STATS_INC(attrsetsAllocated);
    for (uint32_t i = 0; i < n; ++i) {
        bindingsSetEntry(b, i, {tmp[i].sid, 0, tmp[i].val});  // Phase D
    }
    v.mkAttrs(b);
    return v;
}

static Value deserializeOne(Reader & r)
{
    uint8_t tag = r.u8();
    switch (tag) {
    case kTagInt: {
        Value v;
        v.mkInt(static_cast<int64_t>(r.u64()));
        return v;
    }
    case kTagFloat: {
        uint64_t bits = r.u64();
        double d;
        std::memcpy(&d, &bits, 8);
        Value v;
        v.mkFloat(d);
        return v;
    }
    case kTagBool: {
        uint8_t b = r.u8();
        return b ? Value::vTrue : Value::vFalse;
    }
    case kTagNull:
        return Value::vNull;
    case kTagString:  return deserializeString(r);
    case kTagPath:    return deserializePath(r);
    case kTagList:    return deserializeList(r);
    case kTagAttrs:   return deserializeAttrs(r);
    default:
        throw SerializeError("unknown tag byte: 0x"
            + std::to_string(static_cast<int>(tag)));
    }
}

Value deserialize(std::string_view in)
{
    Reader r(in);
    auto m = r.bytes(4);
    if (std::memcmp(m.data(), kMagic, 4) != 0)
        throw SerializeError("bad magic — not a V3VR blob");
    uint8_t schema = r.u8();
    if (schema != kSchema)
        throw SerializeError("schema mismatch: expected "
            + std::to_string(kSchema) + ", got " + std::to_string(schema));
    Value v = deserializeOne(r);
    if (!r.atEnd())
        throw SerializeError("trailing bytes after value");
    return v;
}

// ---------------------------------------------------------------------------
// Structural equality.
// ---------------------------------------------------------------------------

bool valuesEqual(const Value & a, const Value & b) noexcept
{
    if (a.tag() != b.tag()) return false;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (a.tag()) {
    case Tag::Int:
        return a.asInt() == b.asInt();
    case Tag::Float:
        return a.floatBits() == b.floatBits();
    case Tag::Bool:
        return (a.asInt() != 0) == (b.asInt() != 0);
    case Tag::Null:
        return true;
    case Tag::String: {
        const char * sa = a.asString();
        const char * sb = b.asString();
        if (!sa || !sb) return sa == sb;
        if (std::strcmp(sa, sb) != 0) return false;
        const auto * ca = lookupStringContextEntries(sa);
        const auto * cb = lookupStringContextEntries(sb);
        size_t na = ca ? ca->size() : 0;
        size_t nb = cb ? cb->size() : 0;
        if (na != nb) return false;
        if (na == 0) return true;
        // Context vectors are unordered semantically (std::set in
        // NixStringContext) but we serialise/deserialise in insertion
        // order — the side-table stores std::vector<std::string>.
        // For determinism check, compare as multisets.
        std::vector<std::string> sortedA = *ca;
        std::vector<std::string> sortedB = *cb;
        std::sort(sortedA.begin(), sortedA.end());
        std::sort(sortedB.begin(), sortedB.end());
        return sortedA == sortedB;
    }
    case Tag::Path: {
        const char * pa = a.asPath();
        const char * pb = b.asPath();
        if (!pa || !pb) return pa == pb;
        return std::strcmp(pa, pb) == 0;
    }
    case Tag::List: {
        const ListVec * la = a.asList();
        const ListVec * lb = b.asList();
        uint32_t sa = la ? la->size : 0;
        uint32_t sb = lb ? lb->size : 0;
        if (sa != sb) return false;
        if (sa == 0) return true;
        for (uint32_t i = 0; i < sa; ++i)
            if (!valuesEqual(la->elems[i], lb->elems[i])) return false;
        return true;
    }
    case Tag::Attrs: {
        const Bindings * ba = a.asAttrs();
        const Bindings * bb = b.asAttrs();
        const bool anyChain = (ba && ba->isChain()) || (bb && bb->isChain());
        uint32_t sa = ba ? (anyChain ? ba->countDistinct() : ba->size) : 0;
        uint32_t sb = bb ? (anyChain ? bb->countDistinct() : bb->size) : 0;
        if (sa != sb) return false;
        if (sa == 0) return true;
        if (anyChain) {
            Bindings::Cursor ca(ba);
            Bindings::Cursor cb(bb);
            while (const Bindings::Entry * ea = ca.next()) {
                const Bindings::Entry * eb = cb.next();
                if (!eb || ea->name != eb->name) return false;
                if (!valuesEqual(ea->value, eb->value)) return false;
            }
            return cb.next() == nullptr;
        }
        // Bindings are SymbolId-sorted; same SymbolId space for both
        // (we deserialise via globalInternSymbol so the input names
        // map to the SAME SymbolIds as the original).  Therefore a
        // positional walk suffices.
        for (uint32_t i = 0; i < sa; ++i) {
            if (ba->entries[i].name != bb->entries[i].name) return false;
            if (!valuesEqual(ba->entries[i].value, bb->entries[i].value))
                return false;
        }
        return true;
    }
    default:
        return false;  // unsupported tag: never round-trippable
    }
#pragma clang diagnostic pop
}

// ---------------------------------------------------------------------------
// Round-trip diagnostics.
// ---------------------------------------------------------------------------

RoundTripStats & roundTripStats() noexcept
{
    static RoundTripStats s;
    return s;
}

bool testModeEnabled() noexcept
{
    // Cached once at first call; getenv is cheap but a per-derivation
    // call could add measurable noise to the 4 ms/call primop cost
    // we're profiling.
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_TEST_DRV_RESULT_SERIALIZE");
        return e && *e && *e != '0';
    }();
    return enabled;
}

void runRoundTripTest(const Value & result) noexcept
{
    if (!testModeEnabled()) return;
    auto & stats = roundTripStats();
    stats.attempts++;
    std::string buf;
    auto t0 = std::chrono::steady_clock::now();
    try {
        serialize(result, buf);
    } catch (const std::exception &) {
        stats.serErrors++;
        return;
    } catch (...) {
        stats.serErrors++;
        return;
    }
    auto t1 = std::chrono::steady_clock::now();
    Value restored;
    try {
        restored = deserialize(buf);
    } catch (const std::exception &) {
        stats.deserErrors++;
        // still count bytes + ser time
        stats.totalBytes += buf.size();
        stats.totalSerNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return;
    } catch (...) {
        stats.deserErrors++;
        stats.totalBytes += buf.size();
        stats.totalSerNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return;
    }
    auto t2 = std::chrono::steady_clock::now();
    bool ok = valuesEqual(result, restored);
    auto t3 = std::chrono::steady_clock::now();
    if (ok) stats.successes++;
    else    stats.mismatches++;
    stats.totalBytes     += buf.size();
    stats.totalSerNs     += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    stats.totalDeserNs   += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
    stats.totalCompareNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
}

// ---------------------------------------------------------------------------
// #741 Phase 2 — canonical Value hash.
// ---------------------------------------------------------------------------

void canonicalHash(const Value & v, uint8_t out[32])
{
    // Reuse the Phase 1 serialiser: it already emits attr names sorted
    // by name-string and context entries in the order they were stored
    // (which v3's encodeStringContext keeps sorted via the std::set
    // backing NixStringContext).  SHA-256 over those bytes is therefore
    // a process-independent function of the Value's structural content.
    std::string buf;
    serialize(v, buf);
    nix::Hash h = nix::hashString(nix::HashAlgorithm::SHA256, buf);
    // Defensive: HashAlgorithm::SHA256 implies hashSize == 32 per
    // libutil/hash.hh's regularHashSize().  Memcpy is safe.
    std::memcpy(out, h.hash, 32);
}

std::string canonicalHashHex(const Value & v)
{
    uint8_t bytes[32];
    canonicalHash(v, bytes);
    static const char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = kHex[(bytes[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[ bytes[i]       & 0xF];
    }
    return out;
}

bool canonicalHashTestModeEnabled() noexcept
{
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_TEST_CANONICAL_HASH");
        return e && *e && *e != '0';
    }();
    return enabled;
}

void dumpCanonicalHashLine(const Value & v) noexcept
{
    if (!canonicalHashTestModeEnabled()) return;
    try {
        std::string hex = canonicalHashHex(v);
        // Single line per derivation result.  Two processes' sorted
        // dumps must diff to empty for the falsifier to pass.
        std::fprintf(stderr, "V3-VAL-HASH: %s\n", hex.c_str());
    } catch (const std::exception & e) {
        std::fprintf(stderr, "V3-VAL-HASH-ERR: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "V3-VAL-HASH-ERR: unknown\n");
    }
}

// ---------------------------------------------------------------------------
// #741 Phase 3a — in-memory SHADOW eval-result cache.
// ---------------------------------------------------------------------------

namespace {

bool evalResultCacheEnabledCached()
{
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_EVAL_RESULT_CACHE");
        return e && *e && *e != '0';
    }();
    return enabled;
}

// Process-local cache.  Keyed on 64-char hex string (canonical SHA-256
// hex form), value is the serialised result blob.
std::unordered_map<std::string, std::string> & evalResultCacheMap()
{
    static std::unordered_map<std::string, std::string> m;
    return m;
}

} // anonymous namespace

EvalResultCacheStats & evalResultCacheStats() noexcept
{
    static EvalResultCacheStats s;
    return s;
}

bool evalResultCacheEnabled() noexcept { return evalResultCacheEnabledCached(); }

// #885 (2026-05-29) — PRODUCTION mode gate.  Cached once at first
// read; lifetime-of-process.  When NIX_V3_EVAL_RESULT_CACHE_PRODUCTION
// is set AND NIX_V3_EVAL_RESULT_CACHE is set, primDerivationStrict
// / primDerivationFromPreprocessed short-circuit on cache hit (skip
// body).  Independent of NIX_V3_EVAL_RESULT_CACHE itself so callers
// can A/B by toggling production alone with the shadow infrastructure
// intact.  Mirrors `drvHashCacheActiveEnabled` for the Phase 3e
// mid-body cache.
bool evalResultCacheProductionEnabled() noexcept
{
    static const bool enabled = []() {
        if (!evalResultCacheEnabledCached()) return false;
        const char * e = std::getenv("NIX_V3_EVAL_RESULT_CACHE_PRODUCTION");
        return e && *e && *e != '0';
    }();
    return enabled;
}

bool evalResultCacheLookup(const Value & input,
                            Value & outResult,
                            std::string & outKey) noexcept
{
    outKey.clear();
    if (!evalResultCacheEnabled()) return false;

    auto & stats = evalResultCacheStats();
    ++stats.lookups;

    // Compute hash.  Phase 3b: serializeOne chases Thunk/App/Slot
    // indirections to their WHNF target when state == Evaluated, so
    // already-evaluated entries hash transparently.  Suspended thunks
    // and un-evaluated Apps still throw — those calls increment
    // hashErrors and bypass the cache.
    auto t0 = std::chrono::steady_clock::now();
    try {
        outKey = canonicalHashHex(input);
    } catch (const SerializeError & e) {
        ++stats.hashErrors;
        // Phase 3b diagnostic: per-cause bucket via NIX_V3_DBG_CACHE_HASH_ERR=1.
        // Reveals whether hashErrors are dominated by un-evaluated
        // Thunks, App memoisation gaps, Closure entries, etc.
        static const bool dbg = std::getenv("NIX_V3_DBG_CACHE_HASH_ERR") != nullptr;
        if (dbg) {
            static thread_local uint64_t errCount = 0;
            if (errCount++ < 16)
                std::fprintf(stderr, "v3-cache hashErr#%llu: %s\n",
                    (unsigned long long)errCount, e.what());
        }
        outKey.clear();
        return false;
    } catch (...) {
        ++stats.hashErrors;
        outKey.clear();
        return false;
    }
    auto t1 = std::chrono::steady_clock::now();
    stats.totalHashNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    auto & cache = evalResultCacheMap();
    auto it = cache.find(outKey);
    auto t2 = std::chrono::steady_clock::now();
    stats.totalLookupNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

    if (it == cache.end()) {
        ++stats.misses;
        return false;
    }

    // Hit — deserialise.  Note: in SHADOW mode the caller IGNORES
    // outResult (always recomputes from scratch), but we deserialise
    // anyway so the verify-cached-against-computed comparison runs.
    try {
        outResult = deserialize(it->second);
    } catch (...) {
        ++stats.deserErrors;
        return false;
    }
    auto t3 = std::chrono::steady_clock::now();
    stats.totalDeserNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
    stats.bytesDelivered += it->second.size();
    ++stats.hits;
    return true;
}

void evalResultCacheInsert(const std::string & key, const Value & result) noexcept
{
    if (!evalResultCacheEnabled()) return;
    if (key.empty()) return;
    auto & stats = evalResultCacheStats();
    std::string blob;
    auto t0 = std::chrono::steady_clock::now();
    try {
        serialize(result, blob);
    } catch (...) {
        return;
    }
    auto t1 = std::chrono::steady_clock::now();
    stats.totalSerNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    stats.bytesCached += blob.size();
    evalResultCacheMap().emplace(key, std::move(blob));
    ++stats.inserts;
}

void dumpEvalResultCacheStats(std::FILE * out)
{
    if (!evalResultCacheEnabled()) return;
    const auto & s = evalResultCacheStats();
    if (s.lookups == 0) return;
    auto safe = [](uint64_t n) -> uint64_t { return n ? n : 1; };
    double avgHashUs   = (s.totalHashNs   / 1000.0) / safe(s.lookups);
    double avgLookupUs = (s.totalLookupNs / 1000.0) / safe(s.lookups);
    double avgDeserUs  = (s.totalDeserNs  / 1000.0) / safe(s.hits);
    double avgSerUs    = (s.totalSerNs    / 1000.0) / safe(s.inserts);
    double hitRate     = 100.0 * static_cast<double>(s.hits) / safe(s.lookups);
    const char * mode = evalResultCacheProductionEnabled() ? "PRODUCTION" : "SHADOW";
    std::fprintf(out,
        "v3-direct eval-result-cache (%s): lookups=%llu hits=%llu misses=%llu "
        "inserts=%llu mismatch=%llu hit_rate=%.1f%% activeSkips=%llu\n"
        "  avg: hash=%.2f us lookup=%.2f us deser-on-hit=%.2f us ser-on-insert=%.2f us\n"
        "  bytes: cached=%.2f MB delivered=%.2f MB\n"
        "  errors: hash=%llu deser=%llu\n",
        mode,
        (unsigned long long)s.lookups, (unsigned long long)s.hits,
        (unsigned long long)s.misses, (unsigned long long)s.inserts,
        (unsigned long long)s.mismatchHits,
        hitRate,
        (unsigned long long)s.activeSkips,
        avgHashUs, avgLookupUs, avgDeserUs, avgSerUs,
        s.bytesCached / (1024.0 * 1024.0),
        s.bytesDelivered / (1024.0 * 1024.0),
        (unsigned long long)s.hashErrors,
        (unsigned long long)s.deserErrors);
}

// ---------------------------------------------------------------------------
// #741 Phase 3e — mid-body drv-hash SHADOW cache.
// ---------------------------------------------------------------------------

namespace {

bool drvHashCacheEnabledCached()
{
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_DRV_HASH_CACHE");
        return e && *e && *e != '0';
    }();
    return enabled;
}

std::unordered_map<std::string, std::string> & drvHashCacheMap()
{
    static std::unordered_map<std::string, std::string> m;
    return m;
}

} // anonymous

DrvHashCacheStats & drvHashCacheStats() noexcept
{
    static DrvHashCacheStats s;
    return s;
}

bool drvHashCacheEnabled() noexcept { return drvHashCacheEnabledCached(); }

bool drvHashCacheActiveEnabled() noexcept
{
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_DRV_HASH_CACHE_ACTIVE");
        return e && *e && *e != '0';
    }();
    return enabled;
}

bool drvHashCacheDiskEnabled() noexcept
{
    static const bool enabled = []() {
        const char * e = std::getenv("NIX_V3_DRV_HASH_CACHE_DISK");
        return e && *e && *e != '0';
    }();
    return enabled;
}

bool drvHashCacheLookup(const std::string & key, Value & outResult) noexcept
{
    if (!drvHashCacheEnabled()
        && !drvHashCacheActiveEnabled()
        && !drvHashCacheDiskEnabled()) return false;
    if (key.empty()) return false;
    auto & stats = drvHashCacheStats();
    ++stats.lookups;
    auto t0 = std::chrono::steady_clock::now();
    auto & cache = drvHashCacheMap();
    auto it = cache.find(key);
    auto t1 = std::chrono::steady_clock::now();
    stats.totalLookupNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (it != cache.end()) {
        // In-memory hit.
        try {
            outResult = deserialize(it->second);
        } catch (...) {
            ++stats.deserErrors;
            return false;
        }
        auto t2 = std::chrono::steady_clock::now();
        stats.totalDeserNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        stats.bytesDelivered += it->second.size();
        ++stats.hits;
        return true;
    }

    // #741 Phase 5: in-memory miss — try the disk cache.  On disk
    // hit, promote into the in-memory map so subsequent in-process
    // lookups for the same drvPath are fast.
    if (drvHashCacheDiskEnabled()) {
        CACHE_HOOK_DEFINE_SITE(siteDrvHashDisk,
            "drvHashCache-disk-lookup");
        CacheHookTimer dt(siteDrvHashDisk);
        auto diskKey = disk_cache::computeKeyForString(key);
        auto diskBlob = disk_cache::lookupEvalResult(diskKey);
        if (diskBlob) {
            cacheHookHit(siteDrvHashDisk);
            try {
                outResult = deserialize(*diskBlob);
            } catch (...) {
                ++stats.deserErrors;
                ++stats.misses;
                return false;
            }
            auto t2 = std::chrono::steady_clock::now();
            stats.totalDeserNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            stats.bytesDelivered += diskBlob->size();
            // Promote into in-memory for fast subsequent hits.
            cache.emplace(key, std::move(*diskBlob));
            ++stats.hits;
            return true;
        }
        cacheHookMiss(siteDrvHashDisk);
    }

    ++stats.misses;
    return false;
}

void drvHashCacheInsert(const std::string & key, const Value & result) noexcept
{
    if (!drvHashCacheEnabled()
        && !drvHashCacheActiveEnabled()
        && !drvHashCacheDiskEnabled()) return;
    if (key.empty()) return;
    auto & stats = drvHashCacheStats();
    std::string blob;
    auto t0 = std::chrono::steady_clock::now();
    try {
        serialize(result, blob);
    } catch (...) {
        return;
    }
    auto t1 = std::chrono::steady_clock::now();
    stats.totalSerNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    stats.bytesCached += blob.size();
    // #741 Phase 5: persist to disk first (best-effort), then move
    // into in-memory.  string_view doesn't consume blob.
    if (drvHashCacheDiskEnabled()) {
        CACHE_HOOK_DEFINE_SITE(siteDrvHashDiskInsert,
            "drvHashCache-disk-insert");
        CacheHookTimer it(siteDrvHashDiskInsert);
        auto diskKey = disk_cache::computeKeyForString(key);
        disk_cache::insertEvalResult(diskKey, blob);
        cacheHookInsert(siteDrvHashDiskInsert, blob.size());
    }
    drvHashCacheMap().emplace(key, std::move(blob));
    ++stats.inserts;
}

void dumpDrvHashCacheStats(std::FILE * out)
{
    if (!drvHashCacheEnabled()
        && !drvHashCacheActiveEnabled()
        && !drvHashCacheDiskEnabled()) return;
    const auto & s = drvHashCacheStats();
    if (s.lookups == 0) return;
    auto safe = [](uint64_t n) -> uint64_t { return n ? n : 1; };
    double avgLookupUs = (s.totalLookupNs / 1000.0) / safe(s.lookups);
    double avgDeserUs  = (s.totalDeserNs  / 1000.0) / safe(s.hits);
    double avgSerUs    = (s.totalSerNs    / 1000.0) / safe(s.inserts);
    double hitRate     = 100.0 * static_cast<double>(s.hits) / safe(s.lookups);
    const char * mode = drvHashCacheActiveEnabled() ? "ACTIVE" : "SHADOW";
    const char * disk = drvHashCacheDiskEnabled() ? "+DISK" : "";
    std::fprintf(out,
        "v3-direct drv-hash-cache (%s%s): lookups=%llu hits=%llu misses=%llu "
        "inserts=%llu mismatch=%llu activeSkips=%llu hit_rate=%.1f%%\n"
        "  avg: lookup=%.2f us deser-on-hit=%.2f us ser-on-insert=%.2f us\n"
        "  bytes: cached=%.2f MB delivered=%.2f MB\n"
        "  errors: deser=%llu\n",
        mode, disk,
        (unsigned long long)s.lookups, (unsigned long long)s.hits,
        (unsigned long long)s.misses, (unsigned long long)s.inserts,
        (unsigned long long)s.mismatchHits,
        (unsigned long long)s.activeSkips,
        hitRate, avgLookupUs, avgDeserUs, avgSerUs,
        s.bytesCached / (1024.0 * 1024.0),
        s.bytesDelivered / (1024.0 * 1024.0),
        (unsigned long long)s.deserErrors);
    // Show disk-side stats from disk_cache namespace when DISK is on.
    if (drvHashCacheDiskEnabled()) {
        const auto & ds = disk_cache::stats();
        std::fprintf(out,
            "  disk: evalLookups=%llu evalHits=%llu evalMisses=%llu "
            "evalInserts=%llu evalInsertFailures=%llu\n",
            (unsigned long long)ds.evalLookups,
            (unsigned long long)ds.evalHits,
            (unsigned long long)ds.evalMisses,
            (unsigned long long)ds.evalInserts,
            (unsigned long long)ds.evalInsertFailures);
    }
}

void dumpStats(std::FILE * out)
{
    if (!testModeEnabled()) return;
    const auto & s = roundTripStats();
    // Suppress 0-attempt dumps: sub-evals (builtins / derivationStrict /
    // wrapper script compilation) call dumpStats() before any
    // derivation has been constructed.  Only the main eval's
    // non-zero summary is useful.
    if (s.attempts == 0) return;
    double avgBytes   = static_cast<double>(s.totalBytes) / s.attempts;
    double avgSerUs   = (s.totalSerNs    / 1000.0) / s.attempts;
    double avgDeserUs = (s.totalDeserNs  / 1000.0) / s.attempts;
    double avgCmpUs   = (s.totalCompareNs/ 1000.0) / s.attempts;
    double avgTotalUs = avgSerUs + avgDeserUs + avgCmpUs;
    std::fprintf(out,
        "v3-direct value-serialize: attempts=%llu success=%llu mismatch=%llu "
        "serErr=%llu deserErr=%llu\n"
        "  avg blob=%.0f B  ser=%.2f µs  deser=%.2f µs  cmp=%.2f µs  total=%.2f µs\n"
        "  total bytes=%.2f MB\n",
        (unsigned long long)s.attempts,
        (unsigned long long)s.successes,
        (unsigned long long)s.mismatches,
        (unsigned long long)s.serErrors,
        (unsigned long long)s.deserErrors,
        avgBytes, avgSerUs, avgDeserUs, avgCmpUs, avgTotalUs,
        s.totalBytes / (1024.0 * 1024.0));
}

} // namespace nix::v3::value_serialize
