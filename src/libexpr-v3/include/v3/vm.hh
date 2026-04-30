#pragma once
/// @file
/// v3 VM: dispatch loop + slim CallFrame.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/bytecode.hh"
#include "v3/value.hh"
#include "v3/closure.hh"

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
struct VMState
{
    std::vector<Value>     valueStack;   // operand + locals
    std::vector<CallFrame> frames;
    /// Stack of in-scope `with` attrset values.  Top of stack = innermost.
    std::vector<Value>     withStack;
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

/// Run a specific FuncId in `cu` as if it were a thunk body — no
/// arguments pushed, the function's nLocals worth of slots reserved,
/// and the dispatch loop runs until that function's OP_RETURN/OP_HALT.
/// Used by the CO-3 force-hook entry path: tree-walker forces a thunk
/// whose Expr* matches a known per-thunk FuncId; we run that FuncId.
/// Phase A only — funcIdx must reference a function with no upvalues
/// (nUpvalues == 0).  Phase B will accept an upvalues array.
Value runFunction(const CompilationUnit & cu, uint32_t funcIdx);

/// CO-2 phase B: run a per-thunk Function with a caller-provided
/// upvalues array.  `upvalues` must have exactly the count and order
/// matching `cu.lambdas[funcIdx].nUpvalues` / the IR Function's
/// `freeVars` list.  The dispatcher synthesizes a Closure whose
/// upvalues = the supplied array, sets the call frame's `closure`
/// field to it, and runs the function until OP_RETURN/OP_HALT.
Value runFunctionWithUpvalues(const CompilationUnit & cu, uint32_t funcIdx,
                               const Value * upvalues, uint32_t nUpvalues);

} // namespace nix::v3
