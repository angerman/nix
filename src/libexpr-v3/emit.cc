/// @file
/// v3 IR → bytecode emit (subset).
///
/// Single-block code only.  Each binding gets a slot index in the function's
/// frame; the binding's RHS code computes a value into that slot.  TermReturn
/// (here, f.returnVar) emits OP_RETURN at the end.
///
/// Free vars become OP_GET_UPVALUE; the param becomes slot 0.  Locals start
/// at slot 1 (or 0 if no param).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/ir.hh"
#include "v3/vm.hh"

#include <cassert>
#include <unordered_map>
#include <stdexcept>

namespace nix::v3 {

namespace {

struct Emitter
{
    const ir::Module & module;
    CompilationUnit unit;

    Emitter(const ir::Module & m) : module(m) {}

    /// Emit a function (top-level or inner).  Returns the code offset where
    /// the function's body starts.
    uint32_t emitFunction(uint32_t funcIdx)
    {
        const auto & f = module.functions[funcIdx];

        uint32_t codeStart = static_cast<uint32_t>(unit.code.size());

        // Slot allocation: param at 0, then bindings in order.
        std::unordered_map<ir::VarId, uint16_t> slots;
        uint16_t nextSlot = 0;
        if (f.param != ir::kInvalid)
            slots[f.param] = nextSlot++;
        for (auto & b : f.bindings)
            slots[b.var] = nextSlot++;

        // Upvalue idx = position in f.freeVars.
        std::unordered_map<ir::VarId, uint16_t> upvalueIdx;
        for (uint16_t i = 0; i < f.freeVars.size(); ++i)
            upvalueIdx[f.freeVars[i]] = i;

        auto emitVarRef = [&](ir::VarId v) {
            auto sit = slots.find(v);
            if (sit != slots.end()) {
                unit.code.push_back(encode(OP_GET_LOCAL, sit->second));
                return;
            }
            auto uit = upvalueIdx.find(v);
            if (uit != upvalueIdx.end()) {
                unit.code.push_back(encode(OP_GET_UPVALUE, uit->second));
                return;
            }
            throw std::runtime_error("emit: unbound VarId " + std::to_string(v));
        };

        // Emit each binding.
        for (auto & b : f.bindings) {
            std::visit([&](const auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::LitInt>) {
                    if (e.value >= -(1 << 23) && e.value < (1 << 23)) {
                        unit.code.push_back(encode(OP_LIT_INT, static_cast<uint32_t>(e.value) & 0x00FFFFFF));
                    } else {
                        uint32_t idx = static_cast<uint32_t>(unit.intConstants.size());
                        unit.intConstants.push_back(e.value);
                        unit.code.push_back(encode(OP_LIT_INT_BIG, idx));
                    }
                } else if constexpr (std::is_same_v<T, ir::VarRef>) {
                    emitVarRef(e.var);
                } else if constexpr (std::is_same_v<T, ir::Lambda>) {
                    // Push captures (in freeVars order — the inner function
                    // expects them in that order via OP_GET_UPVALUE).
                    for (auto fv : e.freeVars) emitVarRef(fv);
                    // OP_MAKE_CLOSURE [funcIdx]; data word = nUpvalues
                    unit.code.push_back(encode(OP_MAKE_CLOSURE, e.funcIdx));
                    unit.code.push_back(static_cast<uint32_t>(e.freeVars.size()));
                } else if constexpr (std::is_same_v<T, ir::App>) {
                    emitVarRef(e.fun);
                    emitVarRef(e.arg);
                    unit.code.push_back(encode(OP_CALL));
                } else if constexpr (std::is_same_v<T, ir::Add>) {
                    emitVarRef(e.lhs); emitVarRef(e.rhs);
                    unit.code.push_back(encode(OP_ADD));
                } else if constexpr (std::is_same_v<T, ir::Sub>) {
                    emitVarRef(e.lhs); emitVarRef(e.rhs);
                    unit.code.push_back(encode(OP_SUB));
                } else if constexpr (std::is_same_v<T, ir::Mul>) {
                    emitVarRef(e.lhs); emitVarRef(e.rhs);
                    unit.code.push_back(encode(OP_MUL));
                }
            }, b.expr);
            unit.code.push_back(encode(OP_SET_LOCAL, slots[b.var]));
        }

        // Return.
        if (f.returnVar != ir::kInvalid)
            emitVarRef(f.returnVar);
        // Top-level emits OP_HALT instead of OP_RETURN.
        unit.code.push_back(encode(funcIdx == 0 ? OP_HALT : OP_RETURN));

        // Register descriptor.
        if (unit.lambdas.size() <= funcIdx) unit.lambdas.resize(funcIdx + 1);
        if (unit.lambdaCodeOffsets.size() <= funcIdx) unit.lambdaCodeOffsets.resize(funcIdx + 1);
        unit.lambdas[funcIdx] = LambdaDescriptor{
            .codeOffset = codeStart,
            .prologueOffset = codeStart,
            .nUpvalues = static_cast<uint16_t>(f.freeVars.size()),
            .nLocals   = nextSlot,
            .arity     = static_cast<uint8_t>(f.param != ir::kInvalid ? 1 : 0),
            .hasFormals = 0,
        };
        unit.lambdaCodeOffsets[funcIdx] = codeStart;

        return codeStart;
    }

    void emitAll()
    {
        // Emit functions in order; each function's code is contiguous in
        // unit.code starting at lambdaCodeOffsets[funcIdx].  The top-level
        // (funcIdx 0) is emitted last so its OP_HALT is at the end of the
        // stream — but we want functions 1.. emitted FIRST so they're
        // available when the top-level is run.  Two passes:
        for (uint32_t i = 1; i < module.functions.size(); ++i)
            emitFunction(i);
        unit.entryOffset = emitFunction(0);
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
