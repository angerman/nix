/// @file
/// WC-32: Minimal v3 bytecode disassembler.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/disasm.hh"
#include "v3/bytecode.hh"
#include "v3/ir.hh"

namespace nix::v3 {

static const char * opName(Op op)
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
    case OP_LIST_INIT:         return "OP_LIST_INIT";
    case OP_LIST_CONCAT:       return "OP_LIST_CONCAT";
    case OP_ATTRS_INIT:        return "OP_ATTRS_INIT";
    case OP_ATTRS_INIT_DYN:    return "OP_ATTRS_INIT_DYN";
    case OP_ATTRS_REC_INIT:    return "OP_ATTRS_REC_INIT";
    case OP_ATTRS_REC_SET:     return "OP_ATTRS_REC_SET";
    case OP_ATTRS_SELECT:      return "OP_ATTRS_SELECT";
    case OP_ATTRS_SELECT_DYN:  return "OP_ATTRS_SELECT_DYN";
    case OP_ATTRS_HAS:         return "OP_ATTRS_HAS";
    case OP_ATTRS_HAS_DYN:     return "OP_ATTRS_HAS_DYN";
    case OP_ATTRS_UPDATE:      return "OP_ATTRS_UPDATE";
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

/// Returns the number of extra Instruction words this op consumes
/// after the opcode word itself.  Conservative — when unknown,
/// returns 0 (caller may end up dis-aligned but still gets useful
/// info before that point).
static uint32_t opExtraWords(Op op, uint32_t operand,
                             const CompilationUnit & cu, uint32_t ip)
{
    (void)cu; (void)ip;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    // Multi-word: nUpvalues + nWithTargets (#530) following.
    case OP_MAKE_CLOSURE:
    case OP_MAKE_THUNK:
        return 2;
    // OP_ATTRS_INIT[n] then 2*n words (name, pos pairs).
    case OP_ATTRS_INIT:
        return 2 * operand;
    case OP_ATTRS_INIT_DYN: {
        // Packed: nStatic (high 12 bits) + nDyn (low 12 bits).  Static
        // entries: 2 words each (name, pos).  Dynamic: 1 word (pos)
        // per entry (positions appended after static block).
        uint32_t nStatic = (operand >> 12) & 0xFFF;
        uint32_t nDyn = operand & 0xFFF;
        return 2 * nStatic + nDyn;
    }
    case OP_ATTRS_REC_INIT:
        // n words: n SymbolId + n PosIdx32 pairs (2*n total).
        return 2 * operand;
    // OP_ATTRS_SELECT consumes 1 word for the inline-cache slot index.
    case OP_ATTRS_SELECT:
        return 1;
    // OP_CALL_PRIMOP n: pops n args; 1 extra word: primop-table index.
    // (vm.cc:3203 reads `cu->code[ip++]` for poIdx).  Without this
    // entry, every disasm past the first OP_CALL_PRIMOP misaligned.
    case OP_CALL_PRIMOP:
        return 1;
    // OP_LIST_INIT n: pops n; no extra words.
    // OP_CALL: no extra words.
    // OP_FORCE: no extra words.
    // OP_WITH_LOOKUP / OP_STR_CONCAT / OP_LIT_PRIMOP / OP_LIT_BUILTINS /
    // OP_APPLY_OVERRIDES / OP_IS_* / OP_HEAD / OP_TAIL / OP_LENGTH /
    // OP_ELEM_AT / OP_ASSERT / OP_POS / OP_WITH_PUSH / OP_WITH_POP:
    // operand-only, no extra words.
    default:
        return 0;
    }
#pragma clang diagnostic pop
}

uint32_t disassembleOne(std::FILE * out,
                        const CompilationUnit & cu,
                        uint32_t ip)
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
    std::fprintf(out, "  [%u] %-22s operand=%u",
        ip, name, operand);
    // Annotate ops whose operand is a SymbolId with the symbol name —
    // makes cycle traces self-explanatory ("OP_ATTRS_SELECT operand=192
    // (release)" beats raw numeric IDs).
    const auto & gst = ir::globalSymbolTable();
    auto symAnnotate = [&](uint32_t sid) {
        if (sid < gst.size())
            std::fprintf(out, " (%s)", gst[sid].c_str());
    };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
    switch (op) {
    case OP_ATTRS_SELECT:
    case OP_ATTRS_SELECT_DYN:
    case OP_ATTRS_HAS:
    case OP_ATTRS_HAS_DYN:
        symAnnotate(operand);
        break;
    default: break;
    }
#pragma clang diagnostic pop
    if (extra > 0) {
        std::fprintf(out, "  data=[");
        for (uint32_t i = 0; i < extra && (ip + 1 + i) < cu.code.size(); ++i) {
            if (i > 0) std::fprintf(out, ",");
            std::fprintf(out, "%u", cu.code[ip + 1 + i]);
        }
        std::fprintf(out, "]");
    }
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

} // namespace nix::v3
