/// @file
/// Bytecode disassembler for debugging.
///
/// Produces human-readable output of CompilationUnit bytecode,
/// showing opcodes, operands, constant pool contents, thunk/lambda
/// descriptors, and jump targets.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/bytecode.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/print.hh"

#include <iomanip>
#include <sstream>

namespace nix::bytecode {

/// Return the mnemonic for an opcode.
const char * opName(uint8_t op)
{
    switch (op) {
        case OP_NOP:              return "NOP";
        case OP_CONST:            return "CONST";
        case OP_TRUE:             return "TRUE";
        case OP_FALSE:            return "FALSE";
        case OP_NULL:             return "NULL";
        case OP_INT:              return "INT";
        case OP_GET_LOCAL_0:      return "GET_LOCAL_0";
        case OP_GET_LOCAL_1:      return "GET_LOCAL_1";
        case OP_GET_LOCAL_2:      return "GET_LOCAL_2";
        case OP_GET_LOCAL_3:      return "GET_LOCAL_3";
        case OP_GET_LOCAL:        return "GET_LOCAL";
        case OP_GET_WITH:         return "GET_WITH";
        case OP_ATTR_SELECT:      return "ATTR_SELECT";
        case OP_ATTR_SELECT_OR:   return "ATTR_SELECT_OR";
        case OP_HAS_ATTR:         return "HAS_ATTR";
        case OP_ATTR_INSERT:      return "ATTR_INSERT";
        case OP_ATTR_INSERT_DYN:  return "ATTR_INSERT_DYN";
        case OP_ATTRS_INIT:       return "ATTRS_INIT";
        case OP_ATTRS_FINISH:     return "ATTRS_FINISH";
        case OP_ATTRS_UPDATE:     return "ATTRS_UPDATE";
        case OP_REC_ATTRS_INIT:   return "REC_ATTRS_INIT";
        case OP_REC_ATTRS_FINISH: return "REC_ATTRS_FINISH";
        case OP_LIST_INIT:        return "LIST_INIT";
        case OP_LIST_ELEM:        return "LIST_ELEM";
        case OP_LIST_FINISH:      return "LIST_FINISH";
        case OP_LIST_CONCAT:      return "LIST_CONCAT";
        case OP_STR_CONCAT_INIT:  return "STR_CONCAT_INIT";
        case OP_STR_CONCAT_PART:  return "STR_CONCAT_PART";
        case OP_STR_CONCAT_FINISH:return "STR_CONCAT_FINISH";
        case OP_COERCE_TO_STRING: return "COERCE_TO_STRING";
        case OP_JUMP:             return "JUMP";
        case OP_JUMP_IF_FALSE:    return "JUMP_IF_FALSE";
        case OP_JUMP_IF_TRUE:     return "JUMP_IF_TRUE";
        case OP_JUMP_IF_NOT_ATTRS:return "JUMP_IF_NOT_ATTRS";
        case OP_JUMP_IF_NO_ATTR:  return "JUMP_IF_NO_ATTR";
        case OP_MAKE_THUNK:       return "MAKE_THUNK";
        case OP_MAKE_CLOSURE:     return "MAKE_CLOSURE";
        case OP_CALL:             return "CALL";
        case OP_CALL_1:           return "CALL_1";
        case OP_TAIL_CALL:        return "TAIL_CALL";
        case OP_FORCE:            return "FORCE";
        case OP_RETURN:           return "RETURN";
        case OP_ENTER_LET:        return "ENTER_LET";
        case OP_LEAVE_SCOPE:      return "LEAVE_SCOPE";
        case OP_SET_ENV_SLOT:     return "SET_ENV_SLOT";
        case OP_PUSH_WITH:        return "PUSH_WITH";
        case OP_EQ:               return "EQ";
        case OP_NEQ:              return "NEQ";
        case OP_NOT:              return "NOT";
        case OP_IMPL:             return "IMPL";
        case OP_ADD:              return "ADD";
        case OP_SUB:              return "SUB";
        case OP_MUL:              return "MUL";
        case OP_DIV:              return "DIV";
        case OP_NEGATE:           return "NEGATE";
        case OP_LESS_THAN:        return "LESS_THAN";
        case OP_ASSERT:           return "ASSERT";
        case OP_INHERIT_FROM_INIT:return "INHERIT_FROM_INIT";
        case OP_INHERIT_FROM_SET: return "INHERIT_FROM_SET";
        case OP_SET_ENV_SLOT_UP:  return "SET_ENV_SLOT_UP";
        case OP_ATTR_SELECT_DYN: return "ATTR_SELECT_DYN";
        case OP_HAS_ATTR_DYN:   return "HAS_ATTR_DYN";
        case OP_ATTRS_DYN_INIT: return "ATTRS_DYN_INIT";
        case OP_GET_LOCAL_0_FORCE: return "GET_LOCAL_0_FORCE";
        case OP_POS:              return "POS";
        case OP_DUP:              return "DUP";
        case OP_POP:              return "POP";
        case OP_SWAP:             return "SWAP";
        case OP_EVAL_EXPR:        return "EVAL_EXPR";
        case OP_SELECT_FORCE:     return "SELECT_FORCE";
        case OP_GET_UPVALUE:      return "GET_UPVALUE";
        case OP_MAKE_CLOSURE_V2:  return "MAKE_CLOSURE_V2";
        case OP_MAKE_THUNK_V2:    return "MAKE_THUNK_V2";
        case OP_GET_STACK_SLOT:   return "GET_STACK_SLOT";
        case OP_SET_STACK_SLOT:   return "SET_STACK_SLOT";
        case OP_ALLOC_VALUE:      return "ALLOC_VALUE";
        case OP_COPY_TO_SLOT:     return "COPY_TO_SLOT";
        case OP_GET_SLOT_FORCE:   return "GET_SLOT_FORCE";
        case OP_GET_SLOT_RETURN:  return "GET_SLOT_RETURN";
        case OP_GET_UV_FORCE:     return "GET_UV_FORCE";
        case OP_SLOT_SLOT_CALL1:  return "SLOT_SLOT_CALL1";
        case OP_ATTR_SELECT_CACHED:       return "ATTR_SELECT_CACHED";
        case OP_ATTR_SELECT_FORCE_CACHED: return "ATTR_SELECT_FORCE_CACHED";
        case OP_MOV_SLOTS:        return "MOV_SLOTS";
        case OP_RFORCE_FROM:      return "RFORCE_FROM";
        case OP_RGET_UV_TO:       return "RGET_UV_TO";
        case OP_RUVF_TO:          return "RUVF_TO";
        case OP_RADD_R:           return "RADD_R";
        case OP_RSUB_R:           return "RSUB_R";
        case OP_RMUL_R:           return "RMUL_R";
        case OP_RLESS_R:          return "RLESS_R";
        case OP_REQ_R:            return "REQ_R";
        case OP_RATTR_SELF_R:     return "RATTR_SELF_R";
        case OP_RCALL1_R:         return "RCALL1_R";
        case OP_TAIL_CALL_1:      return "TAIL_CALL_1";
        case OP_CALL_PRIMOP:      return "CALL_PRIMOP";
        case OP_CELL_GET:         return "CELL_GET";
        case OP_CELL_SET:         return "CELL_SET";
        case OP_ALLOC_CELL:       return "ALLOC_CELL";
        case OP_RLIT_INT:         return "RLIT_INT";
        case OP_RCONST:           return "RCONST";
        case OP_RUPDATE_R:        return "RUPDATE_R";
        case OP_RCONCATLIST_R:    return "RCONCATLIST_R";
        case OP_RNOT_R:           return "RNOT_R";
        case OP_RNEG_R:           return "RNEG_R";
        case OP_RMAKE_THUNK_V2:   return "RMAKE_THUNK_V2";
        case OP_RMAKE_CLOSURE_V2: return "RMAKE_CLOSURE_V2";
        case OP_RATTRS_INIT:      return "RATTRS_INIT";
        case OP_RLIST_INIT:       return "RLIST_INIT";
        case OP_GET_UV2:          return "GET_UV2";
        case OP_GET_SLOT2:        return "GET_SLOT2";
        case OP_GET_UV_SLOT:      return "GET_UV_SLOT";
        case OP_GET_SLOT_UV:      return "GET_SLOT_UV";
        default:                  return "???";
    }
}

std::string disassemble(const CompilationUnit & unit, const EvalState * state)
{
    std::ostringstream out;

    out << "=== CompilationUnit ===\n";
    out << "  code:      " << unit.code.size() << " instructions\n";
    out << "  constants: " << unit.constants.size() << "\n";
    out << "  symbols:   " << unit.symbols.size() << "\n";
    out << "  thunks:    " << unit.thunks.size() << "\n";
    out << "  lambdas:   " << unit.lambdas.size() << "\n";
    out << "  exprs:     " << unit.exprPool.size() << "\n";
    out << "\n";

    // Constants pool
    if (!unit.constants.empty()) {
        out << "--- Constants ---\n";
        for (size_t i = 0; i < unit.constants.size(); i++) {
            out << "  [" << i << "] ";
            if (unit.constants[i]) {
                auto * v = unit.constants[i];
                if (state)
                    out << ValuePrinter(*const_cast<EvalState *>(state), *v, PrintOptions{.maxDepth = 1});
                else
                    out << showType(*v);
            } else {
                out << "<null>";
            }
            out << "\n";
        }
        out << "\n";
    }

    // Thunk descriptors
    if (!unit.thunks.empty()) {
        out << "--- Thunks ---\n";
        for (size_t i = 0; i < unit.thunks.size(); i++) {
            out << "  [" << i << "] codeOffset=" << unit.thunks[i].codeOffset << "\n";
        }
        out << "\n";
    }

    // Lambda descriptors
    if (!unit.lambdas.empty()) {
        out << "--- Lambdas ---\n";
        for (size_t i = 0; i < unit.lambdas.size(); i++) {
            auto & desc = unit.lambdas[i];
            out << "  [" << i << "] codeOffset=" << desc.codeOffset
                << " envSize=" << desc.envSize;
            if (state && desc.name)
                out << " name=" << (*state).symbols[desc.name];
            out << "\n";
        }
        out << "\n";
    }

    // Bytecode listing
    out << "--- Code ---\n";
    for (size_t i = 0; i < unit.code.size(); i++) {
        Instruction instr = unit.code[i];
        uint8_t op = decodeOp(instr);
        uint32_t operand = decodeOperand(instr);

        // Position annotation (available via unit.posForOffset(i) if needed)

        out << "  " << std::setw(4) << i << ": "
            << std::setw(18) << std::left << opName(op);

        // Print operand based on instruction type
        switch (op) {
            case OP_CONST:
                out << operand;
                if (operand < unit.constants.size() && unit.constants[operand] && state)
                    out << "  ; " << ValuePrinter(
                        *const_cast<EvalState *>(state), *unit.constants[operand],
                        PrintOptions{.maxDepth = 0});
                break;

            case OP_INT:
                out << operand;
                break;

            case OP_GET_LOCAL_0:
            case OP_GET_LOCAL_1:
            case OP_GET_LOCAL_2:
            case OP_GET_LOCAL_3:
                out << "displ=" << operand;
                break;

            case OP_GET_LOCAL: {
                uint8_t level = unpackLevel(operand);
                uint16_t displ = unpackDispl(operand);
                out << "level=" << (int)level << " displ=" << displ;
                break;
            }

            case OP_GET_WITH:
            case OP_ATTR_SELECT:
            case OP_HAS_ATTR:
            case OP_ATTR_INSERT:
                out << "sym=" << operand;
                if (state && operand < unit.symbols.size())
                    out << "  ; " << (*state).symbols[unit.symbols[operand]];
                break;

            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_TRUE:
            case OP_JUMP_IF_NOT_ATTRS: {
                int32_t offset = decodeSigned(instr);
                out << offset << "  ; -> " << (static_cast<int32_t>(i) + 1 + offset);
                break;
            }

            case OP_MAKE_THUNK:
                out << "thunk=" << operand;
                if (operand < unit.thunks.size())
                    out << "  ; -> offset " << unit.thunks[operand].codeOffset;
                break;

            case OP_MAKE_CLOSURE:
                out << "lambda=" << operand;
                if (operand < unit.lambdas.size())
                    out << "  ; -> offset " << unit.lambdas[operand].codeOffset;
                break;

            case OP_CALL:
            case OP_TAIL_CALL:
                out << "nArgs=" << operand;
                break;

            case OP_ENTER_LET:
            case OP_REC_ATTRS_INIT:
                out << "envSize=" << operand;
                break;

            case OP_SET_ENV_SLOT:
            case OP_INHERIT_FROM_SET:
                out << "displ=" << operand;
                break;

            case OP_EVAL_EXPR:
                out << "expr=" << operand;
                break;

            // VM v2 opcodes
            case OP_GET_UPVALUE:
                out << "idx=" << operand;
                break;

            case OP_MAKE_CLOSURE_V2:
                out << "lambda=" << operand;
                if (operand < unit.lambdas.size())
                    out << "  ; -> offset " << unit.lambdas[operand].codeOffset;
                break;

            case OP_MAKE_THUNK_V2:
                out << "thunk=" << operand;
                if (operand < unit.thunks.size())
                    out << "  ; -> offset " << unit.thunks[operand].codeOffset;
                break;

            case OP_GET_STACK_SLOT:
            case OP_SET_STACK_SLOT:
            case OP_COPY_TO_SLOT:
                out << "slot=" << operand;
                break;

            case OP_ALLOC_VALUE:
                break;

            default:
                if (operand != 0)
                    out << operand;
                break;
        }

        out << "\n";
    }

    return out.str();
}

} // namespace nix::bytecode
