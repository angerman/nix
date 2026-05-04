#pragma once
/// @file
/// v3 VM: dispatch loop + slim CallFrame.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/value.hh"
#include "v3/closure.hh"
#include "nix/expr/eval-gc.hh"

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
    std::vector<CallFrame> frames;
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

} // namespace nix::v3
