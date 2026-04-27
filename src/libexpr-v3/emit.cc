/// @file
/// v3 IR → bytecode emit.
///
/// Per Function:
///   - allocate stack slots for param + every reachable binding (lazily,
///     on first reference)
///   - emit the entry Block; emit any sub-Blocks (if branches, &&/||/->
///     rhs, with/assert bodies) in-line at their reference site
///   - terminate with OP_HALT (top-level) or OP_RETURN
///
/// Slot allocation is per-Function: a VarId resolves to a frame slot if it
/// is defined within the Function, or an upvalue index if it is a free
/// variable captured by the enclosing closure.
///
/// Each Block's TermReturn leaves the result Value on the operand stack.
/// The parent context (e.g., the surrounding If binding) consumes that
/// stack-top value, typically via OP_SET_LOCAL.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/ir.hh"
#include "v3/primop.hh"
#include "v3/vm.hh"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3 {

namespace {

struct Emitter
{
    const ir::Module & m;
    CompilationUnit unit;

    struct FuncCtx
    {
        ir::FuncId                          fid;
        std::unordered_map<ir::VarId, uint16_t> slot;
        std::unordered_map<ir::VarId, uint16_t> upvalue;
        uint16_t nextSlot = 0;
        uint16_t nLocals  = 0;
    };
    FuncCtx * ctx = nullptr;

    Emitter(const ir::Module & mod) : m(mod) {}

    // Helpers ---------------------------------------------------------------

    uint16_t getOrAssignSlot(ir::VarId v)
    {
        auto it = ctx->slot.find(v);
        if (it != ctx->slot.end()) return it->second;
        uint16_t s = ctx->nextSlot++;
        ctx->slot[v] = s;
        if (s + 1 > ctx->nLocals) ctx->nLocals = s + 1;
        return s;
    }

    void emitVarRef(ir::VarId v)
    {
        if (auto it = ctx->slot.find(v); it != ctx->slot.end()) {
            unit.code.push_back(encode(OP_GET_LOCAL, it->second));
            return;
        }
        if (auto uit = ctx->upvalue.find(v); uit != ctx->upvalue.end()) {
            unit.code.push_back(encode(OP_GET_UPVALUE, uit->second));
            return;
        }
        throw std::runtime_error("v3 emit: unbound VarId " + std::to_string(v));
    }

    uint32_t addIntConst(int64_t n)
    {
        unit.intConstants.push_back(n);
        return static_cast<uint32_t>(unit.intConstants.size() - 1);
    }
    uint32_t addFloatConst(double d)
    {
        unit.floatConstants.push_back(d);
        return static_cast<uint32_t>(unit.floatConstants.size() - 1);
    }
    uint32_t addStringConst(std::string_view s)
    {
        unit.stringConstants.emplace_back(s);
        return static_cast<uint32_t>(unit.stringConstants.size() - 1);
    }

    // Patch helpers ---------------------------------------------------------

    /// Emit a placeholder jump and return the index of the operand word
    /// (so the caller can patch in the absolute target later).
    uint32_t emitJumpPlaceholder(Op op)
    {
        unit.code.push_back(encode(op, 0));
        return static_cast<uint32_t>(unit.code.size() - 1);
    }

    void patchJump(uint32_t at, uint32_t target)
    {
        // Preserve the opcode byte; replace the 24-bit operand.
        Instruction prev = unit.code[at];
        unit.code[at] = (prev & 0xFF000000u) | (target & 0x00FFFFFFu);
    }

    // Block emit ------------------------------------------------------------

    /// Emit a Block in-line within the current function.  After emission,
    /// the Block's TermReturn value is on top of the operand stack.
    void emitBlock(ir::BlockId bid)
    {
        const ir::Block & b = m.blocks[bid];

        // Params have already been bound by the caller (e.g., function
        // prologue assigned param-VarId -> slot 0).
        for (auto & bd : b.bindings) {
            emitExpr(bd.expr);
            uint16_t slot = getOrAssignSlot(bd.var);
            unit.code.push_back(encode(OP_SET_LOCAL, slot));
        }

        // Terminal: TermReturn for now (only variant supported).
        const auto & ret = std::get<ir::TermReturn>(b.terminal);
        if (ret.value != ir::kInvalid)
            emitVarRef(ret.value);
        else
            unit.code.push_back(encode(OP_LIT_NULL));
    }

    // Expr emit -------------------------------------------------------------

    void emitExpr(const ir::Expr & expr)
    {
        std::visit([&](auto const & e) { emitOne(e); }, expr);
    }

    // -- Literals
    void emitOne(const ir::LitInt & e)
    {
        if (e.value >= -(1 << 23) && e.value < (1 << 23)) {
            unit.code.push_back(encode(OP_LIT_INT, static_cast<uint32_t>(e.value) & 0x00FFFFFF));
        } else {
            unit.code.push_back(encode(OP_LIT_INT_BIG, addIntConst(e.value)));
        }
    }
    void emitOne(const ir::LitFloat & e)
    {
        unit.code.push_back(encode(OP_LIT_FLOAT, addFloatConst(e.value)));
    }
    void emitOne(const ir::LitBool & e)
    {
        unit.code.push_back(encode(e.value ? OP_LIT_TRUE : OP_LIT_FALSE));
    }
    void emitOne(const ir::LitNull &) { unit.code.push_back(encode(OP_LIT_NULL)); }
    void emitOne(const ir::LitString & e)
    {
        unit.code.push_back(encode(OP_LIT_STR, addStringConst(e.value)));
    }
    void emitOne(const ir::LitPath & e)
    {
        // For now: store path string in stringConstants; accessor table TBD.
        unit.code.push_back(encode(OP_LIT_PATH, addStringConst(e.path)));
    }
    void emitOne(const ir::PosExpr &)
    {
        unit.code.push_back(encode(OP_POS));
    }

    // -- Variable / scoping
    void emitOne(const ir::VarRef & e) { emitVarRef(e.var); }
    void emitOne(const ir::WithLookup & e)
    {
        unit.code.push_back(encode(OP_WITH_LOOKUP, e.name));
        unit.code.push_back(static_cast<uint32_t>(e.depth));
    }

    // -- Functions
    void emitOne(const ir::Lambda & e)
    {
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_CLOSURE, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
    }
    void emitOne(const ir::MkThunk & e)
    {
        for (auto fv : e.freeVars) emitVarRef(fv);
        unit.code.push_back(encode(OP_MAKE_THUNK, e.funcIdx));
        unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
    }
    void emitOne(const ir::App & e)
    {
        emitVarRef(e.fun); emitVarRef(e.arg);
        unit.code.push_back(encode(OP_CALL));
    }
    void emitOne(const ir::Force & e)
    {
        emitVarRef(e.thunk);
        unit.code.push_back(encode(OP_FORCE));
    }

    // -- Arithmetic / comparison / logical
    void emitOne(const ir::Add & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_ADD)); }
    void emitOne(const ir::Sub & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_SUB)); }
    void emitOne(const ir::Mul & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_MUL)); }
    void emitOne(const ir::Div & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_DIV)); }
    void emitOne(const ir::Negate& e){ emitVarRef(e.operand); unit.code.push_back(encode(OP_NEGATE)); }
    void emitOne(const ir::Eq  & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_EQ));  }
    void emitOne(const ir::NEq & e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_NEQ)); }
    void emitOne(const ir::Less& e)  { emitVarRef(e.lhs); emitVarRef(e.rhs); unit.code.push_back(encode(OP_LESS));}
    void emitOne(const ir::Not & e)  { emitVarRef(e.operand); unit.code.push_back(encode(OP_NOT)); }

    // -- Short-circuit
    void emitOne(const ir::And & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_AND_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Or & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_OR_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }
    void emitOne(const ir::Impl & e)
    {
        emitVarRef(e.lhs);
        uint32_t at = emitJumpPlaceholder(OP_IMPL_BRANCH);
        emitBlock(e.rhsBlock);
        patchJump(at, static_cast<uint32_t>(unit.code.size()));
    }

    // -- If
    void emitOne(const ir::If & e)
    {
        emitVarRef(e.cond);
        uint32_t bf = emitJumpPlaceholder(OP_BRANCH_FALSE);
        emitBlock(e.thenBlock);
        uint32_t je = emitJumpPlaceholder(OP_JUMP);
        patchJump(bf, static_cast<uint32_t>(unit.code.size()));
        emitBlock(e.elseBlock);
        patchJump(je, static_cast<uint32_t>(unit.code.size()));
    }

    // -- Lists / strings
    void emitOne(const ir::ListExpr & e)
    {
        for (auto v : e.elems) emitVarRef(v);
        unit.code.push_back(encode(OP_LIST_INIT, static_cast<uint32_t>(e.elems.size())));
    }
    void emitOne(const ir::ConcatLists & e)
    {
        emitVarRef(e.lhs); emitVarRef(e.rhs);
        unit.code.push_back(encode(OP_LIST_CONCAT));
    }
    void emitOne(const ir::ConcatStrings & e)
    {
        for (auto v : e.parts) emitVarRef(v);
        // pack forceString as the LSB of the count operand
        uint32_t opnd = (static_cast<uint32_t>(e.parts.size()) << 1) | (e.forceString ? 1u : 0u);
        unit.code.push_back(encode(OP_STR_CONCAT, opnd));
    }

    // -- Attrsets
    void emitOne(const ir::AttrSet & e)
    {
        for (auto & en : e.entries) emitVarRef(en.value);
        unit.code.push_back(encode(OP_ATTRS_INIT, static_cast<uint32_t>(e.entries.size())));
        for (auto & en : e.entries) unit.code.push_back(en.name);
    }
    void emitOne(const ir::AttrSetDyn & e)
    {
        // Emit static values in order, then dynamic name+value pairs.
        for (auto & en : e.statics)  emitVarRef(en.value);
        for (auto & en : e.dynamics) { emitVarRef(en.nameVar); emitVarRef(en.value); }
        uint32_t packed = (static_cast<uint32_t>(e.statics.size()) << 12)
                        | (static_cast<uint32_t>(e.dynamics.size()) & 0xFFFu);
        unit.code.push_back(encode(OP_ATTRS_INIT_DYN, packed));
        for (auto & en : e.statics) unit.code.push_back(en.name);
    }
    void emitOne(const ir::RecAttrSet & e)
    {
        // For now treat as non-rec; real rec lowering is done by the
        // AST→IR pass (it emits MkThunk wrappers + a synthetic selfVar).
        for (auto & en : e.entries) emitVarRef(en.value);
        unit.code.push_back(encode(OP_ATTRS_REC_INIT, static_cast<uint32_t>(e.entries.size())));
        for (auto & en : e.entries) unit.code.push_back(en.name);
    }
    void emitOne(const ir::AttrSelect & e)
    {
        emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_ATTRS_SELECT, e.name));
    }
    void emitOne(const ir::AttrSelectDyn & e)
    {
        emitVarRef(e.attrs); emitVarRef(e.nameVar);
        unit.code.push_back(encode(OP_ATTRS_SELECT_DYN));
    }
    void emitOne(const ir::HasAttr & e)
    {
        emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_ATTRS_HAS, e.name));
    }
    void emitOne(const ir::HasAttrDyn & e)
    {
        emitVarRef(e.attrs); emitVarRef(e.nameVar);
        unit.code.push_back(encode(OP_ATTRS_HAS_DYN));
    }
    void emitOne(const ir::Update & e)
    {
        emitVarRef(e.lhs); emitVarRef(e.rhs);
        unit.code.push_back(encode(OP_ATTRS_UPDATE));
    }

    // -- Recursive let / rec attrset
    void emitOne(const ir::LetRec & e)
    {
        // Sort entries by SymbolId so the resulting Bindings are valid
        // (Bindings::lookup uses binary search on the sorted array).
        // Each REC_SET's operand becomes the entry's slot in the
        // sorted Bindings.
        const uint32_t n = static_cast<uint32_t>(e.entries.size());
        std::vector<uint32_t> sortedOrder(n);
        for (uint32_t i = 0; i < n; ++i) sortedOrder[i] = i;
        std::sort(sortedOrder.begin(), sortedOrder.end(),
            [&](uint32_t a, uint32_t b) {
                return e.entries[a].name < e.entries[b].name;
            });
        std::vector<uint32_t> entryToSlot(n);
        for (uint32_t slot = 0; slot < n; ++slot)
            entryToSlot[sortedOrder[slot]] = slot;

        // 1. OP_ATTRS_REC_INIT[n] + n sorted SymbolIds: push placeholder
        //    rec Bindings on operand stack.
        unit.code.push_back(encode(OP_ATTRS_REC_INIT, n));
        for (uint32_t slot = 0; slot < n; ++slot)
            unit.code.push_back(e.entries[sortedOrder[slot]].name);

        // 2. For each IR entry, build a Thunk capturing whatever
        //    upvalues its body needs and write it into the sorted slot.
        //
        //    Each thunk body's freeVars list (sorted by VarId,
        //    populated by computeFreeVars) IS the upvalue layout: the
        //    body references upvalues[i] = freeVars[i].  We push them
        //    in that order so OP_MAKE_THUNK pops in reverse and
        //    upvalues[i] ends up correct.
        //
        //    Special case: when freeVars contains the rec attrset
        //    VarId (Plain entries that reference siblings via the rec
        //    scope), we DUP from the operand stack instead of
        //    emitVarRef (which would look for a slot/upvalue in the
        //    enclosing function — the rec attrs is on top of the op
        //    stack, not in any slot).
        for (uint32_t i = 0; i < n; ++i) {
            auto & en = e.entries[i];
            const auto & ff = m.functions[en.thunkBody].freeVars;
            for (auto fv : ff) {
                if (fv == e.recVar) {
                    unit.code.push_back(encode(OP_DUP));
                } else {
                    emitVarRef(fv);
                }
            }
            unit.code.push_back(encode(OP_MAKE_THUNK, en.thunkBody));
            unit.code.push_back(static_cast<uint32_t>(ff.size()));
            unit.code.push_back(encode(OP_ATTRS_REC_SET, entryToSlot[i]));
        }
        // After all SETs, rec attrs is on top of the operand stack —
        // becomes the value of the LetRec binding.
    }

    // -- Primop direct call
    uint32_t internPrimOp(const PrimOp * po)
    {
        for (uint32_t i = 0; i < unit.primops.size(); ++i)
            if (unit.primops[i] == po) return i;
        unit.primops.push_back(po);
        return static_cast<uint32_t>(unit.primops.size() - 1);
    }
    void emitOne(const ir::PrimOpCall & e)
    {
        for (auto v : e.args) emitVarRef(v);
        unit.code.push_back(encode(OP_CALL_PRIMOP, static_cast<uint32_t>(e.args.size())));
        unit.code.push_back(internPrimOp(e.primop));
    }
    void emitOne(const ir::LitPrimOp & e)
    {
        unit.code.push_back(encode(OP_LIT_PRIMOP, internPrimOp(e.primop)));
    }

    // -- With / assert
    void emitOne(const ir::With & e)
    {
        emitVarRef(e.attrs);
        unit.code.push_back(encode(OP_WITH_PUSH));
        emitBlock(e.bodyBlock);
        unit.code.push_back(encode(OP_WITH_POP));
    }
    void emitOne(const ir::Assert & e)
    {
        emitVarRef(e.cond);
        unit.code.push_back(encode(OP_ASSERT));
        emitBlock(e.bodyBlock);
    }

    // Function emit ---------------------------------------------------------

    /// Recursively walk all bindings reachable from `bid` (within the
    /// same function — sub-blocks for if-branches, with-bodies, etc.)
    /// and assign each binding's VarId a frame slot.  This pre-pass
    /// allows references to forward bindings (e.g., let-rec in lambdas)
    /// to resolve at emit time without needing two-pass slot resolution
    /// later.
    void preassignSlotsInBlock(FuncCtx & fc, ir::BlockId bid,
                               std::unordered_set<ir::BlockId> & visited)
    {
        if (!visited.insert(bid).second) return;
        const ir::Block & b = m.blocks[bid];
        for (auto pv : b.params) {
            auto _slot = getOrAssignSlot(fc, pv); (void)_slot;
        }
        for (auto & bd : b.bindings) {
            (void)getOrAssignSlot(fc, bd.var);
            std::vector<ir::BlockId> subs;
            std::visit([&](auto const & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::If>) {
                    subs.push_back(e.thenBlock); subs.push_back(e.elseBlock);
                } else if constexpr (std::is_same_v<T, ir::With>  ||
                                     std::is_same_v<T, ir::Assert>) {
                    subs.push_back(e.bodyBlock);
                } else if constexpr (std::is_same_v<T, ir::And> ||
                                     std::is_same_v<T, ir::Or>  ||
                                     std::is_same_v<T, ir::Impl>) {
                    subs.push_back(e.rhsBlock);
                }
            }, bd.expr);
            for (auto sb : subs) preassignSlotsInBlock(fc, sb, visited);
        }
    }

    uint16_t getOrAssignSlot(FuncCtx & fc, ir::VarId v)
    {
        auto it = fc.slot.find(v);
        if (it != fc.slot.end()) return it->second;
        uint16_t s = fc.nextSlot++;
        fc.slot[v] = s;
        if (s + 1 > fc.nLocals) fc.nLocals = s + 1;
        return s;
    }

    void emitFunction(ir::FuncId fid)
    {
        const ir::Function & f = m.functions[fid];

        FuncCtx fc;
        fc.fid = fid;
        // Param goes in slot 0 if present.  For `{a, b}: ...` formals
        // without an @arg name, the param VarId still represents the
        // attrset arg passed at call time.
        if (f.paramVar != ir::kInvalid &&
            (f.argName != ir::kInvalidSymbol || f.hasFormals))
            (void)getOrAssignSlot(fc, f.paramVar);
        // Upvalue order = freeVars.
        for (uint16_t i = 0; i < f.freeVars.size(); ++i)
            fc.upvalue[f.freeVars[i]] = i;
        // Pre-assign slots for every VarId reachable from the entry
        // block (including in sub-blocks: if-branches, with bodies, etc.)
        // so forward references resolve at emit time.
        if (f.entryBlock != ir::kInvalidBlock) {
            std::unordered_set<ir::BlockId> visited;
            preassignSlotsInBlock(fc, f.entryBlock, visited);
        }

        ctx = &fc;
        uint32_t codeStart = static_cast<uint32_t>(unit.code.size());

        if (f.entryBlock != ir::kInvalidBlock)
            emitBlock(f.entryBlock);
        else
            unit.code.push_back(encode(OP_LIT_NULL));

        unit.code.push_back(encode(fid == 0 ? OP_HALT : OP_RETURN));

        if (unit.lambdas.size() <= fid)         unit.lambdas.resize(fid + 1);
        if (unit.lambdaCodeOffsets.size() <= fid) unit.lambdaCodeOffsets.resize(fid + 1);
        unit.lambdas[fid] = LambdaDescriptor{
            .codeOffset     = codeStart,
            .prologueOffset = codeStart,
            .nUpvalues      = static_cast<uint16_t>(f.freeVars.size()),
            .nLocals        = fc.nLocals,
            .arity          = static_cast<uint8_t>(f.argName != ir::kInvalidSymbol ? 1 : (f.hasFormals ? 1 : 0)),
            .hasFormals     = static_cast<uint8_t>(f.hasFormals ? 1 : 0),
            .formals        = {},
        };
        if (f.hasFormals) {
            auto & desc = unit.lambdas[fid];
            desc.formals.reserve(f.formals.size());
            for (auto & fm : f.formals)
                desc.formals.emplace_back(fm.name, fm.defaultBlock != ir::kInvalidBlock);
        }
        unit.lambdaCodeOffsets[fid] = codeStart;

        ctx = nullptr;

        if (fid == 0)
            unit.entryOffset = codeStart;
    }

    void emitAll()
    {
        // Mirror the IR symbol table into the CompilationUnit so the VM
        // can use SymbolId at runtime without round-tripping to strings.
        // Copy the global symbol table so SymbolIds in this CU map to
        // the same names that any other CU in the process uses.
        unit.symbolTable = ir::globalSymbolTable();

        // Emit inner functions first so their descriptors and code are
        // available before the top-level (which references them via
        // OP_MAKE_CLOSURE / OP_MAKE_THUNK).  Top-level is functions[0].
        for (ir::FuncId i = 1; i < m.functions.size(); ++i)
            emitFunction(i);
        emitFunction(0);
    }
};

} // namespace

CompilationUnit compile(const ir::Module & m)
{
    Emitter e(m);
    e.emitAll();
    return std::move(e.unit);
}

} // namespace nix::v3
