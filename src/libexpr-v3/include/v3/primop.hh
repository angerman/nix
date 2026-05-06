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
#include <optional>
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

} // namespace nix::v3

namespace nix {
    struct Expr;
    struct Value;
    class PosIdx;
}

namespace nix::v3 {

/// REVIEW §2.1: RAII guard for the thread-local fallback Expr pointer
/// that primV3{CallBridge1,ForceAttr,ForceListElem} read on cycle
/// detection.  Setting it via raw save/restore was leaking the prior
/// outer Expr's fallback into the catch path on some throw shapes,
/// routing a subsequent re-entry's cycle-fallback to the wrong Expr.
/// Use `ScopedBridgeFallbackExpr` to bind for an RAII scope; pop on
/// every exit including throws.
struct ScopedBridgeFallbackExpr {
    nix::Expr * saved;
    ScopedBridgeFallbackExpr(nix::Expr * e);
    ~ScopedBridgeFallbackExpr();
};

/// #458 step 2: when TW is about to dispatch a `__v3_call_bridge_1`
/// PrimOpApp, route the call directly through v3's callClosure
/// instead of going through TW's primop dispatch (which fires bridge1
/// and forces args eagerly, the cardano-node #455 cycle source).
///
/// `funValue` must be a Value of v3 internal type bound to the bridge1
/// PrimOp; `arg` is the TW Value passed in (unforced -- v3 will force
/// on demand inside the closure body via the Bridge thunk).
/// `out` receives the result, bridged back to a TW Value.
///
/// Returns true if the value was dispatched directly via v3.
/// Returns false if the value isn't a bridge1 PrimOpApp or the
/// handle is invalid (caller should then fall through to TW's
/// regular primop dispatch).
bool tryDispatchBridge1Direct(nix::EvalState & ns,
                              const nix::Value & funValue,
                              nix::Value * arg,
                              nix::Value & out,
                              const nix::PosIdx pos);

/// #466 OP_CALL Bridge round-trip elimination.
///
/// If `funTw` is a forced TW Value of the shape
/// `mkPrimOpApp(__v3_call_bridge_1, vHandle)` — i.e., a v3 closure
/// that was bridged TO TW via v3ToTreeWalker — return the original v3
/// closure Value.  Returns an unset Value (tag=Uninitialized) for any
/// other shape.
///
/// Lets the OP_CALL Bridge handler skip the ns->callFunction round-
/// trip and dispatch the v3 closure directly via callClosure on the
/// caller's VMState — eliminating the cross-VMState force scenario
/// that motivates the lambda-skip cycle (#466).  Caller must have
/// forced `funTw` first; this function performs no forcing.
bool tryUnwrapBridge1Closure(const nix::Value & funTw, Value & outV3Fn);

/// REVIEW_2026-05-06b PR4: `clearBridgeTables()` removed.  The function
/// existed for "long-running daemon" cleanup but had zero callers --
/// keeping it advertised an option that nothing exercises and that
/// can't be wired safely without lifetime tracking on outstanding
/// PrimOpApp handles.  Future daemon support will need a real
/// lifetime-aware solution rather than a manual flush hook.

/// Apply a closure (or single-arg primop) to one argument and return
/// the result, by re-entering the VM dispatch loop on the same VMState.
/// Used by callback primops.  Throws if `fun` is not callable.
Value callClosure(VMState & vm, Value fun, Value arg);

/// #466 active-v3-vm tracking.  Returns the OUTER v3 VMState that is
/// currently bridging out via OP_CALL Bridge or forceBridgeThunk's
/// TW force; nullptr when no v3 vm is in flight.  Used by the call-
/// hook to detect "we're being re-entered from inside an outer v3
/// force chain" and refuse early to prevent the cross-VMState
/// BlackHole cycle.
VMState * activeV3VM();
struct ScopedActiveV3VM {
    VMState * prev;
    ScopedActiveV3VM(VMState * cur);
    ~ScopedActiveV3VM();
};

/// Force a thunk to WHNF.  Pass-through for non-thunk values.  Re-enters
/// the dispatch loop on the same VMState (used by primops like tryEval
/// that need to force from C++).
Value forceValue(VMState & vm, Value v);

/// #458 step A.2: per-attribute lookup against a Bridge thunk's TW
/// Value source WITHOUT forcing the whole TW Value.  Used by
/// OP_WITH_LOOKUP to resolve names against a partially-constructed
/// fix-point attrset (cardano-node #455 shape: `with self;` over
/// `extends overlay self` where `self` is mid-construction).
///
/// Returns nullopt when the lookup can't be made safely (src still
/// thunk-shaped, not an attrset, name absent, entry itself mid-
/// blackhole).  Returns the bridged v3 Value otherwise -- itself
/// possibly a fresh Bridge thunk if the attr's body is still a TW
/// thunk, preserving laziness one more level.
std::optional<Value> tryBridgeAttrLookup(Thunk * t, uint32_t v3name);

/// #458 step B (canonicalization): scalar fast-path for TW->v3
/// bridging.  When `nv` is an already-forced scalar (Int / Float /
/// Bool / Null), inline-write the equivalent v3::Value into `out`
/// and return true -- skipping the heavyweight `treeWalkerToV3Public`
/// path (VMState allocation + depth check + fiber yield + the type
/// switch).  Returns false if `nv` is not a known scalar; caller
/// must use the regular bridge path.
///
/// This reduces TW reliance: every scalar arg / attr / element
/// previously cost a Bridge-thunk alloc + future treeWalkerToV3
/// callback; now zero TW work after the fast bridge.
bool tryFastBridgeScalarTwToV3(const nix::Value & nv, Value & out);


/// #458 step A.4: existence check sibling to tryBridgeAttrLookup,
/// for the `attrs ? name` operator (OP_ATTRS_HAS).  Returns:
///   - 0: src not in a state where we can answer (still thunk-shaped,
///        not an attrset).  Caller should fall back to wholesale force.
///   - 1: name is present in the partial bindings.
///   - 2: src is an attrset, name is NOT present.
/// Distinguishes 0 from 2 because for `has`, "not present in partial
/// bindings" is the authoritative answer if the OUTER thunk is already
/// nAttrs (Bindings is published, even if entries are still thunks --
/// no entry will be added later).
enum class BridgeAttrHasResult : uint8_t { Indeterminate = 0, Present = 1, Absent = 2 };
BridgeAttrHasResult tryBridgeAttrHas(Thunk * t, uint32_t v3name);

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

/// #458 step B: bridge telemetry.  Track every v3<->TW value bridging
/// site so we can see (a) how often each direction fires and (b) how
/// much wall-time we spend in each.  The user directive: minimise
/// v3->tw->v3 round-trips.  The telemetry is the ground truth that
/// tells us where to attack next.
///
/// Counters are atomic uint64_t and always-on (~1 ns per call).
/// Timings are atomic uint64_t nanoseconds, gated by
/// NIX_V3_BRIDGE_TIMING=1 because steady_clock::now() is ~10-30 ns
/// per call and would dominate the bridges we're measuring.
///
/// Six bridge sites are instrumented:
///   - twToV3_full:   `treeWalkerToV3Public` whole-value bridge.
///   - twToV3_scalar: scalar fast-path hits.
///   - twToV3_attr:   `tryBridgeAttrLookup` per-attr peek (success).
///   - twToV3_has:    `tryBridgeAttrHas` per-attr existence (success).
///   - v3ToTw:        `v3ToTreeWalkerPublic` whole-value bridge.
///   - twForce:       TW state.forceValue calls from v3 hooks (force
///                    cycles that route TW->v3->TW->v3).
enum class BridgeKind : uint8_t {
    TwToV3Full = 0,
    TwToV3Scalar,
    TwToV3Attr,
    TwToV3Has,
    V3ToTw,
    TwForce,
    Count
};

void bridgeTelemetryBump(BridgeKind k, uint64_t ns);
void dumpBridgeTelemetry(std::FILE * out);

/// RAII timer that bumps the per-kind counter and (when timing is
/// enabled) accumulates wall time.  Use:
///   { BridgeTimer t(BridgeKind::TwToV3Full); ... heavy work ... }
struct BridgeTimer {
    BridgeKind kind;
    uint64_t startNs;
    BridgeTimer(BridgeKind k);
    ~BridgeTimer();
};

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
