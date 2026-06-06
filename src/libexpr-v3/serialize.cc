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
         || op == OP_GET_UPVALUE_REC_BINDING) {
            bump(operand);
            // OP_ATTRS_SELECT has 1 IC follow-up word.
            // #779 Schema 10: OP_REC_BINDING_SLOT_REF also has 1 IC
            // follow-up word.
            if (op == OP_ATTRS_SELECT
             || op == OP_REC_BINDING_SLOT_REF) ++ip;
            // §2(b): OP_GET_UPVALUE_REC_BINDING carries [upvalIdx, icIdx]
            // (2 trailing words); its operand is the looked-up SymbolId.
            else if (op == OP_GET_UPVALUE_REC_BINDING) ip += 2;
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
         || op == OP_WITH_LOOKUP) {
            // No trailer.
        } else if (op == OP_ATTRS_SELECT
                || op == OP_REC_BINDING_SLOT_REF) {
            ++ip;  // 1 IC follow-up word
        } else if (op == OP_GET_UPVALUE_REC_BINDING) {
            ip += 2;  // §2(b): [upvalIdx, icIdx] — no PosIdx in trailer
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

    // Section: code.
    w.u32(static_cast<uint32_t>(cu.code.size()));
    w.writeBytes(cu.code.data(), cu.code.size() * sizeof(uint32_t));

    // Section: intConstants.
    w.u32(static_cast<uint32_t>(cu.intConstants.size()));
    for (auto v : cu.intConstants) w.i64(v);

    // Section: floatConstants.
    w.u32(static_cast<uint32_t>(cu.floatConstants.size()));
    for (auto v : cu.floatConstants) w.f64(v);

    // Section: stringConstants.
    w.u32(static_cast<uint32_t>(cu.stringConstants.size()));
    for (auto & s : cu.stringConstants) w.str(s);

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
    }

    // Section: lambdaCodeOffsets.
    w.u32(static_cast<uint32_t>(cu.lambdaCodeOffsets.size()));
    w.writeBytes(cu.lambdaCodeOffsets.data(),
                  cu.lambdaCodeOffsets.size() * sizeof(uint32_t));

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
    w.u32(static_cast<uint32_t>(cu.attrSelectCache.size()));

    // Section: recSlotCache size (#779 Schema 10; entries zeroed on load).
    w.u32(static_cast<uint32_t>(cu.recSlotCache.size()));

    // Section: entryOffset.
    w.u32(cu.entryOffset);

    return out;
}

CompilationUnit deserializeCU(std::string_view blob)
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

    // Section: code.
    {
        uint32_t n = r.u32();
        cu.code.resize(n);
        r.readBytes(cu.code.data(), n * sizeof(uint32_t));
    }
    if (dbg) { breakdown().codeNs += nowNs() - t0; t0 = nowNs(); }

    // Section: intConstants.
    {
        uint32_t n = r.u32();
        cu.intConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.intConstants.push_back(r.i64());
    }
    if (dbg) { breakdown().intConstantsNs += nowNs() - t0; t0 = nowNs(); }

    // Section: floatConstants.
    {
        uint32_t n = r.u32();
        cu.floatConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.floatConstants.push_back(r.f64());
    }
    if (dbg) { breakdown().floatConstantsNs += nowNs() - t0; t0 = nowNs(); }

    // Section: stringConstants.
    {
        uint32_t n = r.u32();
        cu.stringConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.stringConstants.push_back(r.str());
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
            remap[origId] = name.empty()
                ? uint32_t{0}
                : ir::globalInternSymbol(name);
        }
    }
    if (dbg) { breakdown().symbolTableNs += nowNs() - t0; t0 = nowNs(); }

    // Schema 14 — Section: sparse posTable.  Mirrors symbolTable.
    // Build a `posRemap[maxId+1]` vector mapping writer-PosIdx →
    // reader-PosIdx via `recordPosSnapshot`.  Entries with
    // present=0 (writer's resolvePosSnapshot returned nullptr) map
    // to 0 (the "no pos" sentinel).
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
                posRemap[origId] = recordPosSnapshot(std::move(ps));
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
            cu.lambdas.push_back(std::move(l));
        }
    }
    if (dbg) { breakdown().lambdasNs += nowNs() - t0; t0 = nowNs(); }

    // Section: lambdaCodeOffsets.
    {
        uint32_t n = r.u32();
        cu.lambdaCodeOffsets.resize(n);
        r.readBytes(cu.lambdaCodeOffsets.data(), n * sizeof(uint32_t));
    }
    if (dbg) { breakdown().lambdaCodeOffsetsNs += nowNs() - t0; t0 = nowNs(); }

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

    // Section: attrSelectCache size (zeroed entries on load).
    {
        uint32_t n = r.u32();
        cu.attrSelectCache.resize(n);
    }

    // Section: recSlotCache size (#779 Schema 10; zeroed on load).
    {
        uint32_t n = r.u32();
        cu.recSlotCache.resize(n);
    }

    // Section: entryOffset.
    cu.entryOffset = r.u32();

    if (r.pos != blob.size())
        throw SerializationError(
            "v3 deserialize: trailing bytes after end-of-stream");
    if (dbg) { breakdown().miscNs += nowNs() - t0; t0 = nowNs(); }

    // SymbolId remapping.  At serialize time, the symbolTable was a
    // snapshot of the global table (entry i == name of global
    // SymbolId i).  The remap table above was built directly from
    // string_views into the blob — no intermediate std::strings
    // materialised.  Walk the bytecode + lambdas to rewrite stale
    // SymbolIds.
    remapSymbolsInBytecode(cu, remap);
    // Schema 14 — apply the PosIdx remap to the in-bytecode trailers
    // (paired with each name in OP_ATTRS_(LET_)REC_INIT and
    // OP_ATTRS_INIT-class opcodes) AND to Formal::pos.  Without this
    // step, cached PosIdx values point into the writer's
    // posSnapshotPool order — meaningless in the reader process.
    // Closes the positional-only DIFF class captured by
    // run-r1-trigger-verify.sh (commit dcfbae871).
    remapPositionsInBytecode(cu, posRemap);
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

} // namespace nix::v3::serialize
