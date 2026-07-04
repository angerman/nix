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


/// #705 (2026-05-21): walk the import-cache results.  Each entry in
/// `importCache().results` holds a Value whose payload may carry a
/// nursery pointer — for instance, a freshly-imported module's
/// closure or attrset.  Without this walk, a repeat
/// `builtins.import` of the same path returns a stale pointer
/// after scavenge.
void walkImportCacheRoots(const std::function<void(Value &)> & visit);

/// LEVER-1 applied-import cache PROBE (NIX_V3_APPLIED_CACHE=probe): print the
/// cumulative would-cache counters.  Self-gates on probe mode + non-zero
/// counts; called from run.cc at end-of-root-eval (the atexit variant loses
/// its output in the `nix` binary — stderr/logger torn down before atexit).
/// Defined in vm.cc next to the probe.  Retires with the probe.
void dumpAppliedCacheProbeStats() noexcept;

/// LEVER-1 applied-import result cache (NIX_V3_APPLIED_CACHE=1; primops.cc,
/// design in the AppliedCache block there).  Lookup/insert by the memo key
/// (callee-CU identity + non-forcing canonical args hash).  The stored Value
/// is the LIVE result graph — GC-rooted via walkAppliedCacheRoots at the SAME
/// 3 sites as walkImportCacheRoots (gc.cc scavenger + precise_root bucketed +
/// global walks); a missed site is a moving-GC UAF.
bool appliedCacheLookup(const std::string & key, Value & out) noexcept;
void appliedCacheInsert(const std::string & key, Value result) noexcept;
void appliedCacheStatsDump() noexcept;
void walkAppliedCacheRoots(const std::function<void(Value &)> & visit);
/// Provenance: record/check import-RESULT closures (desc-keyed; see the
/// appliedImportResultDescs block in primops.cc for the soundness argument).
void appliedCacheRecordImportResult(const Value & v) noexcept;
struct LambdaDescriptor;  // fwd (full decl in closure.hh)
bool appliedCacheIsImportResultDesc(const LambdaDescriptor * d) noexcept;

/// Memory-bucket accounting (2026-06-04): size the "CU cache".
///   * `importCacheBytecodeBytes` — libc-malloc'd CompilationUnit
///     bytecode bytes (NOT in the arena; invisible to the arena mark).
///   * `importCacheCuCount`       — cached CompilationUnits (one/file).
///   * `importCacheResultCount`   — cached eval-result Values.
size_t importCacheBytecodeBytes() noexcept;
size_t importCacheCuCount() noexcept;
size_t importCacheResultCount() noexcept;
struct CompilationUnit;  // fwd (full decl at bytecode.hh; also below) for the M2.1 helper
/// M2.1 (BOUNDED_MEMORY_PLAN): sum approxBytesUsed + count of cached CUs the mark did NOT
/// reach (cold = no live thunk/closure/frame references them = evictable). `isCold(&cu)`
/// returns true for an unreferenced CU (the caller checks the mark's referenced-CU set).
/// Sizes the realizable CU-eviction win (the M2 GO/NO-GO) without evicting.
size_t importCacheColdBytes(
    const std::function<bool(const CompilationUnit *)> & isCold,
    size_t & coldCount) noexcept;
size_t importCacheStringConstRefs() noexcept;  // M-10: total interned-string refs
/// #139 CU-shrink RCA: per-field decomposition of the libc-malloc'd CU footprint
/// across all cached CUs, printed to stderr.  Separates runtime-irreducible fields
/// (code/symbols/ICs) from droppable DIAGNOSTIC side-tables (forceEmitSites,
/// LambdaDescriptor::name).  Gated by the caller (NIX_V3_MEM_BUCKETS).
void importCachePrintFieldBreakdown() noexcept;

/// 2026-05-29 evening (DIAG analysis spike): clear in-memory import
/// cache result set so a subsequent LiveTracer / GC walk sees the
/// nixpkgs evaluation graph as freeable.  Safe to call AFTER run()
/// returns; UNSAFE mid-eval (orphans in-flight imports).  Gated
/// via NIX_V3_END_OF_EVAL_CLEAR_IMPORT_CACHE=1 in run.cc.
void clearImportCacheResultsForDiag() noexcept;






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

/// T1 (LIST_ITERATION_FIX_PLAN_2026-06-08) — saturated 2-arg call.
/// Applies `fun` to `arg1` and `arg2`.  When `fun` is a plain arity-2
/// closure (the common multi-arg callback shape: `acc: x: …`, default-on
/// eval/apply), it enters the body ONCE with both args in slots 0..1 —
/// skipping the throwaway partial-application `ValuePair` (App PAP) that
/// the curried `callClosure(callClosure(fun,arg1),arg2)` allocates per
/// call, plus one dispatch prologue.  Any other callee shape (PAP, primop,
/// __functor, arity!=2, under/over-application) falls back to the curried
/// form, so the result stays byte-identical.  Gated NIX_V3_SATURATED_CALL.
Value callClosure2(VMState & vm, Value fun, Value arg1, Value arg2);

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

/// P2.1 step-0 measure (2026-07-02, TEMPORARY): print the per-formal wrapper
/// thunk share of runtime thunk allocations (audit §4.1).  Remove with the
/// instrument once P2.1 is decided.
void dumpFormalWrapperStats(std::FILE * out, const CompilationUnit * entryCu);

} // namespace nix::v3
