/// @file
/// v3 bytecode disassembler.
///
/// Originally "WC-32 minimal" (eval-order-divergence debugging); upgraded
/// 2026-06-05 to a high-quality disassembler:
///   - per-function framing (`; func N "name" arity=A nUp=U nLocals=L entry=E`),
///     segmented via `CompilationUnit::lambdaCodeOffsets`;
///   - operand resolution — literals show their VALUE (`OP_LIT_INT ; = 42`,
///     `OP_LIT_STR ; "hi"`), `OP_*_PRIMOP` show the primop NAME, `OP_MAKE_*`
///     show the target function name, attr ops show the symbol, `OP_POS`
///     shows `file:line:col`;
///   - symbolic jump labels (`OP_BRANCH_FALSE ; -> L53` + an `L53:` leader),
///     making control flow readable AND `v3-check` CHECK-LABEL-friendly.
///
/// Drives `NIX_V3_EMIT_BYTECODE` + `v3-eval --emit-bytecode`, crash context
/// (`limits.cc`), and the #815 cached-vs-fresh cache-coherence RCA.
///
/// `disassembleOne` is self-contained (resolution needs only the CU) so all
/// callers benefit; `disassembleModule` adds the function framing + labels.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/disasm.hh"
#include "v3/bytecode.hh"
#include "v3/closure.hh"   // LambdaDescriptor (function metadata)
#include "v3/primop.hh"    // PrimOp::name (CALL_PRIMOP / LIT_PRIMOP resolution)
#include "v3/ir.hh"        // globalSymbolTable (SymbolId -> name)
#include "v3/alloc.hh"     // resolvePosSnapshot / PosSnapshot (OP_POS)

#include <algorithm>
#include <cstring>
#include <vector>

namespace nix::v3 {

const char * opName(Op op)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    case OP_LIT_INT:           return "OP_LIT_INT";
    case OP_LIT_INT_BIG:       return "OP_LIT_INT_BIG";
    case OP_LIT_FLOAT:         return "OP_LIT_FLOAT";
    case OP_LIT_STR:           return "OP_LIT_STR";
    case OP_LIT_PATH:          return "OP_LIT_PATH";
    case OP_LIT_TRUE:          return "OP_LIT_TRUE";
    case OP_LIT_FALSE:         return "OP_LIT_FALSE";
    case OP_LIT_NULL:          return "OP_LIT_NULL";
    case OP_GET_LOCAL:         return "OP_GET_LOCAL";
    case OP_SET_LOCAL:         return "OP_SET_LOCAL";
    case OP_GET_UPVALUE:       return "OP_GET_UPVALUE";
    case OP_DUP:               return "OP_DUP";
    case OP_POP:               return "OP_POP";
    case OP_SWAP:              return "OP_SWAP";
    case OP_ADD:               return "OP_ADD";
    case OP_SUB:               return "OP_SUB";
    case OP_MUL:               return "OP_MUL";
    case OP_DIV:               return "OP_DIV";
    case OP_NEGATE:            return "OP_NEGATE";
    case OP_EQ:                return "OP_EQ";
    case OP_NEQ:               return "OP_NEQ";
    case OP_LESS:              return "OP_LESS";
    case OP_NOT:               return "OP_NOT";
    case OP_AND_BRANCH:        return "OP_AND_BRANCH";
    case OP_OR_BRANCH:         return "OP_OR_BRANCH";
    case OP_IMPL_BRANCH:       return "OP_IMPL_BRANCH";
    case OP_JUMP:              return "OP_JUMP";
    case OP_BRANCH_FALSE:      return "OP_BRANCH_FALSE";
    case OP_BRANCH_TRUE:       return "OP_BRANCH_TRUE";
    case OP_MAKE_CLOSURE:      return "OP_MAKE_CLOSURE";
    case OP_MAKE_THUNK:        return "OP_MAKE_THUNK";
    case OP_CALL:              return "OP_CALL";
    case OP_RETURN:            return "OP_RETURN";
    case OP_FORCE:             return "OP_FORCE";
    case OP_GET_LOCAL_FORCE:   return "OP_GET_LOCAL_FORCE";
    case OP_GET_UPVALUE_FORCE: return "OP_GET_UPVALUE_FORCE";
    case OP_TAIL_CALL:         return "OP_TAIL_CALL";
    case OP_CALL_N:            return "OP_CALL_N";
    case OP_TAIL_CALL_N:       return "OP_TAIL_CALL_N";
    case OP_SET_LOCAL_KEEP:    return "OP_SET_LOCAL_KEEP";
    case OP_LIST_INIT:         return "OP_LIST_INIT";
    case OP_LIST_CONCAT:       return "OP_LIST_CONCAT";
    case OP_ATTRS_INIT:        return "OP_ATTRS_INIT";
    case OP_ATTRS_INIT_DYN:    return "OP_ATTRS_INIT_DYN";
    case OP_ATTRS_REC_INIT:    return "OP_ATTRS_REC_INIT";
    case OP_ATTRS_LET_REC_INIT: return "OP_ATTRS_LET_REC_INIT";
    case OP_ATTRS_REC_INIT_TAIL: return "OP_ATTRS_REC_INIT_TAIL";
    case OP_ATTRS_REC_SET:     return "OP_ATTRS_REC_SET";
    case OP_ATTRS_SELECT:      return "OP_ATTRS_SELECT";
    case OP_ATTRS_SELECT_DYN:  return "OP_ATTRS_SELECT_DYN";
    case OP_ATTRS_HAS:         return "OP_ATTRS_HAS";
    case OP_ATTRS_HAS_DYN:     return "OP_ATTRS_HAS_DYN";
    case OP_ATTRS_UPDATE:      return "OP_ATTRS_UPDATE";
    case OP_ATTRS_UPDATE_TAIL: return "OP_ATTRS_UPDATE_TAIL";
    case OP_IFD_PROBE:         return "OP_IFD_PROBE";
    case OP_REC_BINDING_SLOT_REF: return "OP_REC_BINDING_SLOT_REF";
    case OP_REC_SLOT_PUBLISH:  return "OP_REC_SLOT_PUBLISH";
    case OP_THUNK_SET_LOCAL_THROUGH_CELL: return "OP_THUNK_SET_LOCAL_THROUGH_CELL";
    case OP_APPLY_OVERRIDES:   return "OP_APPLY_OVERRIDES";
    case OP_WITH_PUSH:         return "OP_WITH_PUSH";
    case OP_WITH_POP:          return "OP_WITH_POP";
    case OP_WITH_LOOKUP:       return "OP_WITH_LOOKUP";
    case OP_STR_CONCAT:        return "OP_STR_CONCAT";
    case OP_ASSERT:            return "OP_ASSERT";
    case OP_POS:               return "OP_POS";
    case OP_CALL_PRIMOP:       return "OP_CALL_PRIMOP";
    case OP_LIT_PRIMOP:        return "OP_LIT_PRIMOP";
    case OP_LIT_BUILTINS:      return "OP_LIT_BUILTINS";
    case OP_IS_NULL:           return "OP_IS_NULL";
    case OP_IS_BOOL:           return "OP_IS_BOOL";
    case OP_IS_INT:            return "OP_IS_INT";
    case OP_IS_FLOAT:          return "OP_IS_FLOAT";
    case OP_IS_STRING:         return "OP_IS_STRING";
    case OP_IS_PATH:           return "OP_IS_PATH";
    case OP_IS_LIST:           return "OP_IS_LIST";
    case OP_IS_ATTRS:          return "OP_IS_ATTRS";
    case OP_IS_FUNCTION:       return "OP_IS_FUNCTION";
    case OP_HEAD:              return "OP_HEAD";
    case OP_TAIL:              return "OP_TAIL";
    case OP_LENGTH:            return "OP_LENGTH";
    case OP_ELEM_AT:           return "OP_ELEM_AT";
    case OP_HALT:              return "OP_HALT";
    default:                   return nullptr;
    }
#pragma clang diagnostic pop
}

/// Number of extra Instruction words this op consumes after the opcode
/// word.  Conservative: 0 when unknown (caller may dis-align but still
/// gets useful info up to that point).  This is the authoritative
/// instruction-width table — keep in sync with emit.cc / vm.cc.
static uint32_t opExtraWords(Op op, uint32_t operand,
                             const CompilationUnit & cu, uint32_t ip)
{
    (void)cu; (void)ip;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    case OP_MAKE_CLOSURE:
    case OP_MAKE_THUNK:
        return 2;                       // nUpvalues + nWithTargets (#530)
    case OP_ATTRS_INIT:
        return 2 * operand;             // n (name, pos) pairs
    case OP_ATTRS_INIT_DYN: {
        uint32_t nStatic = (operand >> 12) & 0xFFF;
        uint32_t nDyn = operand & 0xFFF;
        return 2 * nStatic + nDyn;
    }
    case OP_ATTRS_REC_INIT:
    case OP_ATTRS_LET_REC_INIT:
    case OP_ATTRS_REC_INIT_TAIL:
        return 2 * operand;             // n (SymbolId, PosIdx32) pairs
    case OP_ATTRS_SELECT:
        return 1;                       // inline-cache slot index
    case OP_REC_BINDING_SLOT_REF:
        return 1;                       // #779 Schema-10 IC follow-up
    case OP_CALL_PRIMOP:
        return 1;                       // primop-table index (poIdx)
    default:
        return 0;
    }
#pragma clang diagnostic pop
}

namespace {

bool isBranchOp(Op op)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    case OP_JUMP: case OP_BRANCH_FALSE: case OP_BRANCH_TRUE:
    case OP_AND_BRANCH: case OP_OR_BRANCH: case OP_IMPL_BRANCH:
        return true;
    default: return false;
    }
#pragma clang diagnostic pop
}

/// Global SymbolId -> name ("?" when out of range).
const char * symName(uint32_t sid)
{
    const auto & gst = ir::globalSymbolTable();
    return sid < gst.size() ? gst[sid].c_str() : "?";
}

/// Print a sanitized, truncated, quoted string literal (control chars
/// shown as '.', so a literal newline can't break the one-line format).
void printStrLit(std::FILE * out, const std::string & s, char q)
{
    std::fputc(q, out);
    size_t n = std::min<size_t>(s.size(), 48);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::fputc((c >= 32 && c < 127) ? (char)c : '.', out);
    }
    if (s.size() > 48) std::fputs("...", out);
    std::fputc(q == '"' ? '"' : '>', out);
}

} // namespace

uint32_t disassembleOne(std::FILE * out,
                        const CompilationUnit & cu,
                        uint32_t ip,
                        uint32_t recInitIp)
{
    if (ip >= cu.code.size()) {
        std::fprintf(out, "  [%u] <out-of-range>\n", ip);
        return ip;
    }
    Instruction inst = cu.code[ip];
    Op op = decodeOp(inst);
    uint32_t operand = decodeOperand(inst);
    const char * name = opName(op);
    if (!name) {
        std::fprintf(out, "  [%u] <unknown op=0x%02x> operand=%u\n",
            ip, (unsigned)op, operand);
        return ip + 1;
    }
    uint32_t extra = opExtraWords(op, operand, cu, ip);
    std::fprintf(out, "  [%u] %-24s operand=%u", ip, name, operand);

    // Raw data words (kept for completeness / byte-exact diffs).
    if (extra > 0) {
        std::fprintf(out, "  data=[");
        for (uint32_t i = 0; i < extra && (ip + 1 + i) < cu.code.size(); ++i) {
            if (i > 0) std::fprintf(out, ",");
            std::fprintf(out, "%u", cu.code[ip + 1 + i]);
        }
        std::fprintf(out, "]");
    }

    // Resolved annotation (`; ...`) — the high-quality part.
    auto dataAt = [&](uint32_t k) -> uint32_t {
        uint32_t a = ip + 1 + k;
        return a < cu.code.size() ? cu.code[a] : 0;
    };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    case OP_LIT_INT:
        std::fprintf(out, "   ; = %d", decodeSignedOperand(inst));
        break;
    case OP_LIT_INT_BIG:
        if (operand < cu.intConstants.size())
            std::fprintf(out, "   ; = %lld",
                static_cast<long long>(cu.intConstants[operand]));
        break;
    case OP_LIT_FLOAT:
        if (operand < cu.floatConstants.size())
            std::fprintf(out, "   ; = %g", cu.floatConstants[operand]);
        break;
    case OP_LIT_STR:
        if (operand < cu.stringConstants.size()) {
            std::fprintf(out, "   ; ");
            printStrLit(out, cu.stringConstants[operand], '"');
        }
        break;
    case OP_LIT_PATH:
        if (operand < cu.stringConstants.size()) {
            std::fprintf(out, "   ; ");
            printStrLit(out, cu.stringConstants[operand], '<');
        }
        break;
    case OP_LIT_PRIMOP:
        if (operand < cu.primops.size() && cu.primops[operand])
            std::fprintf(out, "   ; primop %.*s",
                (int)cu.primops[operand]->name.size(),
                cu.primops[operand]->name.data());
        break;
    case OP_CALL_PRIMOP: {
        uint32_t poIdx = dataAt(0);
        if (poIdx < cu.primops.size() && cu.primops[poIdx])
            std::fprintf(out, "   ; %.*s (%u arg%s)",
                (int)cu.primops[poIdx]->name.size(),
                cu.primops[poIdx]->name.data(),
                operand, operand == 1 ? "" : "s");
        break;
    }
    case OP_CALL_N:
    case OP_TAIL_CALL_N:
        // operand = saturated arg count; args are already on the stack and
        // the callee closure is dynamic, so there is no name to resolve —
        // surface the count so a bare `operand=3` reads as "3 args".
        std::fprintf(out, "   ; %u arg%s", operand, operand == 1 ? "" : "s");
        break;
    case OP_LIST_INIT:
        // operand = element count (emit.cc: e.elems.size()).
        std::fprintf(out, "   ; %u elem%s", operand, operand == 1 ? "" : "s");
        break;
    case OP_STR_CONCAT: {
        // operand packs (nParts << 1) | forceString (emit.cc), so the raw
        // value is misleading on its own (operand=4 is *2* parts) — decode.
        uint32_t nParts = operand >> 1;
        std::fprintf(out, "   ; %u part%s%s", nParts,
                     nParts == 1 ? "" : "s", (operand & 1u) ? ", force" : "");
        break;
    }
    case OP_ATTRS_INIT_DYN: {
        // operand packs (nStatic << 12) | nDyn (mirrors opExtraWords); the
        // packed value is opaque, and unlike OP_ATTRS_INIT/REC_INIT this op
        // was previously un-annotated — decode for consistency.
        uint32_t nStatic = (operand >> 12) & 0xFFF;
        uint32_t nDyn = operand & 0xFFF;
        std::fprintf(out, "   ; %u static + %u dyn", nStatic, nDyn);
        break;
    }
    case OP_MAKE_CLOSURE:
    case OP_MAKE_THUNK:
        if (operand < cu.lambdas.size()) {
            const std::string & fn = cu.lambdas[operand].name;
            if (!fn.empty()) std::fprintf(out, "   ; func \"%s\"", fn.c_str());
            else             std::fprintf(out, "   ; func %u", operand);
        }
        break;
    case OP_ATTRS_SELECT:
    case OP_ATTRS_SELECT_DYN:
    case OP_ATTRS_HAS:
    case OP_ATTRS_HAS_DYN:
    case OP_REC_BINDING_SLOT_REF:
    case OP_WITH_LOOKUP:
        std::fprintf(out, "   ; %s", symName(operand));
        break;
    case OP_ATTRS_INIT:
    case OP_ATTRS_REC_INIT:
    case OP_ATTRS_LET_REC_INIT:
    case OP_ATTRS_REC_INIT_TAIL:
        // operand = n; data = (SymbolId, posHandle) pairs.
        std::fprintf(out, "   ; {");
        for (uint32_t k = 0; k < operand; ++k) {
            if (k > 0) std::fprintf(out, ", ");
            std::fprintf(out, "%s", symName(dataAt(2 * k)));
        }
        std::fprintf(out, "}");
        break;
    case OP_ATTRS_REC_SET:
        // operand = sorted rank into the governing REC_INIT's symbol list
        // (emit.cc: symRank[i]); the init's trailer is SymbolId-sorted, so
        // the symbol at rank r is data[2*r].  disassembleModule supplies the
        // governing init's ip (`recInitIp`) — per-function and sequential,
        // because nested attrsets are deferred to thunk functions, so an
        // init is never interleaved with another init's REC_SETs in the same
        // function.  Bounds-guarded: if recInitIp is unset (e.g. standalone
        // disassembleOne / windowed view) or the rank is out of range, the
        // operand stays bare rather than risk a wrong name.
        if (recInitIp != UINT32_MAX && recInitIp < cu.code.size()) {
            uint32_t nInit = decodeOperand(cu.code[recInitIp]);
            uint32_t symPos = recInitIp + 1 + 2 * operand;
            if (operand < nInit && symPos < cu.code.size())
                std::fprintf(out, "   ; %s", symName(cu.code[symPos]));
        }
        break;
    case OP_JUMP:
    case OP_BRANCH_FALSE:
    case OP_BRANCH_TRUE:
    case OP_AND_BRANCH:
    case OP_OR_BRANCH:
    case OP_IMPL_BRANCH:
        std::fprintf(out, "   ; -> L%u", operand);
        break;
    case OP_POS:
        if (const PosSnapshot * ps = resolvePosSnapshot(operand))
            std::fprintf(out, "   ; %s:%u:%u", ps->file.c_str(), ps->line, ps->column);
        break;
    default:
        break;
    }
#pragma clang diagnostic pop

    std::fprintf(out, "\n");
    return ip + 1 + extra;
}

uint32_t disassembleWindow(std::FILE * out,
                           const CompilationUnit & cu,
                           uint32_t startIp,
                           uint32_t endIp)
{
    if (endIp > cu.code.size()) endIp = static_cast<uint32_t>(cu.code.size());
    uint32_t ip = startIp;
    while (ip < endIp) {
        ip = disassembleOne(out, cu, ip);
    }
    return ip;
}

void disassembleModule(std::FILE * out, const CompilationUnit & cu)
{
    std::fprintf(out, "; module functions=%zu code=%zu entry=%u\n",
        cu.lambdas.size(), cu.code.size(), cu.entryOffset);

    // Function ranges from lambdaCodeOffsets, sorted by start offset.
    // Code is emitted per-function contiguously (emit.cc), so a function
    // spans [its offset, the next-higher offset) — or end-of-code.
    std::vector<std::pair<uint32_t, uint32_t>> fns;  // (codeOffset, fid)
    uint32_t nf = static_cast<uint32_t>(
        std::min(cu.lambdaCodeOffsets.size(), cu.lambdas.size()));
    for (uint32_t fid = 0; fid < nf; ++fid)
        fns.emplace_back(cu.lambdaCodeOffsets[fid], fid);
    std::sort(fns.begin(), fns.end());

    // Pre-pass: collect branch targets (operands are ABSOLUTE code
    // offsets, so this is module-wide).  Walk via opExtraWords so data
    // words are never misread as opcodes.
    std::vector<uint8_t> isTarget(cu.code.size() + 1, 0);
    for (uint32_t ip = 0; ip < cu.code.size();) {
        Op op = decodeOp(cu.code[ip]);
        uint32_t operand = decodeOperand(cu.code[ip]);
        if (isBranchOp(op) && operand <= cu.code.size())
            isTarget[operand] = 1;
        uint32_t adv = 1 + opExtraWords(op, operand, cu, ip);
        ip += adv ? adv : 1;  // never stall on a 0-width unknown
    }

    if (fns.empty()) {  // no function table — fall back to a flat dump
        for (uint32_t ip = 0; ip < cu.code.size();) {
            if (isTarget[ip]) std::fprintf(out, "L%u:\n", ip);
            ip = disassembleOne(out, cu, ip);
        }
        return;
    }

    for (size_t i = 0; i < fns.size(); ++i) {
        uint32_t start = fns[i].first;
        uint32_t fid   = fns[i].second;
        uint32_t end   = (i + 1 < fns.size())
            ? fns[i + 1].first
            : static_cast<uint32_t>(cu.code.size());
        const LambdaDescriptor & ld = cu.lambdas[fid];
        std::fprintf(out, "\n; func %u%s%s%s  arity=%u nUp=%u nLocals=%u entry=%u%s\n",
            fid,
            ld.name.empty() ? "" : " \"",
            ld.name.empty() ? "" : ld.name.c_str(),
            ld.name.empty() ? "" : "\"",
            (unsigned)ld.arity, (unsigned)ld.nUpvalues, (unsigned)ld.nLocals,
            start, fid == 0 ? "  (top-level)" : "");
        // Track the governing REC_INIT for this function's REC_SETs.  Reset
        // per function; updated on each (let-)rec-init.  Sequential within a
        // function (nested attrsets are thunked into other functions), so a
        // single "most recent init" is the correct context for the REC_SETs
        // that follow it.
        uint32_t curRecInit = UINT32_MAX;
        for (uint32_t ip = start; ip < end && ip < cu.code.size();) {
            if (isTarget[ip]) std::fprintf(out, "L%u:\n", ip);
            Op op = decodeOp(cu.code[ip]);
            if (op == OP_ATTRS_REC_INIT || op == OP_ATTRS_LET_REC_INIT
                || op == OP_ATTRS_REC_INIT_TAIL)
                curRecInit = ip;
            ip = disassembleOne(out, cu, ip, curRecInit);
        }
    }
}

} // namespace nix::v3
