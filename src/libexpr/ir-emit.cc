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
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <boost/unordered/unordered_flat_set.hpp>

namespace nix::bytecode {

// Per-call emit-phase timing observer.  Filled in when non-null.
thread_local EmitPhaseTiming * emitPhaseTiming = nullptr;

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

    /// Cell references for variables accessed via OP_CELL_GET.
    /// In sub-block contexts, forward-ref vars from a parent block are
    /// read through a shared GC-traced cell captured as an upvalue.
    /// Supports multiple cell layers (parent + grandparent) via per-entry
    /// upvalue indices.
    struct CellRef {
        uint32_t upvalueIdx;  ///< Upvalue index holding the cell pointer
        uint32_t entryIdx;    ///< Entry index within the cell
    };
    std::unordered_map<ir::VarId, CellRef> cellRefs;

    /// Current block's cell for forward references.
    /// Set by emitBlock when the block has forward-referenced bindings.
    /// IRMkThunk/IRLambda handlers read these to build cell-aware
    /// sub-block contexts.  UINT32_MAX means no cell is active.
    uint32_t blockCellSlot = UINT32_MAX;
    std::unordered_map<ir::VarId, uint32_t> blockCellMap;

    /// Next available stack slot in this frame.
    uint32_t nextSlot = 0;

    /// Per-VarId use count within the current block (collected at the
    /// start of each emitBlock; cleared after).  Used by the ANF
    /// de-materialization peephole.
    boost::unordered_flat_map<ir::VarId, uint32_t> blockUseCounts;

    /// VarId whose result was just stored via OP_SET_STACK_SLOT (the
    /// most recent binding that took the operand-stack-then-store
    /// path).  kInvalidVar means the last emit was something else.
    /// emitVarRef inspects this together with blockUseCounts to decide
    /// whether the SET_STACK_SLOT/GET_STACK_SLOT round-trip can be
    /// elided.
    ir::VarId lastBoundVar = ir::kInvalidVar;

    /// Allocate a fresh stack slot for a VarId.
    /// If the var already has a slot in this context (e.g., pre-allocated
    /// for a forward ref or set by an earlier strictness-spliced binding),
    /// return the existing slot rather than burning a new one.  Without
    /// this guard, the second allocation would overwrite the entry,
    /// leaving the original slot dangling and bumping nextSlot for no
    /// reason.
    uint32_t allocSlot(ir::VarId var)
    {
        auto it = localSlots.find(var);
        if (it != localSlots.end())
            return it->second;
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
    /// Maximum slot used by the most recently emitted sub-block.
    /// Set by emitSubBlock / emitSubBlockWithFormals; consumed by the
    /// IRMkThunk / IRLambda emitter to populate ThunkDescriptor.maxSlot.
    /// Also read by lazyEmitThunkBody (Phase 3.1f-6).
    uint16_t lastSubBlockMaxSlot = 0;

    IREmitter(EvalState & state, CompilationUnit & unit, const ir::IRModule & module)
        : state(state)
        , unit(unit)
        , module(module)
    {}

    /// Emit the entire module.  Entry block is block 0.
    void emit();

    /// Phase 3.1f-4: Emit JUST the body of a sub-block (no jump-over,
    /// no parent-flow management).  Public so the lazy-emit path
    /// (Phase 3.1f-6) can call it from a fresh emitter at first force.
    /// See the private declaration below for parameters.
    uint32_t emitBlockOnly(
        ir::BlockId blockId,
        const ir::FreeVars & freeVars);

    /// Phase 3.1f-7: public wrapper around emitBlock for the cell-capture
    /// lazy-emit path.  The caller is responsible for constructing a
    /// suitable BlockContext (cellRefs + upvalueSlots populated).
    void emitBlockPublic(const ir::IRBlock & block, BlockContext & ctx)
    {
        emitBlock(block, ctx);
    }

    /// Phase 3.1f-9: public wrapper around emitFormalsBodyOnly for the
    /// formals-lambda lazy-emit path.  Defined out-of-line below.
    uint32_t emitFormalsBodyPublic(
        ir::BlockId blockId,
        const ir::FreeVars & freeVars,
        const ir::IRFormals & params);

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

    /// Phase 3.1f-9: emit the formals prologue + body at the END of
    /// unit.code (no jump-over wrapping).  Used by both the eager
    /// path (via emitSubBlockWithFormals) and the lazy-emit path.
    uint32_t emitFormalsBodyOnly(
        ir::BlockId blockId,
        const ir::FreeVars & freeVars,
        const ir::IRFormals & params);
};


// ============================================================================
// Top-level entry
// ============================================================================

CompilationUnit * emitFromIR(EvalState & state, const ir::IRModule & module)
{
    using Clock = std::chrono::steady_clock;
    auto micros = [](auto a, auto b) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };

    auto * unit = new (GC) CompilationUnit();
    IREmitter emitter(state, *unit, module);

    auto t0 = Clock::now();
    emitter.emit();
    auto t1 = Clock::now();

    // Phase 3.1 lite: cachedExpr is now allocated lazily by
    // OP_MAKE_THUNK_V2 / OP_MAKE_CLOSURE_V2 on first thunk/closure
    // creation.  The opcode handlers store the result back into the
    // descriptor so subsequent creations reuse the cached pointer.
    //
    // Skipping the eager pre-allocation here saves N allocations at
    // compile time for descriptors that get instantiated at runtime
    // anyway, and entirely skips the work for descriptors whose code
    // path is never taken.  Net total allocations are the same on a
    // full-coverage workload but the cost shifts off the cold-start
    // critical path, where it overlaps with subsequent evaluation
    // work rather than blocking it.

    if (emitPhaseTiming) {
        emitPhaseTiming->emitCoreUs        += micros(t0, t1);
        emitPhaseTiming->numInstructions      += unit->code.size();
        emitPhaseTiming->numThunkDescriptors  += unit->thunks.size();
        emitPhaseTiming->numLambdaDescriptors += unit->lambdas.size();
    }

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

    // Reserve roughly enough capacity in every CU vector to avoid
    // mid-emission reallocation.  Empirically the bytecode is ~5-8
    // instructions per IR binding; round up generously.  A wrong guess
    // just costs a single realloc at the end.  These vectors all grew
    // unreserved before, contributing to the 24% compile-time share.
    size_t totalBindings = 0;
    size_t totalThunks = 0, totalLambdas = 0;
    for (const auto & blk : module.blocks)
        totalBindings += blk.bindings.size();
    for (const auto & blk : module.blocks) {
        for (const auto & b : blk.bindings) {
            if (std::holds_alternative<ir::IRMkThunk>(b.expr)) totalThunks++;
            if (std::holds_alternative<ir::IRLambda>(b.expr)) totalLambdas++;
        }
    }
    // Phase 3.1f-3: When lazy emit is enabled (NIX_VM_V2_LAZY_EMIT=1),
    // unit.code grows incrementally as deferred bodies are realized.
    // The dispatch loop re-reads cu->code[ip] every iteration, so a
    // realloc-during-execute is safe semantically — but we'd still
    // pay the copy.  Reserve generously upfront so the typical
    // never-grow case is hit.  Extra cost: ~megabyte of unused
    // virtual address space per CU; the OS doesn't fault until
    // touched.
    const char * lazyEmitEnv = ::getenv("NIX_VM_V2_LAZY_EMIT");
    bool lazyEmit = lazyEmitEnv && std::string(lazyEmitEnv) == "1";
    size_t codeReserve = lazyEmit
        ? (totalBindings * 32 + 4096)   // generous for lazy growth
        : (totalBindings * 8 + 64);
    unit.code.reserve(codeReserve);
    unit.thunks.reserve(totalThunks + 16);
    unit.lambdas.reserve(totalLambdas + 16);
    unit.constants.reserve(totalBindings + 32);
    unit.exprPool.reserve(totalBindings / 4 + 16);
    unit.positions.reserve(totalBindings * 2 + 32);
    unit.posPool.reserve(totalBindings + 16);

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
    boost::unordered_flat_set<ir::VarId> definedInBlock;
    for (const auto & binding : block.bindings) {
        definedInBlock.insert(binding.result);
    }

    // Single fused pass: per-VarId use counts (for the ANF peephole)
    // AND forward-reference detection (for cell-based late binding).
    //
    // Use counts: a binding whose result is referenced exactly ONCE
    // inside this block can have its SET_STACK_SLOT/GET_STACK_SLOT
    // round-trip elided when the GET immediately follows the SET (the
    // value is already on the operand stack from the producer).  Use
    // counts only consider DIRECT refs from binding expressions, NOT
    // sub-block captures (those go through emitCapture, which doesn't
    // benefit from the peephole).
    //
    // Forward refs: any VarId defined in this block but referenced by
    // an EARLIER binding's expression OR by an IRMkThunk/IRLambda's
    // freeVars set.  Triggers cell-based late binding (ALLOC_CELL +
    // CELL_GET on read; COPY_TO_SLOT + CELL_SET on write).
    //
    // Previously these were two separate passes that each called
    // collectRefs() per binding.  Merging cuts the per-binding work
    // in half (B2).
    ctx.blockUseCounts.clear();
    boost::unordered_flat_set<ir::VarId> forwardRefs;
    {
        boost::unordered_flat_set<ir::VarId> seenDefined;
        for (const auto & binding : block.bindings) {
            // M5: use the cached direct-refs from blockFreeVars instead
            // of re-running collectRefs.  Falls back to a fresh walk if
            // the cache is empty (defensive — disk-cache or
            // alternative compile paths may not populate it).
            const ir::FreeVars * directRefsPtr = &binding.directRefs;
            ir::FreeVars fallback;
            if (binding.directRefs.vars.empty()) {
                collectRefs(binding.expr, fallback);
                directRefsPtr = &fallback;
            }
            const ir::FreeVars & directRefs = *directRefsPtr;

            // Use counts: count direct refs only.  Sub-block captures
            // are not eligible for the peephole, so they are excluded.
            for (auto v : directRefs.vars)
                if (definedInBlock.count(v))
                    ctx.blockUseCounts[v]++;

            // For forward-ref detection we also need IRMkThunk /
            // IRLambda freeVars (a sub-block can forward-ref a later
            // binding).  Iterate them in-place rather than building a
            // merged FreeVars copy.
            for (auto v : directRefs.vars)
                if (definedInBlock.count(v) && !seenDefined.count(v))
                    forwardRefs.insert(v);
            std::visit([&](const auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::IRMkThunk>
                           || std::is_same_v<T, ir::IRLambda>) {
                    for (auto fv : e.freeVars.vars)
                        if (definedInBlock.count(fv) && !seenDefined.count(fv))
                            forwardRefs.insert(fv);
                }
            }, binding.expr);

            seenDefined.insert(binding.result);
        }
        // Terminal references contribute to use counts only — terminals
        // never forward-ref (they execute after all bindings).
        std::visit([&](const auto & t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, ir::TermReturn>) {
                if (definedInBlock.count(t.value))
                    ctx.blockUseCounts[t.value]++;
            } else if constexpr (std::is_same_v<T, ir::TermTailCall>) {
                if (definedInBlock.count(t.func))
                    ctx.blockUseCounts[t.func]++;
                if (definedInBlock.count(t.arg))
                    ctx.blockUseCounts[t.arg]++;
            } else if constexpr (std::is_same_v<T, ir::TermBranch>) {
                if (definedInBlock.count(t.cond))
                    ctx.blockUseCounts[t.cond]++;
            }
        }, block.terminal);
    }

    // Cell-based forward references.
    //
    // Cell-augmented forward references.
    //
    // Direct references within the same block need pointer sharing
    // through a pre-allocated Value* (ALLOC_VALUE + COPY_TO_SLOT).
    // Sub-blocks (thunks/lambdas) need late-binding through the cell
    // (CELL_GET at force time).  We do BOTH:
    //   - ALLOC_VALUE for the slot placeholder (pointer sharing)
    //   - COPY_TO_SLOT to write INTO the pre-allocated Value*
    //   - CELL_SET to update the cell entry for sub-block reads
    //
    // emitVarRef checks localSlots FIRST, so direct references in
    // binding expressions use the local slot (not the cell).  The cell
    // is only used in sub-block contexts where the VarId is NOT local.
    uint32_t nFwd = static_cast<uint32_t>(forwardRefs.size());

    if (nFwd > 0) {
        // Allocate cell at runtime (per block entry).
        ctx.blockCellSlot = ctx.nextSlot++;
        unit.emit(OP_ALLOC_CELL, nFwd);
        unit.emit(OP_SET_STACK_SLOT, ctx.blockCellSlot);

        // Map forward-ref VarIds to cell indices + allocate local slots.
        // Use ALLOC_VALUE (not OP_NULL) so direct references within the
        // same block get a shared Value* — COPY_TO_SLOT writes INTO it.
        uint32_t cellIdx = 0;
        for (auto fv : forwardRefs) {
            ctx.blockCellMap[fv] = cellIdx++;
            if (ctx.localSlots.find(fv) == ctx.localSlots.end()) {
                uint32_t slot = ctx.allocSlot(fv);
                unit.emit(OP_ALLOC_VALUE);
                unit.emit(OP_SET_STACK_SLOT, slot);
            }
        }
    }

    // Emit each binding.
    for (const auto & binding : block.bindings) {
        // Reset the ANF peephole tracker at the start of every iteration.
        // ALL register-form fast paths below `continue;` past the
        // operand-stack flow without touching lastBoundVar; if we relied
        // on the reset before emitExpr, a prior binding's lastBoundVar
        // could persist through a register-form binding and then trip
        // emitVarRef's assertion (it would expect unit.code.back() to be
        // SET_STACK_SLOT, but the register-form binding overwrote that).
        ctx.lastBoundVar = ir::kInvalidVar;

        // Phase 1 register-form optimization.
        // Detect patterns where we can write the result DIRECTLY to a
        // slot without going through the operand stack.
        if (!(nFwd > 0 && forwardRefs.count(binding.result))) {
            // -- M3b: literal int -> slot via OP_RCONST.
            // Pre-allocate the Value once at compile time and store the
            // pointer in the constants pool, then emit OP_RCONST.  Saves
            // one allocValue per OP_RLIT_INT *execution* (was ~26K on
            // nixpkgs#hello.name) at the cost of one allocValue per
            // distinct literal *site* at compile time (constant — does
            // not scale with eval depth).  Disk cache amortises the
            // compile-time alloc on warm runs.
            //
            // We could instead encode a tagged immediate directly into
            // the slot, but that violates the "slots hold real Value*"
            // contract that several readers (OP_COPY_TO_SLOT at
            // vm.cc:3443, primop arg materialisation) still depend on.
            // Pre-allocation keeps that invariant intact.

            // -- B4-impl: literal int/float/string/path -> slot via
            // OP_RCONST.  Encoding: [dst:8|constIdx:16].
            // Fall back to the operand-stack path if dst/constIdx
            // exceed encoding limits.
            {
                Value * constVal = nullptr;
                if (auto * litI = std::get_if<ir::IRLitInt>(&binding.expr)) {
                    constVal = state.allocValue();
                    constVal->mkInt(static_cast<NixInt::Inner>(litI->value));
                } else if (auto * litF = std::get_if<ir::IRLitFloat>(&binding.expr)) {
                    constVal = state.allocValue();
                    constVal->mkFloat(litF->value);
                } else if (auto * litS = std::get_if<ir::IRLitString>(&binding.expr)) {
                    constVal = state.allocValue();
                    constVal->mkString(litS->value, state.mem);
                } else if (auto * litP = std::get_if<ir::IRLitPath>(&binding.expr)) {
                    auto * accessor = static_cast<SourceAccessor *>(litP->accessor);
                    SourcePath sp(
                        ref<SourceAccessor>(accessor->shared_from_this()),
                        CanonPath(CanonPath::unchecked_t(), std::string(litP->path)));
                    constVal = state.allocValue();
                    constVal->mkPath(sp, state.mem);
                }
                if (constVal) {
                    uint32_t constIdx = unit.addConstant(constVal);
                    if (constIdx <= 0xFFFF) {
                        uint32_t dstSlot = ctx.allocSlot(binding.result);
                        if (dstSlot <= 0xFF) {
                            unit.emit(OP_RCONST,
                                (dstSlot << 16) | constIdx);
                            continue;
                        }
                    }
                    // Constant already added; fall through to operand-
                    // stack path which will OP_CONST + OP_SET_STACK_SLOT
                    // using the SAME constant pool entry.  But the
                    // operand-stack path will re-allocate a Value on
                    // its own emitExpr call, leaking constVal.  To
                    // avoid: only commit constVal if we'll use OP_RCONST.
                    // Simpler: don't pre-allocate; recompute below.
                    // Implementation: drop the pre-allocated Value if
                    // we're falling through.  Boehm GC will reclaim it.
                }
            }

            // -- Pattern: alias `let a = b;` (no runtime work) --
            // Most lowerLet bindings end with `bound = IRVarRef(value)`
            // (the "linking binding").  We can fold these at emit time
            // by aliasing in the slot map — no opcode required.  The
            // consumer of `bound` is then satisfied by reading the same
            // slot as `value`.  Preserves cell-update semantics: the
            // forward-ref branch above is taken first for any binding
            // whose result is in `forwardRefs`.
            if (auto * varRef = std::get_if<ir::IRVarRef>(&binding.expr)) {
                auto srcIt = ctx.localSlots.find(varRef->var);
                if (srcIt != ctx.localSlots.end()) {
                    ctx.localSlots[binding.result] = srcIt->second;
                    continue;
                }
                // -- Pattern: read upvalue into slot --
                auto uvIt = ctx.upvalueSlots.find(varRef->var);
                if (uvIt != ctx.upvalueSlots.end()
                    && uvIt->second <= 0xFFFF) {
                    uint32_t dstSlot = ctx.allocSlot(binding.result);
                    if (dstSlot <= 0xFF) {
                        unit.emit(OP_RGET_UV_TO,
                            (dstSlot << 16) | uvIt->second);
                        continue;
                    }
                }
            }

            // -- Pattern: force a local or upvalue, store result --
            if (auto * forceExpr = std::get_if<ir::IRForce>(&binding.expr)) {
                auto srcIt = ctx.localSlots.find(forceExpr->thunk);
                if (srcIt != ctx.localSlots.end()
                    && srcIt->second <= 0xFFFF) {
                    uint32_t dstSlot = ctx.allocSlot(binding.result);
                    if (dstSlot <= 0xFF) {
                        unit.emitPos(binding.pos);
                        unit.emit(OP_RFORCE_FROM,
                            (dstSlot << 16) | srcIt->second);
                        continue;
                    }
                }
                auto uvIt = ctx.upvalueSlots.find(forceExpr->thunk);
                if (uvIt != ctx.upvalueSlots.end()
                    && uvIt->second <= 0xFFFF) {
                    uint32_t dstSlot = ctx.allocSlot(binding.result);
                    if (dstSlot <= 0xFF) {
                        unit.emitPos(binding.pos);
                        unit.emit(OP_RUVF_TO,
                            (dstSlot << 16) | uvIt->second);
                        continue;
                    }
                }
            }

            // -- Pattern: register-form arithmetic/comparison --
            // For binary ops where both operands are local slots <256,
            // emit register-form op directly to dst slot.
            #define TRY_REGISTER_BINOP(IR_TYPE, OPCODE) \
                if (auto * binOp = std::get_if<ir::IR_TYPE>(&binding.expr)) { \
                    auto lhsIt = ctx.localSlots.find(binOp->lhs); \
                    auto rhsIt = ctx.localSlots.find(binOp->rhs); \
                    if (lhsIt != ctx.localSlots.end() \
                        && rhsIt != ctx.localSlots.end() \
                        && lhsIt->second <= 0xFF \
                        && rhsIt->second <= 0xFF) { \
                        uint32_t dstSlot = ctx.allocSlot(binding.result); \
                        if (dstSlot <= 0xFF) { \
                            unit.emitPos(binding.pos); \
                            unit.emit(OPCODE, bytecode::packABC( \
                                static_cast<uint8_t>(dstSlot), \
                                static_cast<uint8_t>(lhsIt->second), \
                                static_cast<uint8_t>(rhsIt->second))); \
                            continue; \
                        } \
                    } \
                }

            TRY_REGISTER_BINOP(IRAdd,  OP_RADD_R)
            TRY_REGISTER_BINOP(IRSub,  OP_RSUB_R)
            TRY_REGISTER_BINOP(IRMul,  OP_RMUL_R)
            TRY_REGISTER_BINOP(IRLess, OP_RLESS_R)
            TRY_REGISTER_BINOP(IREq,   OP_REQ_R)
            // B4-impl: more register-form binops.
            TRY_REGISTER_BINOP(IRUpdate,      OP_RUPDATE_R)
            TRY_REGISTER_BINOP(IRConcatLists, OP_RCONCATLIST_R)

            #undef TRY_REGISTER_BINOP

            // B4-impl: register-form unary ops (RNOT_R, RNEG_R).
            // Encoding: [dst:8|srcSlot:16].  Source must be a local
            // slot (upvalue/cell sources fall through to operand-stack).
            #define TRY_REGISTER_UNOP(IR_TYPE, OPCODE) \
                if (auto * un = std::get_if<ir::IR_TYPE>(&binding.expr)) { \
                    auto srcIt = ctx.localSlots.find(un->operand); \
                    if (srcIt != ctx.localSlots.end() \
                        && srcIt->second <= 0xFFFF) { \
                        uint32_t dstSlot = ctx.allocSlot(binding.result); \
                        if (dstSlot <= 0xFF) { \
                            unit.emitPos(binding.pos); \
                            unit.emit(OPCODE, \
                                (dstSlot << 16) | srcIt->second); \
                            continue; \
                        } \
                    } \
                }
            TRY_REGISTER_UNOP(IRNot,    OP_RNOT_R)
            TRY_REGISTER_UNOP(IRNegate, OP_RNEG_R)
            #undef TRY_REGISTER_UNOP

            // -- Pattern: register-form attr select (cached) --
            // For IRAttrSelect where attrs is a local slot, emit
            // OP_RATTR_SELF_R to write directly to dst slot.
            if (auto * sel = std::get_if<ir::IRAttrSelect>(&binding.expr)) {
                auto attrsIt = ctx.localSlots.find(sel->attrs);
                if (attrsIt != ctx.localSlots.end()
                    && attrsIt->second <= 0xFF) {
                    uint32_t cacheIdx = unit.addAttrCache(sel->name);
                    if (cacheIdx <= 0xFF) {
                        uint32_t dstSlot = ctx.allocSlot(binding.result);
                        if (dstSlot <= 0xFF) {
                            unit.emitPos(binding.pos);
                            unit.emit(OP_RATTR_SELF_R, bytecode::packABC(
                                static_cast<uint8_t>(dstSlot),
                                static_cast<uint8_t>(attrsIt->second),
                                static_cast<uint8_t>(cacheIdx)));
                            continue;
                        }
                    }
                }
            }

            // -- Pattern: register-form function call (RCALL1_R) --
            // For IRApp where both func and arg are local slots, emit
            // OP_RCALL1_R + OP_SET_STACK_SLOT.  The handler's v2 fast path
            // writes the result directly to dst slot and advances IP past
            // the SET_STACK_SLOT (no operand-stack round trip).  The slow
            // path delegates to OP_CALL_1 which pushes the result, and
            // the SET_STACK_SLOT pops and stores normally.  This way the
            // optimization is applied opportunistically without losing
            // correctness for non-v2-closure callees (functors, primops,
            // env-chain lambdas).
            if (auto * app = std::get_if<ir::IRApp>(&binding.expr)) {
                auto funcIt = ctx.localSlots.find(app->func);
                auto argIt = ctx.localSlots.find(app->arg);
                if (funcIt != ctx.localSlots.end()
                    && argIt != ctx.localSlots.end()
                    && funcIt->second <= 0xFF
                    && argIt->second <= 0xFF) {
                    uint32_t dstSlot = ctx.allocSlot(binding.result);
                    if (dstSlot <= 0xFF) {
                        unit.emitPos(binding.pos);
                        unit.emit(OP_RCALL1_R, bytecode::packABC(
                            static_cast<uint8_t>(dstSlot),
                            static_cast<uint8_t>(funcIt->second),
                            static_cast<uint8_t>(argIt->second)));
                        // Slow path falls through to this; fast path skips it.
                        unit.emit(OP_SET_STACK_SLOT, dstSlot);
                        continue;
                    }
                }
            }
        }

        emitExpr(binding.expr, binding.pos, ctx);

        if (nFwd > 0 && forwardRefs.count(binding.result)) {
            // Forward-ref binding: COPY_TO_SLOT preserves pointer
            // sharing for in-block refs.  CELL_SET updates the cell
            // entry for sub-block late-binding via CELL_GET.
            uint32_t slot = ctx.localSlots[binding.result];
            unit.emit(OP_COPY_TO_SLOT, slot);
            unit.emit(OP_GET_STACK_SLOT, slot);
            unit.emit(OP_CELL_SET,
                (ctx.blockCellSlot << 16) | ctx.blockCellMap[binding.result]);
        } else {
            uint32_t slot = ctx.allocSlot(binding.result);

            // B4-impl batch 3 peephole: fuse MAKE_THUNK_V2 +
            // OP_SET_STACK_SLOT into OP_RMAKE_THUNK_V2 (and likewise
            // for MAKE_CLOSURE_V2, ATTRS_INIT, LIST_INIT).  The
            // pattern is the LAST emitted opcode for those expression
            // types, so a one-step lookback on unit.code is enough.
            // Encoding requires slot ≤ 0xFF and the inner index ≤ 0xFFFF.
            //
            // Per the B5 profiler: MAKE_THUNK_V2 + SET_STACK_SLOT is
            // ~6.0% of all dispatches, MAKE_CLOSURE_V2 + SET_STACK_SLOT
            // is ~1.5%.  Both are addressed here.
            bool peepholed = false;
            if (slot <= 0xFF && !unit.code.empty()) {
                // OP_MAKE_THUNK_V2 / OP_MAKE_CLOSURE_V2 emit two
                // words: the opcode and an OP_NOP for nUpvalues.
                // Lookback target = unit.code.size() - 2.
                if (unit.code.size() >= 2) {
                    uint32_t opcodeIdx = unit.code.size() - 2;
                    uint8_t op = decodeOp(unit.code[opcodeIdx]);
                    uint32_t innerIdx = decodeOperand(unit.code[opcodeIdx]);
                    if (innerIdx <= 0xFFFF) {
                        if (op == OP_MAKE_THUNK_V2) {
                            unit.code[opcodeIdx] = encode(
                                OP_RMAKE_THUNK_V2,
                                (slot << 16) | innerIdx);
                            peepholed = true;
                        } else if (op == OP_MAKE_CLOSURE_V2) {
                            unit.code[opcodeIdx] = encode(
                                OP_RMAKE_CLOSURE_V2,
                                (slot << 16) | innerIdx);
                            peepholed = true;
                        }
                    }
                }
                // OP_LIST_INIT is a single opcode word (no follow-on
                // data words like ATTRS_INIT).  Lookback target =
                // unit.code.size() - 1.
                if (!peepholed) {
                    uint32_t opcodeIdx = unit.code.size() - 1;
                    uint8_t op = decodeOp(unit.code[opcodeIdx]);
                    uint32_t nElems = decodeOperand(unit.code[opcodeIdx]);
                    if (op == OP_LIST_INIT && nElems <= 0xFFFF) {
                        unit.code[opcodeIdx] = encode(
                            OP_RLIST_INIT,
                            (slot << 16) | nElems);
                        peepholed = true;
                    }
                }
                // OP_ATTRS_INIT is followed by 2*nAttrs OP_NOP data
                // words.  Lookback target = unit.code.size() - 1 - 2*nAttrs.
                // Decode nAttrs from the suspected OP_ATTRS_INIT word.
                if (!peepholed && unit.code.size() >= 1) {
                    uint32_t lastIdx = unit.code.size() - 1;
                    // Walk back for OP_NOP data words; if we find an
                    // OP_ATTRS_INIT before any non-NOP, we can fuse.
                    // The data word count must equal 2*nAttrs.
                    size_t back = 0;
                    while (back < lastIdx) {
                        uint8_t op = decodeOp(unit.code[lastIdx - back]);
                        if (op != OP_NOP) break;
                        back++;
                    }
                    if (back > 0 && back <= lastIdx) {
                        uint32_t opcodeIdx = lastIdx - back;
                        uint8_t op = decodeOp(unit.code[opcodeIdx]);
                        if (op == OP_ATTRS_INIT) {
                            uint32_t nAttrs = decodeOperand(unit.code[opcodeIdx]);
                            if (nAttrs <= 0xFFFF && back == 2 * nAttrs) {
                                unit.code[opcodeIdx] = encode(
                                    OP_RATTRS_INIT,
                                    (slot << 16) | nAttrs);
                                peepholed = true;
                            }
                        }
                    }
                }
            }

            if (!peepholed) {
                unit.emit(OP_SET_STACK_SLOT, slot);
                // Mark this as a candidate for ANF de-materialization:
                // if the next emitVarRef is for this VarId AND useCount
                // is 1, both ops can be elided.
                ctx.lastBoundVar = binding.result;
            } else {
                // Reset the ANF tracker — the peephole already
                // delivered the value to the slot, so the next
                // emitVarRef should NOT pop a SET_STACK_SLOT (there
                // isn't one).
                ctx.lastBoundVar = ir::kInvalidVar;
            }
        }
    }

    // Clear block cell state after bindings.
    if (nFwd > 0) {
        ctx.blockCellMap.clear();
        ctx.blockCellSlot = UINT32_MAX;
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

            // Determine cell-capture sources per free variable (mirrors
            // IRMkThunk path; see the comment there).  Handles forward
            // refs from BOTH the parent block's blockCellMap (own cell)
            // and the parent's cellRefs (inherited from grandparent +).
            struct InnerCellSrc {
                bool fromOwnSlot;
                uint32_t parentRef;
                uint32_t innerUvIdx;
            };
            std::vector<InnerCellSrc> innerCells;
            auto findOrAddCell = [&](bool fromSlot, uint32_t ref,
                                     uint32_t & nextUv) -> uint32_t {
                for (auto & c : innerCells) {
                    if (c.fromOwnSlot == fromSlot && c.parentRef == ref)
                        return c.innerUvIdx;
                }
                uint32_t uv = nextUv++;
                innerCells.push_back({fromSlot, ref, uv});
                return uv;
            };

            ir::FreeVars nonCellFreeVars;
            std::vector<std::tuple<ir::VarId, uint32_t, uint32_t>>
                cellFreeVars;
            uint32_t nextInnerUv = 0;
            for (auto fv : e.freeVars.vars) {
                if (ctx.blockCellSlot != UINT32_MAX) {
                    auto cmIt = ctx.blockCellMap.find(fv);
                    if (cmIt != ctx.blockCellMap.end()) {
                        uint32_t uv = findOrAddCell(true, ctx.blockCellSlot,
                            nextInnerUv);
                        cellFreeVars.emplace_back(fv, uv, cmIt->second);
                        continue;
                    }
                }
                auto crIt = ctx.cellRefs.find(fv);
                if (crIt != ctx.cellRefs.end()) {
                    uint32_t uv = findOrAddCell(false,
                        crIt->second.upvalueIdx, nextInnerUv);
                    cellFreeVars.emplace_back(fv, uv,
                        crIt->second.entryIdx);
                    continue;
                }
                nonCellFreeVars.insert(fv);
            }

            bool needsCellCapture = !innerCells.empty();

            if (needsCellCapture) {
                uint32_t nNonCell = static_cast<uint32_t>(nonCellFreeVars.size());
                uint32_t totalUpvalues = nextInnerUv + nNonCell;

                // Build cell-aware sub-block context.
                const auto & bodyBlock = module.blocks[e.bodyBlock];
                BlockContext subCtx;
                for (auto & [varId, innerUv, entryIdx] : cellFreeVars)
                    subCtx.cellRefs[varId] = {innerUv, entryIdx};
                uint32_t uvIdx = nextInnerUv;
                for (auto fv : nonCellFreeVars.vars)
                    subCtx.upvalueSlots[fv] = uvIdx++;

                // Emit the lambda body.
                uint32_t bodyOffset;
                uint16_t envSize;

                if (hasFormals) {
                    // For formals lambdas, emit a prologue inline.
                    // The sub-block context already has cell-aware upvalues;
                    // emitSubBlockWithFormals will use it via the parent ctx.
                    // We build the sub-block manually here.
                    uint32_t jumpOver = unit.emit(OP_JUMP, 0);
                    bodyOffset = static_cast<uint32_t>(unit.code.size());

                    // Slot 0: raw attrset argument.
                    uint32_t argSlot = subCtx.nextSlot++;
                    for (auto p : bodyBlock.params)
                        subCtx.allocSlot(p);
                    // Extend stack frame with null placeholders.
                    for (uint32_t s = 1; s < subCtx.nextSlot; ++s) {
                        unit.emit(OP_NULL);
                        unit.emit(OP_SET_STACK_SLOT, s);
                    }

                    // Formals cell for default thunks (same as normal path).
                    bool hasAnyDefault = false;
                    for (auto & f : e.params.formals)
                        if (f.defaultBody != ir::kInvalidBlock) { hasAnyDefault = true; break; }

                    uint32_t formalParamStart = e.params.arg ? 1 : 0;
                    uint32_t nFormals = static_cast<uint32_t>(e.params.formals.size());
                    uint32_t formalsCellSlot = 0;
                    uint32_t formalsCellSize = nFormals + (e.params.arg ? 1 : 0);
                    if (hasAnyDefault) {
                        formalsCellSlot = subCtx.nextSlot++;
                        unit.emit(OP_ALLOC_CELL, formalsCellSize);
                        unit.emit(OP_SET_STACK_SLOT, formalsCellSlot);
                    }

                    // Force the raw attrset argument.
                    unit.emit(OP_GET_STACK_SLOT, argSlot);
                    unit.emit(OP_FORCE);

                    // @-pattern.
                    if (e.params.arg) {
                        unit.emit(OP_DUP);
                        auto it2 = subCtx.localSlots.find(bodyBlock.params[0]);
                        assert(it2 != subCtx.localSlots.end());
                        unit.emit(OP_SET_STACK_SLOT, it2->second);
                        if (hasAnyDefault) {
                            unit.emit(OP_GET_STACK_SLOT, it2->second);
                            unit.emit(OP_CELL_SET, (formalsCellSlot << 16) | nFormals);
                        }
                    }

                    // Single-pass formals prologue.
                    for (uint32_t i = 0; i < nFormals; ++i) {
                        auto & formal = e.params.formals[i];
                        ir::VarId formalVarId = bodyBlock.params[formalParamStart + i];
                        auto slotIt = subCtx.localSlots.find(formalVarId);
                        assert(slotIt != subCtx.localSlots.end());
                        uint32_t formalSlot = slotIt->second;

                        if (formal.defaultBody != ir::kInvalidBlock) {
                            unit.emit(OP_DUP);
                            uint32_t symIdx = unit.addSymbol(formal.name);
                            unit.emit(OP_HAS_ATTR, symIdx);
                            uint32_t jumpToDefault = unit.emit(OP_JUMP_IF_FALSE, 0);

                            // Attr exists — use cached attr lookup.
                            unit.emit(OP_DUP);
                            unit.emit(OP_ATTR_SELECT_CACHED, unit.addAttrCache(formal.name));
                            unit.emit(OP_SET_STACK_SLOT, formalSlot);
                            unit.emit(OP_GET_STACK_SLOT, formalSlot);
                            unit.emit(OP_CELL_SET, (formalsCellSlot << 16) | i);
                            uint32_t jumpPastDefault = unit.emit(OP_JUMP, 0);

                            // Attr missing: create default thunk.
                            unit.patchJump(jumpToDefault);
                            {
                                boost::unordered_flat_set<ir::VarId> siblingVarIds;
                                for (uint32_t j = 0; j < nFormals; ++j)
                                    siblingVarIds.insert(bodyBlock.params[formalParamStart + j]);
                                if (e.params.arg)
                                    siblingVarIds.insert(bodyBlock.params[0]);

                                ir::FreeVars nonSiblingFreeVars;
                                for (auto fv : formal.defaultFreeVars.vars)
                                    if (!siblingVarIds.count(fv))
                                        nonSiblingFreeVars.insert(fv);

                                uint32_t defTotalUpvalues = 1 + static_cast<uint32_t>(nonSiblingFreeVars.size());

                                const auto & defBlock = module.blocks[formal.defaultBody];
                                BlockContext thunkCtx;
                                for (uint32_t j = 0; j < nFormals; ++j) {
                                    ir::VarId sib = bodyBlock.params[formalParamStart + j];
                                    thunkCtx.cellRefs[sib] = {0, j};
                                }
                                if (e.params.arg)
                                    thunkCtx.cellRefs[bodyBlock.params[0]] = {0, nFormals};
                                uint32_t defUvIdx = 1;
                                for (auto fv : nonSiblingFreeVars.vars)
                                    thunkCtx.upvalueSlots[fv] = defUvIdx++;
                                for (auto p : defBlock.params)
                                    thunkCtx.allocSlot(p);

                                uint32_t defJumpOver = unit.emit(OP_JUMP, 0);
                                uint32_t defBodyOffset = static_cast<uint32_t>(unit.code.size());
                                emitBlock(defBlock, thunkCtx);
                                unit.patchJump(defJumpOver);

                                uint32_t defThunkIdx = static_cast<uint32_t>(unit.thunks.size());
                                unit.thunks.push_back(ThunkDescriptor{
                                    .codeOffset = defBodyOffset,
                                    .pos = formal.pos,
                                    .sourceExpr = nullptr,
                                    .nUpvalues = static_cast<uint16_t>(defTotalUpvalues),
                                });

                                unit.emit(OP_GET_STACK_SLOT, formalsCellSlot);
                                for (auto fv : nonSiblingFreeVars.vars)
                                    emitCapture(fv, formal.pos, subCtx);

                                unit.emitPos(formal.pos);
                                unit.emit(OP_MAKE_THUNK_V2, defThunkIdx);
                                unit.emit(OP_NOP, defTotalUpvalues);
                            }
                            unit.emit(OP_DUP);
                            unit.emit(OP_SET_STACK_SLOT, formalSlot);
                            unit.emit(OP_CELL_SET, (formalsCellSlot << 16) | i);
                            unit.patchJump(jumpPastDefault);
                        } else {
                            unit.emit(OP_DUP);
                            unit.emit(OP_ATTR_SELECT_CACHED, unit.addAttrCache(formal.name));
                            unit.emit(OP_SET_STACK_SLOT, formalSlot);
                            if (hasAnyDefault) {
                                unit.emit(OP_GET_STACK_SLOT, formalSlot);
                                unit.emit(OP_CELL_SET, (formalsCellSlot << 16) | i);
                            }
                        }
                    }

                    unit.emit(OP_POP);
                    emitBlock(bodyBlock, subCtx);
                    unit.patchJump(jumpOver);
                    envSize = 1 + static_cast<uint16_t>(e.params.formals.size());
                } else {
                    // Simple lambda: emit sub-block with cell-aware context.
                    for (auto p : bodyBlock.params)
                        subCtx.allocSlot(p);

                    uint32_t jumpOver = unit.emit(OP_JUMP, 0);
                    bodyOffset = static_cast<uint32_t>(unit.code.size());
                    emitBlock(bodyBlock, subCtx);
                    unit.patchJump(jumpOver);
                    envSize = static_cast<uint16_t>(bodyBlock.params.size());
                }

                // Register lambda descriptor.
                uint32_t lambdaIdx = static_cast<uint32_t>(unit.lambdas.size());
                uint32_t bodyThunkIdx = static_cast<uint32_t>(unit.thunks.size());
                unit.thunks.push_back(ThunkDescriptor{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .sourceExpr = nullptr,
                    .nUpvalues = static_cast<uint16_t>(totalUpvalues),
                });

                Formals * formals = nullptr;
                // Register name/arg in the symbol pool for disk-cache
                // round-trip (Phase 3.2).  Without this the serializer
                // can't recover the descriptor's symbol references on
                // load and emits Symbol{} (id 0) instead.  Bug found
                // by debug agent for issue #159.
                if (e.name) unit.addSymbol(e.name);
                if (e.params.arg) unit.addSymbol(e.params.arg);

                // Capture formals signature for disk-cache survival of
                // `builtins.functionArgs` (issue #159 follow-up).  The
                // bytecode prologue handles formals during execution,
                // but the AST Formals* dies at end-of-process; we record
                // (name, hasDefault, ellipsis) so a disk-loaded CU can
                // still answer functionArgs / intersectAttrs queries.
                bool srcHasFormals = !e.params.formals.empty();
                std::vector<std::pair<uint32_t, bool>> srcFormals;
                if (srcHasFormals) {
                    srcFormals.reserve(e.params.formals.size());
                    for (auto & f : e.params.formals) {
                        uint32_t symIdx = unit.addSymbol(f.name);
                        srcFormals.emplace_back(symIdx,
                            f.defaultBody != ir::kInvalidBlock);
                    }
                }
                unit.lambdas.push_back(LambdaDescriptor{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .name = e.name,
                    .arg = e.params.arg,
                    .formals = formals,
                    .sourceHasFormals = srcHasFormals,
                    .sourceFormalsEllipsis = e.params.ellipsis,
                    .sourceFormals = std::move(srcFormals),
                    .envSize = envSize,
                    .nUpvalues = static_cast<uint16_t>(totalUpvalues),
                    .sourceExpr = e.sourceExpr,
                    .bodyThunkIdx = bodyThunkIdx,
                    .prologueOffset = bodyOffset,
                });

                // Push captures: cells first (in innerUvIdx order),
                // then non-cell free vars.
                for (auto & c : innerCells) {
                    if (c.fromOwnSlot)
                        unit.emit(OP_GET_STACK_SLOT, c.parentRef);
                    else
                        unit.emit(OP_GET_UPVALUE, c.parentRef);
                }
                for (auto fv : nonCellFreeVars.vars)
                    emitCapture(fv, pos, ctx);

                unit.emitPos(pos);
                unit.emit(OP_MAKE_CLOSURE_V2, lambdaIdx);
                unit.emit(OP_NOP, totalUpvalues);
            } else {
                // Normal path: no cell capture needed.
                uint32_t bodyOffset;
                uint16_t envSize;
                uint16_t bodyMaxSlot = 0;

                // Phase 3.1f-8: defer non-formals lambda bodies when
                // NIX_VM_V2_LAZY_EMIT=1 and the CU's IRModule is pinned.
                // Formals lambdas keep the eager path until 3.1f-9
                // because their inline prologue is order-sensitive.
                static const bool lazyEmitLambda =
                    ::getenv("NIX_VM_V2_LAZY_EMIT") != nullptr
                    && std::string(::getenv("NIX_VM_V2_LAZY_EMIT")) == "1";
                bool deferThis = lazyEmitLambda
                    && unit.irModule
                    && !hasFormals;

                // Phase 3.1f-9: also defer formals-lambda bodies.
                bool deferFormals = lazyEmitLambda
                    && unit.irModule
                    && hasFormals;

                if (hasFormals && !deferFormals) {
                    bodyOffset = emitSubBlockWithFormals(
                        e.bodyBlock, e.freeVars, e.params, ctx);
                    envSize = 1 + static_cast<uint16_t>(e.params.formals.size());
                    bodyMaxSlot = lastSubBlockMaxSlot;
                } else if (hasFormals /* && deferFormals */) {
                    // Pending formals lambda.  envSize covers the raw
                    // attrset slot + each formal, mirroring the eager
                    // path's allocation.
                    bodyOffset = 0;
                    envSize = 1 + static_cast<uint16_t>(e.params.formals.size());
                } else if (!deferThis) {
                    bodyOffset = emitSubBlock(e.bodyBlock, e.freeVars, ctx);
                    auto & bodyBlock = module.blocks[e.bodyBlock];
                    envSize = static_cast<uint16_t>(bodyBlock.params.size());
                    bodyMaxSlot = lastSubBlockMaxSlot;
                } else {
                    // Pending: codeOffset placeholder, envSize derivable
                    // from the IR block's param count.
                    bodyOffset = 0;
                    auto & bodyBlock = module.blocks[e.bodyBlock];
                    envSize = static_cast<uint16_t>(bodyBlock.params.size());
                }

                uint32_t lambdaIdx = static_cast<uint32_t>(unit.lambdas.size());
                uint32_t bodyThunkIdx = static_cast<uint32_t>(unit.thunks.size());
                ThunkDescriptor td{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .sourceExpr = nullptr,
                    .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
                    .maxSlot = bodyMaxSlot,
                };
                if (deferThis || deferFormals) {
                    td.state = ThunkDescriptor::State::Pending;
                    td.bodyBlockId = e.bodyBlock;
                    td.deferredState = std::make_shared<DeferredEmitState>();
                    td.deferredState->bodyBlockId = e.bodyBlock;
                    td.deferredState->freeVarsSorted.assign(
                        e.freeVars.vars.begin(), e.freeVars.vars.end());
                    if (deferFormals) {
                        // Phase 3.1f-9: snapshot formals pointer.
                        // The IRModule is pinned, so &e.params is
                        // stable for the CU's lifetime.
                        td.deferredState->formals = &e.params;
                    }
                }
                unit.thunks.push_back(std::move(td));

                Formals * formals = nullptr;
                // Register name/arg in the symbol pool — same fix as
                // the cell-capture path above.  Required for disk-cache
                // round-trip.
                if (e.name) unit.addSymbol(e.name);
                if (e.params.arg) unit.addSymbol(e.params.arg);

                // Capture formals signature (see cell-capture branch
                // for rationale).
                bool srcHasFormals = !e.params.formals.empty();
                std::vector<std::pair<uint32_t, bool>> srcFormals;
                if (srcHasFormals) {
                    srcFormals.reserve(e.params.formals.size());
                    for (auto & f : e.params.formals) {
                        uint32_t symIdx = unit.addSymbol(f.name);
                        srcFormals.emplace_back(symIdx,
                            f.defaultBody != ir::kInvalidBlock);
                    }
                }
                unit.lambdas.push_back(LambdaDescriptor{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .name = e.name,
                    .arg = e.params.arg,
                    .formals = formals,
                    .sourceHasFormals = srcHasFormals,
                    .sourceFormalsEllipsis = e.params.ellipsis,
                    .sourceFormals = std::move(srcFormals),
                    .envSize = envSize,
                    .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
                    .sourceExpr = e.sourceExpr,
                    .bodyThunkIdx = bodyThunkIdx,
                    .prologueOffset = bodyOffset,
                });

                for (auto freeVar : e.freeVars.vars)
                    emitCapture(freeVar, pos, ctx);

                unit.emitPos(pos);
                unit.emit(OP_MAKE_CLOSURE_V2, lambdaIdx);
                unit.emit(OP_NOP, static_cast<uint32_t>(e.freeVars.size()));
            }
        }

        // -- Function application --
        else if constexpr (std::is_same_v<T, ir::IRApp>) {
            // Superinstruction: SLOT_SLOT_CALL1 when both func and arg
            // are locals with slot indices < 4096.
            auto funcIt = ctx.localSlots.find(e.func);
            auto argIt = ctx.localSlots.find(e.arg);
            if (funcIt != ctx.localSlots.end()
                && argIt != ctx.localSlots.end()
                && funcIt->second < 4096
                && argIt->second < 4096) {
                unit.emitPos(pos);
                unit.emit(OP_SLOT_SLOT_CALL1,
                    (funcIt->second << 12) | argIt->second);
            } else {
                emitVarRef(e.func, pos, ctx);
                emitVarRef(e.arg, pos, ctx);
                unit.emitPos(pos);
                unit.emit(OP_CALL_1);
            }
        }

        // -- Force --
        else if constexpr (std::is_same_v<T, ir::IRForce>) {
            // Superinstruction: GET_SLOT_FORCE or GET_UV_FORCE when the
            // variable is a local slot or upvalue.
            auto localIt = ctx.localSlots.find(e.thunk);
            if (localIt != ctx.localSlots.end()) {
                unit.emitPos(pos);
                unit.emit(OP_GET_SLOT_FORCE, localIt->second);
            } else {
                auto uvIt = ctx.upvalueSlots.find(e.thunk);
                if (uvIt != ctx.upvalueSlots.end()) {
                    unit.emitPos(pos);
                    unit.emit(OP_GET_UV_FORCE, uvIt->second);
                } else {
                    emitVarRef(e.thunk, pos, ctx);
                    unit.emitPos(pos);
                    unit.emit(OP_FORCE);
                }
            }
        }

        // -- Thunk creation --
        else if constexpr (std::is_same_v<T, ir::IRMkThunk>) {
            // Determine cell-capture sources per free variable:
            //   - parent's blockCellMap → capture parent's own cell
            //     (accessible at ctx.blockCellSlot).
            //   - parent's cellRefs → capture an inherited cell
            //     (accessible at ctx.cellRefs[fv].upvalueIdx).
            // The inner sub-block needs all relevant cells captured into
            // its own upvalue array, plus cellRefs entries that point
            // through those new upvalue slots.  This handles forward
            // refs that must remain late-binding across multiple
            // thunk/lambda nesting levels.
            //
            // For each unique cell source (own slot OR parent upvalue),
            // we capture once and assign one upvalue index to the inner
            // sub-block.  Multiple free vars sharing the same source
            // cell share the same upvalue index.
            struct InnerCellSrc {
                bool fromOwnSlot;     // true: parent owns cell at slot
                uint32_t parentRef;   // slot or upvalue idx
                uint32_t innerUvIdx;  // assigned upvalue idx in inner thunk
            };
            // Key: (fromOwnSlot, parentRef) — same cell shared across vars.
            std::vector<InnerCellSrc> innerCells;
            auto findOrAddCell = [&](bool fromSlot, uint32_t ref,
                                     uint32_t & nextUv) -> uint32_t {
                for (auto & c : innerCells) {
                    if (c.fromOwnSlot == fromSlot && c.parentRef == ref)
                        return c.innerUvIdx;
                }
                uint32_t uv = nextUv++;
                innerCells.push_back({fromSlot, ref, uv});
                return uv;
            };

            // Classify free vars; collect (varId, innerUvIdx, entryIdx)
            // for each cell-resident; rest go to nonCellFreeVars.
            ir::FreeVars nonCellFreeVars;
            std::vector<std::tuple<ir::VarId, uint32_t, uint32_t>>
                cellFreeVars;
            uint32_t nextInnerUv = 0;
            for (auto fv : e.freeVars.vars) {
                if (ctx.blockCellSlot != UINT32_MAX) {
                    auto cmIt = ctx.blockCellMap.find(fv);
                    if (cmIt != ctx.blockCellMap.end()) {
                        uint32_t uv = findOrAddCell(true, ctx.blockCellSlot,
                            nextInnerUv);
                        cellFreeVars.emplace_back(fv, uv, cmIt->second);
                        continue;
                    }
                }
                auto crIt = ctx.cellRefs.find(fv);
                if (crIt != ctx.cellRefs.end()) {
                    uint32_t uv = findOrAddCell(false,
                        crIt->second.upvalueIdx, nextInnerUv);
                    cellFreeVars.emplace_back(fv, uv,
                        crIt->second.entryIdx);
                    continue;
                }
                nonCellFreeVars.insert(fv);
            }

            bool needsCellCapture = !innerCells.empty();

            if (needsCellCapture) {
                uint32_t nNonCell = static_cast<uint32_t>(nonCellFreeVars.size());
                uint32_t totalUpvalues = nextInnerUv + nNonCell;

                // Phase 3.1f-7: gate the cell-capture path on the same
                // lazy-emit flag as the simple path.  When deferred, we
                // skip the body emit AND the surrounding jump-over, and
                // record a DeferredEmitState rich enough to rebuild the
                // cell-aware BlockContext at first force.
                static const bool lazyEmitCells =
                    ::getenv("NIX_VM_V2_LAZY_EMIT") != nullptr
                    && std::string(::getenv("NIX_VM_V2_LAZY_EMIT")) == "1";
                bool deferThis = lazyEmitCells && unit.irModule;

                uint32_t bodyOffset = 0;
                uint16_t maxSlot = 0;

                if (!deferThis) {
                    // Build cell-aware sub-block context.
                    const auto & bodyBlock = module.blocks[e.bodyBlock];
                    BlockContext subCtx;
                    for (auto & [varId, innerUv, entryIdx] : cellFreeVars)
                        subCtx.cellRefs[varId] = {innerUv, entryIdx};
                    uint32_t uvIdx = nextInnerUv;
                    for (auto fv : nonCellFreeVars.vars)
                        subCtx.upvalueSlots[fv] = uvIdx++;
                    for (auto p : bodyBlock.params)
                        subCtx.allocSlot(p);

                    // Emit the sub-block body.
                    uint32_t jumpOver = unit.emit(OP_JUMP, 0);
                    bodyOffset = static_cast<uint32_t>(unit.code.size());
                    emitBlock(bodyBlock, subCtx);
                    unit.patchJump(jumpOver);
                    maxSlot = static_cast<uint16_t>(
                        subCtx.nextSlot > 0xFFFF ? 0xFFFF : subCtx.nextSlot);
                }

                // Register thunk descriptor.
                uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
                ThunkDescriptor td{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .sourceExpr = e.sourceExpr,
                    .nUpvalues = static_cast<uint16_t>(totalUpvalues),
                    .maxSlot = maxSlot,
                };
                if (deferThis) {
                    td.state = ThunkDescriptor::State::Pending;
                    td.bodyBlockId = e.bodyBlock;
                    td.deferredState = std::make_shared<DeferredEmitState>();
                    td.deferredState->bodyBlockId = e.bodyBlock;
                    // Cell-capture snapshot.
                    td.deferredState->cellFreeVars.reserve(cellFreeVars.size());
                    for (auto & [varId, innerUv, entryIdx] : cellFreeVars) {
                        td.deferredState->cellFreeVars.push_back(
                            {static_cast<uint32_t>(varId),
                             innerUv, entryIdx});
                    }
                    td.deferredState->nextInnerUv = nextInnerUv;
                    td.deferredState->freeVarsSorted.assign(
                        nonCellFreeVars.vars.begin(),
                        nonCellFreeVars.vars.end());
                }
                unit.thunks.push_back(std::move(td));

                // Push captures: cells first (in innerUvIdx order),
                // then non-cell free vars.  These run in the parent
                // body and must be emitted eagerly regardless of
                // whether the inner body is deferred.
                for (auto & c : innerCells) {
                    if (c.fromOwnSlot)
                        unit.emit(OP_GET_STACK_SLOT, c.parentRef);
                    else
                        unit.emit(OP_GET_UPVALUE, c.parentRef);
                }
                for (auto fv : nonCellFreeVars.vars)
                    emitCapture(fv, pos, ctx);

                unit.emitPos(pos);
                unit.emit(OP_MAKE_THUNK_V2, thunkIdx);
                unit.emit(OP_NOP, totalUpvalues);
            } else {
                // Normal path: no cell capture needed.
                //
                // Phase 3.1f-6: when NIX_VM_V2_LAZY_EMIT=1 and the
                // CU's IRModule is pinned, register a Pending
                // descriptor and skip body emission.  The body will
                // be emitted at first force via lazyEmitThunkBody.
                static const bool lazyEmit =
                    ::getenv("NIX_VM_V2_LAZY_EMIT") != nullptr
                    && std::string(::getenv("NIX_VM_V2_LAZY_EMIT")) == "1";
                bool deferThis = lazyEmit && unit.irModule;

                uint32_t bodyOffset = 0;
                uint16_t maxSlot = 0;
                if (!deferThis) {
                    bodyOffset = emitSubBlock(e.bodyBlock, e.freeVars, ctx);
                    maxSlot = lastSubBlockMaxSlot;
                }

                uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
                ThunkDescriptor td{
                    .codeOffset = bodyOffset,
                    .pos = e.pos,
                    .sourceExpr = e.sourceExpr,
                    .nUpvalues = static_cast<uint16_t>(e.freeVars.size()),
                    .maxSlot = maxSlot,
                };
                if (deferThis) {
                    td.state = ThunkDescriptor::State::Pending;
                    td.bodyBlockId = e.bodyBlock;
                    td.deferredState = std::make_shared<DeferredEmitState>();
                    td.deferredState->bodyBlockId = e.bodyBlock;
                    td.deferredState->freeVarsSorted.assign(
                        e.freeVars.vars.begin(), e.freeVars.vars.end());
                }
                unit.thunks.push_back(std::move(td));

                for (auto freeVar : e.freeVars.vars)
                    emitCapture(freeVar, pos, ctx);

                unit.emitPos(pos);
                unit.emit(OP_MAKE_THUNK_V2, thunkIdx);
                unit.emit(OP_NOP, static_cast<uint32_t>(e.freeVars.size()));
            }
        }

        // -- Attribute select --
        else if constexpr (std::is_same_v<T, ir::IRAttrSelect>) {
            emitVarRef(e.attrs, pos, ctx);
            unit.emitPos(pos);
            // Skip the leading OP_FORCE: OP_ATTR_SELECT_CACHED's
            // handler does forceAttrs internally.  Use the fused
            // OP_ATTR_SELECT_FORCE_CACHED for the trailing force —
            // saves one dispatch per attr select (very hot in nixpkgs).
            uint32_t cacheIdx = unit.addAttrCache(e.name);
            unit.emit(OP_ATTR_SELECT_FORCE_CACHED, cacheIdx);
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

        // -- PrimOp call (VM-native) --
        else if constexpr (std::is_same_v<T, ir::IRPrimOpCall>) {
            // Push all arguments left-to-right.
            for (auto arg : e.args) {
                emitVarRef(arg, pos, ctx);
            }
            // Emit OP_CALL_PRIMOP: direct call to primop impl.
            // No intermediate PrimOpApp values or callFunction overhead.
            auto * primVal = state.allocValue();
            primVal->mkPrimOp(const_cast<PrimOp *>(e.primOp));
            uint16_t constIdx = static_cast<uint16_t>(unit.addConstant(primVal));
            uint8_t arity = static_cast<uint8_t>(e.primOp->arity);
            unit.emitPos(e.pos);
            unit.emit(OP_CALL_PRIMOP, packArityConst(arity, constIdx));
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
            // Create a patched ExprVar with the v2-adjusted level.
            // The original ExprVar's level counts ALL scopes (lambda,
            // let, with) but the v2 env chain only has carrier envs
            // (skipped) and with/enter_let envs.  Using the original
            // level would overshoot.
            assert(e.sourceVar && "IRWithLookup must have a sourceVar");
            auto * patchedVar = state.mem.exprs.add<ExprVar>(
                e.sourceVar->pos, e.sourceVar->name);
            patchedVar->level = e.v2Level;
            patchedVar->displ = e.sourceVar->displ;
            patchedVar->fromWith = e.sourceVar->fromWith;
            uint32_t exprIdx = unit.addExpr(patchedVar);
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

// Up to 4 hash lookups per var reference is theoretically expensive, but
// the dominant case (localSlots) hits on the first lookup so the
// fall-through is rarely exercised.  A consolidated single-map design
// (VarBinding variant keyed by VarId) was considered; the projected
// win is <1% of compile time given the localSlots-first hit pattern.
// Keeping the four-map layout for readability; revisit if profiling
// shows emitVarRef in the hot path.
void IREmitter::emitVarRef(ir::VarId var, PosIdx pos, BlockContext & ctx)
{
    // ANF de-materialization peephole.  If the IMMEDIATELY-PRECEDING
    // emit was a binding's SET_STACK_SLOT for THIS exact VarId AND
    // the VarId has a single use in this block, both the SET and the
    // GET are redundant — the value is still on the operand stack
    // from the producer's emitExpr, and no later reader will need
    // the slot.
    //
    // Tracking via ctx.lastBoundVar (not just the last opcode) is
    // critical: the IRVarRef alias optimization in the binding loop
    // can re-map slots, so a SET_STACK_SLOT may belong to a different
    // logical binding than the one we're now reading.  lastBoundVar
    // is set by the binding loop only when SET_STACK_SLOT was emitted
    // FOR THIS BINDING.
    if (var != ir::kInvalidVar
        && var == ctx.lastBoundVar)
    {
        auto ucIt = ctx.blockUseCounts.find(var);
        if (ucIt != ctx.blockUseCounts.end()
            && ucIt->second == 1)
        {
            ctx.lastBoundVar = ir::kInvalidVar;
            // Pop the SET_STACK_SLOT; the value stays on the operand
            // stack for this consumer to use directly.
            assert(!unit.code.empty()
                && decodeOp(unit.code.back()) == OP_SET_STACK_SLOT);
            unit.code.pop_back();
            ucIt->second = 0;
            return;
        }
    }
    // Reset the tracker — anything emitVarRef does (or anything
    // between bindings) invalidates the elision opportunity.
    ctx.lastBoundVar = ir::kInvalidVar;

    // B5 fusion peephole: if the immediately-preceding emit was an
    // OP_GET_STACK_SLOT (or OP_GET_UPVALUE) and the operand fits in
    // 12 bits AND the new emit also fits, replace the pair with the
    // dual-form superinstruction OP_GET_SLOT2 (or OP_GET_UV2).
    // Tries to fuse a NEW emit (slot or upvalue) with the previous
    // emit if the latter was a single-form get.  Covers the four
    // pairings: SLOT+SLOT, UV+UV, UV+SLOT, SLOT+UV.
    auto fusePair = [&](bool newIsSlot, uint32_t newOperand) -> bool {
        if (newOperand > 0xFFF) return false;
        if (unit.code.empty()) return false;
        uint32_t lastIdx = unit.code.size() - 1;
        uint8_t prevOp = decodeOp(unit.code[lastIdx]);
        uint32_t prevOperand = decodeOperand(unit.code[lastIdx]);
        if (prevOperand > 0xFFF) return false;
        uint8_t fusedOp;
        if (prevOp == OP_GET_STACK_SLOT && newIsSlot)
            fusedOp = OP_GET_SLOT2;
        else if (prevOp == OP_GET_UPVALUE && !newIsSlot)
            fusedOp = OP_GET_UV2;
        else if (prevOp == OP_GET_UPVALUE && newIsSlot)
            fusedOp = OP_GET_UV_SLOT;
        else if (prevOp == OP_GET_STACK_SLOT && !newIsSlot)
            fusedOp = OP_GET_SLOT_UV;
        else
            return false;
        unit.code[lastIdx] = encode(fusedOp, (prevOperand << 12) | newOperand);
        return true;
    };

    // Check local slots first (defined in this block).
    auto localIt = ctx.localSlots.find(var);
    if (localIt != ctx.localSlots.end()) {
        if (!fusePair(true, localIt->second))
            unit.emit(OP_GET_STACK_SLOT, localIt->second);
        return;
    }

    // Check cell references (forward-ref vars from parent blocks,
    // or sibling formals in default thunks).
    auto cellIt = ctx.cellRefs.find(var);
    if (cellIt != ctx.cellRefs.end()) {
        uint32_t packed = (cellIt->second.upvalueIdx << 16)
                        | cellIt->second.entryIdx;
        unit.emit(OP_CELL_GET, packed);
        return;
    }

    // Check upvalue slots (captured from enclosing scope).
    auto upIt = ctx.upvalueSlots.find(var);
    if (upIt != ctx.upvalueSlots.end()) {
        if (!fusePair(false, upIt->second))
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
                // Superinstruction: GET_SLOT_RETURN skips push+pop,
                // writes directly to resultSlot.
                auto localIt = ctx.localSlots.find(t.value);
                if (localIt != ctx.localSlots.end()) {
                    unit.emitPos(t.pos);
                    unit.emit(OP_GET_SLOT_RETURN, localIt->second);
                } else {
                    emitVarRef(t.value, t.pos, ctx);
                    unit.emitPos(t.pos);
                    unit.emit(OP_RETURN);
                }
            } else {
                unit.emitPos(t.pos);
                unit.emit(OP_RETURN);
            }
        }
        else if constexpr (std::is_same_v<T, ir::TermTailCall>) {
            // Emit OP_TAIL_CALL_1: handler reuses the current frame
            // when the callee is a v2 bytecode-proxy lambda; otherwise
            // falls back to op_call_1 which uses the trailing
            // OP_RETURN to deliver the result.
            emitVarRef(t.func, t.pos, ctx);
            emitVarRef(t.arg, t.pos, ctx);
            unit.emitPos(t.pos);
            unit.emit(OP_TAIL_CALL_1);
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
    // Eager path: wrap the body emission with a jump-over so the
    // body sits inline in the parent's code stream but isn't
    // executed when control reaches it from above.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);
    uint32_t bodyOffset = emitBlockOnly(blockId, freeVars);
    unit.patchJump(jumpOver);
    return bodyOffset;
}

uint32_t IREmitter::emitBlockOnly(
    ir::BlockId blockId,
    const ir::FreeVars & freeVars)
{
    const auto & block = module.blocks[blockId];

    uint32_t bodyOffset = static_cast<uint32_t>(unit.code.size());

    // Build a fresh BlockContext for the sub-block.
    // Free variables become upvalue slots; params become local slots.
    BlockContext subCtx = buildSubBlockContext(block, freeVars);

    emitBlock(block, subCtx);

    // Record the max slot reached so the caller can populate
    // ThunkDescriptor.maxSlot for frame-entry stack pre-extension.
    lastSubBlockMaxSlot = static_cast<uint16_t>(
        subCtx.nextSlot > 0xFFFF ? 0xFFFF : subCtx.nextSlot);

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
    // Jump over the sub-block body in the parent's code stream.
    uint32_t jumpOver = unit.emit(OP_JUMP, 0);

    // Delegate the actual body emission to the no-jump-wrapper helper.
    uint32_t bodyOffset = emitFormalsBodyOnly(blockId, freeVars, params);

    // Patch the jump-over.
    unit.patchJump(jumpOver);

    return bodyOffset;
}

uint32_t IREmitter::emitFormalsBodyPublic(
    ir::BlockId blockId,
    const ir::FreeVars & freeVars,
    const ir::IRFormals & params)
{
    return emitFormalsBodyOnly(blockId, freeVars, params);
}

uint32_t IREmitter::emitFormalsBodyOnly(
    ir::BlockId blockId,
    const ir::FreeVars & freeVars,
    const ir::IRFormals & params)
{
    const auto & block = module.blocks[blockId];
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

    // Allocate formal slots (no pre-allocation).
    for (auto p : block.params) {
        subCtx.allocSlot(p);
    }
    // Extend stack frame with null placeholders.
    for (uint32_t s = 1; s < subCtx.nextSlot; ++s) {
        unit.emit(OP_NULL);
        unit.emit(OP_SET_STACK_SLOT, s);
    }

    // --- Formals cell ---
    //
    // A shared GC-traced Value*[] array that default thunks read from
    // at FORCE time (via OP_CELL_GET).  This provides late-binding:
    // the cell is fully populated by the prologue before any thunk is
    // forced.  Stack slots use SET_STACK_SLOT (pointer sharing for
    // blackhole detection).  No COPY_TO_SLOT needed.

    bool hasAnyDefault = false;
    for (auto & f : params.formals)
        if (f.defaultBody != ir::kInvalidBlock) { hasAnyDefault = true; break; }

    uint32_t formalParamStart = params.arg ? 1 : 0;
    uint32_t nFormals = static_cast<uint32_t>(params.formals.size());
    uint32_t cellSlot = 0;

    uint32_t cellSize = nFormals + (params.arg ? 1 : 0); // extra entry for @-pattern
    if (hasAnyDefault) {
        // Allocate the cell at RUNTIME (per-call, GC-traced).
        cellSlot = subCtx.nextSlot++;
        unit.emit(OP_ALLOC_CELL, cellSize);
        unit.emit(OP_SET_STACK_SLOT, cellSlot);
    }

    // Read and force the raw attrset argument.
    unit.emit(OP_GET_STACK_SLOT, argSlot);
    unit.emit(OP_FORCE);

    // @-pattern
    if (params.arg) {
        unit.emit(OP_DUP);
        auto it = subCtx.localSlots.find(block.params[0]);
        assert(it != subCtx.localSlots.end());
        unit.emit(OP_SET_STACK_SLOT, it->second);
        // Write @-pattern into cell for late-binding.
        if (hasAnyDefault) {
            unit.emit(OP_GET_STACK_SLOT, it->second);
            unit.emit(OP_CELL_SET, (cellSlot << 16) | nFormals); // @-pattern at cell[nFormals]
        }
    }

    // --- Single-pass formals prologue ---
    //
    // For each formal:
    //   If present: SET_STACK_SLOT + CELL_SET (original pointer)
    //   If default: create thunk, SET_STACK_SLOT + CELL_SET (thunk)
    //
    // Default thunks capture the CELL pointer as upvalue 0.
    // Their bodies use OP_CELL_GET for sibling formal references.
    // This gives late-binding: the cell is read at force time,
    // after the entire prologue has completed.

    for (uint32_t i = 0; i < nFormals; ++i) {
        auto & formal = params.formals[i];
        ir::VarId formalVarId = block.params[formalParamStart + i];
        auto slotIt = subCtx.localSlots.find(formalVarId);
        assert(slotIt != subCtx.localSlots.end());
        uint32_t formalSlot = slotIt->second;

        if (formal.defaultBody != ir::kInvalidBlock) {
            // Formal with default.
            unit.emit(OP_DUP);
            uint32_t symIdx = unit.addSymbol(formal.name);
            unit.emit(OP_HAS_ATTR, symIdx);
            uint32_t jumpToDefault = unit.emit(OP_JUMP_IF_FALSE, 0);

            // Attr exists: store original pointer (cached select).
            unit.emit(OP_DUP);
            unit.emit(OP_ATTR_SELECT_CACHED, unit.addAttrCache(formal.name));
            unit.emit(OP_SET_STACK_SLOT, formalSlot);
            // Write into cell for late-binding.
            unit.emit(OP_GET_STACK_SLOT, formalSlot);
            unit.emit(OP_CELL_SET, (cellSlot << 16) | i);
            uint32_t jumpPastDefault = unit.emit(OP_JUMP, 0);

            // Attr missing: create default thunk.
            unit.patchJump(jumpToDefault);
            {
                // Build sibling formal VarId → cell index mapping.
                boost::unordered_flat_set<ir::VarId> siblingVarIds;
                for (uint32_t j = 0; j < nFormals; ++j)
                    siblingVarIds.insert(block.params[formalParamStart + j]);
                if (params.arg)
                    siblingVarIds.insert(block.params[0]);

                // Split free vars: sibling → cell, others → upvalues.
                ir::FreeVars nonSiblingFreeVars;
                for (auto fv : formal.defaultFreeVars.vars)
                    if (!siblingVarIds.count(fv))
                        nonSiblingFreeVars.insert(fv);

                // Upvalue layout: [0]=cell, [1..N]=non-sibling free vars.
                uint32_t totalUpvalues = 1 + static_cast<uint32_t>(nonSiblingFreeVars.size());

                // Build sub-block context for the default thunk.
                const auto & defBlock = module.blocks[formal.defaultBody];
                BlockContext thunkCtx;
                // Cell-based references for sibling formals + @-pattern.
                for (uint32_t j = 0; j < nFormals; ++j) {
                    ir::VarId sib = block.params[formalParamStart + j];
                    thunkCtx.cellRefs[sib] = {0, j};
                }
                if (params.arg)
                    thunkCtx.cellRefs[block.params[0]] = {0, nFormals};
                // Normal upvalues for non-sibling free vars.
                uint32_t uvIdx = 1;
                for (auto fv : nonSiblingFreeVars.vars)
                    thunkCtx.upvalueSlots[fv] = uvIdx++;
                // Params → local slots.
                for (auto p : defBlock.params)
                    thunkCtx.allocSlot(p);

                // Emit the sub-block body.
                uint32_t jumpOver = unit.emit(OP_JUMP, 0);
                uint32_t bodyOffset = static_cast<uint32_t>(unit.code.size());
                emitBlock(defBlock, thunkCtx);
                unit.patchJump(jumpOver);

                // Register thunk descriptor.
                uint32_t thunkIdx = static_cast<uint32_t>(unit.thunks.size());
                unit.thunks.push_back(ThunkDescriptor{
                    .codeOffset = bodyOffset,
                    .pos = formal.pos,
                    .sourceExpr = nullptr,
                    .nUpvalues = static_cast<uint16_t>(totalUpvalues),
                });

                // Push captures: cell first, then non-sibling free vars.
                unit.emit(OP_GET_STACK_SLOT, cellSlot);
                for (auto fv : nonSiblingFreeVars.vars)
                    emitCapture(fv, formal.pos, subCtx);

                unit.emitPos(formal.pos);
                unit.emit(OP_MAKE_THUNK_V2, thunkIdx);
                unit.emit(OP_NOP, totalUpvalues);
            }
            // Store thunk and write into cell.
            unit.emit(OP_DUP);
            unit.emit(OP_SET_STACK_SLOT, formalSlot);
            unit.emit(OP_CELL_SET, (cellSlot << 16) | i);

            unit.patchJump(jumpPastDefault);
        } else {
            // Required formal: select and store (cached).
            unit.emit(OP_DUP);
            unit.emit(OP_ATTR_SELECT_CACHED, unit.addAttrCache(formal.name));
            unit.emit(OP_SET_STACK_SLOT, formalSlot);
            // Write into cell for late-binding by default thunks.
            if (hasAnyDefault) {
                unit.emit(OP_GET_STACK_SLOT, formalSlot);
                unit.emit(OP_CELL_SET, (cellSlot << 16) | i);
            }
        }
    }

    // Pop the attrset.
    unit.emit(OP_POP);

    // --- Emit the body block ---
    emitBlock(block, subCtx);

    // Mirror emitSubBlock's contract so the IRLambda emitter can
    // populate ThunkDescriptor.maxSlot correctly when it consults
    // lastSubBlockMaxSlot after this call.
    lastSubBlockMaxSlot = static_cast<uint16_t>(
        subCtx.nextSlot > 0xFFFF ? 0xFFFF : subCtx.nextSlot);

    // Caller (emitSubBlockWithFormals) handles the jump-over patch.
    // For the lazy-emit path this is a tail emission with no wrapper.
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
    boost::unordered_flat_set<ir::VarId> definedInBlock;
    for (const auto & binding : block.bindings)
        definedInBlock.insert(binding.result);

    // Same forward-reference detection as emitBlock().  M5: prefer the
    // cached binding.directRefs from blockFreeVars; fall back to a
    // collectRefs walk if the cache is empty.
    boost::unordered_flat_set<ir::VarId> forwardRefs;
    {
        boost::unordered_flat_set<ir::VarId> seenDefined;
        for (const auto & binding : block.bindings) {
            ir::FreeVars fallback;
            const ir::FreeVars * directRefsPtr = &binding.directRefs;
            if (binding.directRefs.vars.empty()) {
                ir::collectRefs(binding.expr, fallback);
                directRefsPtr = &fallback;
            }
            // We need exprRefs = directRefs ∪ sub-block freeVars.
            // Build it cheaply by copying directRefs and inserting
            // sub-block vars in place.
            ir::FreeVars exprRefs = *directRefsPtr;
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

    // Cell-based forward references (same approach as emitBlock).
    // Since inline blocks share the parent's ctx, save/restore the
    // parent's block cell state to avoid clobbering it.
    uint32_t nFwd = static_cast<uint32_t>(forwardRefs.size());
    uint32_t savedBlockCellSlot = ctx.blockCellSlot;
    auto savedBlockCellMap = std::move(ctx.blockCellMap);
    ctx.blockCellMap.clear();

    if (nFwd > 0) {
        ctx.blockCellSlot = ctx.nextSlot++;
        unit.emit(OP_ALLOC_CELL, nFwd);
        unit.emit(OP_SET_STACK_SLOT, ctx.blockCellSlot);

        uint32_t cellIdx = 0;
        for (auto fv : forwardRefs) {
            ctx.blockCellMap[fv] = cellIdx++;
            if (ctx.localSlots.find(fv) == ctx.localSlots.end()) {
                uint32_t slot = ctx.allocSlot(fv);
                unit.emit(OP_ALLOC_VALUE);
                unit.emit(OP_SET_STACK_SLOT, slot);
            }
        }
    }

    for (const auto & binding : block.bindings) {
        emitExpr(binding.expr, binding.pos, ctx);
        if (nFwd > 0 && forwardRefs.count(binding.result)) {
            uint32_t slot = ctx.localSlots[binding.result];
            unit.emit(OP_COPY_TO_SLOT, slot);
            unit.emit(OP_GET_STACK_SLOT, slot);
            unit.emit(OP_CELL_SET,
                (ctx.blockCellSlot << 16) | ctx.blockCellMap[binding.result]);
        } else {
            uint32_t slot = ctx.allocSlot(binding.result);
            unit.emit(OP_SET_STACK_SLOT, slot);
        }
    }

    // Restore parent's block cell state.
    ctx.blockCellMap = std::move(savedBlockCellMap);
    ctx.blockCellSlot = savedBlockCellSlot;

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

// =====================================================================
// Phase 3.1f-6: Lazy emission of deferred thunk bodies
// =====================================================================

void lazyEmitThunkBody(
    EvalState & state, CompilationUnit & unit, uint32_t thunkIdx)
{
    auto & desc = unit.thunks[thunkIdx];
    if (desc.state == ThunkDescriptor::State::Compiled) [[likely]]
        return;
    if (!desc.deferredState)
        throw Error(
            "lazyEmitThunkBody: descriptor in Pending state but "
            "deferredState is null (compile-emit path bug)");
    if (!unit.irModule)
        throw Error(
            "lazyEmitThunkBody: descriptor in Pending state but "
            "unit.irModule was not retained (Phase 3.1f-1 not active)");

    auto * deferred = desc.deferredState.get();

    // Construct a fresh emitter targeting the existing CU.  The CU's
    // unit.code grows by appending the body's instructions to the
    // end — distinct from the eager path's "wrap-with-jump-over"
    // approach.
    IREmitter emitter(state, unit, *unit.irModule);

    uint32_t bodyOffset;
    uint16_t maxSlot;

    if (deferred->formals) {
        // Phase 3.1f-9: formals-lambda body — re-run the prologue+body
        // via emitFormalsBodyOnly.  No jump-over wrapping needed since
        // we are appending at the end of unit.code.
        ir::FreeVars freeVars;
        freeVars.vars.assign(
            deferred->freeVarsSorted.begin(),
            deferred->freeVarsSorted.end());
        bodyOffset = emitter.emitFormalsBodyPublic(
            deferred->bodyBlockId, freeVars, *deferred->formals);
        // emitFormalsBodyOnly drives subCtx internally; record the
        // max slot via the public mirror, which the helper updates.
        maxSlot = emitter.lastSubBlockMaxSlot;
    } else if (deferred->cellFreeVars.empty()) {
        // Simple case (Phase 3.1f-6): all free vars become upvalues
        // at index i = position-in-list.
        ir::FreeVars freeVars;
        freeVars.vars.assign(
            deferred->freeVarsSorted.begin(),
            deferred->freeVarsSorted.end());
        bodyOffset = emitter.emitBlockOnly(deferred->bodyBlockId, freeVars);
        maxSlot = emitter.lastSubBlockMaxSlot;
    } else {
        // Cell-capture case (Phase 3.1f-7): rebuild the cell-aware
        // BlockContext from the saved snapshot, then emit the body
        // directly via emitBlock (no jump-over wrapping needed since
        // we are appending at the end of unit.code).
        const auto & bodyBlock = unit.irModule->blocks[deferred->bodyBlockId];
        BlockContext subCtx;
        for (auto & cr : deferred->cellFreeVars) {
            subCtx.cellRefs[ir::VarId(cr.varId)] =
                {cr.innerUv, cr.entryIdx};
        }
        uint32_t uvIdx = deferred->nextInnerUv;
        for (auto fv : deferred->freeVarsSorted)
            subCtx.upvalueSlots[ir::VarId(fv)] = uvIdx++;
        for (auto p : bodyBlock.params)
            subCtx.allocSlot(p);

        bodyOffset = static_cast<uint32_t>(unit.code.size());
        emitter.emitBlockPublic(bodyBlock, subCtx);
        maxSlot = static_cast<uint16_t>(
            subCtx.nextSlot > 0xFFFF ? 0xFFFF : subCtx.nextSlot);
    }

    desc.codeOffset = bodyOffset;
    desc.maxSlot = maxSlot;
    desc.state = ThunkDescriptor::State::Compiled;
    desc.deferredState.reset();
}

} // namespace nix::bytecode
