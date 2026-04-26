/// @file
/// CompilationUnit serialization (Phase 3.2-2).
///
/// Hand-rolled binary format with a magic prefix + schema version.
/// All process-bound references (Symbol IDs, PosIdx values, AST Expr*,
/// Boehm-allocated Value*) are normalized to either content (strings,
/// leaf payloads) or DROPPED entirely.
///
/// First-pass scope: code buffer, symbols-as-strings, leaf constants,
/// thunk/lambda descriptors (sans formals, sans sourceExpr, sans
/// cachedExpr), attrCache symbol-index list (PIC entries zeroed on
/// load).  Positions are dropped for now — error messages from cached
/// CUs degrade to line 0:0 but evaluation is correct.  Position
/// relocation is Phase 3.2-3.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode-serialize.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/expr/value.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/util/hash.hh"

#include <cstring>
#include <vector>

namespace nix::bytecode {

namespace {

// ---------------------------------------------------------------------------
// Constant-payload kind tags.  Independent of InternalType so the
// schema can survive InternalType reorderings.
// ---------------------------------------------------------------------------
enum class ConstKind : uint8_t {
    Int     = 1,
    BoolFalse = 2,
    BoolTrue  = 3,
    Null    = 4,
    Float   = 5,
    String  = 6,  // string with no context
    StringWithContext = 7,
    Path    = 8,
    PrimOpByName = 9,
    Failed  = 10, // sentinel, no payload
};

// ---------------------------------------------------------------------------
// Writer — pure append, no seeking.
// ---------------------------------------------------------------------------
struct Writer
{
    std::string buf;

    void put(const void * src, size_t n)
    {
        buf.append(static_cast<const char *>(src), n);
    }

    template<typename T>
    void putPod(const T & v)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        put(&v, sizeof(T));
    }

    void putU8(uint8_t v) { putPod(v); }
    void putU32(uint32_t v) { putPod(v); }
    void putU64(uint64_t v) { putPod(v); }

    void putString(std::string_view s)
    {
        putU32(static_cast<uint32_t>(s.size()));
        put(s.data(), s.size());
    }
};

// ---------------------------------------------------------------------------
// Reader — bounds-checked sequential read.
// ---------------------------------------------------------------------------
struct Reader
{
    const char * cur;
    const char * end;

    Reader(std::string_view sv)
        : cur(sv.data()), end(sv.data() + sv.size()) {}

    void need(size_t n) const
    {
        if (cur + n > end)
            throw SerializationError("bytecode-serialize: truncated input");
    }

    void get(void * dst, size_t n)
    {
        need(n);
        std::memcpy(dst, cur, n);
        cur += n;
    }

    template<typename T>
    T getPod()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        T out;
        get(&out, sizeof(T));
        return out;
    }

    uint8_t  getU8() { return getPod<uint8_t>(); }
    uint32_t getU32() { return getPod<uint32_t>(); }
    uint64_t getU64() { return getPod<uint64_t>(); }

    std::string_view getString()
    {
        uint32_t len = getU32();
        need(len);
        std::string_view sv(cur, len);
        cur += len;
        return sv;
    }
};

// ---------------------------------------------------------------------------
// Constant serialization — leaf types only.  Caller has already verified
// cacheabilityCheck(unit) != NonLeafConstant, so these branches cover
// every valid input.
// ---------------------------------------------------------------------------
void writeConstant(Writer & w, const Value * v)
{
    if (!v) {
        // Defensive: empty slot in constants pool.  Encode as Null.
        w.putU8(static_cast<uint8_t>(ConstKind::Null));
        return;
    }
    switch (v->type()) {
        case nInt:
            w.putU8(static_cast<uint8_t>(ConstKind::Int));
            w.putU64(static_cast<uint64_t>(v->integer().value));
            break;
        case nBool:
            w.putU8(v->boolean()
                ? static_cast<uint8_t>(ConstKind::BoolTrue)
                : static_cast<uint8_t>(ConstKind::BoolFalse));
            break;
        case nNull:
            w.putU8(static_cast<uint8_t>(ConstKind::Null));
            break;
        case nFloat: {
            w.putU8(static_cast<uint8_t>(ConstKind::Float));
            double d = v->fpoint();
            w.putPod(d);
            break;
        }
        case nString: {
            // Strings may carry a context.  Encode the bytes; if there
            // is a context, encode it as a list of context strings.
            const Value::StringWithContext::Context * ctx = v->context();
            if (ctx && ctx->size() > 0) {
                w.putU8(static_cast<uint8_t>(ConstKind::StringWithContext));
                w.putString(v->string_view());
                w.putU32(static_cast<uint32_t>(ctx->size()));
                for (auto * sd : *ctx)
                    w.putString(sd->view());
            } else {
                w.putU8(static_cast<uint8_t>(ConstKind::String));
                w.putString(v->string_view());
            }
            break;
        }
        case nPath: {
            w.putU8(static_cast<uint8_t>(ConstKind::Path));
            // Encode the canonical path string.  The accessor is
            // re-resolved on load against the loading EvalState's rootFS.
            auto sp = v->path();
            w.putString(sp.path.abs());
            break;
        }
        case nFunction:
            // Only PrimOps reach here (cacheabilityCheck rejected
            // lambdas).  Serialize by name; resolve on load.
            w.putU8(static_cast<uint8_t>(ConstKind::PrimOpByName));
            w.putString(v->primOp()->name);
            break;
        case nFailed:
            w.putU8(static_cast<uint8_t>(ConstKind::Failed));
            break;
        case nThunk:
        case nAttrs:
        case nList:
        case nExternal:
            // cacheabilityCheck should have rejected these.  Defensive abort.
            throw SerializationError("bytecode-serialize: non-leaf constant slipped past cacheability check");
    }
}

Value * readConstant(Reader & r, EvalState & state)
{
    auto kind = static_cast<ConstKind>(r.getU8());
    auto * v = state.allocValue();
    switch (kind) {
        case ConstKind::Int: {
            int64_t n = static_cast<int64_t>(r.getU64());
            v->mkInt(static_cast<NixInt::Inner>(n));
            return v;
        }
        case ConstKind::BoolFalse:
            v->mkBool(false);
            return v;
        case ConstKind::BoolTrue:
            v->mkBool(true);
            return v;
        case ConstKind::Null:
            v->mkNull();
            return v;
        case ConstKind::Float: {
            double d = r.getPod<double>();
            v->mkFloat(d);
            return v;
        }
        case ConstKind::String: {
            auto sv = r.getString();
            v->mkString(sv, state.mem);
            return v;
        }
        case ConstKind::StringWithContext: {
            auto sv = r.getString();
            uint32_t nCtx = r.getU32();
            // Build a context: array of c-string pointers terminated by null.
            // For simplicity, re-allocate each context entry into the eval
            // memory and assemble.
            std::vector<std::string> ctxStrings;
            ctxStrings.reserve(nCtx);
            for (uint32_t i = 0; i < nCtx; i++)
                ctxStrings.emplace_back(r.getString());
            // mkString with context: build a NixStringContext and convert.
            NixStringContext nsc;
            for (auto & s : ctxStrings)
                nsc.insert(NixStringContextElem::parse(s));
            v->mkString(sv, nsc, state.mem);
            return v;
        }
        case ConstKind::Path: {
            auto pathStr = r.getString();
            // Re-resolve through the loading state's rootFS.
            SourcePath sp(state.rootFS,
                          CanonPath(CanonPath::unchecked_t(), std::string(pathStr)));
            v->mkPath(sp, state.mem);
            return v;
        }
        case ConstKind::PrimOpByName: {
            auto name = r.getString();
            // Try internalPrimOps first (covers __ops with the prefix
            // stripped), then fall back to the public `builtins`
            // attrset for regular primops like `mul` / `sub` / etc.
            auto it = state.internalPrimOps.find(std::string(name));
            if (it != state.internalPrimOps.end()) {
                *v = *it->second;
                return v;
            }
            try {
                Value & b = state.getBuiltin(std::string(name));
                *v = b;
                return v;
            } catch (...) {
                throw SerializationError(
                    "bytecode-serialize: cached primop '" + std::string(name)
                    + "' not resolvable in this EvalState");
            }
        }
        case ConstKind::Failed:
            // Should never appear in a fresh CU; defensive Null.
            v->mkNull();
            return v;
    }
    throw SerializationError("bytecode-serialize: unknown constant kind");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------------

std::string serializeCU(const CompilationUnit & unit, const EvalState & state)
{
    auto reason = cacheabilityCheck(unit);
    if (reason != UncacheableReason::Cacheable)
        throw SerializationError(
            std::string("bytecode-serialize: refuse to serialize uncacheable CU (")
            + uncacheableReasonName(reason) + ")");

    Writer w;
    // Magic + schema version + reserved flags word.
    w.put(kBytecodeMagic, sizeof(kBytecodeMagic));
    w.putU32(kBytecodeSerializeSchemaVersion);
    w.putU32(0);  // flags reserved

    // Code buffer: just the raw uint32_t stream.
    w.putU32(static_cast<uint32_t>(unit.code.size()));
    if (!unit.code.empty())
        w.put(unit.code.data(), unit.code.size() * sizeof(Instruction));

    // Symbols as strings.
    w.putU32(static_cast<uint32_t>(unit.symbols.size()));
    for (auto & sym : unit.symbols) {
        std::string_view name = state.symbols[sym];
        w.putString(name);
    }

    // Constants.
    w.putU32(static_cast<uint32_t>(unit.constants.size()));
    for (auto * v : unit.constants)
        writeConstant(w, v);

    // Thunk descriptors.
    w.putU32(static_cast<uint32_t>(unit.thunks.size()));
    for (auto & td : unit.thunks) {
        w.putU32(td.codeOffset);
        // Drop pos for now (Phase 3.2-3 will add origin+offset).
        w.putU32(static_cast<uint32_t>(td.nUpvalues));
        w.putU32(static_cast<uint32_t>(td.maxSlot));
        // sourceExpr, cachedExpr dropped — re-allocate lazily on load.
    }

    // Lambda descriptors.
    w.putU32(static_cast<uint32_t>(unit.lambdas.size()));
    for (auto & ld : unit.lambdas) {
        w.putU32(ld.codeOffset);
        w.putU32(ld.prologueOffset);
        w.putU32(ld.bodyThunkIdx);
        // Symbols name/arg as pool indices (or sentinel UINT32_MAX
        // if empty).  Caller has already added all referenced symbols
        // via addSymbol; we serialize their pool indices.
        //
        // If a non-empty Symbol is missing from the index, that's a
        // compile-time bug — emit-time forgot to register the symbol.
        // Throwing here surfaces it immediately rather than silently
        // writing UINT32_MAX (which becomes an empty Symbol on load
        // and produces "attribute X missing" later).
        auto symIdxOrSentinel = [&](Symbol s) -> uint32_t {
            if (!s) return UINT32_MAX;
            auto it = unit.symbolIndex.find(s);
            if (it == unit.symbolIndex.end())
                throw SerializationError(
                    "bytecode-serialize: LambdaDescriptor symbol '"
                    + std::string(state.symbols[s])
                    + "' not in unit.symbolIndex (forgot addSymbol at emit time?)");
            return it->second;
        };
        w.putU32(symIdxOrSentinel(ld.name));
        w.putU32(symIdxOrSentinel(ld.arg));
        w.putU32(static_cast<uint32_t>(ld.envSize));
        w.putU32(static_cast<uint32_t>(ld.nUpvalues));
        // cacheabilityCheck rejected lambdas with formals; assert null.
        assert(ld.formals == nullptr);

        // Phase 3.2-2b (issue #159 fix): persist the formals signature.
        // Without this, `builtins.functionArgs` returns `{}` for cached
        // formals lambdas, which makes `lib.callPackageWith` invoke
        // them with `{}` and the prologue then fails with "attribute X
        // missing".  We serialize ONLY (name, hasDefault) per formal —
        // sufficient for functionArgs / intersectAttrs; the bytecode
        // prologue handles the actual default-thunk binding at runtime.
        w.putU8(ld.sourceHasFormals ? 1 : 0);
        if (ld.sourceHasFormals) {
            w.putU8(ld.sourceFormalsEllipsis ? 1 : 0);
            w.putU32(static_cast<uint32_t>(ld.sourceFormals.size()));
            for (auto & [symIdx, hasDef] : ld.sourceFormals) {
                w.putU32(symIdx);
                w.putU8(hasDef ? 1 : 0);
            }
        }
    }

    // AttrCaches: only the symbol pool index is stable; PIC entries
    // are populated at runtime.  Same hardening as LambdaDescriptor:
    // a missing symbol means the emitter forgot to addSymbol (a
    // compile-time bug).  addAttrCache itself calls addSymbol, so
    // this should always succeed.
    w.putU32(static_cast<uint32_t>(unit.attrCaches.size()));
    for (auto & ac : unit.attrCaches) {
        auto it = unit.symbolIndex.find(ac.name);
        if (it == unit.symbolIndex.end())
            throw SerializationError(
                "bytecode-serialize: AttrCache.name '"
                + std::string(state.symbols[ac.name])
                + "' not in unit.symbolIndex");
        w.putU32(it->second);
    }

    // Phase 3.2-3a: positions table.  Each PosIdx in unit.positions
    // and unit.posPool is an index into the EvalState's global
    // PosTable.  We extract the SourcePath origin once (we assume one
    // origin per CU since each CU is compiled from a single source
    // file) plus a list of (instrOffset, byteOffsetWithinOrigin)
    // entries.  On load we re-add the origin to the new state's
    // PosTable and translate offsets back to PosIdx values.
    //
    // If any entry references an origin that ISN'T a SourcePath
    // (e.g. Stdin or String origins from REPL/string-eval inputs),
    // we degrade gracefully: the entry is dropped (no source
    // position on hit-cache errors) but the rest of the table is
    // still serialized.  The CU itself was already deemed cacheable
    // by isCacheable; a non-SourcePath position table is just
    // missing fidelity, not a correctness issue.
    {
        // Collect origin (first non-noPos position dictates).  Also
        // build a flat list of (instrOffset, byteOffset) entries.
        std::vector<std::pair<uint32_t, uint32_t>> entries;
        std::optional<SourcePath> origin;
        size_t originSize = 0;
        for (auto & pe : unit.positions) {
            if (pe.pos == noPos) continue;
            auto thisOrigin = state.positions.originOf(pe.pos);
            auto * sp = std::get_if<SourcePath>(&thisOrigin);
            if (!sp) continue;
            if (!origin) {
                origin = *sp;
                // We don't know the origin size here; use a generous
                // upper bound (UINT32_MAX-ish).  The PosTable.add()
                // call on load will reject offsets > size, so we'd
                // have to be careful — but for our use the offsets
                // are inherently bounded by the source file size,
                // which fits in uint32 for any sensible input.
                originSize = std::numeric_limits<uint32_t>::max();
            }
            // Drop the entry: proper PosIdx round-trip requires the
            // PosTable::Origin record to compute byte offsets, but
            // PosTable::originOf returns the Pos::Origin variant only.
            // Future work will extend the public API.
            (void)pe;
        }
        if (origin) {
            // Serialize origin path for forward-compat even though
            // entries are empty.  On load we addOrigin but don't
            // try to fill in PosIdx values.
            std::string p = origin->path.abs();
            w.putU32(static_cast<uint32_t>(p.size()));
            w.put(p.data(), p.size());
            w.putU32(static_cast<uint32_t>(originSize > UINT32_MAX
                                          ? UINT32_MAX : originSize));
        } else {
            w.putU32(0);  // no origin
            w.putU32(0);
        }
        // Position entries: drop until proper PosIdx round-trip lands.
        w.putU32(0);
    }

    return std::move(w.buf);
}

CompilationUnit * deserializeCU(std::string_view blob, EvalState & state)
{
    Reader r(blob);

    // Magic + version.
    char magic[8];
    r.get(magic, sizeof(magic));
    if (std::memcmp(magic, kBytecodeMagic, sizeof(magic)) != 0)
        throw SerializationError("bytecode-serialize: magic mismatch");
    uint32_t version = r.getU32();
    if (version != kBytecodeSerializeSchemaVersion)
        throw SerializationError("bytecode-serialize: schema version mismatch");
    (void) r.getU32();  // flags reserved

    auto * unit = new (GC) CompilationUnit();

    // Code.
    uint32_t codeLen = r.getU32();
    unit->code.resize(codeLen);
    if (codeLen > 0)
        r.get(unit->code.data(), codeLen * sizeof(Instruction));

    // Symbols: re-intern strings.
    uint32_t nSymbols = r.getU32();
    unit->symbols.reserve(nSymbols);
    for (uint32_t i = 0; i < nSymbols; i++) {
        auto name = r.getString();
        Symbol sym = state.symbols.create(name);
        unit->symbols.push_back(sym);
        unit->symbolIndex.emplace(sym, i);
    }

    // Constants.
    uint32_t nConstants = r.getU32();
    unit->constants.reserve(nConstants);
    for (uint32_t i = 0; i < nConstants; i++)
        unit->constants.push_back(readConstant(r, state));

    // Thunks.
    uint32_t nThunks = r.getU32();
    unit->thunks.reserve(nThunks);
    for (uint32_t i = 0; i < nThunks; i++) {
        ThunkDescriptor td;
        td.codeOffset = r.getU32();
        td.nUpvalues  = static_cast<uint16_t>(r.getU32());
        td.maxSlot    = static_cast<uint16_t>(r.getU32());
        td.pos        = noPos;          // dropped
        td.sourceExpr = nullptr;        // dropped
        td.cachedExpr = nullptr;        // lazy
        unit->thunks.push_back(td);
    }

    // Lambdas.
    uint32_t nLambdas = r.getU32();
    unit->lambdas.reserve(nLambdas);
    for (uint32_t i = 0; i < nLambdas; i++) {
        LambdaDescriptor ld;
        ld.codeOffset     = r.getU32();
        ld.prologueOffset = r.getU32();
        ld.bodyThunkIdx   = r.getU32();
        uint32_t nameIdx  = r.getU32();
        uint32_t argIdx   = r.getU32();
        ld.name = (nameIdx == UINT32_MAX) ? Symbol{} : unit->symbols.at(nameIdx);
        ld.arg  = (argIdx  == UINT32_MAX) ? Symbol{} : unit->symbols.at(argIdx);
        ld.envSize    = static_cast<uint16_t>(r.getU32());
        ld.nUpvalues  = static_cast<uint16_t>(r.getU32());
        ld.formals    = nullptr;
        ld.sourceExpr = nullptr;
        ld.cachedExpr = nullptr;
        ld.pos        = noPos;

        // Phase 3.2-2b: formals signature.  See serializer note for
        // rationale (issue #159).  Reads (name, hasDefault) per formal
        // so `getFormals()` / `functionArgs` answer correctly.
        ld.sourceHasFormals = (r.getU8() != 0);
        if (ld.sourceHasFormals) {
            ld.sourceFormalsEllipsis = (r.getU8() != 0);
            uint32_t nFormals = r.getU32();
            ld.sourceFormals.reserve(nFormals);
            for (uint32_t k = 0; k < nFormals; ++k) {
                uint32_t symIdx = r.getU32();
                bool hasDef = (r.getU8() != 0);
                ld.sourceFormals.emplace_back(symIdx, hasDef);
            }
        }
        unit->lambdas.push_back(ld);
    }

    // AttrCaches: rebuild with fresh PIC entries.
    uint32_t nAttrCaches = r.getU32();
    unit->attrCaches.reserve(nAttrCaches);
    for (uint32_t i = 0; i < nAttrCaches; i++) {
        AttrCache ac;
        uint32_t symIdx = r.getU32();
        if (symIdx != UINT32_MAX)
            ac.name = unit->symbols.at(symIdx);
        // ac.entries already zero-initialized.
        unit->attrCaches.push_back(ac);
    }

    // Phase 3.2-3a: position origin + entries.
    // Read the origin path (may be empty for non-SourcePath origins).
    uint32_t originPathLen = r.getU32();
    std::string originPath;
    if (originPathLen > 0) {
        r.need(originPathLen);
        originPath.assign(r.cur, originPathLen);
        r.cur += originPathLen;
    }
    uint32_t originSize = r.getU32();
    (void)originPath;
    (void)originSize;
    // Entries: currently always 0 (PosIdx round-trip pending).
    uint32_t nPositions = r.getU32();
    if (nPositions != 0)
        throw SerializationError(
            "bytecode-serialize: positions table populated but reader "
            "doesn't yet support PosIdx relocation (Phase 3.2-3a)");

    if (r.cur != r.end)
        throw SerializationError("bytecode-serialize: trailing bytes");

    return unit;
}

// ---------------------------------------------------------------------------
// Phase 3.2-5: Cache key derivation
// ---------------------------------------------------------------------------

CacheKey computeCacheKey(const SourcePath & sp, uint32_t optimizationFlags)
{
    CacheKey key{Hash(HashAlgorithm::SHA256)};

    // Try to extract a stable fingerprint from the source.  We need a
    // physical path + stat info to construct a key tied to file
    // identity.  If the source isn't a regular file (e.g. an
    // in-memory accessor), return the empty key — the caller will
    // skip caching.
    std::string canonical;
    try {
        canonical = sp.path.abs();
    } catch (...) {
        return key;
    }

    // stat for mtime/size/dev/ino; non-existent files are uncacheable.
    std::optional<SourceAccessor::Stat> st;
    try {
        st = sp.maybeLstat();
    } catch (...) {
        return key;
    }
    if (!st) return key;
    if (st->type != SourceAccessor::Type::tRegular) return key;

    // Build the fingerprint string: schema + flags + path + size + (mtime if present).
    // We deliberately omit dev/ino (they vary across mount points and
    // remote systems) and rely on (path, size, mtime) for invalidation.
    std::string fingerprint;
    fingerprint.reserve(canonical.size() + 64);
    fingerprint.append("nix-bcv1\0", 9);
    {
        uint32_t v = kBytecodeSerializeSchemaVersion;
        fingerprint.append(reinterpret_cast<const char *>(&v), sizeof(v));
    }
    {
        uint32_t v = optimizationFlags;
        fingerprint.append(reinterpret_cast<const char *>(&v), sizeof(v));
    }
    fingerprint.append(canonical);
    fingerprint.push_back('\0');
    {
        uint64_t sz = static_cast<uint64_t>(st->fileSize.value_or(0));
        fingerprint.append(reinterpret_cast<const char *>(&sz), sizeof(sz));
    }
    // mtime via std::filesystem (SourceAccessor::Stat doesn't carry it).
    // For non-physical-fs accessors this will throw; we already know
    // the source IS physical (canonical path resolved + lstat above).
    try {
        namespace fs = std::filesystem;
        auto t = fs::last_write_time(canonical);
        uint64_t mt = static_cast<uint64_t>(t.time_since_epoch().count());
        fingerprint.append(reinterpret_cast<const char *>(&mt), sizeof(mt));
    } catch (...) {
        // mtime unavailable; the (path, size) pair still gives a
        // weaker key.  Don't refuse caching.
    }

    key.hash = hashString(HashAlgorithm::SHA256, fingerprint);
    return key;
}

} // namespace nix::bytecode
