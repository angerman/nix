/// @file
/// v3 CompilationUnit serialization.  See include/v3/serialize.hh.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/serialize.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nix::v3::serialize {

namespace {

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
};

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
            {"OP_LIST_INIT",       OP_LIST_INIT},
            {"OP_LIST_CONCAT",     OP_LIST_CONCAT},
            {"OP_ATTRS_INIT",      OP_ATTRS_INIT},
            {"OP_ATTRS_INIT_DYN",  OP_ATTRS_INIT_DYN},
            {"OP_ATTRS_REC_INIT",  OP_ATTRS_REC_INIT},
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
/// OP_ATTRS_REC_INIT) using the supplied remap table.
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
        } else if (op == OP_ATTRS_REC_INIT) {
            // Remap names + remember their old positions so we can
            // re-sort and propagate the permutation to the matching
            // OP_ATTRS_REC_SETs.
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
        } else if (op == OP_CALL_PRIMOP) {
            ip++;  // primop-index follow-up
        } else if (op == OP_MAKE_CLOSURE || op == OP_MAKE_THUNK) {
            ip++;  // nUpvalues follow-up
        }
        // All other opcodes either have no trailing data or no
        // SymbolIds in their data; leave ip alone.
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

    // Section: symbolTable.
    w.u32(static_cast<uint32_t>(cu.symbolTable.size()));
    for (auto & s : cu.symbolTable) w.str(s);

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

    // Section: entryOffset.
    w.u32(cu.entryOffset);

    return out;
}

CompilationUnit deserializeCU(std::string_view blob)
{
    Reader r{blob};

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

    // Section: code.
    {
        uint32_t n = r.u32();
        cu.code.resize(n);
        r.readBytes(cu.code.data(), n * sizeof(uint32_t));
    }

    // Section: intConstants.
    {
        uint32_t n = r.u32();
        cu.intConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.intConstants.push_back(r.i64());
    }

    // Section: floatConstants.
    {
        uint32_t n = r.u32();
        cu.floatConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.floatConstants.push_back(r.f64());
    }

    // Section: stringConstants.
    {
        uint32_t n = r.u32();
        cu.stringConstants.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.stringConstants.push_back(r.str());
    }

    // Section: symbolTable.
    {
        uint32_t n = r.u32();
        cu.symbolTable.reserve(n);
        for (uint32_t i = 0; i < n; ++i) cu.symbolTable.push_back(r.str());
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
            cu.lambdas.push_back(std::move(l));
        }
    }

    // Section: lambdaCodeOffsets.
    {
        uint32_t n = r.u32();
        cu.lambdaCodeOffsets.resize(n);
        r.readBytes(cu.lambdaCodeOffsets.data(), n * sizeof(uint32_t));
    }

    // Section: primops (resolve by name).
    {
        uint32_t n = r.u32();
        cu.primops.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            std::string name = r.str();
            if (name.empty()) {
                throw SerializationError(
                    "v3 deserialize: primop slot has empty name");
            }
            const PrimOp * po = findPrimOp(name);
            if (!po) {
                throw SerializationError(
                    "v3 deserialize: unknown primop '" + name + "'");
            }
            cu.primops.push_back(po);
        }
    }

    // Section: attrSelectCache size (zeroed entries on load).
    {
        uint32_t n = r.u32();
        cu.attrSelectCache.resize(n);
    }

    // Section: entryOffset.
    cu.entryOffset = r.u32();

    if (r.pos != blob.size())
        throw SerializationError(
            "v3 deserialize: trailing bytes after end-of-stream");

    // SymbolId remapping.  At serialize time, cu.symbolTable was a
    // snapshot of the global table (cu.symbolTable[i] == name of
    // global SymbolId i).  At deserialize, the global table may
    // have a different layout — we re-resolve every name through
    // globalInternSymbol() and walk the bytecode + lambdas to
    // rewrite stale SymbolIds.
    std::vector<uint32_t> remap;
    remap.reserve(cu.symbolTable.size());
    for (auto & name : cu.symbolTable) {
        remap.push_back(name.empty()
            ? uint32_t{0}  // sentinel for ""
            : ir::globalInternSymbol(name));
    }
    remapSymbolsInBytecode(cu, remap);
    for (auto & l : cu.lambdas) {
        for (auto & f : l.formals) {
            if (f.name < remap.size()) f.name = remap[f.name];
        }
    }
    // Now overwrite cu.symbolTable with the global table so that
    // any code that later looks up cu.symbolTable[id] (e.g. error
    // messages) sees the correct names for the remapped IDs.
    cu.symbolTable = ir::globalSymbolTable();

    return cu;
}

} // namespace nix::v3::serialize
