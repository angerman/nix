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
};

/// Tiny streaming reader from a string_view.  Throws on
/// out-of-bounds reads — the format is fixed-layout so we know
/// the exact byte counts at every step.
struct Reader {
    std::string_view buf;
    size_t pos = 0;
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

    // Formals carry PosIdx per parameter.
    for (const auto & l : cu.lambdas) {
        for (const auto & f : l.formals) bump(f.pos);
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

    // Section: lambdas (LambdaDescriptor with Formal vector).
    w.u32(static_cast<uint32_t>(cu.lambdas.size()));
    for (auto & l : cu.lambdas) {
        w.u32(l.codeOffset);
        w.u32(l.prologueOffset);
        w.u32(l.nUpvalues);
        w.u32(l.nLocals);
        w.u8(l.arity);
        w.u8(l.hasFormals);
        w.u8(l.ellipsis);
        w.u8(0);  // _pad
        // Schema 5 (#530): per-descriptor with-target count.
        w.u32(l.nWithTargets);
        w.u32(static_cast<uint32_t>(l.formals.size()));
        for (auto & f : l.formals) {
            w.u32(f.name);
            w.u8(f.hasDefault ? 1 : 0);
            w.u8(0); w.u8(0); w.u8(0);  // pad to align pos
            w.u32(f.pos);
        }
        // Schema 4 (#495/#509 STG-13d): native-intrinsic metadata.
        // Carries kind + upvalue indices for ExtendsBody / ComposeBody
        // dispatch through the disk cache so cache-loaded CUs participate
        // in native dispatch instead of running the bytecode body.
        w.u8(static_cast<uint8_t>(l.intrinsicKind));
        w.u8(static_cast<uint8_t>(l.intrinsicVar0));  // signed int8 reinterpreted
        w.u8(static_cast<uint8_t>(l.intrinsicVar1));
        w.u8(static_cast<uint8_t>(l.intrinsicVar2));
        // Schema 11 (#803): diagnostic metadata.  Without these, cache-
        // loaded CUs show "anonymous lambda" + src=?:0:0 in error
        // messages, hiding which source file the lambda came from.
        // Required for the H10 RCA on haskell.nix-class workloads.
        w.str(l.name);
        w.str(l.contextualName);
        // posHandle is an index into the process-static posSnapshotPool;
        // we serialise the RESOLVED file/line/column so the load-time
        // process can rebuild a fresh posHandle from its own pool.
        const PosSnapshot * ps = resolvePosSnapshot(l.posHandle);
        if (ps) {
            w.u8(1);
            w.str(ps->file);
            w.u32(ps->line);
            w.u32(ps->column);
        } else {
            w.u8(0);
        }
        // Schema 12 (#814): emit-time peephole flags.  See
        // serialize.hh kSchemaVersion comment for rationale.
        w.u32(l.selectorSym);
        w.u8(l.identityLambda ? 1 : 0);
        w.u8(l.secondArgIdentityLambda ? 1 : 0);
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
        remap.assign(maxId + 1, 0u);
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
        posRemap.assign(maxId + 1, 0u);
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

    // Section: lambdas.
    {
        uint32_t n = r.u32();
        cu.lambdas.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            LambdaDescriptor l;
            l.codeOffset    = r.u32();
            l.prologueOffset = r.u32();
            l.nUpvalues     = static_cast<uint16_t>(r.u32());
            l.nLocals       = static_cast<uint16_t>(r.u32());
            l.arity         = r.u8();
            l.hasFormals    = r.u8();
            l.ellipsis      = r.u8();
            r.u8();  // _pad
            // Schema 5 (#530): per-descriptor with-target count.
            l.nWithTargets  = static_cast<uint16_t>(r.u32());
            uint32_t nFormals = r.u32();
            l.formals.reserve(nFormals);
            for (uint32_t j = 0; j < nFormals; ++j) {
                LambdaDescriptor::Formal f;
                f.name       = r.u32();
                f.hasDefault = r.u8() != 0;
                r.u8(); r.u8(); r.u8();  // pad
                f.pos        = r.u32();
                l.formals.push_back(f);
            }
            // Schema 4 (#495/#509 STG-13d): native-intrinsic metadata.
            l.intrinsicKind = static_cast<LambdaDescriptor::Intrinsic>(r.u8());
            l.intrinsicVar0 = static_cast<int8_t>(r.u8());
            l.intrinsicVar1 = static_cast<int8_t>(r.u8());
            l.intrinsicVar2 = static_cast<int8_t>(r.u8());
            // Schema 11 (#803): diagnostic metadata.
            l.name           = std::string(r.strv());
            l.contextualName = std::string(r.strv());
            if (r.u8()) {
                PosSnapshot ps;
                ps.file   = std::string(r.strv());
                ps.line   = r.u32();
                ps.column = r.u32();
                l.posHandle = recordPosSnapshot(std::move(ps));
            }
            // Schema 12 (#814): emit-time peephole flags.
            l.selectorSym    = r.u32();
            l.identityLambda = (r.u8() != 0);
            l.secondArgIdentityLambda = (r.u8() != 0);
            cu.lambdas.push_back(std::move(l));
        }
    }
    if (dbg) { breakdown().lambdasNs += nowNs() - t0; t0 = nowNs(); }

    // (lambdaCodeOffsets was read from the WS5-D2a POD block above.)

    // Section: primops (resolve by name).  #777 (2026-05-23): zero-
    // copy lookup — findPrimOp() accepts string_view, so no need to
    // materialise a std::string per primop.
    {
        uint32_t n = r.u32();
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
        cu.rt.attrSelectCache.resize(n);
    }

    // Section: recSlotCache size (#779 Schema 10; zeroed on load).
    {
        uint32_t n = r.u32();
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
    if (allowBorrow) {
        ++g_borrowCUs;
        if (borrowCode) ++g_borrowCodeBorrowed; else ++g_borrowCodeOwned;
        if (aligned) ++g_borrowPodBorrowed;
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
    // Formals live in the OWNED `lambdas` vector, so they are always
    // rewritten — a no-op under identity (remap[i]==i, stable re-sort).
    for (auto & l : cu.lambdas) {
        for (auto & f : l.formals) {
            if (f.name < remap.size()) f.name = remap[f.name];
            if (f.pos < posRemap.size()) f.pos = posRemap[f.pos];
        }
        // Schema 12 (#814): formals are sorted by SymbolId for the
        // OP_CALL formals validation pass.  After cross-process
        // remap the SymbolIds change, so the original sort may be
        // invalidated.  Re-sort here to restore the invariant.
        // hasDefault + pos travel with name; std::sort with a
        // lambda comparing names handles the permutation.  In-
        // process remap is identity, so this is a no-op cost on
        // the common path.
        if (l.formals.size() > 1) {
            std::sort(l.formals.begin(), l.formals.end(),
                [](const LambdaDescriptor::Formal & a,
                   const LambdaDescriptor::Formal & b) {
                    return a.name < b.name;
                });
        }
        // Schema 12 (#814): selectorSym is a SymbolId stored on
        // LambdaDescriptor (not in the bytecode), referenced by the
        // emit-time peephole fast path in OP_CALL.  Without remap, a
        // cross-process cache hit looks up the writer's SymbolId in
        // the reader's attrset, producing "missing attr" errors on
        // the firefox-class overlay workload.
        if (l.selectorSym != 0 && l.selectorSym < remap.size())
            l.selectorSym = remap[l.selectorSym];
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
             g_borrowPodBorrowed };
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

} // namespace nix::v3::serialize
