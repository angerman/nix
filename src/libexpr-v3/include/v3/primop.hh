#pragma once
/// @file
/// v3 primop infrastructure.
///
/// PrimOp = a fixed-arity native function.  Each PrimOp has a name, an arity,
/// and a function pointer that takes (EvalState, args) and returns a Value.
///
/// PrimOps are registered in a thread-safe global registry at process start.
/// They appear as ordinary Closure-like Values (Tag::PrimOp) and participate
/// in OP_CALL via partial application: a PrimOp with arity > 1 wraps
/// successive calls in PrimOpApp pairs until all args are gathered.
///
/// Bring-up: only fully-applied PrimOps are supported (single-arg primops, or
/// multi-arg primops dispatched through OP_CALL_PRIMOP_N).  Partial
/// application TBD.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nix::v3 {

struct VMState;

/// Forward decl for the (optional) nix-side EvalState pointer used by
/// `builtins.import` and similar primops that need to load+parse files.
} // namespace nix::v3
namespace nix { class EvalState; }
namespace nix::v3 {

/// Placeholder EvalState — most primops don't need any of its fields,
/// but we want a stable type for the function-pointer signature.
///
/// `vm` is set by the dispatch loop just before invoking a primop, and
/// is used by callback primops (map, filter, foldl', genList) to
/// re-enter the VM via callClosure().
///
/// `nixEvalState` is set by the integrating CLI (v3-eval) when v3 is
/// running on top of the nix parser; primops like `import` use it to
/// parse files / run bindVars on the host evaluator's symbol table.
/// Null when v3 runs standalone.
struct EvalState
{
    VMState * vm = nullptr;
    nix::EvalState * nixEvalState = nullptr;
};

/// Set the thread-local nix::EvalState that primop dispatch will inject
/// into the v3 EvalState passed to each primop fn.  v3-eval calls this
/// once at startup.
void setNixEvalState(nix::EvalState * st);
nix::EvalState * getNixEvalState();

/// REVIEW MED-14: drop every entry from v3BridgeAttrs / v3BridgeLists /
/// v3BridgeClosures.  These tables grow unboundedly with the number of
/// lazy-bridged attrsets / lists / closures bridged across to tree-
/// walker; on long-running daemons (Hydra, LSP, library consumers) the
/// tables retain memory for the process lifetime.  Single-EvalState
/// CLIs (like v3-eval) don't need to call this -- the tables are torn
/// down at process exit.
///
/// SAFETY: only safe to call between top-level evals.  Existing
/// PrimOpApp values referencing handles in these tables would
/// dangle.  Caller is responsible for not retaining such values across
/// the clear.
void clearBridgeTables();

/// Apply a closure (or single-arg primop) to one argument and return
/// the result, by re-entering the VM dispatch loop on the same VMState.
/// Used by callback primops.  Throws if `fun` is not callable.
Value callClosure(VMState & vm, Value fun, Value arg);

/// Force a thunk to WHNF.  Pass-through for non-thunk values.  Re-enters
/// the dispatch loop on the same VMState (used by primops like tryEval
/// that need to force from C++).
Value forceValue(VMState & vm, Value v);

/// Function pointer signature.  The primop is given a span of forced
/// argument Values (the dispatcher arranges forcing) and writes its
/// result into `out`.
using PrimOpFn = void (*)(EvalState & state, Value * args, Value & out);

struct PrimOp
{
    std::string_view name;
    uint8_t          arity;
    PrimOpFn         fn;
    /// Bitmask of argument indices that should NOT be force-pre-evaluated
    /// before the primop body runs.  Bit `i` set means args[i] is passed
    /// in whatever form the caller had (Thunk / Tag::App / WHNF).  The
    /// primop body forces only what it actually needs.  Mirrors
    /// tree-walker's per-primop lazy-arg semantics.
    /// Bit 0 = arg 0, bit 1 = arg 1, etc.
    uint8_t          lazyArgs = 0;
    std::string_view doc;        // optional
};

/// Look up a primop by name.  Returns nullptr if not registered.
const PrimOp * findPrimOp(std::string_view name);

/// All registered primops keyed by name.  Used by the lowerer to
/// materialize the `builtins` attrset on demand.
const std::unordered_map<std::string, PrimOp> & allRegisteredPrimOps();

/// Register a primop (or replace an existing one — last write wins).
void registerPrimOp(const PrimOp & op);

/// Register the bring-up subset of primops (length, head, tail, ...).
/// Idempotent; call during EvalState init.
void registerBuiltinPrimOps();

/// Per-primop call counter.  Bumped on every OP_CALL_PRIMOP.  Used
/// for profiling — invaluable for working out which primops are hot
/// on a real-world workload (nixpkgs, cardano-node) vs the synthetic
/// fib/attrs benchmarks.  Keyed by primop name to survive across
/// process invocations and to avoid threading an index everywhere.
/// Print via `dumpPrimOpStats()` (called automatically under
/// NIX_VM_STATS=1 from v3-eval / the cutover hook).
void bumpPrimOpCallCount(const PrimOp * po);
void dumpPrimOpStats(std::FILE * out);

struct CompilationUnit;
/// Walk every CompilationUnit reachable from the import cache plus the
/// supplied entry CU and print the top `limit` LambdaDescriptors by
/// `forceCount`.  Used by V3_DBG_FORCES to surface the dominant hot
/// thunk-bodies across the whole evaluation, not just the top-level
/// expression's CU.
void dumpHotDescriptors(std::FILE * out, size_t limit,
                         const CompilationUnit * entryCu);

} // namespace nix::v3
