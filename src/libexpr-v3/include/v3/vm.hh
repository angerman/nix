#pragma once
/// @file
/// v3 VM: dispatch loop + slim CallFrame.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/value.hh"
#include "v3/closure.hh"
#include "nix/expr/eval-gc.hh"  // traceable_allocator (Boehm) — used below

#include <vector>
#include <cstdint>

namespace nix::v3 {

/// CallFrame flags.
enum CallFrameFlag : uint8_t
{
    CFF_NONE     = 0,
    /// On OP_RETURN, write the return value into the Thunk pointed to by
    /// `thunk` (state -> Evaluated, copy value into evaluated slot).
    CFF_THUNK_RETURN = 1 << 0,
    /// WC-38: GHC STG-style indirection retry.  When set on the CALLER
    /// frame at OP_RETURN's caller-resume path, if the just-popped frame's
    /// return value is still a Thunk or App, re-enter forcing on it
    /// (`goto op_force_slow`).  Set by OP_FORCE / OP_GET_LOCAL_FORCE /
    /// OP_GET_UPVALUE_FORCE before they push the thunk frame.  This
    /// replaces the over-eager OP_RETURN chain-push: instead of running
    /// the inner thunk's body INSIDE the outer's RETURN (which causes
    /// deep eager eval and breaks `with self;` lookups in lib.fix
    /// patterns), we install a forwarding pointer
    /// (`outer.evaluated = innerThunk`) and let the consumer drive the
    /// chain.  Mirrors GHC's stg_IND mechanism + tree-walker's slot
    /// mutation.
    CFF_FORCE_RETRY = 1 << 1,
    /// A8 (2026-05-13): the upper 16 bits of `flags` encode a
    /// stack-base-relative slot offset for a pending force writeback.
    /// Clear unless an opcode has set up an iterative force.  See
    /// applyForceWriteback in vm.cc for the protocol.
    CFF_FORCE_WB = 1 << 3,
    /// A8 (2026-05-13) phase 2: pointer-target writeback.  When set,
    /// `forceWriteTarget` (heap-pointer or list/attr slot) is the
    /// destination, NOT a stackBase-relative slot.  Used by OP_CALL_PRIMOP
    /// / OP_CALL when pre-forcing list elements iteratively into their
    /// ListVec storage.  Mutually exclusive with CFF_FORCE_WB (the
    /// applyForceWriteback helper checks pointer first).  Pops the forced
    /// value from the stack after writing.
    CFF_FORCE_WB_PTR = 1 << 4,
    /// 2026-05-17: pointer-target writeback that KEEPS the forced value
    /// on the stack.  Same target semantics as CFF_FORCE_WB_PTR but
    /// applyForceWriteback does NOT pop.  Used by opcodes whose
    /// architectural contract is "leave the selected value on top of
    /// stack" while ALSO memoizing the force into the source slot
    /// (e.g. OP_ATTRS_SELECT_IC's mapAttrs-entry force path).  Also
    /// gated on WHNF: returns false (no write, flag kept) if the top is
    /// still Thunk/App/Slot so the retry chain can chase further
    /// without polluting the source slot with a non-WHNF intermediate.
    CFF_FORCE_WB_PTR_KEEP = 1 << 5,
};

/// Slim CallFrame — 40 bytes, 2 fit in a 64B cache line minus 24B.
/// resultSlot/resultPtr were never read on return paths and are gone;
/// the return value is pushed onto valueStack and consumed by the caller.
struct CallFrame
{
    const CompilationUnit * cu;        // 8
    const Closure * closure;            // 8
    /// Optional thunk pointer for CFF_THUNK_RETURN frames.  When set, the
    /// return value is also copied into thunk->evaluated and the thunk's
    /// state is set to Evaluated.
    Thunk *   thunk;                    // 8
    uint32_t  ip;                       // 4
    uint32_t  stackBaseOffset;          // 4
    /// Floor on `vm.withStack` index for this frame: OP_WITH_LOOKUP only
    /// searches from `vm.withStack.size()` down to `withStackBase`, so a
    /// callee can't see its caller's `with`s.  At call entry we set this
    /// to the caller's `vm.withStack.size()` and then push the closure's
    /// captured snapshot.  At OP_RETURN we truncate to this base.
    uint32_t  withStackBase;            // 4
    uint32_t  flags;                    // 4 (widened from u8 for clean 40-byte layout)
    /// A8 phase 2 (2026-05-13): heap-pointer writeback target.  Set
    /// by opcode handlers that pre-force collection elements (e.g.
    /// OP_CALL_PRIMOP's deepForceList phase) so the forced WHNF gets
    /// stored back into the source list/attrset directly, not onto
    /// the value stack.  Valid only when `flags & CFF_FORCE_WB_PTR`.
    /// Cleared by applyForceWriteback after the write.
    Value *   forceWriteTarget = nullptr;   // 8
};

/// Per-EvalState VM state.
///
/// REVIEW CRIT-2: valueStack and withStack hold Value payloads with
/// Boehm-managed pointers (Closure*, Bindings*, Thunk*, ListVec*).
/// Use traceable_allocator so the storage is in a region Boehm
/// scans for roots; std::allocator's malloc'd storage was invisible
/// to the GC, leaving payloads reachable only via the conservative
/// C-stack scan.
struct VMState
{
    std::vector<Value, traceable_allocator<Value>> valueStack;
    // N10 (audit Round 2): traceable_allocator so frame storage is
    // Boehm-scanned.  CallFrame holds closure/thunk/forceWriteTarget
    // pointers; today they're rooted via other paths (arena
    // GC_add_roots) AND the scavenger walks frames directly, but
    // traceable_allocator makes the storage robust against any
    // future refactor that skips the explicit frames-walk.
    std::vector<CallFrame, traceable_allocator<CallFrame>> frames;
    /// Stack of in-scope `with` attrset values.  Top of stack = innermost.
    std::vector<Value, traceable_allocator<Value>> withStack;
    uint64_t nrInstructions = 0;
    /// OP_TAIL_CALL iteration counter — bumped on every tail call
    /// and reset whenever the frame stack grows or shrinks via
    /// non-tail OP_CALL / OP_RETURN.  Used to detect infinite tail
    /// recursion (`let f = x: f x; in f 1`) which v3's TCO would
    /// otherwise let run forever in O(1) frame space.
    uint64_t tailCallCount = 0;
};

/// Bytecode IR → CompilationUnit pipeline.
namespace ir { struct Module; }
CompilationUnit compile(const ir::Module & m);

/// Run the top-level CU's entry until OP_HALT, returning the final value.
Value run(const CompilationUnit & cu);

/// #698 Phase 3 diagnostic: returns the inner-most dispatchLoop's
/// VMState pointer on the current thread, or nullptr if no dispatch
/// loop is active.  Used by limits.cc's `V3_DBG_TRAP_ON_LIMIT` to
/// dump frame stacks on wall-time / cpu-time / heap-cap abort.
VMState * currentDispatchVM();

/// #705 (2026-05-21): every dispatchLoop pushes/pops its `vm` here
/// on entry/exit (RAII).  Scavenger walks every entry so nested
/// VMStates' roots are visible to a scavenge fired from any inner
/// dispatch.  Order of entries: outermost-first.  Same vm may
/// appear more than once under forceValue / inner re-entries;
/// scavenge dedupes via its `walked` set.
const std::vector<VMState *> & activeVMStack();

/// #425: process-wide lazy singleton of the `builtins` attrset.  Built
/// on first call from the registered primops table (matching the
/// OP_LIT_BUILTINS dispatch); subsequent calls return the same Value.
/// Used by the v3 force/call hook to materialise an upvalue when a
/// sub-Expr captured `builtins` as a freeVar.
Value getBuiltinsValue() noexcept;

/// Run a specific FuncId in `cu` as if it were a thunk body — no
/// arguments pushed, the function's nLocals worth of slots reserved,
/// and the dispatch loop runs until that function's OP_RETURN/OP_HALT.
/// Used by the CO-3 force-hook entry path: tree-walker forces a thunk
/// whose Expr* matches a known per-thunk FuncId; we run that FuncId.
/// Phase A only — funcIdx must reference a function with no upvalues
/// (nUpvalues == 0).  Phase B will accept an upvalues array.
///
/// `capturedWiths` (optional, nullable): outer-scope with-attrset
/// snapshot computed at force-hook entry from the tree-walker env
/// chain.  If non-null, pushed onto the VM withStack BEFORE the
/// frame's withStackBase is set, so OP_WITH_LOOKUP inside the body
/// sees those frames as outer scope.  The function's own ir::With
/// blocks push/pop on top of this snapshot.  Default null preserves
/// the pre-fix behaviour (empty outer with-stack).
Value runFunction(const CompilationUnit & cu, uint32_t funcIdx,
                  ListVec * capturedWiths = nullptr);

/// CO-2 phase B: run a per-thunk Function with a caller-provided
/// upvalues array.  `upvalues` must have exactly the count and order
/// matching `cu.lambdas[funcIdx].nUpvalues` / the IR Function's
/// `freeVars` list.  The dispatcher synthesizes a Closure whose
/// upvalues = the supplied array, sets the call frame's `closure`
/// field to it, and runs the function until OP_RETURN/OP_HALT.
///
/// `capturedWiths` (optional, nullable): see runFunction.
Value runFunctionWithUpvalues(const CompilationUnit & cu, uint32_t funcIdx,
                               const Value * upvalues, uint32_t nUpvalues,
                               ListVec * capturedWiths = nullptr);

/// #426: invoke a v3 lambda body Function with one argument.  Mirrors
/// runFunctionWithUpvalues but ALSO seeds the function's first slot
/// with `arg` so the body's OP_GET_LOCAL paramSlot reads the caller-
/// supplied value.  Used by the tree-walker -> v3 callFunction
/// cutover hook when applying a tree-walker lambda whose body has
/// been pre-lowered to v3 IR.
///
/// Preconditions: cu.lambdas[funcIdx] must describe a function with
///   - arity == 1 OR hasFormals == 1 (ie. takes a single argument
///     or a single attrset)
///   - nUpvalues == nUpvalues passed in
///
/// Behaviour mirrors v3's own OP_CALL: pushes `arg` onto the value
/// stack, sets up the call frame, runs to OP_RETURN.
Value runLambda(const CompilationUnit & cu, uint32_t funcIdx,
                Value arg,
                const Value * upvalues, uint32_t nUpvalues,
                ListVec * capturedWiths = nullptr);

/// EXIT_GC_SPIRAL Day 13-15 (2026-05-29): singleton interning pool for
/// 1-element capturedWiths ListVecs.  Exposed for NIX_VM_STATS dump.
/// Hits/Misses counter the cache hit-rate; Evicts counts collisions
/// (entry overwritten by a different key — a fresh alloc-+-install).
uint64_t getCapWithsHits()   noexcept;
uint64_t getCapWithsMisses() noexcept;
uint64_t getCapWithsEvicts() noexcept;

} // namespace nix::v3
