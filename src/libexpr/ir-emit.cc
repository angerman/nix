/// @file
/// IR -> Bytecode emitter implementation (VM v2).
///
/// Translates an IRModule into a CompilationUnit with upvalue-based closures.
/// See ir-emit.hh for the design overview.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/ir-emit.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/bytecode-thunk.hh"
#include "nix/expr/nixexpr.hh"

#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nix::bytecode {

// ============================================================================
// Emitter state
// ============================================================================

/// Per-block emission context.  Tracks how VarIds map to either stack slots
/// (locals within the current block) or upvalue indices (captured from
/// enclosing scope).
struct BlockContext
{
    /// Map from VarId to a stack slot index (frame-relative).
    /// Only for variables defined within this block (bindings + params).
    std::unordered_map<ir::VarId, uint32_t> localSlots;

    /// Map from VarId to an upvalue index.
    /// Only for variables captured from an enclosing scope.
    std::unordered_map<ir::VarId, uint32_t> upvalueSlots;

    /// Next available stack slot in this frame.
    uint32_t nextSlot = 0;

    /// Allocate a fresh stack slot for a VarId.
    uint32_t allocSlot(ir::VarId var)
    {
        uint32_t slot = nextSlot++;
        localSlots[var] = slot;
        return slot;
    }
};

/// Internal emitter state.  One instance per emitFromIR() call.
class IREmitter
{
    EvalState & state;
    CompilationUnit & unit;
    const ir::IRModule & module;

    /// Mapping from BlockId to the code offset where it was emitted.
    /// Used to patch forward jumps.
    std::unordered_map<ir::BlockId, uint32_t> blockOffsets;

public:
    IREmitter(EvalState & state, CompilationUnit & unit, const ir::IRModule & module)
        : state(state)
        , unit(unit)
        , module(module)
    {}

    /// Emit the entire module.  Entry block is block 0.
    void emit();

private:
    /// Emit a single block's bindings + terminal.
    /// `ctx` holds the local/upvalue mapping for this block.
    void emitBlock(const ir::IRBlock & block, BlockContext & ctx);

    /// Emit code for a single IR binding.
    void emitBinding(const ir::Binding & binding, BlockContext & ctx);

    /// Emit code for an IR expression (the RHS of a binding).
    /// Leaves one Value* on the operand stack.
    void emitExpr(const ir::IRExpr & expr, PosIdx pos, BlockContext & ctx);

    /// Emit code for a terminal instruction.
    void emitTerminal(const ir::Terminal & term, BlockContext & ctx);

    /// Emit a variable reference: either a local slot read, an upvalue
    /// read, or (for inline blocks like if-branches) a parent-scope slot.
    void emitVarRef(ir::VarId var, PosIdx pos, BlockContext & ctx);

    /// Emit the code for a lambda/thunk sub-block as a separate code region.
    /// Returns the code offset where the sub-block's code begins.
    /// Also populates lambdaDesc/thunkDesc and returns the descriptor index.
    uint32_t emitSubBlock(
        ir::BlockId blockId,
        const ir::FreeVars & freeVars,
        BlockContext & parentCtx);

    /// Emit an inline block (for if-branches, with bodies, etc).
    /// These share the parent frame's stack slots; no upvalue capture.
    /// Returns the code offset where the block begins.
    uint32_t emitInlineBlock(ir::BlockId blockId, BlockContext & ctx);

    /// Build a BlockContext for a sub-block (lambda/thunk body),
    /// mapping free variables to upvalue indices and params to stack slots.
    BlockContext buildSubBlockContext(
        const ir::IRBlock & block,
        const ir::FreeVars & freeVars);

    /// Push a variable's value onto the operand stack for capture.
    /// Used when emitting upvalue capture at a closure/thunk creation site.
    void emitCapture(ir::VarId var, PosIdx pos, BlockContext & ctx);

    /// Emit a lambda body with a formals-binding prologue.
    /// The prologue unpacks the attrset argument (pushed by OP_CALL_1 as
    /// slot 0) into individual formal parameter slots.
    /// Returns the code offset where the prologue begins.
    uint32_t emitSubBlockWithFormals(
        ir::BlockId blockId,
        const ir::FreeVars & freeVars,
        const ir::IRFormals & params,
        BlockContext & parentCtx);
};


// ============================================================================
// Top-level entry
// ============================================================================

CompilationUnit * emitFromIR(EvalState & state, const ir::IRModule & module)
{
    auto * unit = new (GC) CompilationUnit();
    IREmitter emitter(state, *unit, module);
    emitter.emit();
    return unit;
}


// ============================================================================
// Module emission
// ============================================================================

void IREmitter::emit()
{
    // The entry block (block 0) is the top-level expression.
    // It has no upvalues and no params (it's the program entry point).
    const auto & entryBlock = module.entryBlock();

    BlockContext ctx;

    // Reserve slots for params (entry block typically has none).
    for (auto p : entryBlock.params) {
        ctx.allocSlot(p);
    }

    // Pre-load external variables (baseEnv builtins like true, false, null,
    // builtins, etc.) into stack slots using OP_GET_LOCAL.
    // The entry block runs with curEnv = baseEnv, so level/displacement
    // from the original ExprVar can be used directly.
    for (auto & [varId, extRef] : module.externalVars) {
        uint32_t slot = ctx.allocSlot(varId);
        // Emit OP_GET_LOCAL to load from the env chain.
        switch (extRef.level) {
            case 0: unit.emit(OP_GET_LOCAL_0, extRef.displacement); break;
            case 1: unit.emit(OP_GET_LOCAL_1, extRef.displacement); break;
            case 2: unit.emit(OP_GET_LOCAL_2, extRef.displacement); break;
            case 3: unit.emit(OP_GET_LOCAL_3, extRef.displacement); break;
            default:
                unit.emit(OP_GET_LOCAL, packLevelDispl(
                    static_cast<uint8_t>(extRef.level),
                    static_cast<uint16_t>(extRef.displacement)));
                break;
        }
        unit.emit(OP_SET_STACK_SLOT, slot);
    }

    emitBlock(entryBlock, ctx);
}


// ============================================================================
// Block emission
// ============================================================================

void IREmitter::emitBlock(const ir::IRBlock & block, BlockContext & ctx)
{
    blockOffsets[block.id] = static_cast<uint32_t>(unit.code.size());

    // Pre-allocation pass for recursive bindings.
    //
    // In a recursive let, a thunk/lambda may capture a VarId that is
    // defined by a LATER binding in the same block (forward reference).
    // The upvalue capture (emitCapture) copies the Value* from the
    // stack slot at creation time.  If the slot hasn't been allocated
    // yet, emitVarRef would assert-fail.  Even if we pre-allocated the
    // slot with a placeholder, the thunk would capture a different
    // Value* than the one eventually written by OP_SET_STACK_SLOT.
    //
    // Solution: for every VarId that is both (a) defined by a binding
    // in this block and (b) referenced as a free variable by an
    // IRMkThunk or IRLambda in a preceding binding, we:
    //   1. Pre-allocate a Value* with OP_ALLOC_VALUE.
    //   2. Store it in the stack slot with OP_SET_STACK_SLOT.
    //   3. When the actual binding is emitted, use OP_COPY_TO_SLOT
    //      instead of OP_SET_STACK_SLOT, so the data is written into
    //      the SAME Value* that upvalues already captured.
    //
    // This mirrors how v1's OP_ENTER_LET pre-allocates env slots.

    // Collect the set of VarIds defined by bindings in this block.
    std::unordered_set<ir::VarId> definedInBlock;
    for (const auto & binding : block.bindings) {
        definedInBlock.insert(binding.result);
    }

    // Collect forward-referenced VarIds: any VarId that is both (a) defined
    // by a binding in this block and (b) referenced by an EARLIER binding.
    //
    // This covers:
    //   - Lambda/Thunk freeVars that reference a later binding (recursive let)
    //   - IRVarRef that references a later binding (rec attrsets where
    //     `isl = isl_0_20` references `isl_0_20` defined later)
    //   - IRRecAttrSet entries that reference later bindings
    //
    // For correctness, we scan all bindings' expressions for VarId refs
    // to later-defined bindings.  This is a superset of the Lambda/Thunk
    // freeVars check but handles all forward-reference patterns.
    std::unordered_set<ir::VarId> forwardRefs;
    {
        std::unordered_set<ir::VarId> seenDefined;
        for (const auto & binding : block.bindings) {
            // Collect all VarIds this binding references.
            ir::FreeVars exprRefs;
            ir::collectRefs(binding.expr, exprRefs);
            // Also check Lambda/Thunk freeVars (references from sub-blocks).
            std::visit([&](const auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::IRMkThunk>
                           || std::is_same_v<T, ir::IRLambda>) {
                    for (auto fv : e.freeVars.vars)
                        exprRefs.insert(fv);
                }
            }, binding.expr);
            // Any ref to a VarId that is defined in this block but
            // hasn't been seen yet is a forward reference.
            for (auto v : exprRefs.vars) {
                if (definedInBlock.count(v) && !seenDefined.count(v))
                    forwardRefs.insert(v);
            }
            seenDefined.insert(binding.result);
        }
    }

    // Pre-allocate stack slots for forward-referenced VarIds.
    for (auto fv : forwardRefs) {
        if (ctx.localSlots.find(fv) == ctx.localSlots.end()) {
            uint32_t slot = ctx.allocSlot(fv);
            unit.emit(OP_ALLOC_VALUE);
            unit.emit(OP_SET_STACK_SLOT, slot);
        }
    }

    // Emit each binding.  Bindings whose VarIds were pre-allocated
    // use OP_COPY_TO_SLOT instead of OP_SET_STACK_SLOT.
    for (const auto & binding : block.bindings) {
        emitExpr(binding.expr, binding.pos, ctx);

        if (forwardRefs.count(binding.result)) {
            // This VarId was pre-allocated: copy the expression result
            // into the existing Value* at the slot, preserving the
            // pointer that upvalues may already reference.
            auto it = ctx.localSlots.find(binding.result);
            assert(it != ctx.localSlots.end());
            unit.emit(OP_COPY_TO_SLOT, it->second);
        } else {
            // Normal binding: allocate a new slot.
            uint32_t slot = ctx.allocSlot(binding.result);
            unit.emit(OP_SET_STACK_SLOT, slot);
        }
    }

    // Emit the terminal.
    emitTerminal(block.terminal, ctx);
}


// ============================================================================
// Binding emission (simple, for inline blocks with no forward references)
// ============================================================================

void IREmitter::emitBinding(const ir::Binding & binding, BlockContext & ctx)
{
    // Emit the expression.  This pushes one Value* onto the operand stack.
    emitExpr(binding.expr, binding.pos, ctx);

    // Store the result into the binding's stack slot.
    uint32_t slot = ctx.allocSlot(binding.result);
    unit.emit(OP_SET_STACK_SLOT, slot);
}


// ============================================================================
// Expression emission
// ============================================================================

void IREmitter::emitExpr(const ir::IRExpr & expr, PosIdx pos, BlockContext & ctx)
{
    std::visit([&](const auto & e) {
        using T = std::decay_t<decltype(e)>;

        // -- Literals --
        if constexpr (std::is_same_v<T, ir::IRLitInt>) {
            auto n = e.value;
            if (n >= 0 && n <= static_cast<int64_t>(kOperandMask)) {
                unit.emit(OP_INT, static_cast<uint32_t>(n));
            } else {
                // Large integer: allocate in constant pool.
                auto * v = state.allocValue();
                v->mkInt(static_cast<NixInt::Inner>(n));
                unit.emit(OP_CONST, unit.addConstant(v));
            }
        }
        else if constexpr (std::is_same_v<T, ir::IRLitFloat>) {
            auto * v = state.allocValue();
            v->mkFloat(e.value);
            unit.emit(OP_CONST, unit.addConstant(v));
        }
        else if constexpr (std::is_same_v<T, ir::IRLitString>) {
            auto * v = state.allocValue();
            // Allocate the string in the evaluation memory arena.
            v->mkString(e.value, state.mem);
            unit.emit(OP_CONST, unit.addConstant(v));
        }
        else if constexpr (std::is_same_v<T, ir::IRLitPath>) {
            auto * v = state.allocValue();
            // Build a SourcePath from the stored accessor and canonical path.
            auto * accessor = static_cast<SourceAccessor *>(e.accessor);
            SourcePath sp(
                ref<SourceAccessor>(accessor->shared_from_this()),
                CanonPath(CanonPath::unchecked_t(), std::string(e.path)));
            v->mkPath(sp, state.mem);
            unit.emit(OP_CONST, unit.addConstant(v));
        }
        else if constexpr (std::is_same_v<T, ir::IRLitBool>) {
            unit.emit(e.value ? OP_TRUE : OP_FALSE);
        }
        else if constexpr (std::is_same_v<T, ir::IRLitNull>) {
            unit.emit(OP_NULL);
        }

        // -- Variable reference --
        else if constexpr (std::is_same_v<T, ir::IRVarRef>) {
            emitVarRef(e.var, pos, ctx);
        }

        // -- Lambda (closure creation) --
        else if constexpr (std::is_same_v<T, ir::IRLambda>) {
            bool hasFormals = !e.params.formals.empty();

            // 1. Emit the lambda body as a sub-block.
            //    For formals lambdas, we emit a prologue that unpacks
            //    the attrset argument into individual formal slots.
            uint32_t bodyOffset;
            uint16_t envSize;

            if (hasFormals) {
                bodyOffset = emitSubBlockWithFormals(
                    e.bodyBlock, e.freeVars, e.params, ctx);
                // envSize: 1 (raw arg) + number of formals + @-pattern.
                envSize = 1 + static_cast<uint16_t>(e.params.formals.size());
            } else {
                bodyOffset = emitSubBlock(e.bodyBlock, e.freeVars, ctx);
                auto & bodyBlock = module.blocks[e.bodyBlock];
                envSize = static_cast<uint16_t>(bodyBlock.params.size());
            }

            // 2. Register the lambda descriptor.
            uint32_t lambdaIdx = static_cast<uint32_t>(unit.lambdas.size());

            // Create a body thunk descriptor (for ExprBytecodeThunk compat).
            uint32_t bodyThunkIdx = static_cast<uint32_t>(unit.thunks.size());
            unit.thunks.push_back(ThunkDescriptor{
                .codeOffset = bodyOffset,
                .pos = e.pos,
                .sourceExpr = nullptr,
                .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
            });

            Formals * formals = nullptr;

            unit.lambdas.push_back(LambdaDescriptor{
                .codeOffset = bodyOffset,
                .pos = e.pos,
                .name = e.name,
                .arg = e.params.arg,
                .formals = formals,
                .envSize = envSize,
                .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
                .sourceExpr = e.sourceExpr,
                .bodyThunkIdx = bodyThunkIdx,
                .prologueOffset = bodyOffset,
            });

            // 3. Push captured upvalues onto the operand stack.
            for (auto freeVar : e.freeVars.vars) {
                emitCapture(freeVar, pos, ctx);
            }

            // 4. Emit OP_MAKE_CLOSURE_V2 with the upvalue count as data word.
            unit.emitPos(pos);
            unit.emit(OP_MAKE_CLOSURE_V2, lambdaIdx);
            unit.emit(OP_NOP, static_cast<uint32_t>(e.freeVars.size()));
        }

        // -- Function application --
        else if constexpr (std::is_same_v<T, ir::IRApp>) {
            emitVarRef(e.func, pos, ctx);
            emitVarRef(e.arg, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_CALL_1);
        }

        // -- Force --
        else if constexpr (std::is_same_v<T, ir::IRForce>) {
            emitVarRef(e.thunk, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_FORCE);
        }

        // -- Thunk creation --
        else if constexpr (std::is_same_v<T, ir::IRMkThunk>) {
            // Emit the thunk body as a sub-block.
            uint32_t bodyOffset = emitSubBlock(e.bodyBlock, e.freeVars, ctx);

            // Register the thunk descriptor.
            uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
            unit.thunks.push_back(ThunkDescriptor{
                .codeOffset = bodyOffset,
                .pos = e.pos,
                .sourceExpr = nullptr,
                .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
            });

            // Push captured upvalues.
            for (auto freeVar : e.freeVars.vars) {
                emitCapture(freeVar, pos, ctx);
            }

            // Emit OP_MAKE_THUNK_V2 + upvalue count data word.
            unit.emitPos(pos);
            unit.emit(OP_MAKE_THUNK_V2, thunkIdx);
            unit.emit(OP_NOP, static_cast<uint32_t>(e.freeVars.size()));
        }

        // -- Attribute select --
        else if constexpr (std::is_same_v<T, ir::IRAttrSelect>) {
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_FORCE);
            uint32_t symIdx = unit.addSymbol(e.name);
            unit.emit(OP_ATTR_SELECT, symIdx);
            unit.emit(OP_FORCE);
        }

        // -- Dynamic attribute select --
        else if constexpr (std::is_same_v<T, ir::IRAttrSelectDynamic>) {
            // OP_ATTR_SELECT_DYN pops nameVal then attrs from the operand stack.
            // Push order: attrs first, then nameVar.
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_FORCE);
            emitVarRef(e.nameVar, pos, ctx);
            unit.emit(OP_FORCE);
            unit.emit(OP_ATTR_SELECT_DYN);
            unit.emit(OP_FORCE);
        }

        // -- Has-attr --
        else if constexpr (std::is_same_v<T, ir::IRHasAttr>) {
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_FORCE);
            uint32_t symIdx = unit.addSymbol(e.name);
            unit.emit(OP_HAS_ATTR, symIdx);
        }

        // -- Has-attr (dynamic) --
        else if constexpr (std::is_same_v<T, ir::IRHasAttrDynamic>) {
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_FORCE);
            emitVarRef(e.nameVar, pos, ctx);
            unit.emit(OP_FORCE);
            unit.emit(OP_HAS_ATTR_DYN);
        }

        // -- Static attribute set --
        else if constexpr (std::is_same_v<T, ir::IRAttrSet>) {
            // Push all attribute values, then OP_ATTRS_INIT.
            for (auto & entry : e.entries) {
                emitVarRef(entry.value, pos, ctx);
            }
            unit.emitPos(pos);
            uint32_t nAttrs = static_cast<uint32_t>(e.entries.size());
            unit.emit(OP_ATTRS_INIT, nAttrs);
            // Emit (symbol, position) data word pairs.
            for (auto & entry : e.entries) {
                unit.emit(OP_NOP, unit.addSymbol(entry.name));
                unit.emit(OP_NOP, unit.addPos(entry.pos));
            }
        }

        // -- Dynamic attribute set --
        else if constexpr (std::is_same_v<T, ir::IRAttrSetDynamic>) {
            // Push static values.
            for (auto & entry : e.staticEntries) {
                emitVarRef(entry.value, pos, ctx);
            }
            // Push dynamic name/value pairs.
            for (auto & entry : e.dynamicEntries) {
                emitVarRef(entry.nameVar, pos, ctx);
                emitVarRef(entry.value, pos, ctx);
            }
            uint32_t nStatic = static_cast<uint32_t>(e.staticEntries.size());
            uint32_t nDynamic = static_cast<uint32_t>(e.dynamicEntries.size());
            assert(nStatic < 4096 && nDynamic < 4096);
            unit.emitPos(pos);
            unit.emit(OP_ATTRS_DYN_INIT, (nStatic << 12) | nDynamic);
            for (auto & entry : e.staticEntries) {
                unit.emit(OP_NOP, unit.addSymbol(entry.name));
                unit.emit(OP_NOP, unit.addPos(entry.pos));
            }
            for (auto & entry : e.dynamicEntries) {
                unit.emit(OP_NOP, unit.addPos(entry.pos));
            }
        }

        // -- Recursive attribute set --
        else if constexpr (std::is_same_v<T, ir::IRRecAttrSet>) {
            // Recursive attrsets need env-chain semantics for self-reference.
            // For v2: allocate an env for the self-var, emit bindings as
            // thunks, then build the attrset.
            uint32_t nAttrs = static_cast<uint32_t>(e.entries.size());

            unit.emitPos(pos);
            unit.emit(OP_ENTER_LET, nAttrs);

            // Emit each binding.
            for (uint32_t i = 0; i < nAttrs; ++i) {
                emitVarRef(e.entries[i].value, pos, ctx);
                unit.emit(OP_SET_ENV_SLOT, i);
            }

            // Build the attrset from env slots.
            for (uint32_t i = 0; i < nAttrs; ++i) {
                unit.emit(OP_GET_LOCAL_0, i);
            }
            unit.emit(OP_ATTRS_INIT, nAttrs);
            for (auto & entry : e.entries) {
                unit.emit(OP_NOP, unit.addSymbol(entry.name));
                unit.emit(OP_NOP, unit.addPos(entry.pos));
            }

            unit.emit(OP_LEAVE_SCOPE);
        }

        // -- List --
        else if constexpr (std::is_same_v<T, ir::IRList>) {
            for (auto elem : e.elems) {
                emitVarRef(elem, pos, ctx);
            }
            unit.emitPos(pos);
            unit.emit(OP_LIST_INIT, static_cast<uint32_t>(e.elems.size()));
        }

        // -- If/then/else --
        else if constexpr (std::is_same_v<T, ir::IRIf>) {
            // The IR has lowered if into separate blocks.
            // Emit inline: cond, JUMP_IF_FALSE, then-block, JUMP, else-block.
            emitVarRef(e.cond, pos, ctx);
            unit.emitPos(pos);
            uint32_t jumpElse = unit.emit(OP_JUMP_IF_FALSE, 0);
            emitInlineBlock(e.thenBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpElse);
            emitInlineBlock(e.elseBlock, ctx);
            unit.patchJump(jumpEnd);
        }

        // -- PrimOp call --
        else if constexpr (std::is_same_v<T, ir::IRPrimOpCall>) {
            // Push all args, then emit sequential CALL_1.
            // Create the primop Value, then call it with each arg.
            auto * primVal = state.allocValue();
            primVal->mkPrimOp(const_cast<PrimOp *>(e.primOp));
            unit.emit(OP_CONST, unit.addConstant(primVal));
            for (auto arg : e.args) {
                emitVarRef(arg, pos, ctx);
                unit.emitPos(e.pos);
                unit.emit(OP_CALL_1);
            }
        }

        // -- With scope --
        else if constexpr (std::is_same_v<T, ir::IRWith>) {
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(e.pos);
            unit.emit(OP_PUSH_WITH);
            emitInlineBlock(e.bodyBlock, ctx);
            unit.emit(OP_LEAVE_SCOPE);
        }

        // -- With lookup --
        else if constexpr (std::is_same_v<T, ir::IRWithLookup>) {
            // Use the original ExprVar from the AST, which has the
            // correct fromWith chain, level, and name set by bindVars().
            // OP_GET_WITH walks the with-chain using this information.
            assert(e.sourceVar && "IRWithLookup must have a sourceVar");
            uint32_t exprIdx = unit.addExpr(e.sourceVar);
            unit.emitPos(e.pos);
            unit.emit(OP_GET_WITH, exprIdx);
        }

        // -- String concatenation --
        else if constexpr (std::is_same_v<T, ir::IRConcatStrings>) {
            for (auto part : e.parts) {
                emitVarRef(part, pos, ctx);
            }
            uint32_t nParts = static_cast<uint32_t>(e.parts.size());
            uint32_t operand = nParts | (e.forceString ? (1u << 23) : 0);
            unit.emitPos(pos);
            unit.emit(OP_STR_CONCAT_INIT, operand);
        }

        // -- Assert --
        else if constexpr (std::is_same_v<T, ir::IRAssert>) {
            emitVarRef(e.cond, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_ASSERT);
            emitInlineBlock(e.bodyBlock, ctx);
        }

        // -- Logical not --
        else if constexpr (std::is_same_v<T, ir::IRNot>) {
            emitVarRef(e.operand, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_NOT);
        }

        // -- Short-circuit AND --
        else if constexpr (std::is_same_v<T, ir::IRAnd>) {
            emitVarRef(e.lhs, pos, ctx);
            unit.emitPos(pos);
            uint32_t jumpFalse = unit.emit(OP_JUMP_IF_FALSE, 0);
            emitInlineBlock(e.rhsBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpFalse);
            unit.emit(OP_FALSE);
            unit.patchJump(jumpEnd);
        }

        // -- Short-circuit OR --
        else if constexpr (std::is_same_v<T, ir::IROr>) {
            emitVarRef(e.lhs, pos, ctx);
            unit.emitPos(pos);
            uint32_t jumpTrue = unit.emit(OP_JUMP_IF_TRUE, 0);
            emitInlineBlock(e.rhsBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpTrue);
            unit.emit(OP_TRUE);
            unit.patchJump(jumpEnd);
        }

        // -- Implication --
        else if constexpr (std::is_same_v<T, ir::IRImpl>) {
            emitVarRef(e.lhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_NOT);
            uint32_t jumpTrue = unit.emit(OP_JUMP_IF_TRUE, 0);
            emitInlineBlock(e.rhsBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpTrue);
            unit.emit(OP_TRUE);
            unit.patchJump(jumpEnd);
        }

        // -- Binary arithmetic and comparison --
        else if constexpr (std::is_same_v<T, ir::IRAdd>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_ADD);
        }
        else if constexpr (std::is_same_v<T, ir::IRSub>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_SUB);
        }
        else if constexpr (std::is_same_v<T, ir::IRMul>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_MUL);
        }
        else if constexpr (std::is_same_v<T, ir::IRDiv>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_DIV);
        }
        else if constexpr (std::is_same_v<T, ir::IRNegate>) {
            emitVarRef(e.operand, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_NEGATE);
        }
        else if constexpr (std::is_same_v<T, ir::IREq>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_EQ);
        }
        else if constexpr (std::is_same_v<T, ir::IRNEq>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_NEQ);
        }
        else if constexpr (std::is_same_v<T, ir::IRLess>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_LESS_THAN);
        }

        // -- Attrset update --
        else if constexpr (std::is_same_v<T, ir::IRUpdate>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_ATTRS_UPDATE);
        }

        // -- List concatenation --
        else if constexpr (std::is_same_v<T, ir::IRConcatLists>) {
            emitVarRef(e.lhs, pos, ctx);
            emitVarRef(e.rhs, pos, ctx);
            unit.emitPos(pos);
            unit.emit(OP_LIST_CONCAT);
        }

        // -- __curPos --
        else if constexpr (std::is_same_v<T, ir::IRPos>) {
            unit.emitPos(e.pos);
            unit.emit(OP_POS, unit.addPos(e.pos));
        }

        else {
            static_assert(!std::is_same_v<T, T>,
                "IREmitter::emitExpr: unhandled IRExpr variant");
        }
    }, expr);
}


// ============================================================================
// Variable reference emission
// ============================================================================

void IREmitter::emitVarRef(ir::VarId var, PosIdx pos, BlockContext & ctx)
{
    // Check local slots first (defined in this block).
    auto localIt = ctx.localSlots.find(var);
    if (localIt != ctx.localSlots.end()) {
        unit.emit(OP_GET_STACK_SLOT, localIt->second);
        return;
    }

    // Check upvalue slots (captured from enclosing scope).
    auto upIt = ctx.upvalueSlots.find(var);
    if (upIt != ctx.upvalueSlots.end()) {
        unit.emit(OP_GET_UPVALUE, upIt->second);
        return;
    }

    // Check if this is an external variable (from baseEnv).
    auto extIt = module.externalVars.find(var);
    if (extIt != module.externalVars.end()) {
        // External variable: load from the base env chain using v1 opcodes.
        auto & ext = extIt->second;
        uint32_t level = ext.level;
        uint32_t displ = ext.displacement;
        switch (level) {
            case 0: unit.emit(OP_GET_LOCAL_0, displ); break;
            case 1: unit.emit(OP_GET_LOCAL_1, displ); break;
            case 2: unit.emit(OP_GET_LOCAL_2, displ); break;
            case 3: unit.emit(OP_GET_LOCAL_3, displ); break;
            default:
                unit.emit(OP_GET_LOCAL, packLevelDispl(
                    static_cast<uint8_t>(level), static_cast<uint16_t>(displ)));
                break;
        }
        return;
    }

    // The variable should always be found in one of the above maps.
    // If not, it's a bug in free variable analysis or slot allocation.
    {
        auto nameIt = module.varNames.find(var);
        std::ostringstream oss;
        oss << state.positions[pos];
        fprintf(stderr, "IREmitter::emitVarRef: VarId %u", var);
        if (nameIt != module.varNames.end())
            fprintf(stderr, " (%s)", nameIt->second.c_str());
        fprintf(stderr, " not found. Locals(%zu):", ctx.localSlots.size());
        for (auto & [v, s] : ctx.localSlots) {
            auto nit = module.varNames.find(v);
            if (nit != module.varNames.end())
                fprintf(stderr, " v%u(%s)=s%u", v, nit->second.c_str(), s);
            else
                fprintf(stderr, " v%u=s%u", v, s);
        }
        fprintf(stderr, "  Upvalues(%zu):", ctx.upvalueSlots.size());
        for (auto & [v, s] : ctx.upvalueSlots) {
            auto nit = module.varNames.find(v);
            if (nit != module.varNames.end())
                fprintf(stderr, " v%u(%s)=u%u", v, nit->second.c_str(), s);
            else
                fprintf(stderr, " v%u=u%u", v, s);
        }
        fprintf(stderr, "  Pos: %s\n", oss.str().c_str());
    }
    assert(false && "IREmitter::emitVarRef: VarId not found in local or upvalue slots");
}


// ============================================================================
// Upvalue capture emission
// ============================================================================

void IREmitter::emitCapture(ir::VarId var, PosIdx pos, BlockContext & ctx)
{
    // The captured variable must be accessible from the current scope
    // (either as a local or as an upvalue that we forward).
    emitVarRef(var, pos, ctx);
}


// ============================================================================
// Terminal emission
// ============================================================================

void IREmitter::emitTerminal(const ir::Terminal & term, BlockContext & ctx)
{
    std::visit([&](const auto & t) {
        using T = std::decay_t<decltype(t)>;

        if constexpr (std::is_same_v<T, ir::TermReturn>) {
            if (t.value != ir::kInvalidVar) {
                emitVarRef(t.value, t.pos, ctx);
            }
            unit.emitPos(t.pos);
            unit.emit(OP_RETURN);
        }
        else if constexpr (std::is_same_v<T, ir::TermTailCall>) {
            // For now, emit as a regular call + return.
            // True tail call optimization can be added later.
            emitVarRef(t.func, t.pos, ctx);
            emitVarRef(t.arg, t.pos, ctx);
            unit.emitPos(t.pos);
            unit.emit(OP_CALL_1);
            unit.emit(OP_RETURN);
        }
        else if constexpr (std::is_same_v<T, ir::TermBranch>) {
            emitVarRef(t.cond, t.pos, ctx);
            unit.emitPos(t.pos);
            uint32_t jumpElse = unit.emit(OP_JUMP_IF_FALSE, 0);
            emitInlineBlock(t.thenBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpElse);
            emitInlineBlock(t.elseBlock, ctx);
            unit.patchJump(jumpEnd);
        }
    }, term);
}


// ============================================================================
// Sub-block emission (lambda/thunk bodies)
// ============================================================================

uint32_t IREmitter::emitSubBlock(
    ir::BlockId blockId,
    const ir::FreeVars & freeVars,
    BlockContext & parentCtx)
{
    const auto & block = module.blocks[blockId];

    // Jump over the sub-block body in the parent's code stream.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);

    uint32_t bodyOffset = static_cast<uint32_t>(unit.code.size());

    // Build a fresh BlockContext for the sub-block.
    // Free variables become upvalue slots; params become local slots.
    BlockContext subCtx = buildSubBlockContext(block, freeVars);

    emitBlock(block, subCtx);

    // Patch the jump-over.
    unit.patchJump(jumpOver);

    return bodyOffset;
}

BlockContext IREmitter::buildSubBlockContext(
    const ir::IRBlock & block,
    const ir::FreeVars & freeVars)
{
    BlockContext ctx;

    // Free variables are accessed via OP_GET_UPVALUE.
    // Their upvalue index is their position in the sorted FreeVars list.
    for (uint32_t i = 0; i < freeVars.vars.size(); ++i) {
        ctx.upvalueSlots[freeVars.vars[i]] = i;
    }

    // Parameters get stack slots.
    for (auto p : block.params) {
        ctx.allocSlot(p);
    }

    return ctx;
}


// ============================================================================
// Sub-block emission with formals prologue
// ============================================================================

uint32_t IREmitter::emitSubBlockWithFormals(
    ir::BlockId blockId,
    const ir::FreeVars & freeVars,
    const ir::IRFormals & params,
    BlockContext & parentCtx)
{
    const auto & block = module.blocks[blockId];

    // Jump over the sub-block body in the parent's code stream.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);

    uint32_t bodyOffset = static_cast<uint32_t>(unit.code.size());

    // Build a custom BlockContext for formals lambdas.
    //
    // Stack layout:
    //   slot 0: raw argument (the attrset pushed by OP_CALL_1)
    //   slot 1..N: formal parameters (unpacked by prologue)
    //   slot N+1..: upvalues are accessed via OP_GET_UPVALUE, not stack slots
    //
    // The IR body block has params = [@-pattern?, formal1, formal2, ...].
    // We need to map each param VarId to the correct stack slot.

    BlockContext subCtx;

    // Free variables -> upvalue slots.
    for (uint32_t i = 0; i < freeVars.vars.size(); ++i) {
        subCtx.upvalueSlots[freeVars.vars[i]] = i;
    }

    // Slot 0: reserved for the raw attrset argument (pushed by caller).
    uint32_t argSlot = subCtx.nextSlot++;

    // Pre-allocate slots for all IR params.
    // This ensures the formal slots exist in the stack frame BEFORE
    // the prologue does any operand-stack operations (OP_DUP etc).
    // Without pre-allocation, OP_SET_STACK_SLOT's auto-extend logic
    // can collide with operand-stack entries.
    for (auto p : block.params) {
        uint32_t slot = subCtx.allocSlot(p);
        unit.emit(OP_ALLOC_VALUE);
        unit.emit(OP_SET_STACK_SLOT, slot);
    }

    // --- Emit the formals-binding prologue ---
    //
    // Read the raw attrset arg from slot 0 and force it.
    // After this sequence, TOS = the forced attrset.
    unit.emit(OP_GET_STACK_SLOT, argSlot);
    unit.emit(OP_FORCE);

    // If there's an @-pattern, copy the attrset into its slot.
    if (params.arg) {
        // The @-pattern is the first param.
        unit.emit(OP_DUP);
        auto it = subCtx.localSlots.find(block.params[0]);
        assert(it != subCtx.localSlots.end());
        unit.emit(OP_COPY_TO_SLOT, it->second);
    }

    // Unpack each formal from the attrset.
    // The attrset remains on the operand stack (via DUP) for each select.
    // OP_COPY_TO_SLOT writes into the pre-allocated Value* at the slot,
    // not overwriting the operand stack pointer.
    uint32_t formalParamStart = params.arg ? 1 : 0;

    for (uint32_t i = 0; i < params.formals.size(); ++i) {
        auto & formal = params.formals[i];
        ir::VarId formalVarId = block.params[formalParamStart + i];

        auto slotIt = subCtx.localSlots.find(formalVarId);
        assert(slotIt != subCtx.localSlots.end());
        uint32_t formalSlot = slotIt->second;

        if (formal.defaultBody != ir::kInvalidBlock) {
            // Formal with default: check if attr exists.
            unit.emit(OP_DUP);
            uint32_t symIdx = unit.addSymbol(formal.name);
            unit.emit(OP_HAS_ATTR, symIdx);
            uint32_t jumpToDefault = unit.emit(OP_JUMP_IF_FALSE, 0);

            // Attr exists: select it and copy into the slot.
            unit.emit(OP_DUP);
            unit.emit(OP_ATTR_SELECT, symIdx);
            unit.emit(OP_COPY_TO_SLOT, formalSlot);
            uint32_t jumpPastDefault = unit.emit(OP_JUMP, 0);

            // Attr missing: create a thunk for the default value.
            //
            // Defaults must be thunks, not eagerly evaluated, because
            // formals are sorted alphabetically by Symbol (which depends
            // on interning order, not source order).  A default may
            // reference a sibling formal that sorts later and hasn't
            // been unpacked yet.  Wrapping defaults as thunks matches
            // tree-walker semantics: the default is only forced when
            // the formal is actually used, at which point all
            // argument-provided formals have been unpacked.
            unit.patchJump(jumpToDefault);
            {
                uint32_t bodyOffset = emitSubBlock(
                    formal.defaultBody, formal.defaultFreeVars, subCtx);

                uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
                unit.thunks.push_back(ThunkDescriptor{
                    .codeOffset = bodyOffset,
                    .pos = formal.pos,
                    .sourceExpr = nullptr,
                    .nUpvalues = static_cast<uint16_t>(formal.defaultFreeVars.size()),
                });

                // Push captured upvalues.
                for (auto freeVar : formal.defaultFreeVars.vars) {
                    emitCapture(freeVar, formal.pos, subCtx);
                }

                unit.emitPos(formal.pos);
                unit.emit(OP_MAKE_THUNK_V2, thunkIdx);
                unit.emit(OP_NOP, static_cast<uint32_t>(formal.defaultFreeVars.size()));
            }
            unit.emit(OP_COPY_TO_SLOT, formalSlot);

            unit.patchJump(jumpPastDefault);
        } else {
            // Required formal: select and copy.
            unit.emit(OP_DUP);
            uint32_t symIdx = unit.addSymbol(formal.name);
            unit.emit(OP_ATTR_SELECT, symIdx);
            unit.emit(OP_COPY_TO_SLOT, formalSlot);
        }
    }

    // Pop the attrset from the operand stack.
    unit.emit(OP_POP);

    // --- Emit the body block ---
    emitBlock(block, subCtx);

    // Patch the jump-over.
    unit.patchJump(jumpOver);

    return bodyOffset;
}


// ============================================================================
// Inline block emission (if-branches, with-bodies)
// ============================================================================

uint32_t IREmitter::emitInlineBlock(ir::BlockId blockId, BlockContext & ctx)
{
    const auto & block = module.blocks[blockId];
    uint32_t offset = static_cast<uint32_t>(unit.code.size());

    blockOffsets[blockId] = offset;

    // Inline blocks share the parent's context.
    // Their bindings get stack slots in the parent's frame.
    // Their terminal's return value is left on the operand stack
    // (instead of OP_RETURN, we just leave the result for the parent).
    //
    // Forward-reference pre-allocation: same logic as emitBlock().
    // Inline blocks can contain let bindings with recursive functions
    // (e.g., `if cond then ... else let f = a: ... f ...; in ...`).
    // Without pre-allocation, the lambda tries to capture `f`'s VarId
    // before it has been assigned a stack slot.
    std::unordered_set<ir::VarId> definedInBlock;
    for (const auto & binding : block.bindings)
        definedInBlock.insert(binding.result);

    // Same forward-reference detection as emitBlock().
    std::unordered_set<ir::VarId> forwardRefs;
    {
        std::unordered_set<ir::VarId> seenDefined;
        for (const auto & binding : block.bindings) {
            ir::FreeVars exprRefs;
            ir::collectRefs(binding.expr, exprRefs);
            std::visit([&](const auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::IRMkThunk>
                           || std::is_same_v<T, ir::IRLambda>) {
                    for (auto fv : e.freeVars.vars)
                        exprRefs.insert(fv);
                }
            }, binding.expr);
            for (auto v : exprRefs.vars) {
                if (definedInBlock.count(v) && !seenDefined.count(v))
                    forwardRefs.insert(v);
            }
            seenDefined.insert(binding.result);
        }
    }

    for (auto fv : forwardRefs) {
        if (ctx.localSlots.find(fv) == ctx.localSlots.end()) {
            uint32_t slot = ctx.allocSlot(fv);
            unit.emit(OP_ALLOC_VALUE);
            unit.emit(OP_SET_STACK_SLOT, slot);
        }
    }

    for (const auto & binding : block.bindings) {
        emitExpr(binding.expr, binding.pos, ctx);
        if (forwardRefs.count(binding.result)) {
            auto it = ctx.localSlots.find(binding.result);
            assert(it != ctx.localSlots.end());
            unit.emit(OP_COPY_TO_SLOT, it->second);
        } else {
            uint32_t slot = ctx.allocSlot(binding.result);
            unit.emit(OP_SET_STACK_SLOT, slot);
        }
    }

    // For inline blocks, the terminal should be TermReturn.
    // Instead of emitting OP_RETURN, just push the return value.
    std::visit([&](const auto & t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, ir::TermReturn>) {
            if (t.value != ir::kInvalidVar) {
                emitVarRef(t.value, t.pos, ctx);
            }
        }
        else if constexpr (std::is_same_v<T, ir::TermBranch>) {
            // Nested branch in an inline block.
            emitVarRef(t.cond, t.pos, ctx);
            unit.emitPos(t.pos);
            uint32_t jumpElse = unit.emit(OP_JUMP_IF_FALSE, 0);
            emitInlineBlock(t.thenBlock, ctx);
            uint32_t jumpEnd = unit.emit(OP_JUMP, 0);
            unit.patchJump(jumpElse);
            emitInlineBlock(t.elseBlock, ctx);
            unit.patchJump(jumpEnd);
        }
        else if constexpr (std::is_same_v<T, ir::TermTailCall>) {
            // Tail call in inline block: emit as normal call.
            emitVarRef(t.func, t.pos, ctx);
            emitVarRef(t.arg, t.pos, ctx);
            unit.emitPos(t.pos);
            unit.emit(OP_CALL_1);
        }
    }, block.terminal);

    return offset;
}


} // namespace nix::bytecode
