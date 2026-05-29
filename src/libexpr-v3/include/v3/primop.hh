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

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
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

// Forward-declare flake::Settings so v3 can hold a pointer without
// pulling in the full libflake header surface from this primop header.
namespace nix::flake { struct Settings; }

namespace nix::v3 {

/// #698 Phase 3: thread-local pointer to libcmd's `nix::flakeSettings`
/// global so v3 can call `nix::flake::lockFlake(*flakeSettings, ...)`
/// directly from `primGetFlake` without linking libcmd (which would
/// create an architecturally-undesirable libexpr-v3 → libcmd edge).
///
/// The CLI (`src/nix/main.cc`) calls `setFlakeSettings(&nix::flakeSettings)`
/// once at startup, alongside the existing `setNixEvalState` wiring.
///
/// Returns nullptr if no caller has wired it (e.g. v3-eval standalone
/// without flake support).  Phase 3's primGetFlake falls back to the
/// existing TW bridge when null — preserves correctness while keeping
/// v3-native opt-in.
void setFlakeSettings(const nix::flake::Settings * s);
const nix::flake::Settings * getFlakeSettings();

} // namespace nix::v3

namespace nix {
    struct Expr;
    struct Value;
    class PosIdx;
}

namespace nix::v3 {

/// #705 (2026-05-20): walk the v3 bridge-table roots for the
/// scavenger.  Each Value held in `v3BridgeClosures()` /
/// `v3BridgeAttrs()` / `v3BridgeLists()` carries a potential
/// nursery pointer (Closure / Bindings / ListVec) in its payload.
/// Without this walk, a TW->v3 bridge handle's underlying v3 Value
/// can be left pointing into freed nursery memory after a scavenge.
///
/// Symptom: hello.drvPath SIGSEGVed after scavenge#1 even when
/// `forwarded=0` (no objects were reachable via the standard roots
/// — vm.valueStack / vm.withStack / vm.frames — yet the dispatch
/// loop later dereferenced a nursery pointer obtained via a bridge
/// table lookup).
///
/// The callback receives each `Value &` in turn; it should forward
/// the payload (e.g. by calling the scavenger's `visitValue`).
/// Implemented in `primops.cc` where the bridge tables live.
void walkV3BridgeRoots(const std::function<void(Value &)> & visit);

/// #705 (2026-05-21): walk the import-cache results.  Each entry in
/// `importCache().results` holds a Value whose payload may carry a
/// nursery pointer — for instance, a freshly-imported module's
/// closure or attrset.  Without this walk, a repeat
/// `builtins.import` of the same path returns a stale pointer
/// after scavenge.
void walkImportCacheRoots(const std::function<void(Value &)> & visit);

/// 2026-05-29 evening (DIAG analysis spike): clear in-memory import
/// cache result set so a subsequent LiveTracer / GC walk sees the
/// nixpkgs evaluation graph as freeable.  Safe to call AFTER run()
/// returns; UNSAFE mid-eval (orphans in-flight imports).  Gated
/// via NIX_V3_END_OF_EVAL_CLEAR_IMPORT_CACHE=1 in run.cc.
void clearImportCacheResultsForDiag() noexcept;

/// 2026-05-29 evening: clear bridge tables (v3 ↔ TW handles).
/// Companion to clearImportCacheResultsForDiag.  Gated via
/// NIX_V3_END_OF_EVAL_CLEAR_BRIDGES=1.  UNSAFE if TW callbacks
/// fire subsequently.
void clearV3BridgesForDiag() noexcept;

/// 2026-05-29 evening (DIAG analysis): bridge-table sizes for
/// periodic L(t) sampling + NIX_VM_STATS dump.  Returns
/// (closures, attrs, lists) entry counts.  Each entry = 24 B
/// (Value + Expr* fallback) + transitive v3-heap retention.
std::array<size_t, 3> v3BridgeTableSizes() noexcept;

/// Unique v3-pointer counts within bridge tables.  If unique <<
/// table size, many entries duplicate the same v3Value (dedup-on-
/// push could collapse).  Returns (closures, attrs, lists).
std::array<size_t, 3> v3BridgeUniquePtrCounts() noexcept;

/// Iterate every v3 ↔ TW bridge entry; calls cb(v3Value, kind, idx)
/// for each.  Used by dumpV3BridgeRetention to compute per-entry
/// transitive retention without exposing the bridge-entry types.
void forEachV3BridgeEntry(
    const std::function<void(const Value &, const char *, size_t)> & cb) noexcept;

/// 2026-05-29 evening (production end-of-eval clear): drop bridges +
/// import-cache results at the end of `nix eval`'s render phase to
/// release the transitive evaluation graph to GC / process exit.
///
/// Per `lode/BRIDGES_HOLD_RETENTION_2026-05-29.md`: bridge tables
/// retain 99.8-99.9 % of arena bytes at end-of-eval on both HNE
/// and M5.  Clearing them allows the eval graph to become
/// unreachable from globals; combined with import-cache clear,
/// near-total reclamation is possible.
///
/// SAFETY: callers MUST sequence this AFTER all rendering /
/// `forceValue` calls complete + BEFORE any further TW callbacks
/// could fire.  `src/nix/eval.cc::run()` is the canonical call
/// site (last action before `return true`).  DO NOT CALL from
/// `nix repl` or chained-eval CLIs.
///
/// Opt-out: NIX_V3_KEEP_GLOBAL_ROOTS=1.
///
/// Reports stats under NIX_VM_STATS=1.
void clearPostEvalGlobalRoots() noexcept;

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

/// #493: dispatch helper for the TW-lambda formals-closure bridge.
///
/// When `v3ToTreeWalker` bridges a v3 Tag::Closure with hasFormals=true
/// to TW, it produces a `Tag::tLambda` Value whose `lambda.env` is a
/// sentinel keyed in `v3FormalsLambdaBridges()` to the underlying v3
/// Closure (with captured upvalues).  TW's `autoCallFunction` introspects
/// formals via `lambda.fun` (the original ExprLambda) and dispatches via
/// `callFunction`; this helper detects the sentinel env, recovers the
/// v3 Closure from the side-table, and runs the body in v3 with the
/// captured upvalues.
///
/// Returns true if the value was dispatched directly via v3.  Returns
/// false if `funValue` is NOT a TW lambda whose `lambda.env` is in the
/// bridge side-table (caller falls through to the regular call hook
/// logic).
bool tryDispatchFormalsLambdaBridge(nix::EvalState & ns,
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

/// STG-14a (#509/#515): direct v3-side dispatch for a TW lambda whose
/// body has been pre-lowered to v3 IR (i.e. lambda.fun is in
/// v3SubExprCache).  Used by vm.cc OP_CALL Bridge handler to bypass
/// `v3ToTreeWalkerPublic(arg)` -- which today forces a v3
/// Tag::Slot/Tag::Thunk arg eagerly, the throw site of the
/// nixpkgs hello.name STG_KEEP_HOOKS cycle (STG-12).
///
/// Mirrors v3CallFunctionEntry's gates + dispatch (cache lookup,
/// closure-result refusal, prepHookUpvaluesAndWiths) but takes the
/// arg as a v3 Value directly -- no TW round-trip, no eager force.
/// Each captured TW upvalue is wrapped as a v3 Bridge thunk lazily,
/// preserving the discipline that bodies force individual
/// captures only when actually needed.
///
/// Return:
///   true  -- body ran in v3; v3Out holds the result (still a v3 Value).
///   false -- funTw doesn't qualify; caller falls back to TW-bridge.
///
/// On BlackholeError from the body, the exception propagates
/// (caller catches via existing recovery, e.g. fallbackExpr).
bool tryDispatchTWLambdaInV3(nix::EvalState & state,
                              nix::Value & funTw,
                              Value v3Arg,
                              Value & v3Out);

/// REVIEW_2026-05-06b PR4: `clearBridgeTables()` removed.  The function
/// existed for "long-running daemon" cleanup but had zero callers --
/// keeping it advertised an option that nothing exercises and that
/// can't be wired safely without lifetime tracking on outstanding
/// PrimOpApp handles.  Future daemon support will need a real
/// lifetime-aware solution rather than a manual flush hook.

/// #466 / #479 Phase 1: cross-primop force-chain cycle detector.
///
/// Every bridge entry point (`primV3CallBridge1`, `primV3ForceAttr`,
/// `primV3ForceListElem`, `forceBridgeThunk`, the OP_CALL Bridge
/// shortcut) allocates a fresh VMState.  When lambda-skip is on (or
/// any time v3 owns more lambda execution), structural cycles can
/// snake across MULTIPLE such entries -- each layer sees a different
/// `(handle, sid)` tuple, so the per-primop `tlsBridge1InProgress` /
/// `tlsForceAttrInProgress` sets miss the cycle entirely.  The C-stack
/// then grows through every layer until SIGSEGV or until the static
/// `bridgePrimopDepth` ceiling fires far above the real cycle.
///
/// `ForceChainGuard` records every bridge entry on a single per-thread
/// set keyed by `(op, primary, secondary)`.  Re-entry on the same
/// identity throws `BlackholeError` BEFORE allocating the next
/// VMState, so the catch-side fallback (`fallbackToTreeWalker`) can
/// route to TW with the C-stack intact.  The set scales to depths
/// ~256 (default; tunable via `NIX_V3_FORCE_CHAIN_DEPTH`); when the
/// depth ceiling is reached, the next entry also reports cycle so a
/// pathological non-repeating chain still surfaces an error rather
/// than running until SIGSEGV.
enum class ForceChainOp : uint8_t {
    CallBridge1      = 1,  // primV3CallBridge1 (handle h, 0)
    ForceAttr        = 2,  // primV3ForceAttr (handle h, sid)
    ForceListElem    = 3,  // primV3ForceListElem (handle h, idx)
    ForceBridgeThunk = 4,  // forceBridgeThunk (thunk*, 0)
    OpCallBridge     = 5,  // OP_CALL Bridge shortcut (funTw*, 0)
};

struct ForceChainGuard {
    ForceChainGuard(ForceChainOp op, uint64_t primary, uint64_t secondary = 0);
    ~ForceChainGuard();
    /// #493 step 3c: cycle if either depth ceiling reached OR per-key
    /// reentry exceeded.  Pre-step-3c the second condition was
    /// !m_inserted (binary set; first re-entry fired); now relaxed to
    /// a counter so legitimate structural recursion (overlay chains)
    /// can re-enter up to NIX_V3_FORCE_CHAIN_REENTRY_MAX times.
    bool isCycle() const noexcept { return m_overDepth || m_overReentry; }
    /// Whether the guard was inserted at depth >= the configured ceiling
    /// (`NIX_V3_FORCE_CHAIN_DEPTH`).  Differentiates "real cycle"
    /// (re-entry on same key) from "depth bound" for diagnostics.
    bool atDepthCeiling() const noexcept { return m_overDepth; }
    ForceChainGuard(const ForceChainGuard &) = delete;
    ForceChainGuard & operator=(const ForceChainGuard &) = delete;
private:
    ForceChainOp m_op;
    uint64_t     m_keyA;
    uint64_t     m_keyB;
    bool         m_inserted    = false;
    bool         m_overDepth   = false;
    bool         m_overReentry = false;
};

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

/// STG-14b (#516): get-or-create a v3 Bridge thunk for a TW Value*.
/// Returns the SAME `Thunk*` for repeated calls with the same `srcV`,
/// so v3's `Thunk*`-keyed blackhole detection terminates the same
/// recursion patterns TW does (TW's blackhole keys on the underlying
/// nix::Value, which is shared; without this cache, v3 issues fresh
/// `Thunk*` per re-entry and never matches its own Black mark).
///
/// Pointer-keyed only -- no ABA stamp -- because the underlying TW
/// Value may legitimately transition (tThunk -> tAttrs etc.) under our
/// Bridge thunk, and we MUST keep returning the same Bridge thunk
/// across that transition to preserve blackhole identity.  Boehm-GC
/// recycling of nix::Value addresses is rare for live values; we
/// accept that small staleness window over breaking the
/// blackhole-identity invariant.
struct Thunk;
Thunk * getOrAllocBridgeThunkCached(nix::Value * srcV);


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

/// FFI plan A13 / migration step 3: per-primop flags for sandbox-at-
/// dispatch.  Today every "pure-eval / restricted-eval / impure /
/// experimental-feature" check is hand-rolled inside the primop body
/// (e.g., `if (settings.pureEval) state.error<EvalError>(...)`).  The
/// flags field declares the gate ONCE per primop; dispatch consults
/// the flags at OP_CALL_PRIMOP entry and refuses before running the
/// body.  Removes per-primop boilerplate and centralises the policy.
///
/// Bitmask, not enum-class, so `flags = Impure | Restricted` works
/// without verbose casts at registration sites.  Held as uint8_t in
/// PrimOp so the struct stays cache-friendly (fits with arity +
/// lazyArgs in one cache line).
enum PrimOpFlags : uint8_t {
    PRIMOP_NONE         = 0,
    /// Side-effecting primop that fails in pure-eval mode (e.g.,
    /// builtins.fetchurl, builtins.exec, builtins.derivationStrict in
    /// some modes).  Dispatch throws a typed error before the body.
    PRIMOP_IMPURE       = 1 << 0,
    /// Subject to restricted-eval gating (allowed-uris, NIX_PATH
    /// scrubbing).  Dispatch consults eval settings before the body.
    PRIMOP_RESTRICTED   = 1 << 1,
    /// Not exposed in `builtins.<name>` (mirrors TW's `bool internal`
    /// in PrimOp).  Used for `__v3_force_attr`, `__v3_call_bridge_1`,
    /// etc. -- bridge-internal helpers.
    PRIMOP_INTERNAL     = 1 << 2,
    /// Gated on an experimental feature flag.  Today's TW uses
    /// `optional<ExperimentalFeature>`; v3 uses a small registry
    /// mapping (PrimOp*, ExperimentalFeature) populated at
    /// registerPrimOp time.  Dispatch checks the registry before the
    /// body.
    PRIMOP_EXPERIMENTAL = 1 << 3,
    /// Suppress the auto-generated trace frame `while calling the
    /// '<name>' builtin` (e.g., for builtins.addErrorContext where the
    /// frame would be redundant).  Mirrors TW's `bool addTrace = true`
    /// (default-on); set this flag to suppress.
    PRIMOP_NO_TRACE     = 1 << 4,
};

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
    /// Bitmask of argument indices whose LIST ELEMENTS should be
    /// pre-forced iteratively by OP_CALL_PRIMOP / OP_CALL before the
    /// primop body runs.  Eliminates the C-recursive forceValue inside
    /// list-walking primops (concatLists, map, foldl', filter, all,
    /// any, etc.); each element is forced through the VM frame stack
    /// via writeback to its ListVec storage slot.  Bit `i` set ⇒ arg i
    /// must already be a List (combine with `lazyArgs` clear so the
    /// outer list itself is also WHNF on entry).
    uint8_t          deepForceList = 0;
    /// FFI plan A13: dispatch-time policy flags (see PrimOpFlags above).
    /// Default `PRIMOP_NONE` preserves today's behaviour for primops
    /// that haven't been audited; the goal is to decorate every primop
    /// with its actual policy and lift the per-body checks out.
    uint8_t          flags = PRIMOP_NONE;
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

/// Sum nanoseconds accumulated across all bridge kinds.  Only
/// populated when NIX_V3_BRIDGE_TIMING=1.  Used by run.cc's
/// PhaseTimer to split run_ms into vm_ms (pure v3 dispatch) and
/// bridge_ms (TW-side time reached via the bridge).  Returns 0 when
/// timing is disabled, so callers can unconditionally subtract.
uint64_t bridgeTotalNs();

/// Whether NIX_V3_BRIDGE_TIMING=1 was set at process start.
bool bridgeTimingEnabled();

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

/// #788 (2026-05-23) — accumulate wall-clock time spent in a
/// primop's body under NIX_VM_PRIMOP_TIME=1.  Caller passes the
/// nanosecond delta measured around the primop's `fn(...)` call.
/// Time is INCLUSIVE of nested forceValue / callClosure calls
/// (per #790 OPCYCLES inclusive accounting model).
void bumpPrimOpNanos(const PrimOp * po, uint64_t deltaNs);

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
