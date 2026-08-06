/// @file
/// v3 CompilationUnit serialization.  See include/v3/serialize.hh.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/serialize.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/alloc.hh"  // PosSnapshot, resolvePosSnapshot, recordPosSnapshot

#include <algorithm>
#include <chrono>
#include <cstddef>   // offsetof (LambdaTable flatten)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nix::v3 {

namespace {
// M-10: pool counters, updated only inside internStringConstant (under its
// mutex), read by stringConstantPoolStats.  Plain size_t — single-threaded VM.
size_t g_poolEntries   = 0;
size_t g_poolCharBytes = 0;
}  // namespace

// M-10 (CODEBASE_REVIEW_2026-06-11): process-wide string-constant intern pool
// (declared in bytecode.hh).  A std::deque gives STABLE element addresses
// (never reallocates existing elements), so the returned pointer is valid for
// the process lifetime.  The index keys on a string_view into the POOLED
// string (stable), not the transient input.  Single-threaded VM; the mutex
// covers the rare concurrent emit/deserialize.
const std::string * internStringConstant(std::string_view s)
{
    static std::deque<std::string> pool;
    static std::unordered_map<std::string_view, const std::string *> index;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto it = index.find(s);
    if (it != index.end()) return it->second;
    pool.emplace_back(s);
    const std::string * p = &pool.back();
    index.emplace(std::string_view(*p), p);  // key views into the stable copy
    g_poolEntries   += 1;
    g_poolCharBytes += p->capacity();
    return p;
}

StringConstPoolStats stringConstantPoolStats() noexcept
{
    return { g_poolEntries, g_poolCharBytes };
}

// ---------------------------------------------------------------------------
// WS5-B2 (D2b) — LambdaTable flatten/unflatten (see bytecode.hh).
// ---------------------------------------------------------------------------

void LambdaTable::finalize()
{
    const uint32_t count = static_cast<uint32_t>(build_.size());
    const std::size_t descBytes = (std::size_t)count * sizeof(LambdaDescriptor);
    // sizeof(LambdaDescriptor) is a multiple of alignof(Formal)=4, so the
    // formals region right after the descriptor array is 4-aligned; chars need
    // only 1-alignment.  The whole block starts alignof(LambdaDescriptor)-
    // aligned (owned: 16-aligned vector buffer; borrowed: 8-aligned blob).
    std::size_t formalsCount = 0, charBytes = 0;
    for (const auto & b : build_) {
        formalsCount += b.formals.size();
        if (!b.name.empty())           charBytes += b.name.size() + 1;   // + NUL
        if (!b.contextualName.empty()) charBytes += b.contextualName.size() + 1;
    }
    const std::size_t formalsOff = descBytes;
    const std::size_t charsOff =
        formalsOff + formalsCount * sizeof(LambdaDescriptor::Formal);
    const std::size_t blockSize = charsOff + charBytes;

    std::vector<uint8_t> buf(blockSize, 0);
    auto * descs = reinterpret_cast<LambdaDescriptor *>(buf.data());
    std::size_t fCur = formalsOff;   // running formals byte cursor
    std::size_t cCur = charsOff;     // running char byte cursor
    for (uint32_t i = 0; i < count; ++i) {
        const LambdaBuild & b = build_[i];
        const std::size_t descOff = (std::size_t)i * sizeof(LambdaDescriptor);
        LambdaDescriptor & d = descs[i];   // aligned lvalue inside buf
        d = LambdaDescriptor{};            // zero-init POD (rel/len = 0)
        d.codeOffset              = b.codeOffset;
        d.prologueOffset          = b.prologueOffset;
        d.nUpvalues               = b.nUpvalues;
        d.nLocals                 = b.nLocals;
        d.arity                   = b.arity;
        d.hasFormals              = b.hasFormals;
        d.ellipsis                = b.ellipsis;
        d.nWithTargets            = b.nWithTargets;
        d.posHandle               = b.posHandle;
        d.selectorSym             = b.selectorSym;
        d.identityLambda          = b.identityLambda;
        d.secondArgIdentityLambda = b.secondArgIdentityLambda;
        d.isFormalWrapper         = b.isFormalWrapper;
        d.isOrDefault             = b.isOrDefault;
        d.isInheritWrapper        = b.isInheritWrapper;
        // formals: copy into the block, store self-relative offset.
        d.formals.count = static_cast<uint32_t>(b.formals.size());
        if (!b.formals.empty()) {
            std::memcpy(buf.data() + fCur, b.formals.data(),
                        b.formals.size() * sizeof(LambdaDescriptor::Formal));
            d.formals.rel = static_cast<int32_t>(
                (std::ptrdiff_t)fCur
                - (std::ptrdiff_t)(descOff + offsetof(LambdaDescriptor, formals)));
            fCur += b.formals.size() * sizeof(LambdaDescriptor::Formal);
        }
        // name / contextualName: copy chars + NUL, store self-relative offset.
        auto packStr = [&](const std::string & s, FlatStr & fs, std::size_t memOff) {
            if (s.empty()) return;
            std::memcpy(buf.data() + cCur, s.data(), s.size());
            buf[cCur + s.size()] = 0;   // NUL terminator for c_str()
            fs.len = static_cast<uint32_t>(s.size());
            fs.rel = static_cast<int32_t>(
                (std::ptrdiff_t)cCur - (std::ptrdiff_t)(descOff + memOff));
            cCur += s.size() + 1;
        };
        packStr(b.name,           d.name,           offsetof(LambdaDescriptor, name));
        packStr(b.contextualName, d.contextualName, offsetof(LambdaDescriptor, contextualName));
    }
    block_.adopt(std::move(buf));
    count_ = count;
    build_.clear();
    build_.shrink_to_fit();
}

void LambdaTable::loadBuildFromBlock(const uint8_t * p, std::size_t nbytes,
                                     uint32_t count)
{
    // The wire bytes may be unaligned (SQLite blob) → copy into an aligned
    // buffer so each descriptor's self-relative FlatStr/FlatArray resolve.
    std::vector<uint8_t> tmp(p, p + nbytes);
    // B1: the fixed-stride descriptor array must fit within the block before
    // we index descs[i] for i<count (a corrupt (count, nbytes) pair — e.g.
    // count=1000, nbytes=8 — would otherwise OOB-read the aligned buffer, and
    // build_.resize(count) below would attempt a multi-GB allocation).  This
    // is defence-in-depth alongside the same check at the deserializeImpl call
    // site; loadBuildFromBlock is a standalone entry point.
    if (nbytes < static_cast<std::size_t>(count) * sizeof(LambdaDescriptor))
        throw serialize::SerializationError(
            "v3 deserialize: lambda block too small for descriptor count");
    const auto * descs = reinterpret_cast<const LambdaDescriptor *>(tmp.data());
    build_.clear();
    build_.resize(count);
    // B1: each descriptor's self-relative FlatStr (name/contextualName) and
    // FlatArray (formals) views point WITHIN the block via a signed byte
    // offset; a crafted rel/len could dereference outside `tmp`.  Validate each
    // sub-region lies inside [0, nbytes) before it is read below.  A valid blob
    // always passes: the emitter packs these regions after the descriptor array
    // inside the same block.
    auto subInBlock = [&](const void * field, int32_t rel, std::size_t bytes) {
        if (bytes == 0) return true;   // empty FlatStr/FlatArray: rel unused
        int64_t base  = reinterpret_cast<const uint8_t *>(field) - tmp.data();
        int64_t start = base + rel;
        return start >= 0
            && static_cast<uint64_t>(start) + bytes <= static_cast<uint64_t>(nbytes);
    };
    for (uint32_t i = 0; i < count; ++i) {
        const LambdaDescriptor & d = descs[i];   // self-relative views valid in tmp
        if (!subInBlock(&d.name, d.name.rel, d.name.len)
         || !subInBlock(&d.contextualName, d.contextualName.rel, d.contextualName.len)
         || !subInBlock(&d.formals, d.formals.rel,
                        static_cast<std::size_t>(d.formals.count)
                            * sizeof(LambdaDescriptor::Formal)))
            throw serialize::SerializationError(
                "v3 deserialize: lambda descriptor self-relative view out of bounds");
        LambdaBuild & b = build_[i];
        b.codeOffset              = d.codeOffset;
        b.prologueOffset          = d.prologueOffset;
        b.nUpvalues               = d.nUpvalues;
        b.nLocals                 = d.nLocals;
        b.arity                   = d.arity;
        b.hasFormals              = d.hasFormals;
        b.ellipsis                = d.ellipsis;
        b.nWithTargets            = d.nWithTargets;
        b.posHandle               = d.posHandle;
        b.selectorSym             = d.selectorSym;
        b.identityLambda          = d.identityLambda;
        b.secondArgIdentityLambda = d.secondArgIdentityLambda;
        b.isFormalWrapper         = d.isFormalWrapper;
        b.isOrDefault             = d.isOrDefault;
        b.isInheritWrapper        = d.isInheritWrapper;
        b.name           = d.name.str();
        b.contextualName = d.contextualName.str();
        b.formals.assign(d.formals.begin(), d.formals.end());
    }
}

} // namespace nix::v3

namespace nix::v3::serialize {

namespace {

/// #777b (2026-05-23) sub-section timing accumulator.  Per-section
/// per-call breakdown printed at end-of-eval under V3_TIMING when
/// `V3_DBG_DESERIALIZE=1` is also set.  Gated by env var so the
/// `clock_gettime` overhead (~50 ns / call) doesn't bloat steady-
/// state production.  Falsifier mechanism for "where inside the
/// 334 ms deserialize budget does the time actually go?"
struct DeserializeBreakdown {
    uint64_t headerNs            = 0;
    uint64_t codeNs              = 0;
    uint64_t intConstantsNs      = 0;
    uint64_t floatConstantsNs    = 0;
    uint64_t stringConstantsNs   = 0;
    uint64_t symbolTableNs       = 0;
    uint64_t lambdasNs           = 0;
    uint64_t lambdaCodeOffsetsNs = 0;
    uint64_t primopsNs           = 0;
    uint64_t miscNs              = 0;
    uint64_t remapNs             = 0;
    uint64_t calls               = 0;
};

DeserializeBreakdown & breakdown()
{
    thread_local DeserializeBreakdown bd;
    return bd;
}

// WS5-D2a — AOT-borrow accounting.  Single-threaded VM; plain counters (like
// the M-10 pool stats).  Incremented only on the deserializeCUBorrowed path;
// surfaced via aotBorrowStats() + NIX_VM_STATS so the cross-process share
// rate is observable without a new env gate.
uint64_t g_borrowCUs = 0;
uint64_t g_borrowCodeBorrowed = 0;
uint64_t g_borrowCodeOwned = 0;
uint64_t g_borrowPodBorrowed = 0;
uint64_t g_borrowLambdasBorrowed = 0;   // WS5-B2 (D2b)
uint64_t g_borrowLambdasOwned = 0;

bool breakdownEnabled()
{
    static const bool s_e = std::getenv("V3_DBG_DESERIALIZE") != nullptr;
    return s_e;
}

inline uint64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Tiny streaming writer that appends to an std::string.
struct Writer {
    std::string & out;
    void writeBytes(const void * data, size_t n) {
        out.append(static_cast<const char *>(data), n);
    }
    void u32(uint32_t v) { writeBytes(&v, 4); }
    void u64(uint64_t v) { writeBytes(&v, 8); }
    void i64(int64_t v)  { writeBytes(&v, 8); }
    void f64(double v)   { writeBytes(&v, 8); }
    void u8(uint8_t v)   { out.push_back(static_cast<char>(v)); }
    void str(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        writeBytes(s.data(), s.size());
    }
    // WS5-B2 — pad the stream to an 8-byte multiple so the next section (the
    // flat lambda block) starts at an 8-aligned blob offset → 8-aligned in the
    // page-aligned AOT mmap → borrowable in place.
    void pad8() { while (out.size() % 8 != 0) out.push_back('\0'); }
};

/// Tiny streaming reader from a string_view.  Throws on
/// out-of-bounds reads — the format is fixed-layout so we know
/// the exact byte counts at every step.
struct Reader {
    std::string_view buf;
    size_t pos = 0;
    /// Bytes still unread.  Used by the corrupt-blob count guards: a
    /// blob-supplied element count can never exceed the bytes left to
    /// hold those elements, so an absurd count is rejected as a typed
    /// SerializationError *before* the pre-allocation (rather than as a
    /// std::bad_alloc a `catch(SerializationError)` caller would miss).
    size_t remaining() const noexcept { return buf.size() - pos; }
    void readBytes(void * dst, size_t n) {
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated blob");
        std::memcpy(dst, buf.data() + pos, n);
        pos += n;
    }
    uint32_t u32() { uint32_t v; readBytes(&v, 4); return v; }
    uint64_t u64() { uint64_t v; readBytes(&v, 8); return v; }
    int64_t  i64() { int64_t v;  readBytes(&v, 8); return v; }
    double   f64() { double v;   readBytes(&v, 8); return v; }
    uint8_t  u8()  {
        if (pos >= buf.size())
            throw SerializationError("v3 deserialize: truncated blob");
        return static_cast<uint8_t>(buf[pos++]);
    }
    std::string str() {
        uint32_t n = u32();
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated string");
        std::string s(buf.data() + pos, n);
        pos += n;
        return s;
    }
    // WS5-B2 — advance `pos` to the next 8-byte multiple (mirrors Writer::pad8
    // before the flat lambda block).  Keeps the block 8-aligned in the blob.
    void pad8() { pos = (pos + 7u) & ~size_t{7}; if (pos > buf.size()) pos = buf.size(); }
    // WS5-D2a: return a pointer to the next `n` bytes and advance WITHOUT
    // copying.  The borrow path reinterpret_casts this into the mmap; the
    // owning path memcpy's from it.  Bounds-checked like the other readers.
    const void * takePtr(size_t n) {
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated POD section");
        const void * p = buf.data() + pos;
        pos += n;
        return p;
    }
    // #777 (2026-05-23) zero-copy variant: returns a view into the
    // input blob.  Caller MUST consume the view before the blob
    // outlives — safe for sections used only for lookup (interning
    // SymbolIds, resolving primop names by string).  Saves a malloc
    // + memcpy per call vs. str().
    std::string_view strv() {
        uint32_t n = u32();
        if (pos + n > buf.size())
            throw SerializationError("v3 deserialize: truncated string");
        std::string_view s(buf.data() + pos, n);
        pos += n;
        return s;
    }
};

/// #781b (2026-05-23) Schema 9: walk the CU's bytecode + lambdas
/// and return the SORTED de-duplicated set of SymbolIds the CU
/// actually references.  Mirrors the dispatch table in
/// remapSymbolsInBytecode (anything that emits a SymbolId
/// operand or trailing-data SymbolId must also be observed here).
/// The serialized symbolTable section then carries only these
/// entries; on load, deserialize builds a sparse remap.  Reduces
/// the serialized section from `globalSymbolTable.size()` entries
/// down to a CU-local fraction (typical: 100-500 of 50 000 by
/// the time hello.drvPath's 269th import is compiled).
std::vector<uint32_t>
collectReferencedSymbols(const CompilationUnit & cu)
{
    std::vector<uint32_t> refs;
    refs.reserve(256);

    auto bump = [&](uint32_t id) { refs.push_back(id); };

    const auto & code = cu.code;
    for (size_t ip = 0; ip < code.size(); ) {
        uint32_t word = code[ip];
        Op op = decodeOp(word);
        uint32_t operand = decodeOperand(word);
        ++ip;

        if (op == OP_ATTRS_HAS
         || op == OP_WITH_LOOKUP
         || op == OP_ATTRS_SELECT
         || op == OP_REC_BINDING_SLOT_REF
         || op == OP_GET_UPVALUE_REC_BINDING
         || op == OP_GET_UPVALUE_REC_BINDING_SLOT
         || op == OP_RAW_FORMAL) {  // P2.1-a: operand=sym, no trailer
            bump(operand);
            // OP_ATTRS_SELECT has 1 IC follow-up word.
            // #779 Schema 10: OP_REC_BINDING_SLOT_REF also has 1 IC
            // follow-up word.
            if (op == OP_ATTRS_SELECT
             || op == OP_REC_BINDING_SLOT_REF) ++ip;
            // §2(b): OP_GET_UPVALUE_REC_BINDING carries [upvalIdx, icIdx]
            // (2 trailing words); its operand is the looked-up SymbolId.
            else if (op == OP_GET_UPVALUE_REC_BINDING) ip += 2;
            // item 5a: OP_GET_UPVALUE_REC_BINDING_SLOT carries
            // [dst, upvalIdx, icIdx] (3 trailing words); operand is the sym.
            else if (op == OP_GET_UPVALUE_REC_BINDING_SLOT) ip += 3;
        } else if (op == OP_ATTRS_INIT) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip < code.size()) bump(code[ip]);
                ip += 2;
            }
        } else if (op == OP_ATTRS_INIT_DYN) {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    =  operand        & 0xFFFu;
            for (uint32_t i = 0; i < nStatic; ++i) {
                if (ip < code.size()) bump(code[ip]);
                ip += 2;
            }
            ip += nDyn;
        } else if (op == OP_ATTRS_REC_INIT
                || op == OP_ATTRS_LET_REC_INIT
                || op == OP_ATTRS_REC_INIT_TAIL) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 2 * i < code.size()) bump(code[ip + 2 * i]);
            }
            ip += 2 * n;
        } else if (op == OP_CALL_PRIMOP || op == OP_R_BRANCH_FALSE || op == OP_R_CALL || op == OP_R_STR_CONCAT2) {
            ++ip;  // primop-index follow-up
        } else if (op == OP_R_PRIMOP2) {
            ip += 2;  // reg-VM: dst + (descA<<16|descB); no SymbolId operand
        } else if (op == OP_MAKE_CLOSURE || op == OP_MAKE_THUNK) {
            ip += 2;  // nUpvalues + nWithTargets
        }
        // OP_ATTRS_REC_SET, OP_ATTRS_SELECT_DYN, OP_ATTRS_HAS_DYN,
        // OP_REC_SLOT_PUBLISH, OP_APPLY_OVERRIDES — operand is not
        // a SymbolId.  Other opcodes have no trailing data.
    }

    // Formals carry SymbolIds for parameter names.
    for (const auto & l : cu.lambdas) {
        for (const auto & f : l.formals) {
            bump(f.name);
        }
        // Schema 12 (#814): selectorSym is a SymbolId on
        // LambdaDescriptor (eval-affecting via the OP_CALL peephole
        // fast path).  Must be in the sparse table so the cross-
        // process remap step has an entry to look up.
        if (l.selectorSym != 0) bump(l.selectorSym);
    }

    // Sort + dedup.  Both serialize side (writes pairs in this
    // order) and deserialize side (builds the remap) depend on
    // sorted order; consumers do a single pass over the result.
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

/// Schema 14 (R1 trigger fix, 2026-05-26): mirror of
/// `collectReferencedSymbols` but for PosIdx values.  Walks the
/// bytecode and lambda metadata and returns the sorted-unique set
/// of in-bytecode PosIdx values referenced by this CU.
///
/// Must stay in lockstep with `remapPositionsInBytecode` — anything
/// observed here must also be patched there, and vice versa.  The
/// trailer layouts mirror the SymbolId walker: PosIdx is the SECOND
/// word of each (name, pos) pair in OP_ATTRS_INIT /
/// OP_ATTRS_INIT_DYN static section / OP_ATTRS_(LET_)REC_INIT_*.
/// Formal::pos is a per-lambda PosIdx.
std::vector<uint32_t>
collectReferencedPositions(const CompilationUnit & cu)
{
    std::vector<uint32_t> refs;
    refs.reserve(256);
    auto bump = [&](uint32_t id) { if (id != 0) refs.push_back(id); };

    const auto & code = cu.code;
    for (size_t ip = 0; ip < code.size(); ) {
        uint32_t word = code[ip];
        Op op = decodeOp(word);
        uint32_t operand = decodeOperand(word);
        ++ip;

        if (op == OP_ATTRS_HAS
         || op == OP_WITH_LOOKUP
         || op == OP_RAW_FORMAL) {  // P2.1-a: operand=sym, no trailer, no PosIdx
            // No trailer.
        } else if (op == OP_ATTRS_SELECT
                || op == OP_REC_BINDING_SLOT_REF) {
            ++ip;  // 1 IC follow-up word
        } else if (op == OP_GET_UPVALUE_REC_BINDING) {
            ip += 2;  // §2(b): [upvalIdx, icIdx] — no PosIdx in trailer
        } else if (op == OP_GET_UPVALUE_REC_BINDING_SLOT) {
            ip += 3;  // item 5a: [dst, upvalIdx, icIdx] — no PosIdx in trailer
        } else if (op == OP_ATTRS_INIT) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 1 < code.size()) bump(code[ip + 1]);  // pos
                ip += 2;
            }
        } else if (op == OP_ATTRS_INIT_DYN) {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    =  operand        & 0xFFFu;
            for (uint32_t i = 0; i < nStatic; ++i) {
                if (ip + 1 < code.size()) bump(code[ip + 1]);  // static pos
                ip += 2;
            }
            // emit.cc:924 appends `nDyn` PosIdx words AFTER the static
            // (name, pos) pairs — one per dyn entry, no name (the dyn
            // names are pushed to the operand stack at runtime).  These
            // must be collected, otherwise their per-process indices
            // leak verbatim across the disk cache.
            for (uint32_t i = 0; i < nDyn; ++i) {
                if (ip < code.size()) bump(code[ip]);
                ++ip;
            }
        } else if (op == OP_ATTRS_REC_INIT
                || op == OP_ATTRS_LET_REC_INIT
                || op == OP_ATTRS_REC_INIT_TAIL) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 2 * i + 1 < code.size())
                    bump(code[ip + 2 * i + 1]);
            }
            ip += 2 * n;
        } else if (op == OP_CALL_PRIMOP || op == OP_R_BRANCH_FALSE || op == OP_R_CALL || op == OP_R_STR_CONCAT2) {
            ++ip;
        } else if (op == OP_R_PRIMOP2) {
            ip += 2;  // reg-VM: dst + descAB
        } else if (op == OP_MAKE_CLOSURE || op == OP_MAKE_THUNK) {
            ip += 2;
        }
    }

    // Formals carry PosIdx per parameter; the descriptor itself carries a
    // body PosIdx (`posHandle`).  WS5-B2 (D2b): posHandle is now a raw PosIdx
    // baked into the borrowable lambda block, so it MUST be in the sparse pos
    // table (→ canonical seeding) for the borrow to be identity + for the
    // owned-path remap to translate it.  (Pre-22 it was serialized inline as
    // file/line/col and rebuilt; that path is gone.)
    for (const auto & l : cu.lambdas) {
        for (const auto & f : l.formals) bump(f.pos);
        bump(l.posHandle);
    }

    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}

} // namespace

bool isCacheable(const CompilationUnit & /*cu*/)
{
    // Every CU the lowerer + emitter produce is cacheable today —
    // the only opaque data we don't serialize directly (primops,
    // attr-select IC) has clean round-trip representations
    // (resolve-by-name and zero-on-load respectively).
    return true;
}

uint64_t opcodeTableFingerprint()
{
    // FNV-1a 64-bit over (name, opcode-value) pairs for every Op the
    // process recognises.  Constructed at static-init time and cached.
    // Stable across processes built from the same bytecode.hh; changes
    // immediately when an opcode is renumbered, added, or removed.
    //
    // We use the names too (not just numeric values) so a swap of two
    // opcodes' meanings -- which could happen if one is renamed and
    // another is reassigned its old value -- is still caught.
    static const uint64_t kFp = []() -> uint64_t {
        struct Entry { const char * name; uint32_t value; };
        // Listed in declaration order from bytecode.hh.  Adding/removing
        // here is the explicit mechanism for the schema bump to detect
        // the change at runtime, in addition to source-control review.
        const Entry table[] = {
            {"OP_LIT_INT",         OP_LIT_INT},
            {"OP_LIT_INT_BIG",     OP_LIT_INT_BIG},
            {"OP_LIT_FLOAT",       OP_LIT_FLOAT},
            {"OP_LIT_STR",         OP_LIT_STR},
            {"OP_LIT_PATH",        OP_LIT_PATH},
            {"OP_LIT_TRUE",        OP_LIT_TRUE},
            {"OP_LIT_FALSE",       OP_LIT_FALSE},
            {"OP_LIT_NULL",        OP_LIT_NULL},
            {"OP_GET_LOCAL",       OP_GET_LOCAL},
            {"OP_SET_LOCAL",       OP_SET_LOCAL},
            {"OP_GET_UPVALUE",     OP_GET_UPVALUE},
            {"OP_DUP",             OP_DUP},
            {"OP_POP",             OP_POP},
            {"OP_SWAP",            OP_SWAP},
            {"OP_ADD",             OP_ADD},
            {"OP_SUB",             OP_SUB},
            {"OP_MUL",             OP_MUL},
            {"OP_DIV",             OP_DIV},
            {"OP_NEGATE",          OP_NEGATE},
            {"OP_EQ",              OP_EQ},
            {"OP_NEQ",             OP_NEQ},
            {"OP_LESS",            OP_LESS},
            {"OP_NOT",             OP_NOT},
            {"OP_AND_BRANCH",      OP_AND_BRANCH},
            {"OP_OR_BRANCH",       OP_OR_BRANCH},
            {"OP_IMPL_BRANCH",     OP_IMPL_BRANCH},
            {"OP_JUMP",            OP_JUMP},
            {"OP_BRANCH_FALSE",    OP_BRANCH_FALSE},
            {"OP_BRANCH_TRUE",     OP_BRANCH_TRUE},
            {"OP_MAKE_CLOSURE",    OP_MAKE_CLOSURE},
            {"OP_MAKE_THUNK",      OP_MAKE_THUNK},
            {"OP_CALL",            OP_CALL},
            {"OP_RETURN",          OP_RETURN},
            {"OP_FORCE",           OP_FORCE},
            {"OP_GET_LOCAL_FORCE", OP_GET_LOCAL_FORCE},
            {"OP_GET_UPVALUE_FORCE", OP_GET_UPVALUE_FORCE},
            {"OP_TAIL_CALL",       OP_TAIL_CALL},
            {"OP_SET_LOCAL_KEEP",  OP_SET_LOCAL_KEEP},
            {"OP_LIST_INIT",       OP_LIST_INIT},
            {"OP_LIST_CONCAT",     OP_LIST_CONCAT},
            {"OP_ATTRS_INIT",      OP_ATTRS_INIT},
            {"OP_ATTRS_INIT_DYN",  OP_ATTRS_INIT_DYN},
            {"OP_ATTRS_REC_INIT",  OP_ATTRS_REC_INIT},
            {"OP_ATTRS_LET_REC_INIT", OP_ATTRS_LET_REC_INIT},
            {"OP_ATTRS_REC_INIT_TAIL", OP_ATTRS_REC_INIT_TAIL},
            {"OP_ATTRS_REC_SET",   OP_ATTRS_REC_SET},
            {"OP_ATTRS_SELECT",    OP_ATTRS_SELECT},
            {"OP_ATTRS_SELECT_DYN", OP_ATTRS_SELECT_DYN},
            {"OP_ATTRS_HAS",       OP_ATTRS_HAS},
            {"OP_ATTRS_HAS_DYN",   OP_ATTRS_HAS_DYN},
            {"OP_ATTRS_UPDATE",    OP_ATTRS_UPDATE},
            {"OP_REC_BINDING_SLOT_REF", OP_REC_BINDING_SLOT_REF},
            {"OP_REC_SLOT_PUBLISH", OP_REC_SLOT_PUBLISH},
            {"OP_APPLY_OVERRIDES", OP_APPLY_OVERRIDES},
            {"OP_WITH_PUSH",       OP_WITH_PUSH},
            {"OP_WITH_POP",        OP_WITH_POP},
            {"OP_WITH_LOOKUP",     OP_WITH_LOOKUP},
            {"OP_STR_CONCAT",      OP_STR_CONCAT},
            {"OP_ASSERT",          OP_ASSERT},
            {"OP_POS",             OP_POS},
            {"OP_CALL_PRIMOP",     OP_CALL_PRIMOP},
            {"OP_LIT_PRIMOP",      OP_LIT_PRIMOP},
            {"OP_LIT_BUILTINS",    OP_LIT_BUILTINS},
            {"OP_IS_NULL",         OP_IS_NULL},
            {"OP_IS_BOOL",         OP_IS_BOOL},
            {"OP_IS_INT",          OP_IS_INT},
            {"OP_IS_FLOAT",        OP_IS_FLOAT},
            {"OP_IS_STRING",       OP_IS_STRING},
            {"OP_IS_PATH",         OP_IS_PATH},
            {"OP_IS_LIST",         OP_IS_LIST},
            {"OP_IS_ATTRS",        OP_IS_ATTRS},
            {"OP_IS_FUNCTION",     OP_IS_FUNCTION},
            {"OP_HEAD",            OP_HEAD},
            {"OP_TAIL",            OP_TAIL},
            {"OP_LENGTH",          OP_LENGTH},
            {"OP_ELEM_AT",         OP_ELEM_AT},
            {"OP_HALT",            OP_HALT},
        };
        uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
        for (auto & e : table) {
            for (const char * p = e.name; *p; ++p) {
                h ^= static_cast<uint8_t>(*p);
                h *= 0x100000001b3ULL;
            }
            // mix in the opcode value too -- catches a renumbering even
            // if the name stayed the same.
            uint32_t v = e.value;
            for (int i = 0; i < 4; ++i) {
                h ^= (v >> (i * 8)) & 0xff;
                h *= 0x100000001b3ULL;
            }
        }
        return h;
    }();
    return kFp;
}

namespace {

/// Walk the bytecode of `cu` and rewrite every SymbolId operand
/// (in OP_WITH_LOOKUP / OP_ATTRS_SELECT / OP_ATTRS_HAS) and every
/// trailing-data SymbolId (in OP_ATTRS_INIT / OP_ATTRS_INIT_DYN /
/// OP_ATTRS_REC_INIT / OP_ATTRS_LET_REC_INIT) using the supplied
/// remap table.
///
/// `remap[oldId] = newGlobalId`.  For ids beyond `remap.size()`
/// (shouldn't happen if cu.symbolTable was the source of truth at
/// serialize time), leave the id unchanged — best-effort.
///
/// Bytecode opcode-data layouts come from emit.cc:
///   OP_ATTRS_INIT      [n:24]            data: 2n words (name, pos) pairs
///   OP_ATTRS_INIT_DYN  [(nStat<<12)|nDyn] data: 2*nStatic (name,pos) +
///                                              nDyn pos words
///   OP_ATTRS_REC_INIT  [n:24]            data: 2n (name, pos) pairs
///   OP_ATTRS_SELECT    [sym:24]          data: 1 IC-index word
///   OP_ATTRS_HAS       [sym:24]          (no follow-up)
///   OP_WITH_LOOKUP     [sym:24]
void remapSymbolsInBytecode(CompilationUnit & cu,
                              const std::vector<uint32_t> & remap)
{
    auto remapId = [&](uint32_t id) -> uint32_t {
        return id < remap.size() ? remap[id] : id;
    };
    auto & code = cu.code;

    // OP_ATTRS_REC_INIT requires its trailing (name, pos) pairs to
    // be sorted by SymbolId — runtime fills b->entries[i] in that
    // order and Bindings::lookup binary-searches.  After remap the
    // names may no longer be in sorted order, so we re-sort the
    // trailing data and build a (oldSlot -> newSlot) permutation.
    // OP_ATTRS_REC_SET[slot] operands that follow within the same
    // emit must be patched accordingly.  See emit.cc::emitOne(LetRec).
    //
    // Active permutations are tracked in a small stack (always at
    // most one in flight per emit, but the stack lets us survive
    // nested LetRecs that don't actually emit between init+set —
    // belt-and-braces).
    struct PendingRec {
        std::vector<uint32_t> oldToNew;  // [oldSlot] -> newSlot
        uint32_t setsRemaining;
    };
    std::vector<PendingRec> pending;

    for (size_t ip = 0; ip < code.size(); ) {
        uint32_t & word = code[ip];
        Op op = decodeOp(word);
        uint32_t operand = decodeOperand(word);
        ip++;

        // Patch SymbolId operands in-place + walk trailing data
        // words.  See bytecode.hh + emit.cc for opcode layouts.
        if (op == OP_ATTRS_HAS) {
            word = encode(op, remapId(operand));
        } else if (op == OP_RAW_FORMAL) {
            // P2.1-a: operand = formal SymbolId, no trailer (prefixes MkThunk).
            word = encode(op, remapId(operand));
        } else if (op == OP_WITH_LOOKUP) {
            word = encode(op, remapId(operand));
        } else if (op == OP_ATTRS_SELECT) {
            word = encode(op, remapId(operand));
            ip++;  // 1 IC-index follow-up word
        } else if (op == OP_REC_BINDING_SLOT_REF) {
            // Phase-13 review CRIT-1 fix: this opcode carries a
            // SymbolId in its operand (the slot name to look up in
            // the rec attrset), same as OP_ATTRS_SELECT.  Without
            // remap, every cached CU using `with rec` / lib.fix
            // resolved the wrong attribute after a process restart.
            word = encode(op, remapId(operand));
            // #779 Schema 10: skip the IC follow-up word.  The IC
            // entry is process-local state (Bindings* pointers
            // change across processes); we leave the cache slot
            // value alone since recSlotCache is re-zeroed on load.
            ip++;
        } else if (op == OP_GET_UPVALUE_REC_BINDING) {
            // §2(b): like OP_REC_BINDING_SLOT_REF, the operand is the
            // looked-up SymbolId and MUST be remapped to the canonical
            // table (else cached vs fresh CUs differ — r1-trigger-verify).
            // The 2 trailing words are [upvalIdx, icIdx]: upvalIdx is a
            // closure-relative index (process-independent) and icIdx is a
            // recSlotCache slot (re-zeroed on load) — neither is remapped.
            word = encode(op, remapId(operand));
            ip += 2;
        } else if (op == OP_GET_UPVALUE_REC_BINDING_SLOT) {
            // item 5a: like GET_UPVALUE_REC_BINDING — operand is the looked-up
            // SymbolId (remap), trailer is [dst, upvalIdx, icIdx] (3 words,
            // process-independent — not remapped).
            word = encode(op, remapId(operand));
            ip += 3;
        } else if (op == OP_ATTRS_INIT) {
            // Names get remapped; runtime sorts on the fly so order
            // doesn't matter.
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip < code.size()) code[ip] = remapId(code[ip]);  // name
                ip += 2;  // skip pos as well
            }
        } else if (op == OP_ATTRS_INIT_DYN) {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    =  operand        & 0xFFFu;
            for (uint32_t i = 0; i < nStatic; ++i) {
                if (ip < code.size()) code[ip] = remapId(code[ip]);
                ip += 2;
            }
            ip += nDyn;
        } else if (op == OP_ATTRS_REC_INIT
                   || op == OP_ATTRS_LET_REC_INIT
                   || op == OP_ATTRS_REC_INIT_TAIL) {
            // Remap names + remember their old positions so we can
            // re-sort and propagate the permutation to the matching
            // OP_ATTRS_REC_SETs.  OP_ATTRS_LET_REC_INIT shares the
            // same trailing data layout (n (name, pos) pairs) as
            // OP_ATTRS_REC_INIT and emits the same OP_ATTRS_REC_SETs
            // afterwards -- only the runtime semantics differ.
            uint32_t n = operand;
            std::vector<std::pair<uint32_t, uint32_t>> namePos(n);  // (newName, pos)
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 2 * i + 1 < code.size()) {
                    namePos[i].first  = remapId(code[ip + 2 * i]);
                    namePos[i].second = code[ip + 2 * i + 1];
                }
            }
            // Sort by new name; build oldSlot -> newSlot permutation.
            std::vector<uint32_t> sortedIdx(n);
            for (uint32_t i = 0; i < n; ++i) sortedIdx[i] = i;
            std::sort(sortedIdx.begin(), sortedIdx.end(),
                [&](uint32_t a, uint32_t b) {
                    return namePos[a].first < namePos[b].first;
                });
            std::vector<uint32_t> oldToNew(n);
            for (uint32_t newSlot = 0; newSlot < n; ++newSlot)
                oldToNew[sortedIdx[newSlot]] = newSlot;
            // Write back trailing data in sorted (new) order.
            for (uint32_t newSlot = 0; newSlot < n; ++newSlot) {
                uint32_t oldSlot = sortedIdx[newSlot];
                if (ip + 2 * newSlot + 1 < code.size()) {
                    code[ip + 2 * newSlot]     = namePos[oldSlot].first;
                    code[ip + 2 * newSlot + 1] = namePos[oldSlot].second;
                }
            }
            ip += 2 * n;
            // Queue the slot permutation for the n upcoming
            // OP_ATTRS_REC_SETs in this emit.
            pending.push_back({std::move(oldToNew), n});
        } else if (op == OP_ATTRS_REC_SET) {
            if (!pending.empty() && pending.back().setsRemaining > 0) {
                auto & p = pending.back();
                uint32_t oldSlot = operand;
                uint32_t newSlot = (oldSlot < p.oldToNew.size())
                    ? p.oldToNew[oldSlot] : oldSlot;
                word = encode(OP_ATTRS_REC_SET, newSlot);
                if (--p.setsRemaining == 0) pending.pop_back();
            }
            // No trailing data.
        } else if (op == OP_CALL_PRIMOP || op == OP_R_BRANCH_FALSE || op == OP_R_CALL || op == OP_R_STR_CONCAT2) {
            ip++;  // primop-index follow-up
        } else if (op == OP_R_PRIMOP2) {
            ip += 2;  // reg-VM: dst + (descA<<16|descB); no SymbolId operand
        } else if (op == OP_MAKE_CLOSURE || op == OP_MAKE_THUNK) {
            ip += 2;  // nUpvalues + nWithTargets (#530)
        }
        // All other opcodes either have no trailing data or no
        // SymbolIds in their data; leave ip alone.
    }
}

/// Schema 14 (R1 trigger fix): rewrite in-bytecode PosIdx values
/// using the load-time remap table.  Same opcode/trailer layout as
/// `remapSymbolsInBytecode` but patches the SECOND of each (name,
/// pos) pair instead of the FIRST.  No sort/propagate dance — PosIdx
/// is not used as a key by runtime, so order is irrelevant.
void remapPositionsInBytecode(CompilationUnit & cu,
                              const std::vector<uint32_t> & posRemap)
{
    auto remapPos = [&](uint32_t id) -> uint32_t {
        if (id == 0) return 0;  // 0 = "no pos"
        return id < posRemap.size() ? posRemap[id] : id;
    };
    auto & code = cu.code;
    for (size_t ip = 0; ip < code.size(); ) {
        uint32_t word = code[ip];
        Op op = decodeOp(word);
        uint32_t operand = decodeOperand(word);
        ++ip;

        if (op == OP_ATTRS_HAS || op == OP_WITH_LOOKUP) {
            // No PosIdx in trailer.
        } else if (op == OP_ATTRS_SELECT
                || op == OP_REC_BINDING_SLOT_REF) {
            ++ip;  // IC follow-up
        } else if (op == OP_GET_UPVALUE_REC_BINDING) {
            ip += 2;  // §2(b): [upvalIdx, icIdx] — no PosIdx in trailer
        } else if (op == OP_GET_UPVALUE_REC_BINDING_SLOT) {
            ip += 3;  // item 5a: [dst, upvalIdx, icIdx] — no PosIdx in trailer
        } else if (op == OP_ATTRS_INIT) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 1 < code.size())
                    code[ip + 1] = remapPos(code[ip + 1]);
                ip += 2;
            }
        } else if (op == OP_ATTRS_INIT_DYN) {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    =  operand        & 0xFFFu;
            for (uint32_t i = 0; i < nStatic; ++i) {
                if (ip + 1 < code.size())
                    code[ip + 1] = remapPos(code[ip + 1]);
                ip += 2;
            }
            // emit.cc:924 — `nDyn` trailing PosIdx words (one per
            // dyn entry, no name).  Same remap as static pos.
            for (uint32_t i = 0; i < nDyn; ++i) {
                if (ip < code.size())
                    code[ip] = remapPos(code[ip]);
                ++ip;
            }
        } else if (op == OP_ATTRS_REC_INIT
                || op == OP_ATTRS_LET_REC_INIT
                || op == OP_ATTRS_REC_INIT_TAIL) {
            uint32_t n = operand;
            for (uint32_t i = 0; i < n; ++i) {
                if (ip + 2 * i + 1 < code.size())
                    code[ip + 2 * i + 1] =
                        remapPos(code[ip + 2 * i + 1]);
            }
            ip += 2 * n;
        } else if (op == OP_CALL_PRIMOP || op == OP_R_BRANCH_FALSE || op == OP_R_CALL || op == OP_R_STR_CONCAT2) {
            ++ip;
        } else if (op == OP_R_PRIMOP2) {
            ip += 2;  // reg-VM: dst + descAB
        } else if (op == OP_MAKE_CLOSURE || op == OP_MAKE_THUNK) {
            ip += 2;
        }
    }
}

} // namespace

std::string serializeCU(const CompilationUnit & cu)
{
    if (!isCacheable(cu))
        throw SerializationError("v3 serialize: CU is not cacheable");

    std::string out;
    // Reserve a generous head-of-line allocation.  Most CUs are
    // small (~1-50 KB).  Growing reserves later is fine.
    out.reserve(64 * 1024);
    Writer w{out};

    // Header: magic + schema version + opcode-table fingerprint.
    // Fingerprint catches opcode renumbering / addition / deletion
    // between two builds with the same kSchemaVersion (REVIEW §1.4).
    w.writeBytes(kMagic, sizeof(kMagic));
    w.u32(kSchemaVersion);
    w.u64(opcodeTableFingerprint());

    // WS5-D2a — POD block (schema 21).  The four read-only POD sections
    // (code, intConstants, floatConstants, lambdaCodeOffsets) are laid out
    // contiguously and NATURALLY ALIGNED relative to the blob start, so the
    // AOT-load path can BORROW them in place from the process-lifetime mmap
    // via reinterpret_cast instead of copying them into private per-process
    // vectors (which is what makes the pages Shared_Clean across independent
    // `nix` processes).  The AOT builder pads each blob to an 8-byte file
    // offset (bench/build-aot-cache.py); combined with the page-aligned mmap
    // base, every array below lands at its required alignment.
    //
    // Layout (offsets relative to blob start):
    //   [20] podBlockOff : u32 (== 24; lets a future header grow)
    //   [24] codeCount, intCount, floatCount, lcoCount : u32 x4  (16 B, 8-aligned)
    //   [40] intConstants   : int64  * intCount   (8-aligned)
    //        floatConstants : double * floatCount (8-aligned; follows 8*N int)
    //        code           : u32    * codeCount  (4-aligned; follows 8*N float)
    //        lambdaCodeOffsets : u32 * lcoCount   (4-aligned; follows 4*N code)
    // 8-aligned arrays (int64/double) come first so the 4-aligned (u32)
    // arrays never break the 8-alignment the doubles/ints need.
    w.u32(24u);  // podBlockOff — POD block starts immediately after this word
    w.u32(static_cast<uint32_t>(cu.code.size()));
    w.u32(static_cast<uint32_t>(cu.intConstants.size()));
    w.u32(static_cast<uint32_t>(cu.floatConstants.size()));
    w.u32(static_cast<uint32_t>(cu.lambdaCodeOffsets.size()));
    if (cu.intConstants.size())
        w.writeBytes(cu.intConstants.data(),
                     cu.intConstants.size() * sizeof(int64_t));
    if (cu.floatConstants.size())
        w.writeBytes(cu.floatConstants.data(),
                     cu.floatConstants.size() * sizeof(double));
    if (cu.code.size())
        w.writeBytes(cu.code.data(), cu.code.size() * sizeof(Instruction));
    if (cu.lambdaCodeOffsets.size())
        w.writeBytes(cu.lambdaCodeOffsets.data(),
                     cu.lambdaCodeOffsets.size() * sizeof(uint32_t));

    // Section: stringConstants.  M-10: entries are interned pointers; write
    // the literal TEXT (disk format unchanged — deserialize re-interns).
    w.u32(static_cast<uint32_t>(cu.stringConstants.size()));
    for (auto * s : cu.stringConstants) w.str(*s);

    // Section: symbolTable.  Schema 9 (#781b): SPARSE — only entries
    // for SymbolIds actually referenced by this CU's bytecode +
    // formals.  Format:
    //   count : u32
    //   maxId : u32           # max origId across all entries
    //   (origId : u32, name : str)*  # sorted by origId
    // The unsorted-source `cu.symbolTable` may be either a copy of
    // the global table (legacy emit path) or empty (post-#770c).
    // In either case the names we serialize come from the global
    // table directly, so the CU doesn't need its symbolTable copy.
    {
        const auto refs = collectReferencedSymbols(cu);
        const auto & gst = ir::globalSymbolTable();
        uint32_t maxId = 0;
        for (auto id : refs) if (id > maxId) maxId = id;
        w.u32(static_cast<uint32_t>(refs.size()));
        w.u32(maxId);
        for (auto id : refs) {
            w.u32(id);
            // Prefer the (likely up-to-date) cu.symbolTable if it
            // was populated; fall back to the global table.  Both
            // are SymbolId-indexed by the same source-of-truth.
            std::string_view name;
            if (id < cu.symbolTable.size()
                && !cu.symbolTable[id].empty()) {
                name = cu.symbolTable[id];
            } else if (id < gst.size()) {
                name = gst[id];
            }
            w.str(name);
        }
    }

    // Schema 14 — Section: sparse posTable.  Mirrors the SymbolId
    // sparse table.  Layout:
    //   count : u32
    //   maxId : u32
    //   (origId : u32, present : u8, [file : str, line : u32, col : u32])*
    // `present == 0` is a sentinel for an unresolved-pos entry
    // (`resolvePosSnapshot` returned nullptr); deserialise leaves
    // remap[origId] = 0 in that case.
    {
        const auto posRefs = collectReferencedPositions(cu);
        uint32_t maxId = 0;
        for (auto id : posRefs) if (id > maxId) maxId = id;
        w.u32(static_cast<uint32_t>(posRefs.size()));
        w.u32(maxId);
        for (auto id : posRefs) {
            w.u32(id);
            const PosSnapshot * ps = resolvePosSnapshot(id);
            if (ps) {
                w.u8(1);
                w.str(ps->file);
                w.u32(ps->line);
                w.u32(ps->column);
            } else {
                w.u8(0);
            }
        }
    }

    // Section: lambdas (schema 22, WS5-B2 D2b).  The `lambdas` array is a
    // single self-relative POD block — [LambdaDescriptor[]][Formal[]][name/
    // contextualName chars] — built by emit's LambdaTable::finalize().  Write
    // it RAW (count + block length + 8-aligned block bytes) so the AOT-load
    // path can BORROW the whole descriptor array in place from the mmap
    // (Shared_Clean) instead of rebuilding per-descriptor heap.  All the
    // per-descriptor ids the block carries (formals name/pos, selectorSym,
    // posHandle) are the writer's global ids, translated on the owned path via
    // the sparse symbol/pos remap (identity on borrow — the canonical table).
    //
    // Layout of these two words + block:
    //   count : u32, blockLen : u32, <pad to 8>, block bytes[blockLen]
    // The blob is 8-aligned in the AOT file + the mmap is page-aligned, so the
    // padded block starts 8-aligned in memory (alignof(LambdaDescriptor)≤8).
    {
        const uint32_t count = static_cast<uint32_t>(cu.lambdas.size());
        const uint8_t * blk = cu.lambdas.block_.data();
        const size_t blkLen = cu.lambdas.block_.size();
        w.u32(count);
        w.u32(static_cast<uint32_t>(blkLen));
        w.pad8();
        if (blkLen) w.writeBytes(blk, blkLen);
    }

    // (lambdaCodeOffsets moved into the WS5-D2a POD block near the header.)

    // Section: primops (resolved by name on load).  PrimOp::name is
    // a string_view referencing the registered name table; copy it
    // out as a length-prefixed string.
    w.u32(static_cast<uint32_t>(cu.primops.size()));
    for (auto * po : cu.primops) {
        if (!po) {
            // Sentinel — should not happen.  Emit empty string and
            // resolve to throw at load time if encountered.
            w.str("");
        } else {
            w.str(po->name);
        }
    }

    // Section: attrSelectCache size (entries are zeroed on load).
    w.u32(static_cast<uint32_t>(cu.rt.attrSelectCache.size()));

    // Section: recSlotCache size (#779 Schema 10; entries zeroed on load).
    w.u32(static_cast<uint32_t>(cu.rt.recSlotCache.size()));

    // Section: entryOffset.
    w.u32(cu.entryOffset);

    return out;
}

// WS5-D2a — the shared deserialize core.  `allowBorrow` selects the path:
//   * false → OWNING (SQLite disk cache, smoke round-trips): copy every
//             section into private vectors + remap the bytecode in place,
//             exactly as before schema 21.  `blob` may be a transient.
//   * true  → AOT-BORROW: `blob.data()` points into the process-lifetime
//             mmap; borrow the read-only POD sections in place so the pages
//             are Shared_Clean across processes.  `code` is borrowed iff its
//             per-process symbol+pos remap is the identity (via id-preferring
//             seeding); otherwise it is materialised (copied) and remapped.
//             The never-remapped POD sections (int/float, lambdaCodeOffsets)
//             are borrowed whenever the mmap is aligned.
// B2: sane upper bound on a serialized symbol/pos `maxId`.  A remap of
// (maxId+1) uint32 entries at this cap is ~1 GB; a crafted blob claiming a
// near-UINT32_MAX maxId would otherwise trigger a ~16 GB assign() DoS.  Real
// global symbol/pos id spaces stay orders of magnitude below this (nixpkgs
// interns ~1e5 symbols; the heaviest measured evals stay under ~1e7), so a
// valid blob is never rejected.
constexpr uint32_t kMaxRemapId = 0x0FFFFFFFu;  // 268,435,455

static CompilationUnit deserializeImpl(std::string_view blob, bool allowBorrow)
{
    Reader r{blob};
    const bool dbg = breakdownEnabled();
    if (dbg) ++breakdown().calls;
    uint64_t t0 = dbg ? nowNs() : 0;

    // Verify magic.
    char magic[sizeof(kMagic)];
    r.readBytes(magic, sizeof(magic));
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
        throw SerializationError("v3 deserialize: bad magic");

    // Verify schema version.
    uint32_t schema = r.u32();
    if (schema != kSchemaVersion)
        throw SerializationError("v3 deserialize: schema mismatch (got "
            + std::to_string(schema) + ", want "
            + std::to_string(kSchemaVersion) + ")");

    // Verify opcode-table fingerprint.  Detects opcode renumbering
    // between two builds at the same schema version (REVIEW §1.4).
    uint64_t fp = r.u64();
    if (fp != opcodeTableFingerprint())
        throw SerializationError("v3 deserialize: opcode-table "
            "fingerprint mismatch -- the cache was produced by a build "
            "with a different opcode layout (rebuild required)");

    CompilationUnit cu;
    if (dbg) { breakdown().headerNs += nowNs() - t0; t0 = nowNs(); }

    // WS5-D2a — POD block (schema 21).  Read the four counts, then take
    // (without copying) an in-blob pointer to each contiguous array.  The
    // borrow/own decision for the never-remapped arrays is made here; the
    // decision for `code` is deferred until the symbol/pos remap is known.
    uint32_t podBlockOff = r.u32();
    if (podBlockOff < r.pos)
        throw SerializationError("v3 deserialize: bad podBlockOff");
    r.pos = podBlockOff;  // (== 24 in schema 21; explicit for header growth)
    const uint32_t codeCount  = r.u32();
    const uint32_t intCount   = r.u32();
    const uint32_t floatCount = r.u32();
    const uint32_t lcoCount   = r.u32();
    const void * intPtr   = r.takePtr((size_t)intCount   * sizeof(int64_t));
    const void * floatPtr = r.takePtr((size_t)floatCount * sizeof(double));
    const void * codePtr  = r.takePtr((size_t)codeCount  * sizeof(Instruction));
    const void * lcoPtr   = r.takePtr((size_t)lcoCount   * sizeof(uint32_t));

    // The int64/double arrays require 8-byte alignment; by the layout
    // (8-aligned arrays first) a single check on the data-region start
    // (== intPtr) covers all four sections.  The AOT builder pads each blob
    // to an 8-byte file offset so this holds; if it doesn't (e.g. an
    // unpadded file), we fall back to copying — correctness over sharing.
    const bool aligned = allowBorrow
        && (reinterpret_cast<uintptr_t>(intPtr) % alignof(int64_t) == 0);

    auto ownPod = [&](auto & vec, const void * p, uint32_t n) {
        using Elem = std::decay_t<decltype(vec[0])>;
        vec.resize(n);
        if (n) std::memcpy(vec.data(), p, (size_t)n * sizeof(Elem));
    };

    // intConstants / floatConstants / lambdaCodeOffsets are POD and NEVER
    // remapped → always safe to borrow when aligned.
    if (aligned) {
        cu.intConstants.borrow(
            reinterpret_cast<const int64_t *>(intPtr), intCount);
        cu.floatConstants.borrow(
            reinterpret_cast<const double *>(floatPtr), floatCount);
        cu.lambdaCodeOffsets.borrow(
            reinterpret_cast<const uint32_t *>(lcoPtr), lcoCount);
    } else {
        ownPod(cu.intConstants,      intPtr,   intCount);
        ownPod(cu.floatConstants,    floatPtr, floatCount);
        ownPod(cu.lambdaCodeOffsets, lcoPtr,   lcoCount);
    }
    if (dbg) { breakdown().codeNs += nowNs() - t0; t0 = nowNs(); }

    // Section: stringConstants.  M-10: re-intern the literal text into the
    // process-wide pool (disk format unchanged — still raw strings on disk).
    {
        uint32_t n = r.u32();
        // B4: each stringConstant is at least a 4-byte length prefix on the
        // wire, so `n` strings need >= 4n bytes remaining.  Reject an absurd
        // count as a typed error before reserve() attempts a multi-GB alloc.
        if (n > r.remaining() / 4)
            throw SerializationError(
                "v3 deserialize: stringConstants count exceeds remaining blob");
        cu.stringConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.stringConstants.push_back(internStringConstant(r.str()));
    }
    if (dbg) { breakdown().stringConstantsNs += nowNs() - t0; t0 = nowNs(); }

    // Section: symbolTable.  Schema 9 (#781b): SPARSE.
    //   count : u32
    //   maxId : u32
    //   (origId : u32, name : str)*  # sorted by origId
    // Build a sparse remap[maxId+1] vector, filled with 0
    // (kInvalidSymbol — empty string) by default.  Unreferenced
    // slots default to 0; valid bytecode operands should never
    // reach those slots, but the remap walk's bounds check
    // (id < remap.size()) protects against corruption.
    //
    // The previous-schema dense format (one entry per global
    // SymbolId at serialize time) caused 296 ms of 304 ms total
    // deserialize cost on hello.drvPath — the entire global
    // symbol table got interned for every CU even though most
    // CUs only reference a few hundred symbols.  Schema 9
    // serializes only the referenced subset.
    //
    // WS5-D2a: the AOT-borrow path interns via `globalSeedSymbol` (adopt the
    // writer's id when free) and tracks whether EVERY entry seeded to the
    // identity — if so the borrowed `code` needs no rewrite.  The owning
    // path uses `globalInternSymbol` and always rewrites, exactly as before.
    bool symIdentity = allowBorrow;
    std::vector<uint32_t> remap;
    {
        uint32_t n = r.u32();
        uint32_t maxId = r.u32();
        // B2: `maxId + 1` is a uint32 that WRAPS to 0 at maxId==0xFFFFFFFF,
        // giving an empty remap whose `origId > maxId` guard can never fire →
        // OOB `remap[origId]` write.  Widen to size_t BEFORE the +1 so the
        // vector is correctly sized and the guard is sound, AND reject an
        // absurd maxId so a crafted blob can't force a multi-GB assign() DoS.
        // kMaxRemapId (256M ids ⇒ ≤1 GB remap) sits far above any real global
        // symbol/pos id (nixpkgs interns ~1e5 symbols; the heaviest evals stay
        // well under 1e7), so no valid blob is ever rejected.
        if (maxId > kMaxRemapId)
            throw SerializationError(
                "v3 deserialize: symbolTable maxId absurdly large");
        remap.assign(size_t(maxId) + 1, 0u);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t origId = r.u32();
            std::string_view name = r.strv();
            if (origId > maxId)
                throw SerializationError(
                    "v3 deserialize: symbolTable entry origId > maxId");
            uint32_t rid;
            if (allowBorrow) {
                rid = ir::globalSeedSymbol(origId, name);
                if (rid != origId) symIdentity = false;
            } else {
                rid = name.empty() ? uint32_t{0}
                                   : ir::globalInternSymbol(name);
            }
            remap[origId] = rid;
        }
    }
    if (dbg) { breakdown().symbolTableNs += nowNs() - t0; t0 = nowNs(); }

    // Schema 14 — Section: sparse posTable.  Mirrors symbolTable.
    // Build a `posRemap[maxId+1]` vector mapping writer-PosIdx →
    // reader-PosIdx.  Entries with present=0 (writer's
    // resolvePosSnapshot returned nullptr) map to 0 (the "no pos"
    // sentinel).  WS5-D2a: the borrow path prefers the writer's PosIdx
    // (seedPosSnapshotAt) and tracks identity for the code-borrow decision.
    bool posIdentity = allowBorrow;
    std::vector<uint32_t> posRemap;
    {
        uint32_t n = r.u32();
        uint32_t maxId = r.u32();
        // B2: same overflow + DoS guard as the symbolTable maxId above.
        if (maxId > kMaxRemapId)
            throw SerializationError(
                "v3 deserialize: posTable maxId absurdly large");
        posRemap.assign(size_t(maxId) + 1, 0u);
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t origId = r.u32();
            uint8_t hasPS = r.u8();
            if (origId > maxId)
                throw SerializationError(
                    "v3 deserialize: posTable entry origId > maxId");
            if (hasPS) {
                PosSnapshot ps;
                ps.file = std::string(r.strv());
                ps.line = r.u32();
                ps.column = r.u32();
                uint32_t pid = allowBorrow
                    ? seedPosSnapshotAt(origId, std::move(ps))
                    : recordPosSnapshot(std::move(ps));
                posRemap[origId] = pid;
                if (allowBorrow && pid != origId) posIdentity = false;
            } else if (allowBorrow && origId != 0) {
                // Writer had an unresolved pos here; the owning path would
                // zero it, so a borrowed (un-rewritten) code word would
                // differ → conservatively force the code to be owned.
                posIdentity = false;
            }
            // hasPS == 0 leaves posRemap[origId] == 0.
        }
    }

    // Section: lambdas (schema 22, WS5-B2 D2b) — the self-relative POD block.
    // Read count + block length, 8-align, and TAKE (no copy) an in-blob
    // pointer to the block.  The borrow-vs-own decision is deferred to the end
    // (alongside `code`) since it shares the symbol/pos identity gate.
    uint32_t lamCount = r.u32();
    uint32_t lamBlockLen = r.u32();
    r.pad8();  // block starts at an 8-aligned blob offset (mirrors the writer)
    const void * lamPtr = r.takePtr(lamBlockLen);
    // B1: `lamCount` and `lamBlockLen` are read independently; only lamBlockLen
    // was bounds-checked (takePtr, above).  The fixed-stride descriptor array
    // [lamCount × sizeof(LambdaDescriptor)] must fit within the block before
    // either path indexes descs[i] for i<lamCount (borrow path via end() =
    // data()+count_, owned path in loadBuildFromBlock).  A header-valid blob
    // with e.g. lamCount=1000, lamBlockLen=8 would otherwise OOB-read.  The
    // product is computed in size_t (no uint32 overflow on 64-bit).
    if (lamBlockLen < static_cast<size_t>(lamCount) * sizeof(LambdaDescriptor))
        throw SerializationError(
            "v3 deserialize: lambda block too small for lamCount descriptors");
    if (dbg) { breakdown().lambdasNs += nowNs() - t0; t0 = nowNs(); }

    // (lambdaCodeOffsets was read from the WS5-D2a POD block above.)

    // Section: primops (resolve by name).  #777 (2026-05-23): zero-
    // copy lookup — findPrimOp() accepts string_view, so no need to
    // materialise a std::string per primop.
    {
        uint32_t n = r.u32();
        // B4: each primop is at least a 4-byte name-length prefix on the wire.
        if (n > r.remaining() / 4)
            throw SerializationError(
                "v3 deserialize: primops count exceeds remaining blob");
        cu.primops.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            std::string_view name = r.strv();
            if (name.empty()) {
                throw SerializationError(
                    "v3 deserialize: primop slot has empty name");
            }
            const PrimOp * po = findPrimOp(name);
            if (!po) {
                throw SerializationError(
                    "v3 deserialize: unknown primop '"
                    + std::string(name) + "'");
            }
            cu.primops.push_back(po);
        }
    }
    if (dbg) { breakdown().primopsNs += nowNs() - t0; t0 = nowNs(); }

    // CACHE-COHERENCE-EXEMPT: WS5-D1 pure refactor — the attrSelectCache /
    // recSlotCache vectors moved from `CompilationUnit` to `CompilationUnit::rt`
    // (per-process runtime side state), but the ON-DISK format is byte-identical:
    // the same u32 IC *sizes* are written/read and the entries are still zeroed
    // on load (never serialized).  No field added/removed/reordered in the disk
    // stream → no kSchemaVersion bump.
    // Section: attrSelectCache size (zeroed entries on load).
    {
        uint32_t n = r.u32();
        // B4: these caches carry NO per-entry wire bytes (entries are zeroed on
        // load), so they can't be bounded by remaining blob bytes.  But every
        // IC entry is referenced by an emitted instruction (an OP_ATTRS_SELECT
        // plus its IC follow-up word), so the count can never exceed the code
        // word count; a larger `n` is corruption → reject before a multi-GB
        // resize().  (codeCount was itself bounds-checked against the blob via
        // takePtr above.)
        if (n > codeCount)
            throw SerializationError(
                "v3 deserialize: attrSelectCache count exceeds code size");
        cu.rt.attrSelectCache.resize(n);
    }

    // Section: recSlotCache size (#779 Schema 10; zeroed on load).
    {
        uint32_t n = r.u32();
        // B4: same invariant — one IC entry per emitted instruction site.
        if (n > codeCount)
            throw SerializationError(
                "v3 deserialize: recSlotCache count exceeds code size");
        cu.rt.recSlotCache.resize(n);
    }

    // Section: entryOffset.
    cu.entryOffset = r.u32();

    if (r.pos != blob.size())
        throw SerializationError(
            "v3 deserialize: trailing bytes after end-of-stream");
    if (dbg) { breakdown().miscNs += nowNs() - t0; t0 = nowNs(); }

    // WS5-D2a — materialise-or-borrow `code`.  The int/float/lco POD
    // sections were already borrowed/owned above; `code` is the only POD
    // section carrying per-process ids (SymbolId + PosIdx operands), so it
    // can be borrowed READ-ONLY only when BOTH remaps are the identity — i.e.
    // the writer's ids were free in this reader and got seeded in place (see
    // globalSeedSymbol / seedPosSnapshotAt).  In that case the operands are
    // already valid and the (shared, read-only) code pages need no rewrite.
    // Otherwise `code` must be rewritten, so we materialise a private copy
    // first and then remap it — NEVER writing through a borrowed span.
    //
    // SymbolId/PosIdx remapping: at serialize time the ids are the writer's
    // global ids; the remap tables above translate them to this reader's ids.
    // remapSymbolsInBytecode also re-sorts OP_ATTRS_REC_INIT trailers +
    // propagates the permutation to OP_ATTRS_REC_SET (a no-op under identity).
    const bool borrowCode = aligned && symIdentity && posIdentity;
    // WS5-B2 (D2b) — the lambda block borrows under the SAME symbol/pos
    // identity gate as `code` (all its ids — formals name/pos, selectorSym,
    // posHandle — are covered by the same remap tables), plus its own
    // alignment check (it lives at an 8-aligned blob offset; on the mmap that
    // is alignof(LambdaDescriptor)-aligned).
    const bool lamAligned = allowBorrow
        && (reinterpret_cast<uintptr_t>(lamPtr) % alignof(LambdaDescriptor) == 0);
    const bool borrowLambdas = lamAligned && symIdentity && posIdentity;
    if (allowBorrow) {
        ++g_borrowCUs;
        if (borrowCode) ++g_borrowCodeBorrowed; else ++g_borrowCodeOwned;
        if (aligned) ++g_borrowPodBorrowed;
        if (borrowLambdas) ++g_borrowLambdasBorrowed; else ++g_borrowLambdasOwned;
    }
    if (borrowCode) {
        cu.code.borrow(reinterpret_cast<const Instruction *>(codePtr),
                       codeCount);
    } else {
        ownPod(cu.code, codePtr, codeCount);
        remapSymbolsInBytecode(cu, remap);
        // Schema 14 — apply the PosIdx remap to the in-bytecode trailers
        // (paired with each name in OP_ATTRS_(LET_)REC_INIT and
        // OP_ATTRS_INIT-class opcodes).  Without this, cached PosIdx values
        // point into the writer's posSnapshotPool order — meaningless here.
        remapPositionsInBytecode(cu, posRemap);
    }
    // WS5-B2 (D2b) — materialise-or-borrow the lambda block.  On borrow
    // (identity) the writer's baked ids are already valid → the read-only
    // descriptor pages stay Shared_Clean.  Otherwise unflatten into staging,
    // remap every per-descriptor id (formals name/pos, selectorSym, posHandle),
    // re-sort formals by (remapped) SymbolId for the OP_CALL validation pass,
    // and re-pack the owned block.  Under identity the remap is a no-op and the
    // re-packed block is byte-for-byte the writer's — so the RESULT is the same
    // whether borrowed or owned (only sharing differs).
    if (borrowLambdas) {
        cu.lambdas.borrowBlock(reinterpret_cast<const uint8_t *>(lamPtr),
                               lamBlockLen, lamCount);
    } else {
        cu.lambdas.loadBuildFromBlock(
            reinterpret_cast<const uint8_t *>(lamPtr), lamBlockLen, lamCount);
        for (auto & b : cu.lambdas.build_) {
            for (auto & f : b.formals) {
                if (f.name < remap.size())    f.name = remap[f.name];
                if (f.pos  < posRemap.size()) f.pos  = posRemap[f.pos];
            }
            if (b.formals.size() > 1) {
                std::sort(b.formals.begin(), b.formals.end(),
                    [](const LambdaDescriptor::Formal & a,
                       const LambdaDescriptor::Formal & c) {
                        return a.name < c.name;
                    });
            }
            if (b.selectorSym != 0 && b.selectorSym < remap.size())
                b.selectorSym = remap[b.selectorSym];
            if (b.posHandle < posRemap.size())
                b.posHandle = posRemap[b.posHandle];
        }
        cu.lambdas.finalize();
    }
    if (dbg) { breakdown().remapNs += nowNs() - t0; }
    // #770b/#770c (2026-05-22): cu.symbolTable was already kept
    // empty (we never populated it on deserialize; the section is
    // consumed directly into the remap table by zero-copy interning).
    // Every VM-side SymbolId lookup goes through ir::globalSymbolTable()
    // directly (vm.cc:905, 1262, 1357, 1597, etc.).  No clear/
    // shrink_to_fit needed.

    return cu;
}

CompilationUnit deserializeCU(std::string_view blob)
{
    // OWNING path — copy every section; safe for transient blobs (SQLite,
    // smoke round-trips).
    return deserializeImpl(blob, /*allowBorrow=*/false);
}

CompilationUnit deserializeCUBorrowed(std::string_view blob)
{
    // AOT-BORROW path — `blob` must live in the process-lifetime mmap.
    return deserializeImpl(blob, /*allowBorrow=*/true);
}

DeserializeBreakdownSnapshot deserializeBreakdown()
{
    const auto & b = breakdown();
    return {
        b.headerNs, b.codeNs, b.intConstantsNs, b.floatConstantsNs,
        b.stringConstantsNs, b.symbolTableNs, b.lambdasNs,
        b.lambdaCodeOffsetsNs, b.primopsNs, b.miscNs, b.remapNs,
        b.calls
    };
}

bool deserializeBreakdownEnabled() { return breakdownEnabled(); }

AotBorrowStats aotBorrowStats() noexcept
{
    return { g_borrowCUs, g_borrowCodeBorrowed, g_borrowCodeOwned,
             g_borrowPodBorrowed, g_borrowLambdasBorrowed, g_borrowLambdasOwned };
}

BlobMaxIds peekMaxIds(std::string_view blob) noexcept
{
    // Parse only far enough to read the sparse symbolTable + posTable maxId
    // fields (schema 21 layout): header, POD block (sized from its counts),
    // stringConstants (variable — must be walked), then symbolTable and
    // posTable each start with (count:u32, maxId:u32).  Any malformed input
    // returns {0,0} so the caller safely skips reservation.
    try {
        Reader r{blob};
        char magic[sizeof(kMagic)];
        r.readBytes(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return {};
        if (r.u32() != kSchemaVersion) return {};   // schema
        r.u64();                                     // fingerprint
        uint32_t podBlockOff = r.u32();
        if (podBlockOff < r.pos) return {};
        r.pos = podBlockOff;
        uint32_t codeCount  = r.u32();
        uint32_t intCount   = r.u32();
        uint32_t floatCount = r.u32();
        uint32_t lcoCount   = r.u32();
        // Skip the four POD arrays.
        r.takePtr((size_t)intCount   * sizeof(int64_t));
        r.takePtr((size_t)floatCount * sizeof(double));
        r.takePtr((size_t)codeCount  * sizeof(Instruction));
        r.takePtr((size_t)lcoCount   * sizeof(uint32_t));
        // Skip stringConstants (count + (len+bytes)*).
        uint32_t nStr = r.u32();
        for (uint32_t i = 0; i < nStr; ++i) (void)r.strv();
        // symbolTable: count, maxId, then `count` (origId:u32, name:str) pairs.
        uint32_t nSym = r.u32();
        uint32_t maxSym = r.u32();
        for (uint32_t i = 0; i < nSym; ++i) { r.u32(); (void)r.strv(); }
        // posTable: count, maxId (that's all we need).
        r.u32();                       // pos count
        uint32_t maxPos = r.u32();
        return { maxSym, maxPos };
    } catch (...) {
        return {};
    }
}

bool readSparseTables(std::string_view blob,
                      std::vector<BlobSymEntry> & syms,
                      std::vector<BlobPosEntry> & poss) noexcept
{
    // WS5-B2 — walk the same schema-21 prefix as peekMaxIds, but emit EVERY
    // sparse symbol/pos entry (not just the maxId).  `name`/`file` are views
    // into `blob` (AOT mmap lifetime).  Mirrors the writer sections in
    // serializeCU (symbolTable then posTable) and the reader in
    // deserializeImpl.  Best-effort: any malformed input → false + empty out.
    syms.clear();
    poss.clear();
    try {
        Reader r{blob};
        char magic[sizeof(kMagic)];
        r.readBytes(magic, sizeof(magic));
        if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) return false;
        if (r.u32() != kSchemaVersion) return false;   // schema
        r.u64();                                        // fingerprint
        uint32_t podBlockOff = r.u32();
        if (podBlockOff < r.pos) return false;
        r.pos = podBlockOff;
        uint32_t codeCount  = r.u32();
        uint32_t intCount   = r.u32();
        uint32_t floatCount = r.u32();
        uint32_t lcoCount   = r.u32();
        r.takePtr((size_t)intCount   * sizeof(int64_t));
        r.takePtr((size_t)floatCount * sizeof(double));
        r.takePtr((size_t)codeCount  * sizeof(Instruction));
        r.takePtr((size_t)lcoCount   * sizeof(uint32_t));
        // stringConstants (skip).
        uint32_t nStr = r.u32();
        for (uint32_t i = 0; i < nStr; ++i) (void)r.strv();
        // symbolTable: count, maxId, then `count` (origId:u32, name:str) pairs.
        uint32_t nSym = r.u32();
        r.u32();                       // maxSym (unused here)
        syms.reserve(nSym);
        for (uint32_t i = 0; i < nSym; ++i) {
            uint32_t id = r.u32();
            std::string_view name = r.strv();
            syms.push_back({ id, name });
        }
        // posTable: count, maxId, then `count`
        //   (origId:u32, present:u8, [file:str, line:u32, col:u32]) entries.
        uint32_t nPos = r.u32();
        r.u32();                       // maxPos (unused here)
        poss.reserve(nPos);
        for (uint32_t i = 0; i < nPos; ++i) {
            uint32_t id = r.u32();
            uint8_t present = r.u8();
            if (present) {
                std::string_view file = r.strv();
                uint32_t line = r.u32();
                uint32_t col  = r.u32();
                poss.push_back({ id, file, line, col });
            }
        }
        return true;
    } catch (...) {
        syms.clear();
        poss.clear();
        return false;
    }
}

} // namespace nix::v3::serialize
