/// @file
/// v3 VM dispatch loop (switch-based for now; computed-goto comes later
/// once the opcode set is stable).
///
/// Frame model:
///   - One large valueStack of Values shared across all frames.
///   - Each CallFrame has a stackBaseOffset; frame-local slots are
///     valueStack[stackBaseOffset .. stackBaseOffset + nLocals).
///   - Operand stack scratch grows beyond locals; the next frame is laid
///     out on top of it.
///
/// OP_FORCE walks: if the value is a Suspended thunk, we push a CFF_THUNK_RETURN
/// frame that runs the thunk's bytecode; on OP_RETURN, the result is written
/// into the thunk (state -> Evaluated, evaluated = result), the thunk is
/// dropped from the side-table, and the result is left on the operand stack.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/alloc.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/disasm.hh"
#include "v3/errors.hh"
#include "v3/bytecode_primops.hh"
#include "v3/limits.hh"
#include "v3/barrier.hh"  // Phase D write-barrier helpers
#include "v3/mark_sweep.hh"      // Stage 6 runMajorMarkSweep dispatch trigger
#include "v3/live_trace.hh"      // Step 4 periodic L(t) trace dispatch hook

// FFI consolidation (audit Phase 2/3): vm.cc's only tree-walker touchpoints
// are COLD FFI-leaf paths — store/path coercion + the TW-bridge call round-
// trip.  The v3-native hot dispatch path (OP_FORCE, GET_LOCAL, native
// OP_CALL) uses NONE of eval.hh/store-api.hh/canon-path.hh (verified by the
// 2026-06-02 eval.hh-removal probe).  Those cold uses now route through
// v3/ffi.hh shims; `nix::evalTrace::*` (hot guards) is re-exported INLINE by
// ffi.hh as a Layer-0 util, so the dispatch loop stays zero-cost.
#include "v3/ffi.hh"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <cstring>

namespace nix::v3 {

// #670 diagnostic: SIGTRAP / SIGBUS / SIGSEGV handler.  Gated on
// `V3_DBG_SIGTRAP=1`.  When the v3 dispatcher hits a `brk #1` or
// other fatal-signal landing pad without a clean exception throw,
// the default macOS handler kills the process with exit 133/138 and
// no diagnostics.  This handler captures the trap PC, signal info,
// and C-stack backtrace to stderr via async-signal-safe primitives
// before exiting cleanly.  Off by default so debuggers' SIGTRAP
// (breakpoint) handling stays normal.
namespace {
[[noreturn]] void v3SignalDiagHandler(int sig, siginfo_t * info, void * /*ucontext*/)
{
    const char header[] = "\n*** v3 fatal-signal diag (V3_DBG_SIGTRAP=1) ***\n";
    (void)::write(2, header, sizeof header - 1);

    char buf[256];
    int n = std::snprintf(buf, sizeof buf,
        "signal=%d si_code=%d si_addr=%p si_errno=%d\n",
        sig, info ? info->si_code : 0,
        info ? info->si_addr : nullptr,
        info ? info->si_errno : 0);
    if (n > 0) (void)::write(2, buf, std::min<int>(n, (int)sizeof buf));

    void * frames[64];
    int nf = ::backtrace(frames, 64);
    if (nf > 0) ::backtrace_symbols_fd(frames, nf, 2);

    // _exit (not exit) to avoid running atexit handlers that may
    // re-throw or recurse.  133 = signal 5 (SIGTRAP) by the +128
    // convention; macOS sets exit code = signum.  Stay consistent
    // with the original signal so callers see the same exit code.
    int code = (sig == SIGTRAP) ? 133 : (sig == SIGBUS) ? 138 : (sig == SIGSEGV) ? 139 : 134;
    _exit(code);
}

struct V3SignalDiagInstaller {
    V3SignalDiagInstaller() {
        // Cold path — constructor runs once at process init.  Cache
        // the env-var probe in a static-const-bool to satisfy
        // `test/lint-no-inline-getenv.sh` (which scans for inline
        // getenv() calls regardless of cold/hot status).
        static const bool s_enabled =
            std::getenv("V3_DBG_SIGTRAP") != nullptr;
        if (!s_enabled) return;
        struct sigaction sa = {};
        sa.sa_sigaction = v3SignalDiagHandler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGTRAP, &sa, nullptr);
        ::sigaction(SIGBUS,  &sa, nullptr);
        ::sigaction(SIGSEGV, &sa, nullptr);
        const char msg[] = "v3 signal-diag installed (SIGTRAP/SIGBUS/SIGSEGV)\n";
        (void)::write(2, msg, sizeof msg - 1);
    }
};
// Static init runs at library load.  Cost on no-env case: one
// std::getenv lookup + a non-taken branch.
static V3SignalDiagInstaller _v3_signal_diag_installer;
} // namespace

// #456 fix: forward decls for the bridge entry points used in
// OP_CALL's Bridge-thunk branch.  Defined in primops.cc.

// #483 part 4 → 2026-05-18: the shallow-TW-attrs RAII helpers retired
// when treeWalkerToV3 became always-shallow for both nAttrs AND
// nList cases.  The push/pop call sites below are now no-ops, kept
// only until the next round of dead-code sweep.  See
// PROFILE_HELLO_NAME_2026-05-18.md option #1 + the comment in
// primops.cc treeWalkerToV3 nAttrs case for the architectural
// reasoning.

// WC-10: forward declaration at namespace scope so the `extern` use sites
// inside the anonymous namespaces below resolve to nix::v3::forceBridgeThunk
// (defined in primops.cc) rather than to a phantom anonymous-namespace symbol.

namespace {

/// REVIEW B5 — chase-vs-call-depth limits, documented.
///
/// Two distinct iteration bounds protect the VM:
///
/// 1. `kMaxIndirectionChase` (100000): max depth of Tag::Slot →
///    Tag::Thunk(eval=Slot→…) indirection chains traversed by
///    forceValue and OP_FORCE.  Fires only on pathological
///    self-referential let-rec patterns (`let x = x; in x`,
///    `let x = y; y = x; in x`) — the Black-state check catches
///    direct recursion, but SECD-style indirection cycles can
///    chase forever without re-entering the Black thunk.
///
///    #757 (2026-05-22): raised from 4096 → 100000 after cardano-
///    node M5 under v3-native callFlake hit the old limit on a
///    LEGITIMATE 4096-deep `prev` slot chain through
///    `lib.composeExtensions`'s `final: prev:` formals (each
///    haskell-nix overlay layer adds one).  The "≤4 indirections"
///    assumption that drove the original 4096 was a small-workload
///    artifact; haskell-nix's `lib.fix (foldr composeExtensions ...)`
///    legitimately stacks thousands of layers.  Diagnostic ring buffer
///    + frame stack at the firing point is gated by `V3_DBG_CHASE=1`.
///    A future optimization can replace this with slot-chain
///    compression (write the resolved WHNF back to each visited slot)
///    so subsequent forces hit in O(1) — see opForceCompress* for the
///    analogous compression on Evaluated thunks.
///
/// 2. `kMaxCallDepth` (5000): max number of CallFrame entries on
///    `vm.frames`.  Mirrors tree-walker's recursive C-stack guard.
///    Triggered by deeply recursive evaluation (e.g., infinite
///    `let f = x: f x; in f 0` chains that aren't tail-call-
///    optimised).  Higher than the chase limit because real
///    programs do legitimately deep call stacks (cardano-node
///    library evaluation has been observed at >2000 frames).
///
/// OP_FORCE applies BOTH bounds: it traverses Tag::Slot/Thunk
/// chains (chase), and may push frames when it triggers a thunk
/// body (call depth).  OP_CALL applies only the call-depth bound —
/// it doesn't traverse indirection chains; forceValue does that
/// before OP_CALL dispatches.  The asymmetry is intentional and
/// noted here so future reviewers don't see "4096 here, 5000 there"
/// and try to "fix" by unification.
constexpr int    kMaxIndirectionChase = 100000;
constexpr size_t kMaxCallDepth        = 5000;

/// eval/apply (#3): is `v` an under-applied multi-arity closure — an App /
/// App3 chain App(…App(closure, a0)…, a_{d-1}) whose leaf is a Closure of
/// arity A with d < A?  Such a value is WHNF: a partial application, a
/// function still awaiting (A − d) args.  The force paths must NOT try to
/// evaluate it as a deferred call (that would enter the arity-A body with too
/// few args), and the type-predicate ops (isFunction/typeOf/functionArgs)
/// must treat it as a `lambda`.
///
/// 2026-06-10: arity>1 closures (hence PAPs) now exist BY DEFAULT — the
/// eval/apply optimisation (NIX_V3_EVAL_APPLY, default-ON) collapses curried
/// `a: b: …` chains into a single multi-arity closure, so a partial call like
/// `(a: b: a + b) 1` materialises a Tag::App PAP rather than the inner
/// closure.  The earlier "returns false everywhere by default" claim was
/// stale; real nixpkgs hits this constantly (lib.isFunction / functionArgs on
/// makeOverridable / setFunctionArgs / callPackage results).  An App3 link
/// carries TWO applied args (`right` + `third`), so it contributes 2 to the
/// applied-arg depth.
// LOW-6 (CODEBASE_REVIEW_2026-06-11) FALSIFIED as a perf lever — do NOT add an
// OP_MAKE_CLOSURE arity byte + cache remaining-arity in App/App3 cells to make
// this O(1).  The spine walk below is O(applied-arg depth), and that depth is
// 1 for the dominant case (`(x: y: …) a` = a single App link).  For depth 1
// the loop runs ONCE — it is ALREADY the O(1) that LOW-6 would buy.  Measured
// on darwin-4 (the dedicated builder): a worst-case PAP storm —
//   builtins.length (builtins.filter builtins.isFunction
//     (builtins.genList (i: (x: y: x + y) i) 2000000))
// (2M depth-1 PAPs, each isFunction-checked → routed through THIS predicate,
// then forced) — completes in 0.98 s.  The spine walk is not a hotspot at any
// realistic PAP count, and deeper spines (depth ≥2) are rare.  Against that
// ~zero benefit, LOW-6 costs an invasive repr change (ValuePair has no spare
// slot for remaining-arity; the App Value's pointer payload has only 3
// alignment-low-bits, which would force a mask on EVERY asPair() VM-wide) and
// risks regressing the just-fixed C-8/9/10 PAP force-handshake — a clear
// measure-twice loss.  (Correctness of the PAP family is ALREADY fixed by
// C-8/9/10 / f5875a89c; LOW-6 was only ever a perf/simplification idea.)
// P-9's saturated-call superinstruction is enabled by this arity byte, so it
// falls with LOW-6; P-9's other emit fusions are marginal+measure-gated per
// the SET_LOCAL_KEEP precedent (+1.4%).
[[gnu::always_inline]] inline bool isUnderappliedClosurePap(const Value & v)
{
    Tag t = v.tag();
    if (t != Tag::App && t != Tag::App3) return false;
    const Value * cur = &v;
    size_t depth = 0;
    while ((cur->tag() == Tag::App || cur->tag() == Tag::App3) && cur->asPair()) {
        depth += (cur->tag() == Tag::App3) ? 2 : 1;
        cur = &cur->asPair()->left;
    }
    return cur->tag() == Tag::Closure && cur->asClosure()
        && cur->asClosure()->desc
        && cur->asClosure()->desc->arity > depth;
}

/// C-8/9/10 (CODEBASE_REVIEW_2026-06-11): the canonical "does this operand
/// still need forcing before an opcode can inspect it" predicate for the
/// force handshake (`if (needsForce(v)) { ip--; flags|=CFF_FORCE_RETRY; goto
/// op_force_slow; }`).  An under-applied closure-PAP is already WHNF (a partial
/// application) — it must NOT be sent back through the handshake, because
/// op_force_slow leaves a PAP unchanged (vm.cc op_force_slow break) and the
/// opcode would re-test the same App/App3 tag and rewind FOREVER (the infinite
/// force-retry loop, where TW instead raises a type error).  Covers App AND
/// App3 (isAppLike) so 3-arg PAPs are handled too.  Excluding PAPs here lets
/// them fall through to each opcode's existing type-error path, matching TW.
[[gnu::always_inline]] inline bool needsForce(const Value & v)
{
    Tag t = v.tag();
    return (t == Tag::Thunk || t == Tag::App || t == Tag::App3
            || t == Tag::Slot)
        && !isUnderappliedClosurePap(v);
}

// C-1 falsifier (CODEBASE_REVIEW_2026-06-11): counts how many times the
// CFF_FORCE_WB_PTR_KEEP branch in applyForceWriteback disarms on an
// under-applied closure-PAP top — the case that previously left the KEEP
// permanently armed (a PAP is permanently App-tagged) and produced the firefox
// getLib="21" cross-write.  Reported at process exit under V3_DBG_KEEP_PAP=1.
// Single-threaded eval, so a plain counter is sufficient.  Retirement
// criterion: delete once the firefox getLib residual is closed and
// byte-identical to TW — this counter exists only to confirm the mechanism is
// live on that workload.
static uint64_t g_keepPapDisarmCount = 0;
namespace {
struct KeepPapStatsAtExit {
    ~KeepPapStatsAtExit() {
        static const bool s_on = std::getenv("V3_DBG_KEEP_PAP") != nullptr;
        if (s_on)
            std::fprintf(stderr, "v3 C-1 KEEP-PAP disarms: %llu\n",
                         (unsigned long long)g_keepPapDisarmCount);
    }
};
KeepPapStatsAtExit _keepPapStatsAtExit;
}  // namespace

// V3_DBG_TRACE_THUNK_X — file-scope thunk-creation registry.  Bumped
// at every OP_MAKE_THUNK; consulted by the OP_WITH_LOOKUP cycle dump
// so we can compare each frame's *current* `t->suspended.desc`
// against the descriptor pointer that was written at creation time.
// A mismatch = in-place mutation (descriptor table relocated, union
// overlap UB, or a different writer to suspended.desc somewhere).
struct ThunkCreationInfo {
    uint32_t funcIdx;
    uint32_t codeOff;
    std::string name;
    const LambdaDescriptor * descPtr;
    const CompilationUnit * cu;
};
static const bool g_traceThunkX =
    std::getenv("V3_DBG_TRACE_THUNK_X") != nullptr;
inline std::unordered_map<const Thunk *, ThunkCreationInfo> & thunkCreationMap()
{
    static thread_local std::unordered_map<const Thunk *, ThunkCreationInfo> m;
    return m;
}

// #558 Phase 3.3 (2026-05-12): partialBindingsRegistry +
// lookupInPartialChain + pickLargestLayer retired.  The cell-update-
// everywhere experiment (Thunk::shapeCell, Phase 1.5) that nominally
// replaced them was itself default-off and has now been removed entirely
// (M-8, CODEBASE_REVIEW_2026-06-11); a local cycle falls through to the
// BlackholeError throw, as it always did in production.
//
// Note: the anon namespace closing/reopening that used to be required
// for the external-linkage `partialBindingsRegistry` forward decl is
// also gone — gc.cc no longer references it either.

// #548c (2026-05-10) per-CU registry for the alloc/force atexit dump.
// V3_DBG_ALLOC_DUMP=1 enables.  At process exit, top-N descriptors
// by (allocCount + forceCount) are dumped with file:line:col, so we
// can identify hot re-instantiation sites.  Populated lazily as
// dispatchLoop sees CUs (the first OP_MAKE_THUNK with a CU pointer
// adds it to the set).
inline std::unordered_set<const CompilationUnit *> & cuRegistry()
{
    static thread_local std::unordered_set<const CompilationUnit *> s;
    return s;
}
static const bool g_dbgAllocDump =
    std::getenv("V3_DBG_ALLOC_DUMP") != nullptr;

// P-2 (CODEBASE_REVIEW_2026-06-11): per-opcode profiling gates, promoted to
// FILE scope.  These used to be `static const bool` declared INSIDE the
// dispatch `while` body, so every dispatched opcode paid a magic-static guard
// (acquire-load + predicted branch) — the exact anti-pattern the file fixes
// elsewhere via kCountInstructions.  File-scope statics are initialised once at
// program start with no per-access guard.
static const bool g_countOpcodes =
    std::getenv("NIX_VM_OPCOUNTS") != nullptr;
static const bool g_countOpCycles =
    std::getenv("NIX_VM_OPCYCLES") != nullptr;

// P-1 (CODEBASE_REVIEW_2026-06-11): major-GC threshold + growth tunables,
// promoted to FILE scope.  They were `static const` declared INSIDE the
// per-opcode major-GC safepoint, so every dispatched opcode (with major GC on,
// the default) paid two magic-static guards.  File-scope → init once, no guard.
static const size_t g_majorGcInitialThresholdBytes = [] {
    const char * v = std::getenv("NIX_V3_MAJOR_GC_THRESHOLD_MB");
    long mb = 256;
    if (v) { long p = std::strtol(v, nullptr, 10); if (p >= 16 && p <= 32768) mb = p; }
    return static_cast<size_t>(mb) << 20;
}();
static const double g_majorGcGrowth = [] {
    const char * v = std::getenv("NIX_V3_MAJOR_GC_GROWTH");
    if (v) { double d = std::strtod(v, nullptr); if (d >= 1.5 && d <= 8.0) return d; }
    return 2.0;
}();
// FP-4 Shape A (GENERATIONAL_MAJOR_DESIGN_2026-06-14): opt-in
// generational-major.  Under the nursery the per-op major GC is hard-disabled
// (M-3: the major marker skips nursery cells → would sweep arena cells reachable
// only through them = UAF).  Shape A composes them safely: at the major-GC
// safepoint, force a scavenge FIRST (promotes all survivors to tenured, empties
// the nursery — single-region), THEN run the major mark-sweep over the now
// nursery-free tenured set.  This reclaims the tenured stranded dead (the Layer-C
// 224 MB) that the nursery alone leaves, while firing only on tenured-growth
// (O(1-few)/eval, dodging the cache-bound-mark wall).  OPT-OUT RETIRED
// (2026-06-15): unconditional now — gen-major Shape A is the SOLE major
// collector under the always-on nursery (the flip soaked clean across all of
// nixpkgs on darwin-4, 24882 attrs, 0 divergence).  Net win on measured deep
// workloads (M5 −624 MB @ +3 % CPU; firefox neutral RSS @ −13 % CPU).
static const bool g_genMajorEnabled = true;

// PLAN_BEAT_TW_V2 workstream A step 2 — shared-parent-writeback COUNTER.
//
// Counts how many chain children each parent has (incremented at the two
// allocChainBindings call sites) and, at the single writeback fire point,
// reports any `forceWriteTarget` that lands inside a parent shared by ≥2
// chains.  Gated V3_DBG_SHARED_WB=1 (V3_DBG_SHARED_WB_ABORT=1 to abort on the
// first hit for inspection); DEFAULT-OFF → one predicted-not-taken branch in
// production.
//
// FINDING (2026-06-13): these writebacks are PERVASIVE and BENIGN.  firefox
// fires 18, git 10, cargo 8, rustc 7 … per eval — yet every drvPath stays
// byte-identical to TW (the 06-07 failure set, 5/5).  They are correct-WHNF
// memoisation into a shared layer, exactly like TW's own layered-Bindings
// memoisation through shared layers (src/libexpr attr-set.hh; PLAN §0.4).  So
// "any writeback into a shared parent is the C-1 signature" is FALSE — most are
// safe.  The C-1 corruption is specifically a WRONG-VALUE / stale-KEEP write;
// its precise detector is a PROVENANCE ASSERT (the armed payload identity must
// still be at the target at fire time), NOT this child-count heuristic.  This
// counter's value for the L1 lever: it (a) confirms shared-parent memoisation
// is benign, and (b) bounds the number of sites L1's "parent hit → no
// writeback" would convert (losing benign slot-flattening, a CPU cost).
// Retirement: remove with the chain-SELECT L1 lever (and add the provenance
// assert there as the real C-1 guard).
static const bool g_sharedWbDetect = std::getenv("V3_DBG_SHARED_WB") != nullptr;
static const bool g_sharedWbAbort  = std::getenv("V3_DBG_SHARED_WB_ABORT") != nullptr;

// Child-count per chain parent (detector-only; populated solely when
// g_sharedWbDetect).  Function-local static so it costs nothing when the gate
// is off (never touched).  Pointers are stable (flat MS does not move cells);
// run the detector with a high NIX_V3_MAJOR_GC_THRESHOLD_MB to avoid a freed
// parent's address being recycled under a stale count.
static std::unordered_map<const Bindings *, uint32_t> & chainChildCount() noexcept
{
    static std::unordered_map<const Bindings *, uint32_t> m;
    return m;
}

// PLAN_BEAT_TW_V2 workstream A step 3 — chain-SELECT L1 lever.
//
// When set, OP_ATTRS_SELECT / OP_ATTRS_SELECT_DYN on a Chain Bindings does a
// chain-WALK lookup (NO materialize() flat copy — that copy is the dominant
// chain-on RSS residual + a shared s_matMemo write surface) instead of
// materialising.  Writeback safety (the 2026-06-07 corruption fix, now retried
// per §0.4 since the four wrong-value writers are fixed): a memoizing KEEP
// writeback is armed ONLY for a LEAF-overlay hit — this chain's own
// freshly-copied entry, never shared.  A hit in a SHARED parent layer pushes
// the value WITHOUT arming a writeback: the App/Thunk still self-memoizes at
// its own level (idempotent, like TW's shared thunks); only the SELECT slot
// flattening is lost (a small re-force CPU cost), and no value is written into
// the shared parent.  v1 skips the inline cache for chain operands.
//
// Gate: NIX_V3_CHAIN_LOOKUP_SELECT — DEFAULT-ON (opt-out =0).  Retired to
// default-on 2026-06-14 (FP-1): −33.6 MB firefox arena / −29 MB maxRSS
// (darwin-4), byte-identical, CPU-neutral; validated (06-07 canary 5/5, lang
// 143, core 21/21, provenance 0 violations = C-1 refuted, aggressive-GC clean).
// The original −≥80 MB single-lever revert bar was MIS-SHAPED: v3's memory
// excess is a DISTRIBUTED per-object tax, so correct levers are 15-40 MB by
// construction; the bar is now CUMULATIVE (FP-0 ratchet), and this lever banks
// −33 MB toward it.  Opt-out =0 retained as the falsifier handle (revert if any
// divergence ever appears).  See lode/MEMORY_FORWARD_PLAN_2026-06-14.md.
static const bool g_chainLookupSelect = [] {
    const char * v = std::getenv("NIX_V3_CHAIN_LOOKUP_SELECT");
    return v == nullptr || (v[0] != '0');  // default-ON unless explicitly =0
}();

// Provenance assert (the precise C-1 guard, gated V3_DBG_SHARED_WB): records the
// value at each KEEP-armed forceWriteTarget at ARM time; at FIRE time the target
// must STILL hold that value.  A mismatch means a re-entrant force overwrote the
// slot between arm and fire — a STALE KEEP about to cross-write an unrelated WHNF
// (the C-1 corruption mechanism) — which the assert reports/aborts.  thread_local
// + populated only under the gate → zero production cost.
static std::unordered_map<const Value *, Value> & armedWritebackValue() noexcept
{
    static thread_local std::unordered_map<const Value *, Value> m;
    return m;
}

// ── Workstream H sizing probe (V3_DBG_UPVAL_DUP, default-off) ────────────────
// Sizes the recoverable RSS from SHARED CAPTURE FRAMES: v3 flat-copies each
// thunk's captured upvalues into its tail[]; sibling thunks that capture a
// byte-identical tuple could instead share ONE frame (TW's Env model).  At each
// OP_MAKE_THUNK we hash the tail[] capture tuple and bucket it; atexit we report
// total thunk-upvalue bytes, distinct tuples, and the duplicated (recoverable)
// fraction = Σ_tuple (count-1)·nUp·8 — a conservative LOWER BOUND (only
// byte-identical tuples; the broader subset-Env sharing could recover more),
// plus the top source lambdas by upvalue volume.  Pointers in the tuple are
// stable (flat MS doesn't move); run with a high NIX_V3_MAJOR_GC_THRESHOLD_MB so
// the same logical thunks aren't re-counted across collections.
static const bool g_dbgUpvalDup = std::getenv("V3_DBG_UPVAL_DUP") != nullptr;

namespace upvaldup {
struct TupleStat { uint64_t count = 0; uint16_t nUp = 0; };
struct LambdaStat { uint64_t thunks = 0; uint64_t upWords = 0; const LambdaDescriptor * desc = nullptr; };
inline std::unordered_map<uint64_t, TupleStat> & tuples() {
    static std::unordered_map<uint64_t, TupleStat> m; return m;
}
inline std::unordered_map<uint32_t, LambdaStat> & lambdas() {
    static std::unordered_map<uint32_t, LambdaStat> m; return m;
}
inline bool & atexitRegistered() { static bool b = false; return b; }
inline void dump();  // defined after resolvePosSnapshot is in scope (below)
} // namespace upvaldup

// Record one thunk's capture tuple (called from OP_MAKE_THUNK when nUp>0 and the
// gate is on).  fmix64-folds tail[0..nUp).w with nUp into a 64-bit tuple hash.
[[gnu::noinline]] static void recordUpvalDup(const Thunk * t, uint16_t nUp)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(nUp) * 0xC2B2AE3D27D4EB4Full);
    for (uint16_t i = 0; i < nUp; ++i) {
        uint64_t x = t->tail[i].w;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
        h ^= x; h *= 0x100000001B3ull;
    }
    auto & ts = upvaldup::tuples()[h];
    ts.count += 1; ts.nUp = nUp;
    const LambdaDescriptor * d = t->suspended.desc;
    uint32_t co = d ? d->codeOffset : 0;
    auto & ls = upvaldup::lambdas()[co];
    ls.thunks += 1; ls.upWords += nUp; ls.desc = d;
    if (!upvaldup::atexitRegistered()) {
        upvaldup::atexitRegistered() = true;
        std::atexit([] { upvaldup::dump(); });
    }
}

namespace upvaldup {
inline void dump()
{
    uint64_t thunksWithUp = 0, totalUpWords = 0, recoverableWords = 0;
    for (auto & kv : tuples()) {
        thunksWithUp     += kv.second.count;
        totalUpWords     += kv.second.count * kv.second.nUp;
        recoverableWords += (kv.second.count - 1) * kv.second.nUp;  // dedup tuple
    }
    const double upMB  = totalUpWords     * double(sizeof(Value)) / 1e6;
    const double recMB = recoverableWords * double(sizeof(Value)) / 1e6;
    std::fprintf(stderr,
        "v3 upval-dup (workstream H sizing): thunks-with-upvalues=%llu  "
        "upvalue-bytes=%.1f MB  distinct-capture-tuples=%zu  "
        "duplicated(recoverable by shared frames)=%.1f MB (%.1f%%)\n",
        (unsigned long long)thunksWithUp, upMB, tuples().size(),
        recMB, upMB > 0 ? 100.0 * recMB / upMB : 0.0);
    // Top source lambdas by upvalue volume (the H candidates).
    std::vector<std::pair<uint32_t, LambdaStat>> v(lambdas().begin(), lambdas().end());
    std::sort(v.begin(), v.end(),
        [](auto & a, auto & b) { return a.second.upWords > b.second.upWords; });
    std::fprintf(stderr, "  top source lambdas by captured-upvalue volume:\n");
    for (size_t i = 0; i < v.size() && i < 12; ++i) {
        const LambdaDescriptor * d = v[i].second.desc;
        const PosSnapshot * ps = d ? resolvePosSnapshot(d->posHandle) : nullptr;
        std::fprintf(stderr,
            "    %7.1f MB  %9llu thunks  avg-nUp=%.1f  %s @%s:%u\n",
            v[i].second.upWords * double(sizeof(Value)) / 1e6,
            (unsigned long long)v[i].second.thunks,
            v[i].second.thunks ? double(v[i].second.upWords) / v[i].second.thunks : 0.0,
            (d && !d->name.empty()) ? d->name.c_str() : "<anon>",
            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
            ps ? ps->line : 0u);
    }
}
} // namespace upvaldup

// (FP-2b sizing probe V3_DBG_THUNK_WITHS retired 2026-06-14 — it measured the
// capturedWiths null-fraction (firefox 74% / M5 97%) to greenlight FP-2b's
// tail-relocation; FP-2b landed byte-identical with M5 arena −151 MB, so the
// one-shot measurement is removed per its retirement criterion.)

// #733 (2026-05-21) hot-path stat-counter gate.  The per-descriptor
// allocCount/forceCount + global thunksForced/thunksAllocated/
// bridgeThunksForced increments live on the hottest paths in the
// interpreter — every OP_MAKE_THUNK and every Suspended→Blackhole
// transition.  Two of the three writes (desc->forceCount,
// allocStats().thunksForced) target cache lines shared across
// thunks/threads, so an unconditional ++ is a cross-cache-line
// dirty write per force.  Five known consumers exist:
//   V3_DBG_ALLOC_DUMP (atexit per-descriptor dump),
//   V3_DBG_FORCES (periodic stride dump),
//   V3_DBG_FORCE_NAME / V3_DBG_FORCE_POS (first-force trace),
//   NIX_VM_STATS (v3-eval --json/--strict completion banner +
//   run.cc completion banner).
// None of these is in the default production path.  Gate all
// counter mutations behind a single static OR of the five env
// vars; the unlikely-branch hint keeps the predicted fall-through
// near the original code.  Retirement criterion: delete this gate
// + restore unconditional increments WHEN a measurement shows the
// gating is no-op (e.g. all consumers move to sampling).
inline bool dbgForceStatsActive() noexcept
{
    static const bool s_active = [] {
        return std::getenv("V3_DBG_ALLOC_DUMP") || std::getenv("V3_DBG_FORCES")
            || std::getenv("V3_DBG_FORCE_NAME") || std::getenv("V3_DBG_FORCE_POS")
            || std::getenv("NIX_VM_STATS");
    }();
    return s_active;
}

// #548c (2026-05-10) atexit dump.  Walks every CU registered above
// and every LambdaDescriptor in each CU's lambdas vector; emits the
// top-N by (allocCount + forceCount).  Source positions are
// resolved via the global posSnapshotPool so output rows look like
//   alloc=12345 force=12345 setType@/nix/store/.../parse.nix:64:44
// suitable for one-pass scanning.
struct AllocDumpInstaller {
    AllocDumpInstaller() {
        if (g_dbgAllocDump) {
            std::atexit([] {
                struct Row {
                    uint64_t alloc;
                    uint64_t force;
                    const LambdaDescriptor * desc;
                };
                std::vector<Row> rows;
                for (auto * cu : cuRegistry()) {
                    if (!cu) continue;
                    for (const auto & ld : cu->lambdas) {
                        if (ld.allocCount == 0 && ld.forceCount == 0)
                            continue;
                        rows.push_back({ld.allocCount, ld.forceCount, &ld});
                    }
                }
                std::sort(rows.begin(), rows.end(),
                    [](const Row & a, const Row & b) {
                        return (a.alloc + a.force) > (b.alloc + b.force);
                    });
                size_t lim = std::min<size_t>(rows.size(), 50);
                std::fprintf(stderr,
                    "\nv3 V3_DBG_ALLOC_DUMP: top %zu/%zu lambdas by (alloc+force)\n",
                    lim, rows.size());
                for (size_t i = 0; i < lim; ++i) {
                    const auto & r = rows[i];
                    const auto * d = r.desc;
                    const char * name = d->name.empty()
                        ? "<anon>" : d->name.c_str();
                    const PosSnapshot * ps = resolvePosSnapshot(d->posHandle);
                    char buf[256];
                    if (ps && !ps->file.empty()) {
                        std::snprintf(buf, sizeof buf,
                            "%s:%u:%u",
                            ps->file.c_str(), ps->line, ps->column);
                    } else {
                        std::snprintf(buf, sizeof buf,
                            "<no-pos> codeOff=%u", d->codeOffset);
                    }
                    std::fprintf(stderr,
                        "  alloc=%llu force=%llu %s @ %s\n",
                        (unsigned long long)r.alloc,
                        (unsigned long long)r.force,
                        name, buf);
                }
                std::fflush(stderr);
            });
        }
    }
};
static AllocDumpInstaller s_allocDumpInstaller;

[[gnu::always_inline]]
inline Value pop(VMState & vm)
{
    Value v = vm.valueStack.back();
    vm.valueStack.pop_back();
    return v;
}

[[gnu::always_inline]]
inline Value & top(VMState & vm) { return vm.valueStack.back(); }

/// V3_DBG_FORCE_SITE diagnostic: log "OP_FORCE@ip=N site=lower.cc:LINE"
/// for each force-flavoured opcode dispatched.  Reads the side-table
/// `cu->forceEmitSites` populated by emit.cc.  The env-var check is
/// done exactly once (static-once-init) so when the var is unset the
/// branch predictor will skip this entirely — no runtime cost in the
/// default build.
///
/// `instrIp` is the bytecode offset of the force opcode itself (i.e.
/// `ip - 1` at the OP_FORCE / OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
/// entry, before any further increments).  Lookup is via std::lower_bound
/// on the (already-sorted) side-table — O(log N) where N is the number
/// of force emit sites in the CU.
/// V3_DBG_FORCE_INSIDE_X — tightly scoped force tracer for the v3-direct
/// nixpkgs eval-order RCA.  Fires only when there's a Black thunk
/// named "x" anywhere on the frame stack (== lib.fix's x_thunk being
/// forced).  Logs the forced thunk's name + codeOffset, the forcing
/// site's bytecode IP, and the immediate enclosing thunk/closure
/// frame.  Capped at 200 entries so it doesn't flood.  Useful for
/// finding the v3-specific eager force that has no TW analog —
/// compare two traces (one v3-direct + STG, one a synthetic that
/// works) and the divergent line is the smoking gun.
///
/// T2 (LIST_ITERATION_FIX_PLAN_2026-06-08) — these force-trace gates are
/// read on the per-element OP_FORCE *slow* path (every non-WHNF force, i.e.
/// once per lazy list element in a fold/map).  Caching them at NAMESPACE
/// scope makes each read a plain global load; a function-local `static const`
/// instead carries a guard-variable check (acquire-load + branch) on every
/// call.  Behaviour is identical (same env var, same enabled/disabled).
/// Retirement: fold into one V3_DBG_* dispatch flag if the trace surface
/// grows.  (lint-no-inline-getenv: `static const` keyword present.)
static const bool g_dbgForceInsideX =
    std::getenv("V3_DBG_FORCE_INSIDE_X") != nullptr;
static const bool g_dbgForceSite =
    std::getenv("V3_DBG_FORCE_SITE") != nullptr;
// T2: the periodic-L(t) trace gate sits at the GC safepoint reached on the
// dispatch hot path; `periodicLiveTraceEnabled()` is a cross-TU call (not
// inlinable into vm.cc) wrapping a function-local-static getenv.  Cache the
// answer once at load so the per-safepoint check is a local bool-load, not a
// `bl` + guard.  Identical semantics (the env var is read once either way).
static const bool g_periodicLiveTrace = periodicLiveTraceEnabled();

[[gnu::cold]]
inline void dbgLogForceInsideX(VMState & vm, const Value * forcing)
{
    if (__builtin_expect(!g_dbgForceInsideX, 1)) return;
    static thread_local int s_logged = 0;
    if (s_logged >= 2000) return;
    bool insideX = false;
    for (const auto & f : vm.frames) {
        if (!(f.flags & CFF_THUNK_RETURN)) continue;
        if (!f.thunk) continue;
        if (f.thunk->state != ThunkState::Blackhole) continue;
        const auto * d = f.thunk->suspended.desc;
        if (d && d->name == "x") { insideX = true; break; }
    }
    if (!insideX) return;
    // Filter: only log Thunk-shaped values (where the force actually
    // does work).  WHNF values (Int/Bool/Attrs/etc.) are no-ops and
    // would flood the log.  We DO want Tag::Slot since that's how
    // captured rec / lambda-param refs reach us.
    if (!forcing) return;
    Value chased = *forcing;
    if (chased.tag() == Tag::Slot && chased.asSlot())
        chased = *chased.asSlot();
    if (chased.tag() != Tag::Thunk && !chased.isAppLike())
        return;
    // Filter: only log Suspended thunks (the FIRST force that flips
    // state to Blackhole).  Already-Evaluated thunks are harmless
    // and just flood the log.  Bridge/Blackhole are also informative.
    if (chased.tag() == Tag::Thunk && chased.asThunk()
        && chased.asThunk()->state == ThunkState::Evaluated)
        return;
    // Identify the forcing site: innermost frame's name + ip.
    const char * outerName = "?";
    uint32_t outerCodeOff = 0;
    uint32_t outerIp = 0;
    if (!vm.frames.empty()) {
        const auto & f = vm.frames.back();
        const LambdaDescriptor * d = nullptr;
        if (f.closure) d = f.closure->desc;
        else if (f.thunk) d = f.thunk->suspended.desc;
        if (d && !d->name.empty()) {
            outerName = d->name.c_str();
            outerCodeOff = d->codeOffset;
        }
        outerIp = f.ip;
    }
    // Identify forcee (the thunk we're about to force).
    const char * forcedName = "?";
    uint32_t forcedCodeOff = 0;
    void * forcedThunk = nullptr;
    int forcedState = -1;
    if (chased.tag() == Tag::Thunk && chased.asThunk()) {
        forcedThunk = (void *)chased.asThunk();
        forcedState = (int)chased.asThunk()->state;
        if (chased.asThunk()->state == ThunkState::Suspended
            && chased.asThunk()->suspended.desc) {
            const auto * d = chased.asThunk()->suspended.desc;
            if (!d->name.empty()) forcedName = d->name.c_str();
            forcedCodeOff = d->codeOffset;
        }
    }
    std::fprintf(stderr,
        "FORCE-IN-X[%d] outer=%s@codeOff=%u ip=%u forced=%s thunk=%p codeOff=%u state=%d frames=%zu\n",
        s_logged++,
        outerName, (unsigned)outerCodeOff, (unsigned)outerIp,
        forcedName, forcedThunk, (unsigned)forcedCodeOff,
        forcedState, vm.frames.size());
}

[[gnu::cold]]
inline void dbgLogForceSite(const CompilationUnit * cu, uint32_t instrIp,
                            const Value * forcing = nullptr)
{
    if (__builtin_expect(!g_dbgForceSite, 1)) return;   // T2: hoisted gate
    if (!cu) return;
    const auto & tbl = cu->forceEmitSites;
    // lower_bound finds the first entry with offset >= instrIp; since
    // entries are unique per offset the equality case is what we want.
    auto it = std::lower_bound(
        tbl.begin(), tbl.end(), instrIp,
        [](const std::pair<uint32_t, const char *> & e, uint32_t v) {
            return e.first < v;
        });
    const char * site = (it != tbl.end() && it->first == instrIp)
        ? it->second
        : "<unknown>";
    // Option-1 enrichment: log the thunk pointer + creation codeOffset
    // when forcing a Thunk-shape value, so post-processing can trace
    // back which Nix expression's thunk is being forced.  pkgs.X
    // thunks created via `inherit (rec {...}) X` have stable codeOffsets
    // identifiable by name in the disasm.
    //
    // Also chase one level through Tag::Slot to surface the underlying
    // thunk pointer — useful because OP_GET_LOCAL_FORCE on a let-rec
    // slot reads through Tag::Slot first.
    const Value * v = forcing;
    Value chased{};
    if (v && v->tag() == Tag::Slot && v->asSlot()) {
        chased = *v->asSlot();
        v = &chased;
    }
    if (v && v->tag() == Tag::Thunk && v->asThunk()) {
        const Thunk * t = v->asThunk();
        uint32_t codeOff = 0;
        const char * tname = "?";
        const void * thunkCu = nullptr;
        if (t->state == ThunkState::Suspended && t->suspended.desc) {
            auto * d = t->suspended.desc;
            codeOff = d->codeOffset;
            if (!d->name.empty()) tname = d->name.c_str();
            thunkCu = (const void *)d->cu;  // FP-2a: was t->suspended.cu
        }
        std::fprintf(stderr,
            "OP_FORCE@ip=%u site=%s thunk=%p name=%s codeOff=%u state=%d cu=%p caller_cu=%p\n",
            (unsigned)instrIp, site, (const void *)t, tname,
            (unsigned)codeOff, (int)t->state, thunkCu, (const void *)cu);
        return;
    }
    std::fprintf(stderr, "OP_FORCE@ip=%u site=%s\n",
                 (unsigned)instrIp, site);
}

[[gnu::always_inline]]
inline void push(VMState & vm, Value v)
{
    vm.valueStack.push_back(v);
}

/// Equality with WHNF forcing — handles lazy list/attr entries.
/// Recurses on List / Attrs after forcing each element.
///
/// `insideContainer` is true when called recursively from list/attr
/// comparison: in that case Nix's "value identity optimization" allows
/// two closures to compare equal if they share the same underlying
/// Closure pointer (matches tree-walker's `if (&v1 == &v2) return true`
/// short-circuit when sibling list/attr entries point to the same
/// in-memory Value).  Top-level `f == f` always returns false because
/// the OP_EQ stack-pop holds two distinct Value structs even when their
/// payload pointer is identical.
inline bool valueEqual(VMState & vm, Value a0, Value b0, bool insideContainer0 = false)
{
    // A12b (2026-05-22): iterative valueEqual via explicit work stack.
    // Pre-fix, this function C-recursed at every nested list/attrset
    // level (`valueEqual(vm, ae, be, true)` in the List and Attrs
    // cases).  Deeply nested comparisons — e.g. cardano-node stdenv
    // assertion chains, nested mkOption defaults — could blow the
    // kMaxCallDepth=5000 guard or worse, overflow the C-stack outright.
    // (Per RCA_FAMILY_DIVERGENCE_A7 + ROADMAP_PROGRESS_SNAPSHOT §6:
    // "A12b depth=5000 — helper-level direct forceValue calls
    // C-recurse to kMaxCallDepth".)
    //
    // The iterative shape uses a heap-allocated work stack of (a, b,
    // insideContainer) triples.  Each pop processes ONE pair; nested
    // container element pairs are pushed onto the stack instead of
    // recursing.  Children are pushed in REVERSE order so left-to-
    // right processing happens via LIFO pop.  Short-circuit on
    // inequality returns immediately.
    //
    // Why not std::stack: small_vector-equivalent with explicit
    // reserve(16) keeps allocation off the hot path for typical
    // comparison shapes (single int / string / small list).
    //
    // Semantics preserved: writeback-force on container elements
    // (#A12); derivation outPath short-circuit (eval-okay-eq-
    // derivations); pointer-identity short-circuit on same list/
    // attrset; closure pointer-equality inside containers.
    // C-16 (CODEBASE_REVIEW_2026-06-11): aSlot/bSlot are the source
    // list/attrs slots for container ELEMENTS (null for the top-level pair and
    // for looked-up values).  We push element pairs WITHOUT forcing them and
    // force at POP time, writing the WHNF back through the slot.  This keeps
    // A12's memoization (resolved Tag::App entries persist in the source — the
    // 583 cache tests rely on this) AND short-circuits: on an early mismatch we
    // return immediately, so later elements that are still on the stack are
    // never popped/forced (`[1 (throw)] == [2 3]` is `false` in TW, not a
    // throw).  GC-safety: the major GC is non-moving (evacuation default-off),
    // and the source containers stay reachable via a0/b0 for the whole loop, so
    // these raw slot pointers remain valid across the forceValue calls below.
    struct Task { Value a, b; bool insideContainer; Value * aSlot; Value * bSlot; };
    std::vector<Task> stack;
    stack.reserve(16);
    stack.push_back({a0, b0, insideContainer0, nullptr, nullptr});

    static const SymbolId tyId = ir::globalInternSymbol("type");
    static const SymbolId opId = ir::globalInternSymbol("outPath");

    while (!stack.empty()) {
        Task t = stack.back();
        stack.pop_back();
        Value a = t.a, b = t.b;
        const bool insideContainer = t.insideContainer;

        // #558 Phase 2: inline WHNF check.  valueEqual is called from
        // primop bodies (primElem, primAll, etc.) and primConcatMap;
        // when both sides are already WHNF (common after a previous
        // force), skip the forceValue function call.
        {
            Tag at = a.tag();
            if (__builtin_expect(at == Tag::Thunk
                                 || at == Tag::App || at == Tag::App3
                                 || at == Tag::Slot, 0)) {
                a = forceValue(vm, a);
                // PhD-6: barrier this writeback. t.aSlot points into the compared
                // container (e.g. &Bindings.entries[i].value, tenured); the forced
                // `a` can be a nursery Closure/ListVec, so a raw `*t.aSlot = a`
                // creates an unbarriered tenured→nursery edge the scavenger misses
                // (the confirmed class-3 missed root).  cellWrite registers the
                // standalone cell under the nursery; no-op-cost under major-GC.
                if (t.aSlot) cellWrite(t.aSlot, a, nullptr);  // was *t.aSlot = a
            }
            Tag bt = b.tag();
            if (__builtin_expect(bt == Tag::Thunk
                                 || bt == Tag::App || bt == Tag::App3
                                 || bt == Tag::Slot, 0)) {
                b = forceValue(vm, b);
                if (t.bSlot) cellWrite(t.bSlot, b, nullptr);  // PhD-6: was *t.bSlot = b (see aSlot)
            }
        }
        if (a.tag() != b.tag()) {
            if (a.isInt() && b.isFloat()) {
                if (static_cast<double>(a.asInt()) != b.asFloat()) return false;
                continue;
            }
            if (a.isFloat() && b.isInt()) {
                if (a.asFloat() != static_cast<double>(b.asInt())) return false;
                continue;
            }
            return false;
        }
        switch (a.tag()) {
        case Tag::Int:
            if (a.asInt() != b.asInt()) return false;
            break;
        case Tag::Float:
            if (a.asFloat() != b.asFloat()) return false;
            break;
        case Tag::Bool:
            if (a.asInt() != b.asInt()) return false;
            break;
        case Tag::Null:
            break;
        case Tag::String:
            if (std::string_view(a.asString()) != std::string_view(b.asString()))
                return false;
            break;
        case Tag::Path:
            if (std::string_view(a.asPath()) != std::string_view(b.asPath()))
                return false;
            break;
        case Tag::List: {
            auto * la = a.asList();
            auto * lb = b.asList();
            if (la == lb) break;
            uint32_t na = la ? la->size : 0;
            uint32_t nb = lb ? lb->size : 0;
            if (na != nb) return false;
            // A12 (2026-05-17) writeback-force on each element: mirrors
            // the primElem fix.  Forces list slots through the lvalue so
            // resolved WHNFs persist in the source list, matching
            // tree-walker's pointer-sharing semantics.  Without this,
            // nested list comparisons re-evaluate Tag::App entries on
            // every outer pass.  Test: test/repro-583-valueEqual-list.nix.
            //
            // A12b: push pairs in REVERSE so index [0] sits on top
            // of the stack and is processed first (left-to-right
            // semantics + short-circuit on first mismatch).
            // C-16: push element pairs UNFORCED with their source slots; the
            // pop-time force (above) resolves + writes back, and an earlier
            // mismatch short-circuits before later elements are ever popped.
            for (uint32_t i = na; i > 0; --i) {
                uint32_t idx = i - 1;
                stack.push_back({la->elems[idx], lb->elems[idx],
                                 /*insideContainer=*/true,
                                 &la->elems[idx], &lb->elems[idx]});
            }
            break;
        }
        case Tag::Attrs: {
            auto * aa = a.asAttrs();
            auto * bb = b.asAttrs();
            if (aa == bb) break;
            // Lever A: this case compares by index (`aa->entries[i]` vs
            // `bb->entries[i]` over `aa->size`).  A Chain's entries[] is
            // the overlay only, so an unmateralised chain compares as a
            // partial attrset → `chain == its-own-materialisation` wrongly
            // returns false, breaking lib.unique / elem / dedup / override
            // equality across nixpkgs.  Materialise both to the full sorted
            // view first (no-op for Sorted; memoised).
            if (aa && aa->isChain()) aa = const_cast<Bindings *>(aa->materialize());
            if (bb && bb->isChain()) bb = const_cast<Bindings *>(bb->materialize());
            // Special-case derivations: if both attrsets are derivations
            // (have `type = "derivation"`), compare their `outPath` fields
            // and ignore the rest.  Matches tree-walker semantics — required
            // by `eval-okay-eq-derivations` (where `drv // { dummy = 1; }`
            // still compares equal to the bare `drv`).
            auto isDrv = [&](const Bindings * binds) {
                if (!binds) return false;
                const Value * tv = binds->lookup(tyId);
                if (!tv) return false;
                Value tf = forceValue(vm, *tv);
                return tf.isString() && std::string_view(tf.asString()) == "derivation";
            };
            if (isDrv(aa) && isDrv(bb)) {
                const Value * pa = aa->lookup(opId);
                const Value * pb = bb->lookup(opId);
                if (pa && pb) {
                    stack.push_back({*pa, *pb, /*insideContainer=*/true,
                                     nullptr, nullptr});
                    break;
                }
            }
            uint32_t na = aa ? aa->size : 0;
            uint32_t nb = bb ? bb->size : 0;
            if (na != nb) return false;
            // A12 (2026-05-17) writeback-force on each entry value: see
            // List case above.  Attrs entries built by primMapAttrs or
            // lower.cc's lazy-binding lowering are Tag::App; without this,
            // every valueEqual on the same attrset re-evaluates them.
            //
            // A12b: name-check inline (cheap), then push value-pair
            // tasks in REVERSE for left-to-right processing.
            for (uint32_t i = 0; i < na; ++i)
                if (aa->entries[i].name != bb->entries[i].name) return false;
            // C-16: push entry-value pairs UNFORCED with their source slots
            // (pop-time force + writeback + short-circuit; see the List case).
            for (uint32_t i = na; i > 0; --i) {
                uint32_t idx = i - 1;
                stack.push_back({aa->entries[idx].value, bb->entries[idx].value,
                                 /*insideContainer=*/true,
                                 &aa->entries[idx].value, &bb->entries[idx].value});
            }
            break;
        }
        case Tag::Closure:
        case Tag::PrimOp:
        case Tag::PrimOpApp:
        // C-16 (CODEBASE_REVIEW_2026-06-11): a Tag::App/App3 reaching here has
        // already been through the pop-time force above, so it is an
        // under-applied closure-PAP (forceValue returns a PAP unchanged) — a
        // FUNCTION value.  Compare it like a Closure (never equal by direct ==;
        // pointer-identity inside a container) instead of falling to the
        // default raw-pointer arm, which compared PAPs even outside a container.
        case Tag::App:
        case Tag::App3:
            // Direct comparison: never equal.  Inside a container: equal iff
            // the underlying pointer matches (matches Nix's value-identity
            // optimization for sibling list/attrset entries).
            if (!insideContainer) return false;
            if (a.asRaw() != b.asRaw()) return false;
            break;
        case Tag::Uninitialized:
        case Tag::Thunk:
        case Tag::Blackhole:
        case Tag::External:
        case Tag::Slot:
        default:
            if (a.asRaw() != b.asRaw()) return false;
            break;
        }
    }
    return true;
}

// Forward declaration — defined later in the file.
inline std::string valueRepr(const Value & v, int depth = 0);

inline bool valueLess(VMState & vm, const Value & a, const Value & b)
{
    if (a.isInt() && b.isInt())     return a.asInt() < b.asInt();
    if (a.isFloat() && b.isFloat()) return a.asFloat() < b.asFloat();
    if (a.isInt() && b.isFloat())   return static_cast<double>(a.asInt()) < b.asFloat();
    if (a.isFloat() && b.isInt())   return a.asFloat() < static_cast<double>(b.asInt());
    if (a.isString() && b.isString())
        return std::string_view(a.asString()) < std::string_view(b.asString());
    // C-23 (CODEBASE_REVIEW_2026-06-11): TW's CompareValues compares two paths
    // lexically by their absolute path string; v3 rejected paths and fell to
    // the "cannot compare" error below.
    if (a.isPath() && b.isPath())
        return std::string_view(a.asPath()) < std::string_view(b.asPath());
    if (a.isList() && b.isList()) {
        // Lexicographic compare; matches tree-walker.  Phase-13
        // review HIGH-3 fix: force lazy elements before recursing.
        // After WC-35, mapAttrs/map install Tag::App entries; without
        // forcing, comparing `[(map id [1]) ...]` would throw the
        // "unsupported operand types" branch even for valid lists.
        // A12 (2026-05-17): force THROUGH the lvalue (writeback) so
        // the resolved WHNF persists in the source list; mirrors the
        // primElem fix.
        uint32_t na = a.asList() ? a.asList()->size : 0;
        uint32_t nb = b.asList() ? b.asList()->size : 0;
        uint32_t n = std::min(na, nb);
        for (uint32_t i = 0; i < n; ++i) {
            Value & ai = a.asList()->elems[i];
            Value & bi = b.asList()->elems[i];
            if (ai.tag() == Tag::Thunk || ai.isAppLike()
                || ai.tag() == Tag::Slot)
                ai = forceValue(vm, ai);
            if (bi.tag() == Tag::Thunk || bi.isAppLike()
                || bi.tag() == Tag::Slot)
                bi = forceValue(vm, bi);
            if (valueLess(vm, ai, bi)) return true;
            if (valueLess(vm, bi, ai)) return false;
        }
        return na < nb;
    }
    // #680 — TW phrasing (libexpr/eval.cc): for same type that's
    // incomparable, "cannot compare a <type> with a <type>; values
    // of that type are incomparable".  For mixed types, "cannot
    // compare a <t1> with a <t2>".  v3 emits the simpler core text
    // (without the "values are ..." suffix); reconstructing exact
    // ValuePrinter output is out of scope for this opcode helper.
    auto typeWord = [](const Value & v) -> std::pair<const char *, const char *> {
        Tag t = v.tag();
        if (t == Tag::Int)    return {"an", "integer"};
        if (t == Tag::Float)  return {"a",  "float"};
        if (t == Tag::Bool)   return {"a",  "Boolean"};
        if (t == Tag::Null)   return {"",   "null"};
        if (t == Tag::String) return {"a",  "string"};
        if (t == Tag::Path)   return {"a",  "path"};
        if (t == Tag::List)   return {"a",  "list"};
        if (t == Tag::Attrs)  return {"a",  "set"};
        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
            return {"a", "function"};
        return {"a", "value"};
    };
    auto withArticle = [&](const Value & v) -> std::string {
        auto [art, name] = typeWord(v);
        std::string r;
        if (*art) { r += art; r += ' '; }
        r += name;
        return r;
    };
    std::string msg = "cannot compare " + withArticle(a) + " with " + withArticle(b);
    if (a.tag() == b.tag())
        msg += "; values of that type are incomparable";
    // #691 — append `(values are X and Y)` / `; values are X and Y`
    // matching TW.  Closes the prior PREFIX gap (`compare-null` etc.
    // tests previously accepted the truncated form).
    msg += a.tag() == b.tag()
        ? " (values are "
        : "; values are ";
    msg += valueRepr(a);
    msg += " and ";
    msg += valueRepr(b);
    if (a.tag() == b.tag()) msg += ")";
    throw std::runtime_error(msg);
}

inline bool isTrueValue(const Value & v)
{
    // 2026-05-18: defensively chase Tag::Slot.  The OP_NOT/OP_ASSERT
    // call sites WHNF-check before invoking us, but their check happens
    // BEFORE the iterative-force retry — when the retry path writes a
    // Slot back via CFF_FORCE_WB_PTR_KEEP (e.g. OP_ATTRS_SELECT_IC's
    // mapAttrs writeback) the next OP_NOT execution can see the Slot
    // again.  Dereferencing here is cheap (1-hop max in practice) and
    // mirrors the chase op_force_slow already does.
    const Value * cur = &v;
    int hops = 0;
    while (cur->tag() == Tag::Slot && cur->asSlot() && hops < 32) {
        cur = cur->asSlot();
        ++hops;
    }
    if (!cur->isBool()) {
        // Diagnostic: when V3_DBG_EXPECTED_BOOL=1, dump tag + C-stack
        // backtrace so we can locate the offending OP_NOT/OP_ASSERT.
        static const bool s_dbg =
            std::getenv("V3_DBG_EXPECTED_BOOL") != nullptr;
        if (__builtin_expect(s_dbg, 0)) {
            std::fprintf(stderr,
                "v3 isTrueValue: not bool — tag=%d (after %d Slot hops)\n",
                (int)cur->tag(), hops);
            void * cstack[32];
            int n = ::backtrace(cstack, 32);
            char ** syms = ::backtrace_symbols(cstack, n);
            for (int i = 0; i < n && i < 12; ++i)
                std::fprintf(stderr, "  %s\n", syms[i]);
            if (syms) std::free(syms);
        }
        throw std::runtime_error("v3: expected bool");
    }
    return cur->asInt() == 1;
}

/// Coerce a Value to its string representation for OP_STR_CONCAT.
/// Bring-up subset: int / float / bool / string / path / null.  Lists,
/// attrsets, and lambdas trigger an error here for now (the AST → IR pass
/// is responsible for inserting `toString` primop calls where needed).
///
/// In interpolation context (`forceString = true`) we route Path values
/// through tree-walker's `copyPathToStore` (DryRun under
/// settings.readOnlyMode = true) so `${./foo}` produces the proper
/// `/nix/store/<32-hash>-name` representation, not the absolute file
/// path.  Required by tests like `eval-okay-context` that count on the
/// store-path prefix length.
/// #680 — coerce a value to a string for the `+` operator and `${...}`
/// interpolation.  Mirrors TW's `coerceToString(coerceMore=false)`
/// (libexpr/eval.cc:2874): String + Path → string (paths optionally
/// copied to store when forceString=true); EVERYTHING else rejects
/// with `cannot coerce <type> to a string: <value>` matching TW's
/// libexpr/eval.cc:2911 phrasing.
///
/// Pre-fix v3 silently accepted Int/Float/Bool/Null: `"x" + 1`
/// returned `"x1"`, `null + 1` returned `"1"`, `"${1}"` returned `"1"`.
/// All of these are TW errors.  The relaxed behavior could hide bugs
/// in nixpkgs / user code where a value of the wrong type leaks into
/// string-context.
///
/// Attrset coercion (`__toString` / `outPath`) is handled BEFORE this
/// helper by the OP_STR_CONCAT attr-unwind loop (vm.cc:8051+), so by
/// the time we get here the value is a primitive.

/// #691 — TW `ValuePrinter` mirror for error-message value rendering.
/// Matches TW's `errorPrintOptions` defaults: maxDepth=10, maxAttrs=10,
/// maxListItems=10, force=false (i.e., don't force lazy values — they
/// might be the source of the very error we're rendering).
///
/// This is the helper that closes the PREFIX-class gap in many v3
/// error messages where v3 currently emits truncated placeholders
/// like `{ ... }` / `[ ... ]` while TW shows the actual values.
/// Used by `coerceToString`, `valueLess`, and the formals-validation
/// error paths in OP_CALL / OP_TAIL_CALL.
///
/// IMPORTANT: this function does NOT force lazy values.  Tag::Thunk /
/// Tag::App / Tag::Slot render as `«…»` placeholders.  Forcing during
/// error rendering risks recursing into the very error we're trying
/// to report — TW's `force=false` discipline is correctness-load-
/// bearing here.
inline std::string valueRepr(const Value & v, int depth)
{
    constexpr int kMaxDepth = 10;
    constexpr int kMaxItems = 10;
    constexpr size_t kMaxStrLen = 1024;
    Tag t = v.tag();
    if (depth > kMaxDepth) return "«…»";
    if (t == Tag::Int) {
        char buf[24];
        std::snprintf(buf, sizeof buf, "%lld", (long long)v.asInt());
        return buf;
    }
    if (t == Tag::Float) {
        // Match TW's `output << double` — default ostream formatting
        // (not %f's fixed 6-decimal).
        std::ostringstream os; os << v.asFloat();
        return os.str();
    }
    if (t == Tag::Bool)   return v.asInt() == 1 ? "true" : "false";
    if (t == Tag::Null)   return "null";
    if (t == Tag::String) {
        std::string out = "\"";
        std::string_view sv = v.asString() ? std::string_view(v.asString())
                                            : std::string_view{};
        size_t n = sv.size();
        size_t lim = n > kMaxStrLen ? kMaxStrLen : n;
        for (size_t i = 0; i < lim; ++i) {
            char c = sv[i];
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '$':
                    if (i + 1 < lim && sv[i + 1] == '{') {
                        out += "\\$";
                    } else {
                        out += c;
                    }
                    break;
                default: out += c;
            }
        }
        if (n > kMaxStrLen) out += "«…elided…»";
        out += "\"";
        return out;
    }
    if (t == Tag::Path)   return v.asPath() ? v.asPath() : "/";
    if (t == Tag::List) {
        if (!v.asList() || v.asList()->size == 0) return "[ ]";
        std::string out = "[ ";
        uint32_t n = v.asList()->size;
        uint32_t lim = n > kMaxItems ? kMaxItems : n;
        for (uint32_t i = 0; i < lim; ++i) {
            out += valueRepr(v.asList()->elems[i], depth + 1);
            out += ' ';
        }
        if (n > kMaxItems) {
            out += "«…";
            out += std::to_string(n - kMaxItems);
            out += " items elided…» ";
        }
        out += ']';
        return out;
    }
    if (t == Tag::Attrs) {
        if (!v.asAttrs() || v.asAttrs()->size == 0) return "{ }";
        std::string out = "{ ";
        auto * b = v.asAttrs();
        const auto & symTab = ir::globalSymbolTable();
        uint32_t n = b->size;
        uint32_t lim = n > kMaxItems ? kMaxItems : n;
        for (uint32_t i = 0; i < lim; ++i) {
            uint32_t nameIdx = b->entries[i].name;
            std::string nm = (nameIdx < symTab.size())
                ? symTab[nameIdx]
                : std::string("<sym?>");
            out += nm;
            out += " = ";
            out += valueRepr(b->entries[i].value, depth + 1);
            out += "; ";
        }
        if (n > kMaxItems) {
            out += "«…";
            out += std::to_string(n - kMaxItems);
            out += " attrs elided…» ";
        }
        out += '}';
        return out;
    }
    if (t == Tag::Closure)   return "«lambda»";
    if (t == Tag::PrimOp)    return "«primop»";
    if (t == Tag::PrimOpApp) return "«partially applied primop»";
    if (t == Tag::Thunk)     return "«unforced thunk»";
    if (t == Tag::App)       return "«unforced app»";
    if (t == Tag::App3)      return "«unforced app3»";
    if (t == Tag::Blackhole) return "«potential infinite recursion»";
    if (t == Tag::Slot)      return "«slot»";
    return "«value»";
}

/// #685 — opcode-side mirror of TW's `forceStringNoCtx`
/// (libexpr/eval.cc:2826).  Throws TW's exact error text when the
/// string value carries any context.  Used for dynamic attr names
/// (`{ ${ctxedStr} = v; }` / `.${ctxedStr}` / `?${ctxedStr}`) which
/// TW gates via forceStringNoCtx in evalDynamicAttrs.  The
/// primops.cc version of this helper takes an EvalState; this one
/// reaches the store via the global `getNixEvalState()` so it can
/// be called from inside opcode dispatch where no `state` is in
/// scope.  Empty-context strings short-circuit cheaply.
inline void requireNoStringContextRuntime(const Value & v,
                                          std::string_view siteHint)
{
    if (!v.isString() || !v.asString()) return;
    auto * raw = lookupStringContextEntries(v.asString());
    if (!raw || raw->empty()) return;
    std::string display = raw->front();
    if (auto * ns = getNixEvalState())
        display = ffi::displayContextElem(*ns, raw->front());
    // V3_DBG_NOCTX_SITE: see matching site in primops.cc.  Cold path.
    static const bool s_dbgNoCtxSite =
        std::getenv("V3_DBG_NOCTX_SITE") != nullptr;
    if (__builtin_expect(s_dbgNoCtxSite, 0))
        std::fprintf(stderr, "v3 NOCTX-SITE: requireNoStringContextRuntime hint=%.*s\n",
                     (int)siteHint.size(), siteHint.data());
    throw std::runtime_error(
        std::string("the string '") + v.asString()
        + "' is not allowed to refer to a store path (such as '"
        + display + "')");
}

inline std::string coerceToString(const Value & vIn, bool forceString)
{
    // C-4b / «slot» residual (CODEBASE_REVIEW_2026-06-11): Tag::Slot is a
    // v3-only TRANSPARENT pointer into a tenured cell — the actual value is
    // `*slot` (tree-walker has no Slot, so every value-expecting leaf must
    // deref it; the VM chases Slots at ~15 other sites).  coerceToString did
    // NOT: a Slot operand fell straight through to the "cannot coerce a value
    // to a string: «slot»" error.  That is exactly the ghc98 .drvPath residual
    // (a Slot reached `${...}` / `+` coercion un-deref'd).  Chase the Slot
    // chain (bounded by kMaxIndirectionChase, mirroring the other deref loops)
    // to the underlying value before coercing.  If the resolved value is still
    // non-coercible, the error below now reflects that REAL type, not «slot».
    Value vDeref = vIn;
    size_t slotGuard = 0;
    while (vDeref.tag() == Tag::Slot && vDeref.asSlot()
           && ++slotGuard < kMaxIndirectionChase)
        vDeref = *vDeref.asSlot();
    const Value & v = vDeref;
    // Use if/else rather than switch to avoid -Wswitch-enum on every
    // tag we don't care to spell out individually.
    auto typeName = [](const Value & v) -> std::pair<const char *, const char *> {
        Tag t = v.tag();
        if (t == Tag::Int)   return {"an", "integer"};
        if (t == Tag::Float) return {"a",  "float"};
        if (t == Tag::Bool)  return {"a",  "Boolean"};
        if (t == Tag::Null)  return {"",   "null"};
        if (t == Tag::List)  return {"a",  "list"};
        if (t == Tag::Attrs) return {"a",  "set"};
        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
            return {"a", "function"};
        if (t == Tag::Thunk || t == Tag::App || t == Tag::App3)
            return {"a", "thunk"};
        return {"a", "value"};
    };
    auto throwCoerceError = [&](const Value & v) -> std::string {
        // #691 — use the shared `valueRepr` for TW-equivalent rendering.
        // Pre-fix this lambda used a stub that emitted `{ ... }` / `[ ... ]`
        // placeholders, leaving a PREFIX-class gap vs TW's actual values.
        auto [article, name] = typeName(v);
        std::string msg = "cannot coerce ";
        if (*article) { msg += article; msg += ' '; }
        msg += name;
        msg += " to a string: ";
        msg += valueRepr(v);
        throw std::runtime_error(msg);
    };

    switch (v.tag()) {
    case Tag::String: return std::string(v.asString());
    case Tag::Path: {
        std::string p(v.asPath() ? v.asPath() : "");
        if (forceString) {
            if (auto * ns = getNixEvalState()) {
                // Let copyPathToStore exceptions propagate — tree-walker
                // raises on missing paths during interpolation, and v3
                // should match.  Note for the caller: this string carries
                // an Opaque context entry for `storePath`; the caller is
                // responsible for recording it (see OP_STR_CONCAT below).
                return ffi::coercePathToStore(*ns, p);
            }
        }
        return p;
    }
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::List:
    case Tag::Attrs:
    case Tag::Closure:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::Thunk:
    case Tag::App:
    case Tag::App3:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Slot:
    case Tag::Uninitialized:
    default:
        return throwCoerceError(v);
    }
}

/// #821 site IDs for per-caller mergeBindings attribution.  Exhaustive
/// list of in-VM call sites is documented above the
/// `mergeBindingsCallsBySite[]` field in alloc.hh; each integer below
/// corresponds to the matching ID slot in that array.  Site 8 is
/// reserved for primIntersectAttrs's two-pass merge in primops.cc; the
/// remaining 7 slots in `kMergeBindingsSiteSlots=16` are spare for
/// future call sites (no need to renumber when adding one).
enum class MergeBindingsSite : uint8_t {
    AttrsUpdate          = 0,
    AttrsUpdateTail      = 1,
    ExtendsCallPrev      = 2,
    ExtendsCallPrevPrime = 3,
    ComposeCallApplied   = 4,
    ExtendsTailPrev      = 5,
    ExtendsTailPrevPrime = 6,
    ComposeTailApplied   = 7,
    // 8+ reserved (primops.cc, future sites)
};

inline Bindings * mergeBindings(const Bindings * a, const Bindings * b,
                                MergeBindingsSite siteId =
                                    MergeBindingsSite::AttrsUpdate)
{
    // #821 per-site call counter — bumped at function entry so the
    // empty-operand short-circuit (below) contributes to the call
    // count even though it doesn't allocate; the BYTES counter is
    // updated only after `Alloc::allocBindings` so its sum matches
    // `bytesBindings` for the merge-attributed slice.
    {
        const uint8_t s = static_cast<uint8_t>(siteId);
        if (s < AllocStats::kMergeBindingsSiteSlots)
            ++allocStats().mergeBindingsCallsBySite[s];
    }

    // #821 — input-size histograms (na, nb).  Only sampled on
    // site 1 (OP_ATTRS_UPDATE_TAIL) since that's the 98 %-dominant
    // caller on HNE and we want to know whether `b` (the overlay)
    // is small enough to justify a ChainBindings/persistent-overlay
    // representation.  Other sites can be added later if data shows
    // they shift the workload.  Bucket via a small switch (5 cmps
    // avg) — negligible cost compared to the merge itself.
    if (siteId == MergeBindingsSite::AttrsUpdateTail) {
        // #821 follow-on: split the 0..1 bucket into nb=0 (short-
        // circuit) vs nb=1 (single-key patch).  The two have very
        // different optimisation implications: nb=0 is already free
        // (just return parent), while nb=1 is the canonical "single-
        // attr override" pattern where a smart patched representation
        // could amortise parent copy.
        auto bucket_of = [](uint32_t n) -> uint8_t {
            if (n == 0)   return 0;   // nb=0 — short-circuit
            if (n == 1)   return 1;   // nb=1 — single-key patch
            if (n <= 4)   return 2;
            if (n <= 8)   return 3;
            if (n <= 16)  return 4;
            if (n <= 32)  return 5;
            if (n <= 64)  return 6;
            if (n <= 128) return 7;
            if (n <= 256) return 8;
            return 9;
        };
        ++allocStats().mergeBindingsNaHist[bucket_of(a ? a->size : 0)];
        ++allocStats().mergeBindingsNbHist[bucket_of(b ? b->size : 0)];
    }

    // Chain knobs — hoisted so both the composition path (just below)
    // and the construction path (further down) share them.  Function-
    // local statics: each initialises once on first call.
    //   NIX_V3_CHAIN_MIN_NA=16 — parent must be "large" to chain-construct.
    //   NIX_V3_CHAIN_MAX_NB=4  — overlay must be "small" (override-delta).
    // The audit can run with AGGRESSIVE chains (MIN_NA=2 MAX_NB=999) to
    // exercise every attrset op against the TW oracle without a rebuild.
    //
    // Lever A default-ON (2026-06-07, MEMORY_REPRESENTATION §10): chains are
    // a measured pure-refactor of `//` (firefox.drvPath −268 MB; 0 real
    // drv-hash divergences across core 19/19 chains-on + 53 packages incl.
    // git/cargo/rustc/withPackages + the pure parity test).  Safety valve:
    // NIX_V3_NO_CHAIN_BINDINGS=1 (or NIX_V3_CHAIN_BINDINGS=0) opts out with
    // NO rebuild — a reversible rollout.  RETIREMENT: drop the valve once the
    // full nixpkgs drvPath CI sweep (bench/chain-nixpkgs-fullsweep.sh) has run
    // several cycles byte-clean.
    static const bool s_chain = []{
        if (std::getenv("NIX_V3_NO_CHAIN_BINDINGS")) return false;
        const char * e = std::getenv("NIX_V3_CHAIN_BINDINGS");
        if (e) return e[0] != '0';   // explicit force on/off (=0 → off)
        return true;                 // default ON
    }();
    static const uint32_t s_minNa = []{
        const char * e = std::getenv("NIX_V3_CHAIN_MIN_NA");
        return e ? (uint32_t) std::strtoul(e, nullptr, 10) : 16u;
    }();
    static const uint32_t s_maxNb = []{
        const char * e = std::getenv("NIX_V3_CHAIN_MAX_NB");
        return e ? (uint32_t) std::strtoul(e, nullptr, 10) : 4u;
    }();

    // Lever A Step 4 (MEMORY_REPRESENTATION §6) — CHAIN COMPOSITION.
    // Before materialising chain inputs, try to EXTEND an existing
    // chain by prepending `b` as a new highest-precedence overlay.
    // This is the firefox win: deep override stacks (`a // b // c // …`
    // from overrideAttrs / wrapFirefox / buildMozillaMach) then cost
    // O(depth) tiny overlays + ONE shared base, instead of re-copying
    // the whole base on every `//` (the prior unconditional
    // `a->materialize()` made a depth-D stack pay O(D) full copies).
    // Conditions: chains enabled; `a` is a chain with room under the
    // Cursor layer cap; `b` is a small Sorted overlay.  Depth is
    // bounded by Cursor::kMaxLayers so chain-aware lookup / cursor
    // iteration stay O(cap).  Beyond the cap we fall through to
    // materialise-and-merge (the Cursor also has a materialise safety
    // net, but capping here keeps every consumer cheap).
    if (s_chain && a->isChain() && !b->isChain()
        && b->size > 0 && b->size <= s_maxNb
        && a->chainDepth() < Bindings::Cursor::kMaxLayers) {
        Bindings * c = Alloc::allocChainBindings(a, b->size);
        for (uint32_t j = 0; j < b->size; ++j)
            bindingsSetEntry(c, j, b->entries[j]);  // overlay sorted; Phase D
        if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];  // WS-A detector
        return c;
    }

    // `a // {}` — return `a` UNCHANGED, preserving its chain form (do
    // not materialise just to drop an empty overlay).  Common in
    // nixpkgs via `a // lib.optionalAttrs cond {…}` when cond is false.
    // Guard `!b->isChain()` because a chain's `size` is its overlay
    // count, not its logical size (a chain is never empty by
    // construction, but the guard keeps the invariant explicit).
    if (!b->isChain() && b->size == 0) return const_cast<Bindings *>(a);

    // Not composing — materialise any Chain input so the two-pass
    // sorted-merge below (and the fresh-chain construction) see flat
    // `entries[]`.  `(big_chain) // {x}` only reaches here when the
    // depth cap is hit or `b` is itself a chain; the common deep-stack
    // case is handled by composition above.
    if (a->isChain()) a = a->materialize();
    if (b->isChain()) b = b->materialize();

    // Sorted-merge two attrsets (b wins on duplicate keys).  Per-attr
    // positions in attrPosTable are keyed by (Bindings*, SymbolId), so
    // when an entry is copied to the freshly-allocated `out`, we
    // forward its source position too.  Without this,
    // `builtins.unsafeGetAttrPos` on a merged attrset returns null
    // for every name (REVIEW critic §8 #4).
    //
    // #747 (2026-05-21) two-pass: pass 1 counts the exact number of
    // distinct keys; pass 2 allocates the precise size and fills.
    // The prior single-pass version called Alloc::allocBindings(na +
    // nb) up front and wrote `out->size = k` at the end, leaving the
    // arena pinned at the worst-case size even when duplicates were
    // collapsed.  #746 attribution measured ~387 MB of that slack on
    // hello.drvPath (~half of all v3-arena Bindings bytes).  The
    // bump-pointer arena cannot reclaim the unused tail, so the slack
    // becomes permanent until process exit.
    //
    // Pass 1 cost: one extra mirror-merge sweep reading only `name`
    // fields (4 B per Entry).  For na+nb in the thousands the array
    // stays L1-warm across both passes.  The arithmetic is identical
    // to the original branch logic so `kExact` and the final `k` from
    // pass 2 always agree by construction.
    const uint32_t na = a->size, nb = b->size;

    // #748 (2026-05-22) zero-operand short-circuit.  `a // {}` and
    // `{} // b` are common in nixpkgs (`pkgs // optionalAttrs cond
    // {...}` where cond is false; lib.optionalAttrs returns {}).
    // Both inputs are already WHNF and `mergeBindings` callers treat
    // the result as immutable, so returning the non-empty operand
    // directly is safe — no entries to merge, no allocation, no
    // per-attr position rebuilding.
    //
    // Why this is safe with #752 inline PosIdx: positions are stored
    // INSIDE Bindings::Entry rather than in a side-table keyed by
    // (Bindings*, SymbolId), so a shared Bindings pointer carries its
    // own positions inherently.  Pre-#752 this short-circuit was
    // unsafe because the side-table would have given the wrong
    // Bindings identity for position lookup.
    //
    // Empty-result `{} // {}` falls through to the (now trivial)
    // two-pass which produces a size-0 Bindings — small enough that
    // a special case here isn't worth its mental overhead.
    if (na == 0 && nb > 0) return const_cast<Bindings *>(b);
    if (nb == 0 && na > 0) return const_cast<Bindings *>(a);

    // MEMORY_ATTACK_PLAN re-test (2026-06-06): the prior 5 Chain Phase C
    // falsifications (ledger below) were all on the PRE-register-VM tree.  The
    // register-VM rework this session materially changed the call / formals /
    // arg-passing path — exactly the subsystem where the unidentified
    // `f origArgs -> {}` collapse lived (the formals destructure at vm.cc:~5350
    // now materialises Chains; the whole calling convention changed via R_CALL).
    // So "5×-falsified" is STALE; per the falsification rule this material change
    // to the failing subsystem is the new insight that justifies a re-test (NOT
    // a blind rebuild).  Gated NIX_V3_CHAIN_BINDINGS=1 (default OFF); conditions
    // mirror attempt #4/#5 (large parent, tiny overlay; a/b already materialised
    // above so neither is a Chain).  Build Chain{parent=a, overlay=b} instead of
    // copying a's na entries.
    // Fresh chain construction (a, b both Sorted here): `big // small`
    // builds Chain{parent=a, overlay=b} instead of copying a's na
    // entries.  Uses the hoisted s_chain / s_minNa / s_maxNb knobs.
    if (s_chain && nb <= s_maxNb && na >= s_minNa) {
        Bindings * c = Alloc::allocChainBindings(a, nb);
        for (uint32_t j = 0; j < nb; ++j)
            bindingsSetEntry(c, j, b->entries[j]);  // overlay sorted; Phase D
        if (__builtin_expect(g_sharedWbDetect, 0)) ++chainChildCount()[a];  // WS-A detector
        return c;
    }

    // #826 / A1a Phase C attempt #4 + #5 (2026-05-30, EXIT_GC_SPIRAL):
    // REVERTED — same failure mode as prior 3 attempts.
    //
    // Enabled NIX_V3_CHAIN_BINDINGS=1 with chain-construct path:
    // when nb ≤ 4 && na ≥ 16 && !a->isChain() && !b->isChain(),
    // build Chain{parent=a, overlay=b}.  All 5 nixpkgs paths failed
    // (hello.name, hello.pname, hello.drvPath, hello.outPath,
    // firefox.name) with the same error as v2/v3:
    // "attribute 'buildPythonApplication' missing".
    //
    // Per [[measure-twice-cut-once]] "Three failed pivots on same
    // premise = falsification" — this is the 4th pivot.  The
    // structural failure mode reproduces exactly across 4 attempts
    // with different chain-construct shapes, confirming the
    // architectural prereq (190-site entries[] audit + Nix-level
    // repro) is genuinely required.
    //
    // See EXIT_PHASE_C_4_FALSIFIED_2026-05-30.md for the 4-pivot
    // ledger + the architectural-blocker writeup.
    //
    // allocChainBindings helper stays in alloc.hh as the (now-
    // documented) staging point for a future multi-session attempt
    // that completes the prereqs first.

    // #826 / A1a Phase C — FALSIFIED across three attempts this
    // session (per measure-twice-cut-once §3.8 "three failed pivots
    // = falsification").  Chain construction reserved for a future
    // multi-session push that includes the full 208-site entries[]
    // audit.  Falsification ledger:
    //
    // - v1 (6f8095cd5): missed Phase D barrier in materialize();
    //   reverted.
    // - v2 (2cf14fdce): fixed materialize() barrier + consumer-site
    //   fallbacks + materialise mergeBindings inputs upfront.  Lang
    //   + core PASS; nixpkgs hello.name FAILED with
    //   `attribute 'buildPythonApplication' missing` on a 2-entry
    //   Bindings.
    // - v3 (this session, reverted in this commit): added
    //   `serializeAttrs` + `valuesEqual` chain-materialise to fix
    //   suspected Phase 5 cache corruption.  Brute audit clean;
    //   nixpkgs hello.name STILL FAILED with the same
    //   2-entry-Bindings error EVEN WITH the disk cache disabled
    //   (`NIX_V3_NO_DISK_CACHE=1`).  Diagnostic with V3_DBG_CHAIN_
    //   SELECT=1 confirmed: chain materialise IS firing correctly
    //   (chain size=1-3 → materialised size=41-494), so the 2-
    //   entry failure Bindings is a *Sorted* of overlay-only-shape,
    //   not a Chain.  Hypothesis: the chain spike is causing some
    //   Nix-level `f origArgs` to silently return `{}`, then
    //   `{} // {override, overrideDerivation}` short-circuits to
    //   the overlay (size=2).  Tracing requires reduced repro
    //   isolating the `f origArgs` failure point.
    //
    // Phase C revival prerequisites (carry-forward to a multi-
    // session task):
    //   1. Build a Nix-level minimal repro that triggers the
    //      `{} // overlay` collapse under chain spike.
    //   2. Identify which value-flow within nixpkgs `lib.make-
    //      Overridable` / `callPackageWith` / `python3.pkgs`
    //      machinery silently returns `{}` under chain interaction.
    //   3. Audit the 208 `entries[]` sites across the v3 tree.
    //   4. Either convert all iteration sites to `forEach` /
    //      `materialize()` OR keep chain construction gated on a
    //      whitelist of confirmed-safe call patterns.

    // Pass 1: count distinct keys.  Mirror of the branch logic
    // below; only reads `name` fields, no allocations, no copies,
    // no position lookups.
    uint32_t kExact = 0;
    {
        uint32_t i = 0, j = 0;
        while (i < na && j < nb) {
            const SymbolId an = a->entries[i].name;
            const SymbolId bn = b->entries[j].name;
            if (an < bn)        { ++kExact; ++i; }
            else if (an > bn)   { ++kExact; ++j; }
            else                { ++kExact; ++i; ++j; } // duplicate collapsed
        }
        kExact += (na - i) + (nb - j);
    }

    // Pass 2: allocate the exact size and fill.  `Alloc::allocBindings`
    // already sets `out->size = kExact`, so no second size-write is
    // needed (or wanted — it would be a redundant store to a Phase D
    // shared cell line, and on the empty-sentinel path a write to
    // shared read-only state).
    Bindings * out = Alloc::allocBindings(kExact);
    // #821 per-site bytes attribution.  Bytes attributed = kExact
    // entries (slack-free since #747's two-pass) × sizeof Entry
    // (24 B post-#752 inline-pos).  Calls counter is bumped at
    // function entry above; bytes counter accumulates only on the
    // allocating path.
    {
        const uint8_t s = static_cast<uint8_t>(siteId);
        if (s < AllocStats::kMergeBindingsSiteSlots)
            allocStats().mergeBindingsBytesBySite[s]
                += uint64_t(kExact) * sizeof(Bindings::Entry);
    }
    uint32_t i = 0, j = 0, k = 0;
    // #752: bindingsSetEntry now copies the entire Entry struct
    // including the inline `pos` field, so the per-attr position is
    // forwarded by the Entry copy itself.  The old explicit
    // recordAttrPos call (which used the side-table) is no longer
    // needed and would be a no-op anyway.
    auto copyA = [&]() {
        bindingsSetEntry(out, k, a->entries[i]);  // Phase D
        ++k; ++i;
    };
    auto copyB = [&]() {
        bindingsSetEntry(out, k, b->entries[j]);  // Phase D
        ++k; ++j;
    };
    while (i < na && j < nb) {
        if (a->entries[i].name < b->entries[j].name) {
            copyA();
        } else if (a->entries[i].name > b->entries[j].name) {
            copyB();
        } else {
            copyB();   // duplicate; b wins (incl. its position)
            ++i;
        }
    }
    while (i < na) copyA();
    while (j < nb) copyB();
    // Invariant: k == kExact by construction (the two passes share
    // identical branch arithmetic).  No need to rewrite out->size.
    return out;
}

/// Look up `name` in the with-stack, walking from top (innermost) outward.
/// Bounded below by the current frame's `withStackBase`: a closure must
/// not see its caller's `with` scopes.  `depth` is currently unused.
///
/// Each with-stack entry is forced lazily on first access — this is the
/// "delayed-with" rule.  `with pkgs; ...` inside a recursive group that
/// also defines pkgs would blackhole if we forced eagerly at
/// OP_WITH_PUSH; instead we keep the thunk on the stack and only force
/// when an unbound name actually triggers a lookup.  The forced value
/// is written back so subsequent lookups skip the force.
// 2026-05-18 cc-wrapper bisection layer 3: when withLookup finds a
// Tag::PrimOp value with arity=0 in a with-scope attrset (typically
// `with builtins; storeDir`), TW evaluates it implicitly to its
// constant value.  v3's vBuiltins stores arity-0 primops as raw
// Tag::PrimOp values; without auto-calling them at lookup-resolution
// time, the raw primop leaks into downstream string coercion / attr
// merge / etc. contexts and throws "toString tag=11" or similar.
//
// lower.cc:826,2861 already handles the STATIC `builtins.storeDir`
// case at lower-time (emits ir::PrimOpCall directly).  This runtime
// helper covers the DYNAMIC paths that lower.cc can't statically
// detect: `with X; primop`, `X.primop` where X is an unknown attrset,
// indirect `let b = builtins; in b.storeDir`, etc.
inline Value autoCallArity0(VMState & vm, const Value & v)
{
    if (v.tag() == Tag::PrimOp && v.asPrimOp()
        && v.asPrimOp()->arity == 0)
    {
        EvalState evs;
        evs.vm = &vm;
        evs.nixEvalState = getNixEvalState();
        Value out;
        v.asPrimOp()->fn(evs, nullptr, out);
        return out;
    }
    return v;
}

inline Value withLookup(VMState & vm, SymbolId name)
{
    // REVIEW §2.8: track whether any with-stack entry blackholed.  If
    // EVERY enclosing scope blackholed, the failure is "with rec { ...
    // self-referential ... }" -- report it as infinite recursion (matching
    // tree-walker's diagnostic), not "name not found in with-scope".
    bool anyBlackholed = false;
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    for (size_t i = vm.withStack.size(); i-- > base; ) {
        Value & w = vm.withStack[i];
        // Tag::Slot: deref the slot pointer.  Slots are mutated when
        // their backing let-rec body publishes a result, so reading
        // through the slot here gives us the LATEST value (matches
        // tree-walker's `state.forceValue(*v2)` on slot pointers).
        if (w.tag() == Tag::Slot) {
            Value * p = w.asSlot();
            if (!p) continue;
            // 2026-05-18 cc-wrapper bisection: nixpkgs's `lib.fix` /
            // `extends` / `callPackage` machinery can produce a CHAIN
            // of Slot indirections for a single with-scope entry
            // (e.g. `Slot -> Slot -> Slot -> Slot -> Slot -> Attrs(121)`
            // for `with targetPlatform;` inside bintools-wrapper's
            // dynamicLinker selection).  A single-level deref would
            // land on the NEXT Slot, fall through `!isAttrs()`, and
            // skip the with-entry entirely — even though the chain
            // resolves to an attrset containing the looked-up name
            // (V3_DBG_WITH dump: `[name-IS-here-but-missed!]`).
            //
            // Chase Slot indirections with cycle detection.  Two
            // distinct failure modes:
            //   - CYCLE: revisiting a Slot pointer we've already seen.
            //     Treat as blackholed (skip this scope, try outer).
            //   - OVERFLOW: chain longer than NIX_V3_WITH_CHAIN_LIMIT
            //     (default 64).  Throw an actionable error reporting
            //     the chain and how to raise the limit.
            //
            // Memory: static thread_local vector reused across calls,
            // cleared per call.  Zero allocation after warmup.
            //
            // NIX_V3_NO_WITH_SLOT_CHASE=1 reverts to the pre-fix
            // single-deref behaviour for A/B comparison and bisecting
            // downstream regressions exposed by the deeper chase.
            static const bool s_noChase =
                std::getenv("NIX_V3_NO_WITH_SLOT_CHASE") != nullptr;
            if (s_noChase) {
                // Pre-fix behaviour: one deref; if result is also a
                // Slot or non-attrset, skip this with-entry.
                Value derefed = *p;
                if (derefed.isThunk() || derefed.isAppLike()) {
                    try { derefed = forceValue(vm, derefed); }
                    catch (const BlackholeError &) { anyBlackholed = true; continue; }
                }
                if (!derefed.isAttrs()) continue;
                if (auto * v = derefed.asAttrs()->lookup(name))
                    return *v;
                continue;
            }
            static const int kChainLimit = []() {
                if (const char * e = std::getenv("NIX_V3_WITH_CHAIN_LIMIT"))
                    return std::max(1, std::atoi(e));
                return 64;
            }();
            static thread_local std::vector<Value *> visitedBuf;
            visitedBuf.clear();
            Value derefed = w;  // start from `w` (which IS a Slot here)
            bool chase_cycle = false;
            bool chase_overflow = false;
            while (derefed.tag() == Tag::Slot) {
                Value * q = derefed.asSlot();
                if (!q) { derefed.mkNull(); break; }
                bool seen = false;
                for (Value * v : visitedBuf) {
                    if (v == q) { seen = true; break; }
                }
                if (seen) { chase_cycle = true; break; }
                if ((int)visitedBuf.size() >= kChainLimit) {
                    visitedBuf.push_back(q);
                    chase_overflow = true;
                    break;
                }
                visitedBuf.push_back(q);
                derefed = *q;
            }
            if (chase_overflow) {
                const auto & symTab2 = ir::globalSymbolTable();
                std::string nm2 = name < symTab2.size()
                    ? symTab2[name] : "<?>";
                std::string chainDump;
                char ptrBuf[40];
                for (size_t i = 0; i < visitedBuf.size(); ++i) {
                    if (i) chainDump += " -> ";
                    std::snprintf(ptrBuf, sizeof ptrBuf,
                        "SLOT(%p)", (void *)visitedBuf[i]);
                    chainDump += ptrBuf;
                }
                throw std::runtime_error(
                    "v3 OP_WITH_LOOKUP: with-scope Slot chain depth "
                    "exceeded " + std::to_string(kChainLimit)
                    + " while resolving '" + nm2
                    + "'.  Try NIX_V3_WITH_CHAIN_LIMIT="
                    + std::to_string(kChainLimit * 2)
                    + " (or higher).  Chain: " + chainDump);
            }
            if (chase_cycle) {
                // Treat unresolvable chain like a blackholed entry —
                // skip this scope but mark anyBlackholed so outer
                // logic reports infinite-recursion correctly when no
                // other scope defines the name.
                anyBlackholed = true;
                continue;
            }
            // #548c STG-style partial-Bindings peek: BEFORE forcing,
            // check if `derefed` is a Black thunk whose construction
            // has registered partial Bindings via
            // publishToNearestBlackThunkFrame.  If so, peek there
            // first — this is the "selector thunk on Con cell"
            // analog that lets `with self;` find sibling entries
            // during a rec-attrset's mid-construction.  No force, no
            // blackhole error: just a Bindings::lookup on the
            if (derefed.isThunk() || derefed.isAppLike()) {
                try {
                    derefed = forceValue(vm, derefed);
                } catch (const BlackholeError &) {
                    anyBlackholed = true;
                    continue;
                }
            }
            if (!derefed.isAttrs()) continue;
            if (auto * v = derefed.asAttrs()->lookup(name))
                return autoCallArity0(vm, *v);
            continue;
        }
        if (w.isThunk() || w.isAppLike()) {
            // #458 step A.2 (slot-threading for fix-point args):
            // before forcing the whole TW Bridge thunk, try a per-
            // attribute lookup that observes a partially-constructed
            // attrset without tripping its outer-thunk BlackHole.
            // Cardano-node `with self;` over `extends overlay self`
            // shape: the partial Bindings already has the entries we
            // need; only the OUTER thunk is mid-blackhole.
            // (#458 with-stack Bridge attr-lookup retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
            try {
                w = forceValue(vm, w);
            } catch (const BlackholeError &) {
                // Delayed-with corner case: the with-stack entry
                // references something that's still being forced from
                // a deeper frame.  Skip it so outer scopes still get a
                // chance to define `name`.  Other errors propagate.
                anyBlackholed = true;
                continue;
            }
        }
        if (!w.isAttrs()) continue;
        if (auto * v = w.asAttrs()->lookup(name))
            return autoCallArity0(vm, *v);
    }
    // §2.8: if every enclosing scope blackholed and none defined the
    // name, the actual cause is infinite recursion (cycles in `with rec`
    // attrsets), not a typo.  Throw a typed BlackholeError so callers
    // (e.g. on-demand-root's auto-eager bridge guard) can route
    // correctly, rather than a vague "name not found" runtime_error.
    if (anyBlackholed) {
        // STG-6 (#498) diagnostic: dump with-stack contents at cycle
        // throw so we can identify which name + which black-thunk
        // shape triggers the cross-VMState fix-point cycle.  Set
        // V3_DBG_WITH_CYCLE=1 to trigger.
        static const bool s_dbgWithCycle =
            std::getenv("V3_DBG_WITH_CYCLE") != nullptr;
        if (__builtin_expect(s_dbgWithCycle, 0)) {
            const auto & sym = ir::globalSymbolTable();
            std::string nm = name < sym.size() ? sym[name] : "<?>";
            // Print source position of the firing frame so we can
            // identify which AST source location triggered the cycle.
            const auto & frFire = vm.frames.back();
            const LambdaDescriptor * dFire = nullptr;
            if (frFire.thunk && (frFire.thunk->state == ThunkState::Suspended
                || frFire.thunk->state == ThunkState::Blackhole))
                dFire = frFire.thunk->suspended.desc;
            if (!dFire && frFire.closure) dFire = frFire.closure->desc;
            const PosSnapshot * psFire =
                dFire ? resolvePosSnapshot(dFire->posHandle) : nullptr;
            std::fprintf(stderr,
                "v3 OP_WITH_LOOKUP cycle: name='%s' base=%zu top=%zu vm=%p frames=%zu pos=%s:%u:%u\n",
                nm.c_str(), base, vm.withStack.size(), (void *)&vm,
                vm.frames.size(),
                (psFire && !psFire->file.empty()) ? psFire->file.c_str() : "<no-pos>",
                psFire ? psFire->line : 0u,
                psFire ? psFire->column : 0u);
            for (size_t i = vm.withStack.size(); i-- > base; ) {
                Value w = vm.withStack[i];
                std::fprintf(stderr, "  with[%zu] tag=%u",
                    i, (unsigned)w.tag());
                if (w.tag() == Tag::Slot && w.asSlot()) {
                    Value d = *w.asSlot();
                    std::fprintf(stderr, " -> SLOT(%p)=tag=%u",
                        (void *)w.asSlot(), (unsigned)d.tag());
                    if (d.isThunk() && d.asThunk()) {
                        std::fprintf(stderr, "(thunk=%p state=%d)",
                            (void *)d.asThunk(),
                            (int)d.asThunk()->state);
                    }
                } else if (w.isThunk() && w.asThunk()) {
                    std::fprintf(stderr, " thunk=%p state=%d",
                        (void *)w.asThunk(),
                        (int)w.asThunk()->state);
                }
                std::fprintf(stderr, "\n");
            }
            // #546 follow-on: dump the call-frame chain so we can
            // identify WHICH thunk is forcing the with-source.  The
            // bottom of the chain holds the OP_WITH_LOOKUP-firing
            // thunk; outer frames show the cause chain leading to it.
            std::fprintf(stderr, "  frames (top=%zu, last 12):\n",
                vm.frames.size());
            size_t lim = vm.frames.size() < 12 ? 0 : vm.frames.size() - 12;
            for (size_t i = vm.frames.size(); i-- > lim; ) {
                const auto & f = vm.frames[i];
                const char * kind = (f.flags & CFF_THUNK_RETURN) ? "thunk"
                    : f.closure ? "call" : "?";
                const LambdaDescriptor * d = nullptr;
                // BUGFIX (2026-05-09): the THUNK_RETURN flag should
                // route to f.thunk's desc, not f.closure's.  Earlier
                // version preferred f.closure unconditionally, which
                // meant a frame that had BOTH set (set by some path
                // we haven't pinpointed) showed the wrong descriptor.
                //
                // suspended.desc is union-shared with `evaluated:Value`
                // / `bridgeSrc:void*`; reading it when state isn't
                // Suspended/Blackhole returns garbage (pre-state-guard
                // version was reporting bogus codeOffsets).  Fall
                // through to the closure's desc in that case.
                bool descValid = f.thunk && (
                    f.thunk->state == ThunkState::Suspended
                    || f.thunk->state == ThunkState::Blackhole);
                if (f.flags & CFF_THUNK_RETURN) {
                    if (descValid) d = f.thunk->suspended.desc;
                    else if (f.closure) d = f.closure->desc;
                } else {
                    if (f.closure) d = f.closure->desc;
                    else if (descValid) d = f.thunk->suspended.desc;
                }
                const CompilationUnit * thunkCu =
                    (descValid && f.thunk) ? thunkCU(f.thunk) : nullptr;  // FP-2a: was suspended.cu
                // OP_TAIL_CALL retargets cur.cu/cur.closure but leaves
                // f.thunk's descriptor pointing at the original
                // thunk-body lambda.  So `d` (the THUNK descriptor)
                // names the *outer* thunk, while f.closure->desc names
                // what the frame is *actually executing*.  Print BOTH
                // when they differ -- the executing-desc + f.cu match
                // the disassembly window below; the thunk-desc is the
                // identity that OP_RETURN will deposit the result into.
                const LambdaDescriptor * exec = f.closure ? f.closure->desc : nullptr;
                bool tailCalled = exec && d && exec != d;
                std::fprintf(stderr,
                    "    [%zu] %s ip=%u thunk-name='%s' thunk-codeOff=%u",
                    i, kind, f.ip,
                    d && !d->name.empty() ? d->name.c_str() : "<anon>",
                    d ? (unsigned)d->codeOffset : 0u);
                if (tailCalled)
                    std::fprintf(stderr, " EXEC=%s codeOff=%u",
                        !exec->name.empty() ? exec->name.c_str() : "<anon>",
                        (unsigned)exec->codeOffset);
                std::fprintf(stderr,
                    " f.cu=%p thunk.cu=%p closure=%p thunk=%p flags=%x",
                    (const void *)f.cu, (const void *)thunkCu,
                    (const void *)f.closure, (const void *)f.thunk,
                    (unsigned)f.flags);
                if (f.thunk)
                    std::fprintf(stderr, " thunk=%p state=%d",
                        (void *)f.thunk, (int)f.thunk->state);
                std::fprintf(stderr, "\n");
                // V3_DBG_TRACE_THUNK_X: cross-check current desc
                // against the descriptor pointer recorded at
                // OP_MAKE_THUNK time.  If `descPtr` differs, the
                // suspended-union has been overwritten.  If `descPtr`
                // matches but `codeOffset`/`name` differ, the
                // descriptor itself was mutated in place (or the
                // cu->lambdas vector relocated its storage).  Either
                // is a hard data-corruption signal.
                if (g_traceThunkX && f.thunk) {
                    auto & m = thunkCreationMap();
                    auto it = m.find(f.thunk);
                    if (it == m.end()) {
                        std::fprintf(stderr,
                            "      [trace-x] thunk=%p NOT in creation "
                            "map (allocated outside OP_MAKE_THUNK)\n",
                            (void *)f.thunk);
                    } else {
                        const auto & ci = it->second;
                        const LambdaDescriptor * curD =
                            (f.thunk->state == ThunkState::Suspended
                              || f.thunk->state == ThunkState::Blackhole)
                            ? f.thunk->suspended.desc : nullptr;
                        bool ptrMatch  = (curD == ci.descPtr);
                        bool codeMatch = curD && curD->codeOffset == ci.codeOff;
                        bool nameMatch = curD && curD->name == ci.name;
                        std::fprintf(stderr,
                            "      [trace-x] created: fid=%u codeOff=%u "
                            "name='%s' descPtr=%p cu=%p%s%s%s\n",
                            ci.funcIdx, ci.codeOff, ci.name.c_str(),
                            (const void *)ci.descPtr,
                            (const void *)ci.cu,
                            ptrMatch ? "" : " [DESC-PTR-CHANGED]",
                            codeMatch ? "" : " [CODEOFF-CHANGED]",
                            nameMatch ? "" : " [NAME-CHANGED]");
                    }
                }
                // Disassemble around current ip for the inner-most few
                // frames so we can see the failing IR-ops + their
                // immediate predecessors (the value that became the
                // failing with-source).
                if (i + 3 >= vm.frames.size() && f.cu) {
                    uint32_t lo = f.ip > 30 ? f.ip - 30 : 0;
                    uint32_t hi = std::min<uint32_t>(f.ip + 6,
                        static_cast<uint32_t>(f.cu->code.size()));
                    if (lo < hi) {
                        std::fprintf(stderr, "      bytecode [%u-%u):\n",
                            lo, hi);
                        disassembleWindow(stderr, *f.cu, lo, hi);
                    }
                }
                // For frames with d->name=="super" but unfamiliar codeOff,
                // dump bytecode from the function's START so we can see
                // its prologue / what kind of function it is (lambda body
                // vs let-rec entry vs hidden-from-expr thunk).
                if (d && d->name == "super" && f.cu && (f.flags & CFF_THUNK_RETURN)) {
                    uint32_t lo = d->codeOffset;
                    uint32_t hi = std::min<uint32_t>(lo + 25,
                        static_cast<uint32_t>(f.cu->code.size()));
                    if (lo < hi) {
                        std::fprintf(stderr,
                            "      'super' THUNK body-start [%u-%u):\n",
                            lo, hi);
                        disassembleWindow(stderr, *f.cu, lo, hi);
                    }
                }
            }
            // Path B investigation (2026-05-17): also dump ALL frames
            // when V3_DBG_WITH_CYCLE_FULL=1 (capped at 200 to avoid
            // spam).  Otherwise show outer 16 + the existing inner 12.
            // Outer frames identify the eager-force whose body invoked
            // the chain.
            static const bool s_dbgWithCycleFull =
                std::getenv("V3_DBG_WITH_CYCLE_FULL") != nullptr;
            size_t outerN;
            if (s_dbgWithCycleFull) {
                outerN = std::min<size_t>(200, vm.frames.size() - 12);
            } else {
                outerN = std::min<size_t>(16, vm.frames.size() - 12);
            }
            if (vm.frames.size() > 12) {
                std::fprintf(stderr,
                    "  frames (OUTER %zu, indices 0..%zu):\n",
                    outerN, outerN - 1);
                for (size_t i = 0; i < outerN; ++i) {
                    const auto & f = vm.frames[i];
                    const char * kind = (f.flags & CFF_THUNK_RETURN) ? "thunk"
                        : f.closure ? "call" : "?";
                    const LambdaDescriptor * d = nullptr;
                    bool descValid = f.thunk && (
                        f.thunk->state == ThunkState::Suspended
                        || f.thunk->state == ThunkState::Blackhole);
                    if (f.flags & CFF_THUNK_RETURN) {
                        if (descValid) d = f.thunk->suspended.desc;
                        else if (f.closure) d = f.closure->desc;
                    } else {
                        if (f.closure) d = f.closure->desc;
                        else if (descValid) d = f.thunk->suspended.desc;
                    }
                    const PosSnapshot * ps =
                        d ? resolvePosSnapshot(d->posHandle) : nullptr;
                    std::fprintf(stderr,
                        "    [%zu] %s ip=%u name='%s' codeOff=%u pos=%s:%u:%u flags=%x\n",
                        i, kind, f.ip,
                        d && !d->name.empty() ? d->name.c_str() : "<anon>",
                        d ? (unsigned)d->codeOffset : 0u,
                        (ps && !ps->file.empty()) ? ps->file.c_str()
                            : "<no-pos>",
                        ps ? ps->line : 0u, ps ? ps->column : 0u,
                        (unsigned)f.flags);
                }
            }
            // Path B investigation (2026-05-17): under WITH_CYCLE_FULL,
            // also dump the firing frame's CU stringConstants/paths.
            // The bytecode disasm shows OP_LIT_PATH operand=N; the path
            // string at that index identifies WHICH `libsForQt5.callPackage
            // <path>` site we're inside.
            if (s_dbgWithCycleFull && !vm.frames.empty()) {
                const auto & fInner = vm.frames.back();
                const CompilationUnit * cuI = fInner.cu;
                if (cuI) {
                    std::fprintf(stderr,
                        "  firing-cu stringConstants (first 32):\n");
                    size_t sn = std::min<size_t>(32, cuI->stringConstants.size());
                    for (size_t i = 0; i < sn; ++i) {
                        const auto & s = *cuI->stringConstants[i];  // M-10: interned ptr
                        // Truncate long strings for display.
                        std::string display = s.substr(0, 100);
                        std::fprintf(stderr,
                            "    [%zu] (%zu bytes) %s%s\n",
                            i, s.size(), display.c_str(),
                            s.size() > 100 ? "..." : "");
                    }
                }
            }
            std::fflush(stderr);
        }
        throw BlackholeError(
            "v3 OP_WITH_LOOKUP: cycle while resolving '"
            + std::string(name < ir::globalSymbolTable().size()
                ? ir::globalSymbolTable()[name] : "<?>") + "'");
    }
    // V3_DBG_WITH: print the missing name + the with-stack contents to
    // help diagnose pure-VM nixpkgs failures where eval-order divergence
    // causes a name to be looked up before its `with` scope is visible.
    static const bool s_dbg_with = std::getenv("V3_DBG_WITH") != nullptr;
    const auto & symTab = ir::globalSymbolTable();
    std::string nm = name < symTab.size() ? symTab[name] : "<?>";
    if (s_dbg_with) {
        std::fprintf(stderr,
            "v3 OP_WITH_LOOKUP miss: name='%s' (sid=%u) base=%zu top=%zu\n",
            nm.c_str(), (unsigned)name, base, vm.withStack.size());
        for (size_t i = vm.withStack.size(); i-- > base; ) {
            Value w = vm.withStack[i];   // copy so we can chase
            void * orig_thunk = w.isThunk() ? (void *)w.asThunk() : nullptr;
            std::fprintf(stderr, "  with[%zu] tag=%u thunk_ptr=%p",
                i, (unsigned)w.tag(), orig_thunk);
            // Chase Evaluated thunk chains and Tag::Slot derefs to find
            // the underlying attrset (or pinpoint where the chain
            // terminates in a Black thunk / non-attrset).
            int chase_lim = 8;
            while (chase_lim-- > 0) {
                if (w.tag() == Tag::Slot) {
                    Value * p = w.asSlot();
                    std::fprintf(stderr, " -> SLOT(%p)", (void*)p);
                    if (!p) break;
                    w = *p;
                    std::fprintf(stderr, "=tag=%u", (unsigned)w.tag());
                    if (w.isThunk())
                        std::fprintf(stderr, "(ptr=%p,state=%d)",
                            (void*)w.asThunk(), (int)w.asThunk()->state);
                    continue;
                }
                if (w.isThunk()
                    && w.asThunk()->state == ThunkState::Evaluated) {
                    w = w.asThunk()->evaluated;
                    std::fprintf(stderr, " -> tag=%u", (unsigned)w.tag());
                    if (w.isThunk())
                        std::fprintf(stderr, "(ptr=%p,state=%d)",
                            (void *)w.asThunk(), (int)w.asThunk()->state);
                    continue;
                }
                break;
            }
            if (w.isAttrs() && w.asAttrs()) {
                auto * b = w.asAttrs();
                std::fprintf(stderr, " attrs size=%u {", b->size);
                for (uint32_t k = 0; k < b->size && k < 30; ++k) {
                    SymbolId s = b->entries[k].name;
                    std::fprintf(stderr, "%s%s",
                        k ? "," : "",
                        s < symTab.size() ? symTab[s].c_str() : "?");
                }
                if (b->size > 30) std::fprintf(stderr, ",...");
                std::fprintf(stderr, "}");
                // Also check if name IS in this attrset (via binary search) —
                // if it is, we have a real bug (lookup failed but it's there).
                if (auto * v = b->lookup(name)) {
                    std::fprintf(stderr, " [name-IS-here-but-missed!]");
                    (void)v;
                }
            } else if (w.isThunk()) {
                Thunk * t = w.asThunk();
                std::fprintf(stderr, " thunk state=%d nUp=%u",
                    (int)t->state, (unsigned)t->nUpvalues);
                if (t->state == ThunkState::Suspended) {
                    auto * d = t->suspended.desc;
                    if (d)
                        std::fprintf(stderr, " %s [%u..)",
                            !d->name.empty() ? d->name.c_str() : "<anon>",
                            d->codeOffset);
                }
            } else if (w.isClosure() && w.asClosure()
                       && w.asClosure()->desc) {
                auto * d = w.asClosure()->desc;
                std::fprintf(stderr, " closure=%s nUp=%u",
                    !d->name.empty() ? d->name.c_str() : "<anon>",
                    w.asClosure()->nUpvalues);
            }
            std::fprintf(stderr, "\n");
        }
        // Dump frame stack — the failing `with` lookup happens during a
        // specific frame's body; identifying it helps localize the source.
        size_t lim = vm.frames.size();
        std::fprintf(stderr, "  frames=%zu (showing all):\n", lim);
        for (size_t i = lim; i > 0; --i) {
            const auto & fr = vm.frames[i - 1];
            const LambdaDescriptor * d = nullptr;
            // #498 fix: only read fr.thunk->suspended.desc when state
            // is Suspended/Blackhole — Evaluated/Bridge thunks have a
            // different union active, reading suspended.desc on them
            // is undefined behaviour and aborts the diagnostic before
            // printing the rest of the stack.
            if (fr.thunk && (fr.thunk->state == ThunkState::Suspended
                          || fr.thunk->state == ThunkState::Blackhole))
                d = fr.thunk->suspended.desc;
            else if (fr.closure) d = fr.closure->desc;
            // PhD-6 gnuabi64 RCA: for each frame, also print nWithTargets (did
            // this lambda/thunk lexically capture a `with`?) and, for thunk
            // frames, whether capturedWiths is null + hasWithsSlot — so a thunk
            // that SHOULD have withs (nWith>0) but lost them (capW=0x0) under
            // scavenge is visible as the empty-with-scope root cause.
            ListVec * capW = (fr.thunk
                && (fr.thunk->state == ThunkState::Suspended
                    || fr.thunk->state == ThunkState::Blackhole))
                ? thunkCapturedWiths(fr.thunk) : nullptr;
            std::fprintf(stderr,
                "    frame[%zu]: %s code=[%u..) ip=%u flags=%u thunk=%p withBase=%u "
                "nWith=%u capW=%p%s\n",
                i - 1,
                d && !d->name.empty() ? d->name.c_str()
                    : (d ? "<anon>" : "<root>"),
                d ? d->codeOffset : 0, fr.ip,
                (unsigned)fr.flags, (void *)fr.thunk, fr.withStackBase,
                d ? (unsigned)d->nWithTargets : 0u, (void *)capW,
                (fr.thunk && fr.thunk->hasWithsSlot) ? " hasSlot" : "");
            std::fflush(stderr);
        }
        // V3_DBG_WITH_DISASM=1: also dump each frame's bytecode in a
        // window around fr.ip in fr.cu (using the frame's actual
        // executing CU, not the thunk descriptor's codeOffset which
        // may be stale after OP_TAIL_CALL retargets cu/ip).
        static const bool s_dbg_disasm =
            std::getenv("V3_DBG_WITH_DISASM") != nullptr;
        // V3_DUMP_LAMBDAS=1: dump every LambdaDescriptor in EVERY
        // unique CU on the call stack with codeOffset + name.
        static const bool s_dumpLambdas =
            std::getenv("V3_DUMP_LAMBDAS") != nullptr;
        if (__builtin_expect(s_dumpLambdas, 0)) {
            std::set<const CompilationUnit *> seenCus;
            for (size_t fi = 0; fi < vm.frames.size(); ++fi) {
                const auto & fr = vm.frames[fi];
                if (!fr.cu) continue;
                if (!seenCus.insert(fr.cu).second) continue;
                std::fprintf(stderr,
                    "  V3_DUMP_LAMBDAS: cu=%p (%zu entries)\n",
                    (const void *)fr.cu, fr.cu->lambdas.size());
                for (size_t li = 0; li < fr.cu->lambdas.size(); ++li) {
                    const auto & d = fr.cu->lambdas[li];
                    std::fprintf(stderr,
                        "    L[%zu] codeOffset=%u nUp=%u name=%s\n",
                        li, (unsigned)d.codeOffset, (unsigned)d.nUpvalues,
                        d.name.empty() ? "<anon>" : d.name.c_str());
                }
            }
        }
        // V3_DUMP_RANGE=START:END dumps bytecode for an arbitrary
        // range from the failing frame's CU.  Use to inspect thunks
        // not currently on the stack (e.g., a thunk that returned
        // through CFF_FORCE_RETRY upstream of the current frame).
        static const char * s_dumpRange = std::getenv("V3_DUMP_RANGE");
        if (const char * ranges = s_dumpRange; __builtin_expect(ranges != nullptr, 0)) {
            // Dump the range from EVERY unique CU on the call stack so
            // we don't miss thunks in CUs other than vm.frames.back().cu
            // (e.g., when force-chasing across imported files).
            std::set<const CompilationUnit *> seenCusR;
            for (size_t fi = 0; fi < vm.frames.size(); ++fi) {
                const auto & fr = vm.frames[fi];
                if (!fr.cu) continue;
                if (!seenCusR.insert(fr.cu).second) continue;
                std::string s(ranges);
                size_t pos = 0;
                while (pos < s.size()) {
                    size_t comma = s.find(',', pos);
                    std::string token = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    size_t colon = token.find(':');
                    if (colon != std::string::npos) {
                        uint32_t lo = std::strtoul(token.substr(0, colon).c_str(), nullptr, 0);
                        uint32_t hi = std::strtoul(token.substr(colon + 1).c_str(), nullptr, 0);
                        std::fprintf(stderr, "  V3_DUMP_RANGE cu=%p [%u..%u):\n",
                            (const void *)fr.cu, lo, hi);
                        disassembleWindow(stderr, *fr.cu, lo, hi);
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
            }
        }
        if (s_dbg_disasm) {
            for (size_t i = lim; i > 0; --i) {
                const auto & fr = vm.frames[i - 1];
                if (!fr.cu) continue;
                uint32_t fip = fr.ip;
                uint32_t lo = fip > 32 ? fip - 32 : 0;
                uint32_t hi = fip + 32;
                if (hi <= lo) continue;
                std::fprintf(stderr,
                    "  frame[%zu] cu-disasm [%u..%u):\n", i - 1, lo, hi);
                disassembleWindow(stderr, *fr.cu, lo, hi);
            }
        }
    }
    // #686 — match TW's `undefined variable '<name>'` phrasing
    // (libexpr/eval-error.cc).  Drops the "v3 OP_WITH_LOOKUP:" debug
    // prefix.
    throw std::runtime_error("undefined variable '" + nm + "'");
}

// ---------------------------------------------------------------------------
// EXIT_GC_SPIRAL Week 2 Day 13-15 (2026-05-29): singleton interning pool
// for 1-element capturedWiths ListVecs.
//
// T1_3_PAIRS_LISTS_ATTR_2026-05-27 measured 544 K MAKE_THUNK allocs on HNE
// with avg-size 1.04, max 2 — overwhelmingly nWiths==1.  Each 1-element
// ListVec is 32 B (16 B header + 16 B element).  Interning shares one
// tenured-arena ListVec across all Thunks/Closures capturing the same
// with-target, eliminating ~95% of the per-call alloc cost.
//
// Correctness model:
//   * ListVec is set-once at MAKE time and read-only thereafter.
//     pushCapturedWiths only READS via ->size / ->elems[i].  Sharing
//     is therefore safe — no inter-thunk mutation.
//   * Allocated arena-resident (Alloc::allocList).  Arena pointers
//     are stable for the process lifetime (no compaction).
//   * Phase D / nursery generational correctness: first miss installs
//     the ListVec and calls listPostConstructBarrier() — if the
//     with-target was a nursery Value, the ListVec lands on
//     dirtyContainers and the next scavenge forwards the embedded
//     payload.  Subsequent cache hits return the SAME pointer, whose
//     elems[0] has already been forwarded (or will be by the pending
//     scavenge).  No additional barriers required.
//   * Cache orphans: when a Value is forwarded by scavenge, ALL live
//     references to it are updated (it's a root walk).  Cache KEYS
//     (uint64 copies) are NOT updated; orphaned entries have stale
//     keys.  No correctness issue (next lookup with the new key
//     misses → allocates fresh).  Orphan footprint is bounded by
//     the fixed bucket count.
//
// Memory footprint of the cache itself: 4096 buckets * 24 B = 96 KB.
//
// Gate: NIX_V3_NO_CAPWITHS_INTERN=1 reverts every call to a fresh
// allocList for A/B measurement.
//
// Retirement criterion (Rule 0): retire the cache when (a) Phase E
// v0.2 default-on makes nursery-allocated ListVecs cheap enough to
// drop the per-call cost, OR (b) Stage 6 production GC reclaims
// per-call ListVecs unaided.  Until then, keep the cache.

namespace {

constexpr size_t kCapWithsCacheBuckets = 4096;

struct CapWithsCacheEntry {
    uint64_t key_tag_payload;
    uint64_t key_payload_raw;
    ListVec * value;  // nullptr → empty entry
};

static CapWithsCacheEntry s_capWithsCache[kCapWithsCacheBuckets] = {};
static uint64_t s_capWithsHits     = 0;
static uint64_t s_capWithsMisses   = 0;
static uint64_t s_capWithsEvicts   = 0;

inline size_t hashCapWithsKey(uint64_t tp, uint64_t pr) noexcept
{
    uint64_t h = tp * 0x9e3779b97f4a7c15ull;
    h ^= pr * 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    return static_cast<size_t>(h) & (kCapWithsCacheBuckets - 1);
}

/// Intern-or-allocate a 1-element ListVec capturing `v`.  Hit returns
/// the existing arena pointer in O(1); miss allocates fresh, installs
/// (overwriting any colliding entry — collisions are cheaper than
/// chaining at this scale).
inline ListVec * internOrAllocSingletonCapWiths(const Value & v) noexcept
{
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_CAPWITHS_INTERN") != nullptr;
    // PhD-6 (2026-06-14): the cache stores raw ListVec* across calls but is NOT a
    // scavenge root (a C++ static, not arena/VM-stack).  Under a MOVING nursery a
    // cached entry goes stale the moment its ListVec is forwarded out of the
    // nursery; a later cache HIT returns the stale pointer (a reused/zeroed slot →
    // size 0), so `pushCapturedWiths` pushes nothing → the body runs with an empty
    // captured-`with` scope → "undefined variable" (reproduced as `gnuabi64` on
    // hello.drvPath under the aggressive 1 MB nursery; BRUTE+AUDIT both miss it
    // because neither scans this static).  Bypass the cache when the nursery is
    // active — nursery allocs are cheap bump-pointer (the cache's own retirement
    // criterion, §"Retirement").  Under the non-moving major-GC default the cache
    // stays correct AND valuable, so the fast path is unchanged there.
    if (__builtin_expect(s_disabled || phaseDActive(), 0)) {
        ListVec * lws = Alloc::allocList(1);
        lws->elems[0] = v;
        listPostConstructBarrier(lws);
        return lws;
    }
    const uint64_t tp = v.rawWord();
    const uint64_t pr = reinterpret_cast<uint64_t>(v.asRaw());
    const size_t idx = hashCapWithsKey(tp, pr);
    CapWithsCacheEntry & e = s_capWithsCache[idx];
    if (e.value
        && e.key_tag_payload == tp
        && e.key_payload_raw == pr)
    {
        ++s_capWithsHits;
        return e.value;
    }
    ++s_capWithsMisses;
    if (e.value) ++s_capWithsEvicts;  // collision: prior entry replaced
    ListVec * lws = Alloc::allocList(1);
    lws->elems[0] = v;
    listPostConstructBarrier(lws);
    e.key_tag_payload = tp;
    e.key_payload_raw = pr;
    e.value = lws;
    return lws;
}

// File-scope accessors require external linkage so run.cc can call
// them.  The statics they read live in the unnamed inner namespace
// above (internal linkage, but visible within this TU).
inline uint64_t internalCapWithsHits()    noexcept { return s_capWithsHits; }
inline uint64_t internalCapWithsMisses()  noexcept { return s_capWithsMisses; }
inline uint64_t internalCapWithsEvicts()  noexcept { return s_capWithsEvicts; }

} // anonymous

/// Snapshot the current frame's visible with-stack (entries from
/// `withStackBase` to top) into a fresh ListVec.  Returns nullptr when
/// no withs are currently in scope (cheap fast-path for the common case
/// of no enclosing `with`).
inline ListVec * snapshotCurrentWiths(VMState & vm)
{
    size_t base = vm.frames.empty() ? 0 : vm.frames.back().withStackBase;
    size_t top  = vm.withStack.size();
    if (top <= base) return nullptr;
    uint32_t n = static_cast<uint32_t>(top - base);
    // Day 13-15 (2026-05-29): intern the 1-element case via the
    // singleton pool — same correctness model as the OP_MAKE_THUNK
    // path (see internOrAllocSingletonCapWiths above).  Larger
    // sizes fall through to per-call alloc.  snapshotCurrentWiths
    // is the secondary T1.3 site (vm.cc:6991 in the 2026-05-27
    // numbering, 5.13 MB on HNE, avg-size 1.38 max 339): some
    // fraction will hit the size-1 fast path.
    if (n == 1) {
        return internOrAllocSingletonCapWiths(vm.withStack[base]);
    }
    ListVec * out = Alloc::allocList(n);
    for (uint32_t i = 0; i < n; ++i)
        out->elems[i] = vm.withStack[base + i];
    listPostConstructBarrier(out);  // Phase D coverage
    return out;
}

/// Push a closure/thunk's captured with-stack onto vm.withStack so it
/// becomes visible to the body's OP_WITH_LOOKUPs.  The caller must have
/// already set the new frame's withStackBase to vm.withStack.size()
/// BEFORE calling this so the floor is correct.
inline void pushCapturedWiths(VMState & vm, ListVec * capturedWiths)
{
    if (!capturedWiths) return;
    for (uint32_t i = 0; i < capturedWiths->size; ++i)
        vm.withStack.push_back(capturedWiths->elems[i]);
}

} // namespace

// ---------------------------------------------------------------------------
// Forward declarations spanning the next anonymous namespace.
// ---------------------------------------------------------------------------

// 2026-05-17: dispatchLoop's body-level try/catch (added so callers
// can tail-call dispatchLoop instead of holding their C-frame open
// to catch + clean up) calls clearBlackMarksOnException before re-
// throwing.  The definition lives at file scope (line ~8330);
// declare it here so the anonymous-namespace-scoped dispatchLoop can
// reach it by unqualified name.
static void clearBlackMarksOnException(VMState & vm, size_t exitDepth);

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

namespace {

/// WC-38 architectural fix attempt: walk the frame stack from BOTTOM
/// up and publish `v` to the OUTERMOST CFF_THUNK_RETURN frame whose
/// `thunk` is in Blackhole state.  Mirrors tree-walker's
/// `v.mkAttrs(...)` semantics writing into the slot of the currently-
/// forcing thunk DURING `ExprAttrs::eval`.  Sub-thunks captured-with
/// the slot will then see the (possibly intermediate) attrset rather
/// than blackholing.
///
/// We target the OUTERMOST (deepest in the stack) Black thunk, not
/// the innermost.  Reason: the inner Black thunks (e.g., a sub-thunk
/// firing INSIDE x's body) have a different final value than x.
/// Publishing to inner thunks would corrupt their evaluated.  The
/// outermost Black thunk is the one whose `with self;` capture is in
/// scope for failing sub-thunks (e.g., x in `let x = f x; in x`).
///
/// Idempotent: the eventual `OP_RETURN` of the outer thunk frame
/// will overwrite `evaluated` with the final retVal.  Risk:
/// intermediate values may be partial.  For the lib.fix bootstrap
/// pattern, the final value IS one of the intermediate attrsets,
/// so a sub-thunk reading the intermediate is reading a valid
/// snapshot.
///
/// Gated behind `NIX_V3_EARLY_PUBLISH=1`.  Default off; verified
/// non-functional for nixpkgs WC-38 and DAMAGING (corrupts slot
/// targets with intermediate values).  Kept as scaffolding only.
///
/// Option 2 from the multi-agent synthesis (publish-to-all variant)
/// was tested and rejected: writing intermediate ExprAttrs values to
/// every Black CFF_THUNK_RETURN frame's thunk wrote tiny intermediate
/// attrsets like `{prev=...}` into the lib.fix slot, replacing the
/// thunk-being-forced with a wrong-typed value.  Each thunk has its
/// OWN final value; without per-thunk destination tracking (which
/// tree-walker has via stack-local `vCur`), v3 cannot safely publish
/// intermediate values.  Outermost-only and publish-to-all both
/// corrupt nixpkgs.
/// #457/#458 partial-bindings registry.  Side-table mapping a
/// currently-being-forced (Black) Thunk to a partial Bindings*
/// produced by OP_ATTRS_REC_INIT in its body.  Used by callers that
/// need to access individual entries of a mid-construction rec-
/// attrset (specifically OP_REC_BINDING_SLOT_REF, OP_ATTRS_SELECT,
/// OP_ATTRS_HAS) without forcing the wrapping thunk.
///
/// Why a side-table instead of EARLY_PUBLISH (which writes to
/// thunk.evaluated directly): EARLY_PUBLISH corrupts nested rec-
/// attrset constructions, since each thunk has its own final value
/// and writing the partial Bindings to an outer thunk's evaluated
/// field can leave a wrong-typed value if the outer thunk's body
/// produces something else.  The side-table is safe because the
/// thunk's actual state machine is unchanged -- the side entry is
/// just a hint that callers can use opportunistically.
///
/// Lifetime: entry added at OP_ATTRS_REC_INIT (when a thunk frame
/// is on the stack); entry removed when forceValue completes the
/// body normally (transition to Evaluated) or when an exception
/// unwinds (clearBlackMarksOnException scans and clears).
//
// #558 Phase 3.3 (2026-05-12): partialBindingsRegistry retired.

// ---------------------------------------------------------------------------
// A8 (2026-05-13): iterative-force writeback protocol
// ---------------------------------------------------------------------------
//
// Multi-arg opcodes (OP_CALL_PRIMOP, OP_LIST_CONCAT, OP_ATTRS_UPDATE,
// OP_STR_CONCAT, V3_IS_OP, OP_HEAD, OP_TAIL, OP_LENGTH, OP_ELEM_AT, etc.)
// historically forced their non-lazy args via the C-recursive
// `forceValue(vm, args[i])` helper.  Each call burns ~1.4 KiB of C-stack,
// and deep nixpkgs evals chain through ~2000+ such forces, blowing the
// 8 MiB macOS thread stack.
//
// To make these iterative without changing bytecode, opcodes use a
// per-frame "writeback slot": before goto op_force_slow, the opcode
//   1. Pushes a duplicate of the unforced value to top-of-stack.
//   2. Encodes the original slot's offset (relative to stackBase) into
//      the upper 16 bits of CallFrame::flags.
//   3. Rewinds `ip` so the opcode re-enters after the force completes.
//   4. Sets CFF_FORCE_RETRY and goto op_force_slow.
//
// When the inner force completes (either synchronously via op_force_slow's
// Slot/Evaluated/Bridge chase OR via the thunk body's OP_RETURN), the
// writeback helper writes the forced result into the caller's original
// slot, pops the top duplicate, clears the writeback field, and suppresses
// the retry chain.  The opcode re-enters its switch case, re-scans its
// args, and either finds the next non-WHNF arg to force or proceeds with
// all args in WHNF.
//
// CFF_FORCE_WB (bit 3 of flags, defined in v3/vm.hh) indicates the
// upper 16 bits of flags encode a stack-base-relative slot offset for
// the writeback target.  When the bit is clear, forces use the legacy
// "push result to top of stack" behavior — which OP_FORCE /
// OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE and the 7 single-arg
// branch opcodes (OP_NOT, OP_*_BRANCH, OP_ATTRS_SELECT,
// OP_REC_BINDING_SLOT_REF) rely on.
//
// We cannot use a sentinel value in the upper 16 bits because a fresh
// CallFrame has flags=0, which would collide with "slot 0".  An
// explicit flag bit is unambiguous.
//
// CFF_FORCE_WB_PTR (bit 4) overrides: target is `forceWriteTarget`
// (a heap Value*), not a stack slot.  Used by deep-force passes
// (OP_CALL_PRIMOP's deepForceList) to writeback into list/attr storage
// directly.

inline uint16_t getForceWriteback(const CallFrame & f) noexcept
{
    return static_cast<uint16_t>(f.flags >> 16);
}

inline void setForceWriteback(CallFrame & f, uint16_t off) noexcept
{
    f.flags = (f.flags & 0x0000FFFFu) | (static_cast<uint32_t>(off) << 16)
            | CFF_FORCE_WB;
}

inline void clearForceWriteback(CallFrame & f) noexcept
{
    f.flags &= 0x0000FFFFu & ~CFF_FORCE_WB;
}

/// Apply pending writeback if any: top-of-stack holds the forced value;
/// write it to either the recorded heap Value* target (CFF_FORCE_WB_PTR,
/// used by deep-force passes) or the stackBase-relative slot
/// (CFF_FORCE_WB, used by per-opcode arg pre-forcing) and pop top.
/// Returns true when a writeback was applied (so callers can suppress
/// the CFF_FORCE_RETRY chain — the opcode will re-scan on re-entry).
// #670 (2026-05-19): dropped the `noexcept` qualifier that was here.
// Under macOS clang+libc++ with hardening, out-of-bounds vector access
// traps via `__builtin_trap` (brk #1).  When that fires inside a
// `noexcept` function, the C++ runtime can't unwind cleanly — it routes
// directly to `std::terminate`, which clang compiles to another `brk #1`
// shared across every throw site in `dispatchLoop`.  Result: silent
// SIGTRAP (exit 133) with no diagnostic, no `__cxa_throw` breakpoint,
// no stderr.  Pinned via lldb: link register at trap time pointed to
// the instruction right after `applyForceWriteback(vm)` at vm.cc:4960
// — i.e. the trap was INSIDE this function.  Bounds-check each
// `valueStack.back()` / `valueStack[...]` access and throw a
// `std::runtime_error` with state.  Without `noexcept`, the exception
// propagates to the dispatch-loop catch and surfaces as a normal error.
// Pre-fix, ghc98.drvPath / ghc910 / ghc984 all SIGTRAPped at this
// point; post-fix they surface a typed error (or succeed, if the
// upstream stack-state corruption isn't really a corruption — see
// project_670_ghc98_sigtrap_2026-05-19.md for the diagnostic chain).
// C-1 belt (CODEBASE_REVIEW_2026-06-11): a CFF_FORCE_WB_PTR_KEEP must never
// already be pending when an arm site sets a fresh one — the arm immediately
// `goto op_force_slow`, which services (and now always disarms, PAPs included)
// the KEEP before returning to dispatch.  A stale KEEP here would be the
// protocol regression C-1 fixes.  Under V3_DBG_KEEP_PAP=1 it surfaces loudly;
// in production it is a single predicted branch (cached gate).
[[gnu::always_inline]] inline void armKeepBeltCheck(const CallFrame & f)
{
    static const bool s_on = std::getenv("V3_DBG_KEEP_PAP") != nullptr;
    if (__builtin_expect(s_on && (f.flags & CFF_FORCE_WB_PTR_KEEP), 0))
        std::fprintf(stderr,
            "v3 C-1 belt: arming CFF_FORCE_WB_PTR_KEEP while one is already "
            "pending (forceWriteTarget=%p) — stale-KEEP protocol regression\n",
            (void *)f.forceWriteTarget);
}

// WS-A step 2 counter (see g_sharedWbDetect): report when `target` — a
// forceWriteTarget about to be written in place — lands inside the entries[] of
// a chain parent shared by ≥2 chains.  These are BENIGN (correct-WHNF
// memoisation; see the block comment) — the report is a measurement, not an
// alarm.  Diagnostic only; resolves the interior pointer to its owning Bindings
// via the arena cell-start bitmap, then checks the detector's child-count map.
[[gnu::noinline]] static void detectSharedParentWriteback(const Value * target)
{
    if (!target) return;
    // (1) PROVENANCE ASSERT (WS-A step 3, the precise C-1 guard): the target
    // must still hold the value that was at it when the KEEP was armed.  The
    // intervening op_force_slow only mutates the forced Thunk/App's INTERNAL
    // state (evaluated), never the slot word — so armed.w must equal target->w
    // at fire.  A mismatch means a re-entrant force overwrote the slot between
    // arm and fire: a STALE KEEP about to cross-write an unrelated WHNF into it
    // (the C-1 corruption mechanism).
    {
        auto & am = armedWritebackValue();
        auto ait = am.find(target);
        if (ait != am.end()) {
            if (ait->second.w != target->w) {
                std::fprintf(stderr,
                    "v3 SHARED-WB PROVENANCE VIOLATION: target=%p armed payload "
                    "changed before fire (armed.w=%#llx now=%#llx) — stale KEEP / "
                    "C-1 cross-write\n",
                    (const void *)target,
                    (unsigned long long)ait->second.w,
                    (unsigned long long)target->w);
                if (g_sharedWbAbort) std::abort();
            }
            am.erase(ait);
        }
    }
    // (2) Shared-parent-writeback counter (benign; see the block comment).
    const char * cs = threadArena().findContainingCellStart(target);
    if (!cs) return;
    const Bindings * owner = reinterpret_cast<const Bindings *>(cs);
    if (owner->kind > uint8_t(Bindings::Kind::Chain)) return;  // not a Bindings cell
    const char * lo = reinterpret_cast<const char *>(owner->entries);
    const char * hi = reinterpret_cast<const char *>(owner->entries + owner->size);
    const char * tp = reinterpret_cast<const char *>(target);
    if (tp < lo || tp >= hi) return;  // target is not inside this Bindings' entries[]
    auto & m = chainChildCount();
    auto it = m.find(owner);
    if (it == m.end() || it->second < 2) return;  // not a ≥2-child (shared) parent
    std::fprintf(stderr,
        "v3 SHARED-WB: writeback into chain parent %p shared by %u chains "
        "(target=%p) — benign WHNF memoisation (byte-identical); L1 would "
        "convert this to no-writeback\n",
        (const void *)owner, it->second, (const void *)target);
    if (g_sharedWbAbort) std::abort();
}

inline bool applyForceWriteback(VMState & vm)
{
    if (__builtin_expect(vm.frames.empty(), 0))
        throw std::runtime_error(
            "v3 applyForceWriteback: vm.frames is empty (FWB invariant violated)");
    CallFrame & f = vm.frames.back();
    auto needTop = [&](const char * where) {
        if (vm.valueStack.empty())
            throw std::runtime_error(
                std::string("v3 applyForceWriteback: valueStack empty in ") + where
                + " (flags=" + std::to_string((unsigned)f.flags) + ")");
    };
    if (f.flags & CFF_FORCE_WB_PTR_KEEP) {
        // 2026-05-17: keep-on-stack variant.  Used by opcodes whose
        // architectural contract leaves the selected value on the
        // stack (e.g. OP_ATTRS_SELECT_IC).  Only fires once retVal
        // is in WHNF — non-WHNF stays on the stack and `retry`
        // resumes op_force_slow to chase further.  This keeps the
        // source slot from being polluted with intermediate
        // Tag::App / Tag::Thunk forwarders.
        needTop("CFF_FORCE_WB_PTR_KEEP");
        Value top = vm.valueStack.back();
        Tag t = top.tag();
        // C-1 (CODEBASE_REVIEW_2026-06-11): an under-applied closure-PAP
        // (App/App3 spine bottoming in a Closure with arity>depth) IS WHNF —
        // a partial application.  The old test treated ANY App/App3 top as
        // "not yet WHNF" and `return false`, leaving the KEEP armed with a raw
        // pointer into a (often shared) Bindings entry.  Because a PAP is
        // permanently App-tagged, no later retry could ever disarm it, so the
        // NEXT force in this frame that resolved through op_force_slow fired
        // the stale KEEP and cross-wrote ITS WHNF (e.g. the String "21" from
        // forcing llvmVersion) into the old entry — the firefox getLib="21"
        // residual.  Treat the PAP as WHNF: write it (the PAP is the entry's
        // true value) and disarm below.
        bool nonWhnf = (t == Tag::Thunk || t == Tag::App || t == Tag::App3
                        || t == Tag::Slot)
                       && !isUnderappliedClosurePap(top);
        if (nonWhnf)
            return false;
        // C-1 falsifier counter: how often the KEEP branch fires on a PAP top
        // (the previously-leaked case).  Nonzero on firefox.drvPath confirms
        // the mechanism is live.  Retirement criterion: delete this counter
        // once the firefox getLib residual is closed and byte-identical to TW.
        if (__builtin_expect(isUnderappliedClosurePap(top), 0))
            ++g_keepPapDisarmCount;
        // C-4 (CODEBASE_REVIEW_2026-06-11): never memoize the transient
        // Blackhole sentinel into shared storage — it means "value not yet
        // known" (a self-cycle in progress), not a real value.  Disarm without
        // writing so a later read re-forces rather than seeing a permanent
        // Blackhole.
        // Phase D coverage: forceWriteTarget can point into a
        // Bindings entry (OP_ATTRS_SELECT_DYN / IC path) or into a
        // stack slot.  We don't track the container here, so route
        // through `cellWrite` with cellContainer=nullptr — the
        // standalone-cell registry catches inter-gen writes.
        if (t != Tag::Blackhole && f.forceWriteTarget) {
            if (__builtin_expect(g_sharedWbDetect, 0))
                detectSharedParentWriteback(f.forceWriteTarget);  // WS-A step 2
            cellWrite(f.forceWriteTarget, top, nullptr);
        }
        f.forceWriteTarget = nullptr;
        f.flags &= ~CFF_FORCE_WB_PTR_KEEP;
        return true;
    }
    if (f.flags & CFF_FORCE_WB_PTR) {
        needTop("CFF_FORCE_WB_PTR");
        Value forced = vm.valueStack.back();
        vm.valueStack.pop_back();
        // Phase D: same standalone-cell route as above.
        // C-4/C-8 (CODEBASE_REVIEW_2026-06-11): never memoize the transient
        // Blackhole sentinel ("value not yet known", a self-cycle in progress)
        // into the target slot/cell — leave the target at its pre-force value
        // so a later read re-forces, rather than baking in a permanent
        // Blackhole that reads fail on where TW succeeds. (A forced PAP IS a
        // valid WHNF here and is written normally.)
        if (f.forceWriteTarget && forced.tag() != Tag::Blackhole) {
            if (__builtin_expect(g_sharedWbDetect, 0))
                detectSharedParentWriteback(f.forceWriteTarget);  // WS-A step 2
            cellWrite(f.forceWriteTarget, forced, nullptr);
        }
        f.forceWriteTarget = nullptr;
        f.flags &= ~CFF_FORCE_WB_PTR;
        return true;
    }
    if (!(f.flags & CFF_FORCE_WB)) return false;
    uint16_t off = getForceWriteback(f);
    needTop("CFF_FORCE_WB");
    Value forced = vm.valueStack.back();
    vm.valueStack.pop_back();
    // #670: bounds-check the slot write.  After pop_back, the
    // valueStack size shrank by 1, so the target index must be
    // < the post-pop size.  When defer-on + with-slot-chase is on,
    // certain emit-time corruptions produced a writeback slot that
    // pointed BEYOND the current frame's reserved locals, hitting
    // the libc++ hardened operator[] trap.
    size_t idx = (size_t)f.stackBaseOffset + (size_t)off;
    if (__builtin_expect(idx >= vm.valueStack.size(), 0)) {
        throw std::runtime_error(
            "v3 applyForceWriteback: writeback slot out of bounds "
            "(stackBase=" + std::to_string(f.stackBaseOffset)
            + " off=" + std::to_string(off)
            + " size=" + std::to_string(vm.valueStack.size()) + ")");
    }
    vm.valueStack[idx] = forced;
    clearForceWriteback(f);
    return true;
}

/// NIX_TRACE_EVAL helpers used by OP_FORCE / OP_RETURN to emit
/// F / W events whenever a CFF_THUNK_RETURN frame is pushed or
/// popped.  Definitions hoisted here so both dispatchLoop (which
/// contains OP_FORCE / OP_RETURN) and forceValue (defined later)
/// can call them with consistent formatting.
static inline std::string v3ValueTypeName(Value v)
{
    switch (v.tag()) {
    case Tag::Int:       return "Int";
    case Tag::Float:     return "Float";
    case Tag::Bool:      return "Bool";
    case Tag::Null:      return "Null";
    case Tag::String:    return "String";
    case Tag::Path:      return "Path";
    case Tag::List: {
        char b[32];
        std::snprintf(b, sizeof b, "List(%zu)",
            v.asList() ? (size_t)v.asList()->size : (size_t)0);
        return b;
    }
    case Tag::Attrs: {
        char b[32];
        std::snprintf(b, sizeof b, "Attrs(%zu)",
            v.asAttrs() ? (size_t)v.asAttrs()->size : (size_t)0);
        return b;
    }
    case Tag::Closure:   return "Lambda";
    case Tag::PrimOp:    return "Lambda";
    case Tag::PrimOpApp: return "Lambda";
    case Tag::Thunk:     return "Thunk";
    case Tag::App:       return "App";
    case Tag::App3:      return "App3";
    case Tag::Blackhole: return "Blackhole";
    case Tag::External:  return "External";
    case Tag::Slot:      return "Slot";
    case Tag::Uninitialized: return "Uninitialized";
    }
    return "?";
}

static inline std::string v3ThunkTracePos(const Thunk * t)
{
    if (!t) return "<no-pos>";
    // 2026-05-18: disambiguate <no-pos> cases for cc-wrapper bisection.
    // Pre-fix the kind annotation when the thunk lacks position info so
    // the cross-evaluator NIX_TRACE_EVAL diff makes the source of the
    // mystery thunk visible.  Gated on NIX_TRACE_EVAL_VERBOSE_NOPOS so
    // normal trace stays clean for fixture comparisons.
    static const bool s_verboseNoPos =
        std::getenv("NIX_TRACE_EVAL_VERBOSE_NOPOS") != nullptr;
    auto formatNoPos = [t](const char * label) -> std::string {
        if (!s_verboseNoPos) return "<no-pos>";
        char buf[96];
        std::snprintf(buf, sizeof buf,
            "<no-pos:%s t=%p st=%d>", label, (void *)t, (int)t->state);
        return std::string(buf);
    };
    if (t->state == ThunkState::Evaluated) return formatNoPos("evald");
    if (t->state == ThunkState::Native) return formatNoPos("native");
    const LambdaDescriptor * d = t->suspended.desc;
    if (!d) return formatNoPos("nodesc");
    const PosSnapshot * ps = resolvePosSnapshot(d->posHandle);
    if (!ps) return formatNoPos("noresol");
    if (ps->file.empty()) return formatNoPos("emptyfile");
    return nix::evalTrace::formatPos(ps->file, ps->line, ps->column);
}

/// V3_DBG_HOT_FORCE=<suffix>: count total Suspended-thunk dispatches
/// and unique Thunk* pointers at any position whose normalised
/// `<store>/...` file path ENDS WITH the configured suffix.  Used to
/// distinguish "same thunk forced many times" (memoisation failure)
/// from "fresh thunk allocated per access" (per-access allocation
/// failure).  Atexit prints a summary.  Suffix matching keeps the
/// option robust against the random `<hash>-source` prefix.
///
/// Example: V3_DBG_HOT_FORCE='lib/systems/parse.nix:61:44'
///   -> total=11504278 unique=1     -> same thunk re-forced (memoisation)
///   -> total=11504278 unique=11504 -> fresh thunk per access (allocation)
static inline void hotForceCheck(const Thunk * t)
{
    static const char * s_suffix = std::getenv("V3_DBG_HOT_FORCE");
    if (__builtin_expect(s_suffix == nullptr, 1)) return;
    if (!t) return;
    const LambdaDescriptor * d = t->suspended.desc;
    if (!d) return;
    const PosSnapshot * ps = resolvePosSnapshot(d->posHandle);
    if (!ps || ps->file.empty()) return;
    std::string pos = nix::evalTrace::formatPos(ps->file, ps->line, ps->column);
    std::string_view suf{s_suffix};
    if (pos.size() < suf.size()) return;
    if (std::string_view(pos).substr(pos.size() - suf.size()) != suf) return;

    static struct HotForceStats {
        std::atomic<uint64_t> total{0};
        std::mutex mtx;
        std::unordered_set<const Thunk *> uniq;
        std::string suffix;
        bool atexitInstalled = false;
    } stats;

    uint64_t n = stats.total.fetch_add(1, std::memory_order_relaxed) + 1;
    // Periodic dump every 100k forces so the count is visible under
    // `timeout`-killed runs (atexit doesn't fire when SIGTERM'd).
    if ((n % 100000) == 0) {
        std::lock_guard<std::mutex> g(stats.mtx);
        stats.uniq.insert(t);
        std::fprintf(stderr,
            "v3 V3_DBG_HOT_FORCE [%s] progress: total=%llu unique=%zu\n",
            s_suffix,
            (unsigned long long)n, stats.uniq.size());
        std::fflush(stderr);
    } else {
        std::lock_guard<std::mutex> g(stats.mtx);
        stats.uniq.insert(t);
    }
    {
        std::lock_guard<std::mutex> g(stats.mtx);
        if (stats.suffix.empty()) stats.suffix = s_suffix;
        if (!stats.atexitInstalled) {
            stats.atexitInstalled = true;
            std::atexit([] {
                std::lock_guard<std::mutex> g(stats.mtx);
                std::fprintf(stderr,
                    "v3 V3_DBG_HOT_FORCE [%s]: total=%llu unique-thunks=%zu "
                    "ratio=%.1f\n",
                    stats.suffix.c_str(),
                    (unsigned long long)stats.total.load(),
                    stats.uniq.size(),
                    stats.uniq.empty() ? 0.0
                        : (double)stats.total.load() / (double)stats.uniq.size());
            });
        }
    }
}

/// Run the dispatch loop on `vm` until either:
///   - OP_HALT is reached (top-level exit), or
///   - The frame stack is popped down to `exitDepth` (used by inner
///     re-entries from callback primops to return to the caller).
/// Returns the final value (whatever was on the operand stack at exit).
///
/// 2026-05-17: an internal try/catch around the main while loop calls
/// `clearBlackMarksOnException(vm, exitDepth)` before re-throwing.
/// Callers (forceValue / callClosure / runOnExistingVm) historically
/// did this cleanup themselves; moving it inside dispatchLoop lets the
/// callers tail-call dispatchLoop instead of holding their C-frame
/// open to catch the exception.  Single-call-site cleanup → caller
/// frames can be elided (saves ~1.2-1.7 KB per Suspended-thunk
/// dispatch).
} // close current anon ns (briefly)
} // close namespace nix::v3 (briefly)

// #698 Phase 3 diagnostic: thread-local pointer to the current
// dispatchLoop's vm.  Set at dispatch entry, restored at exit.
// Read by limits.cc's V3_DBG_TRAP_ON_LIMIT to dump frame state on
// wall-time / cpu-time / heap-cap abort.  Cheap (one thread-local
// store on entry/exit, no per-instruction cost).
namespace nix::v3 {
    // EXIT_GC_SPIRAL Day 13-15 (2026-05-29): external-linkage wrappers
    // around the internal capWiths stats.  The implementations live in
    // an anonymous namespace at the top of this TU; these wrappers
    // export them so run.cc's NIX_VM_STATS dump can read the counters.
    uint64_t getCapWithsHits()   noexcept { return internalCapWithsHits(); }
    uint64_t getCapWithsMisses() noexcept { return internalCapWithsMisses(); }
    uint64_t getCapWithsEvicts() noexcept { return internalCapWithsEvicts(); }

    thread_local VMState * tlCurrentDispatchVM = nullptr;
    VMState * currentDispatchVM() { return tlCurrentDispatchVM; }

    // #790 (2026-05-23) OPCYCLES inter-dispatch-loop attribution fix.
    // File-scope thread_locals so dispatchLoop entry/exit can save+
    // restore the prev-op state across nested dispatch boundaries.
    // The OPCYCLES sample code (vm.cc dispatch loop) reads/writes
    // these directly.  Without this, an opcode that EXITS a dispatch
    // loop (OP_RETURN at exitDepth) leaves `g_opcyclesPrevOp` set;
    // the next dispatch in a different (later) dispatch loop credits
    // ALL the inter-loop outer-C++ work to that opcode.  Per #787 RCA
    // this was crediting ~360 ms to OP_RETURN on hello.drvPath that
    // actually lived in primop continuations + callClosure cleanup.
    //
    // The fix: dispatchLoop entry saves the outer (g_opcyclesPrevOp,
    // g_opcyclesPrevTs) to locals and resets them to (0xFF, 0).
    // Exit restores them.  Nested opcodes' first dispatch sees
    // prevOp=0xFF and skips credit (clean boundary).  The outer's
    // NEXT sample after the nested loop exit sees prevOp=OUTER_OP
    // with prevTs from BEFORE the nested call — correctly crediting
    // the ENTIRE outer-case-body-including-nested time to the outer
    // opcode.
    thread_local uint8_t g_opcyclesPrevOp = 0xFF;
    thread_local uint64_t g_opcyclesPrevTs = 0;

    // #705 (2026-05-21): thread-local stack of active VMStates.
    //
    // Without this, a nested runFunctionWithUpvalues / runFunction
    // creates a new local VMState whose dispatch fires its own
    // scavenge.  The OUTER VMState's frames hold nursery closure /
    // thunk pointers that the inner scavenge doesn't walk — the
    // shared nursery is memset, leaving outer's f.closure stale.
    //
    // The scavenger walks ALL entries here, so every VMState on the
    // call chain has its roots forwarded.  Same VM may appear
    // multiple times under forceValue / inner-dispatch re-entries;
    // scavenge dedupes via its `walked` set.
    //
    // Identified 2026-05-21 by V3_DBG_NURSERY_BRUTE + crash
    // diagnostic: a stale closure at frame[50] with 0 arena refs
    // (because the only reference is in vm.frames of an OUTER vm
    // not seen by the inner scavenge).
    thread_local std::vector<VMState *> tlActiveVMStack;
    void pushActiveVMState(VMState * vm) { tlActiveVMStack.push_back(vm); }
    void popActiveVMState(VMState * vm) {
        // Pop the matching VM (typically the back, but defensively
        // search for it in case of exception unwind ordering issues).
        if (!tlActiveVMStack.empty() && tlActiveVMStack.back() == vm) {
            tlActiveVMStack.pop_back();
            return;
        }
        for (auto it = tlActiveVMStack.rbegin(); it != tlActiveVMStack.rend(); ++it) {
            if (*it == vm) {
                tlActiveVMStack.erase(std::next(it).base());
                return;
            }
        }
    }
    const std::vector<VMState *> & activeVMStack() { return tlActiveVMStack; }
}

namespace nix::v3 {
namespace { // re-open anon ns

Value dispatchLoop(VMState & vm, size_t exitDepth, bool reuseScope = false)
{
    // Stage 2 (LIST_ITERATION_FIX_PLAN_2026-06-08): per-element callback
    // re-entry (primFoldl/primFoldlMap → callClosure2 → here, once per fold
    // element) re-does the active-VM/current-VM bookkeeping every time.  But
    // the fold primop already runs UNDER this vm's dispatch (the outer
    // OP_CALL_PRIMOP), so `vm` is already current + on the active stack;
    // re-pushing it is pure per-element overhead.  reuseScope=true (passed
    // only from such callers) skips it.  Exception-safe (nothing pushed ⇒
    // nothing to unwind), and the distinct-vm scavenge checks (`vmp != &vm`)
    // are unaffected because `vm` stays on the stack from the outer push.
    // NB: the TLS this avoids is NOT the lever (Stages 0/1 falsified that —
    // _tlv leaf-time is a sampling artifact); the win is skipping the
    // std::vector push/pop + frame bookkeeping of the re-entry.  Measured
    // ~−5.7% foldl; the body dispatch that remains is the interpreter ceiling.
    VMState * prevDispatchVM = reuseScope ? nullptr : tlCurrentDispatchVM;
    if (!reuseScope) {
        tlCurrentDispatchVM = &vm;
        // #705: register on the active-VM stack so nested scavenges
        // walk THIS vm's roots even when fired from another dispatch.
        pushActiveVMState(&vm);
    }

    // #790 (2026-05-23) OPCYCLES inter-dispatch-loop boundary.
    // Save the OUTER prev-op/Ts before this nested loop runs.
    // Reset to 0xFF so the first opcode of this loop doesn't get
    // credited with whatever was on the outer frame.  Restore on
    // exit (via destructor below) so the outer's NEXT sample
    // correctly credits its OWN previous opcode with the time
    // from BEFORE the nested call (entire outer-case-body cost
    // attributed to the outer opcode).
    const uint8_t  savedOpcyclesPrevOp = g_opcyclesPrevOp;
    const uint64_t savedOpcyclesPrevTs = g_opcyclesPrevTs;
    g_opcyclesPrevOp = 0xFF;
    g_opcyclesPrevTs = 0;

    struct VMScope {
        VMState * prev;
        VMState * vm;
        uint8_t   savedOpcyclesPrevOp;
        uint64_t  savedOpcyclesPrevTs;
        bool      reuseScope;
        ~VMScope() {
            // Restore the outer prev-op/Ts so the outer's NEXT
            // OPCYCLES sample credits the entire outer-case-body
            // (including this nested loop's runtime) to the OUTER
            // opcode — semantically correct: that's what the outer
            // case body did.
            g_opcyclesPrevOp = savedOpcyclesPrevOp;
            g_opcyclesPrevTs = savedOpcyclesPrevTs;
            // Stage 2: only unwind the active-VM/current-vm bookkeeping if we
            // set it up (reuseScope=false); when reused, the outer dispatch
            // owns it and pops it via its own VMScope.
            if (!reuseScope) {
                popActiveVMState(vm);
                tlCurrentDispatchVM = prev;
            }
        }
    } _vmScope{prevDispatchVM, &vm, savedOpcyclesPrevOp, savedOpcyclesPrevTs, reuseScope};

    const CallFrame & topFrame = vm.frames.back();
    const CompilationUnit * cu = topFrame.cu;
    uint32_t ip = topFrame.ip;
    const Closure * closure = topFrame.closure;
    size_t stackBase = topFrame.stackBaseOffset;

    Value finalResult{};
    finalResult.mkNull();

    // V3_DUMP_AT_START=1: on entry, dump every lambda descriptor in
    // the top frame's CU.  Useful for profiling tools that want to
    // see the full bytecode without forcing an error condition.
    // Idempotent (we'd dump on every dispatchLoop entry but the
    // outer call gates on initial entry depth).
    static const bool s_dump_at_start =
        std::getenv("V3_DUMP_AT_START") != nullptr;
    if (__builtin_expect(s_dump_at_start, 0)) [[unlikely]] {
        if (cu) {
            static std::set<const CompilationUnit *> dumpedCus;
            if (dumpedCus.insert(cu).second) {
                std::fprintf(stderr,
                    "  V3_DUMP_AT_START: cu=%p (%zu lambdas, code.size=%zu)\n",
                    (const void *)cu, cu->lambdas.size(), cu->code.size());
                for (size_t li = 0; li < cu->lambdas.size(); ++li) {
                    const auto & d = cu->lambdas[li];
                    uint32_t end = (li + 1 < cu->lambdas.size())
                        ? cu->lambdas[li + 1].codeOffset
                        : static_cast<uint32_t>(cu->code.size());
                    std::fprintf(stderr,
                        "    L[%zu] code=[%u..%u) nUp=%u nLocals=%u nWiths=%u name=%s\n",
                        li, (unsigned)d.codeOffset, end,
                        (unsigned)d.nUpvalues, (unsigned)d.nLocals,
                        (unsigned)d.nWithTargets,
                        d.name.empty() ? "<anon>" : d.name.c_str());
                    disassembleWindow(stderr, *cu, d.codeOffset, end);
                }
            }
        }
    }

    bool running = true;
    // Gate the per-instruction counter behind an env var: it adds a
    // memory write to every instruction and is only useful for
    // profiling.  Overhead on fib32 was ~3% on first-run timings.
    static const bool s_kCountInstructions = std::getenv("NIX_VM_STATS") != nullptr;
    // V3_DBG_TRACE_THUNK_BODY: per-instruction trace gated on the
    // currently-running thunk frame having a specific (codeOffset, nUp).
    // Used to nail down WC-37 frame/thunk mismatch. Format:
    //   V3_DBG_TRACE_THUNK_BODY=1346,5  ← trace any thunk with codeOffset
    //                                     1346 and nUp=5
    static const char * s_trace_env_static = std::getenv("V3_DBG_TRACE_THUNK_BODY");
    static const uint32_t s_trace_codeoff_static =
        s_trace_env_static ? static_cast<uint32_t>(std::strtoul(s_trace_env_static, nullptr, 10)) : 0;
    static const uint16_t s_trace_nup_static = []() -> uint16_t {
        const char * e = std::getenv("V3_DBG_TRACE_THUNK_BODY");
        if (!e) return 0;
        const char * comma = std::strchr(e, ',');
        if (!comma) return 0;
        return static_cast<uint16_t>(std::strtoul(comma + 1, nullptr, 10));
    }();
    // Profile (post-#062e3c502): even with [[unlikely]], the compiler
    // kept reloading the function-local statics every iteration --
    // 478 + 277 = 755 samples on the two checks (~15% of dispatchLoop
    // time on fib38).  Promote to plain function-scope const locals
    // so the loop sees them as loop-invariant load-once values; the
    // compiler then hoists them entirely out of the inner loop and
    // the trace branch becomes a single dead-code path under -O2.
    const bool kCountInstructions = s_kCountInstructions;
    const char * const s_trace_env = s_trace_env_static;
    const uint32_t s_trace_codeoff = s_trace_codeoff_static;
    const uint16_t s_trace_nup = s_trace_nup_static;
    // P-6 (CODEBASE_REVIEW_2026-06-11): fold the per-dispatch *default-off*
    // diagnostic gates into ONE loop-invariant disjunction.  Without it the
    // hot path pays ~6 separate branch-predicted-not-taken tests every
    // iteration (periodic-live-trace, thunk-body-trace, instr-count, limit
    // poll, opcycles, opcounts).  Every term is either a startup env const or
    // `limitsActive()` (set once per run by initLimits() before dispatch
    // begins, constant for this dispatchLoop invocation), so the disjunction
    // is loop-invariant and the compiler hoists it; the common no-gate path
    // then tests a single bool instead of six.  The major-GC safepoint is
    // NOT folded in here — it is default-ON, not a diagnostic gate.
    // Retire when computed-goto dispatch lands (the bigger lever per P-6;
    // measure the mask first — measure-twice).
    const bool kAnySlowGate =
        kCountInstructions | (s_trace_env != nullptr)
        | g_periodicLiveTrace | g_countOpcodes | g_countOpCycles
        | nix::v3::limitsActive();
    // Cheney nursery (#434 Phase C): scavenge gate at top-of-loop.
    // We avoid the per-iteration env-var check by promoting the gate
    // to a function-scope const.  When nursery+scavenge are both on,
    // every Nth iteration we sync `ip` back to the current frame
    // (so root-walking sees consistent state) and ask the nursery
    // whether it wants to scavenge.  If it does, we re-read the
    // dispatch locals from `vm.frames.back()` because frame
    // pointers may have been forwarded in place.
    //
    // CRITICAL: scavenge ONLY runs in the outermost dispatchLoop
    // (`exitDepth == 0`).  Inner dispatchLoops are entered from
    // C++ helpers (forceValue, runOnExistingVm, runFunction) that
    // hold v3 nursery pointers in C-stack locals across the call
    // — those locals are NOT in any walked root set.  If we
    // scavenged inside an inner dispatchLoop, the outer caller's
    // popped-but-still-used `Value`s would have stale payload
    // pointers after the call returned.  By restricting scavenge
    // to the outermost loop, every C++ local that holds a
    // potentially-nursery pointer is one of:
    //   (1) bounded by an opcode handler that runs to completion
    //       within ONE iteration (no scavenge can interleave); or
    //   (2) on `vm.valueStack` / in a `vm.frames[]` slot (which
    //       the scavenger walks).
    // Cost: re-entry chains let the nursery fill to its overflow
    // ceiling before they exit; the next outer iteration will then
    // reclaim.  Acceptable because re-entry depth is bounded by
    // the call depth, and primop callbacks return promptly.
    // OPT-OUT RETIRED (2026-06-15): nursery + scavenge are unconditional (the
    // flip soaked clean across all of nixpkgs on darwin-4).  These mirror
    // nursery.hh enabled/scavengeEnabled and barrier.cc g_phaseDActive (all now
    // hard-true).  The NIX_V3_NURSERY_SIZE / _TRIGGER_PCT tuning knobs (read in
    // nursery.hh) and V3_DBG_GC_STRESS (below) still apply.
    static const bool s_kNurseryOn_static = true;
    static const bool s_kScavengeOn_static = true;
    // Stage 3 prereq (action plan Phase 1.7): V3_DBG_GC_STRESS=N
    // forces a scavenge every N opcodes regardless of nursery
    // occupancy.  Surfaces missed-root bugs that natural scavenge
    // frequency hides.  Retirement criterion: retire this gate once
    // Stage 3 ships nursery-default-on AND
    // `./run-brute-audit.sh` passes under
    // `V3_DBG_GC_STRESS=10` on the full nixpkgs slice in CI for two
    // consecutive weeks without a new missed-root finding.
    //
    // 0 / unset → STRESS off.  N > 0 → scavenge every N dispatch
    // iterations at exitDepth==0.  N capped at 10^6 to avoid silent
    // typos (`STRESS=1000000000` would effectively disable STRESS).
    //
    // The action plan calls for "thread-local opcode-counter
    // decrement at the dispatch loop top"; we use a per-
    // dispatchLoop-entry uint32_t counter (allocated on the C stack,
    // not thread_local) which is fine because the action's
    // requirement is per-thread monotonicity, not cross-loop state.
    // Re-entries (forceValue → inner dispatchLoop) get their own
    // counter, which is intentional — inner loops have exitDepth>0
    // and don't fire scavenge anyway.
    static const uint32_t s_kGcStressBudget = []() -> uint32_t {
        const char * v = std::getenv("V3_DBG_GC_STRESS");
        if (!v || v[0] == '\0' || v[0] == '0') return 0;
        long n = std::strtol(v, nullptr, 10);
        if (n <= 0 || n > 1000000) return 0;
        return static_cast<uint32_t>(n);
    }();
    const bool kNurseryGate = (s_kNurseryOn_static && s_kScavengeOn_static
                               && exitDepth == 0)
                              || (s_kGcStressBudget > 0
                                  && s_kNurseryOn_static
                                  && exitDepth == 0);
    // Cache the per-thread Nursery* once per dispatchLoop entry to
    // avoid the thread_local re-resolution per iteration (Darwin's
    // tlv_atomic_thunk is cheap but not free; on fib33 the per-
    // iteration cost was visible in -fprofile-generate runs).
    // `nullptr` when the gate is off — the check below short-circuits.
    Nursery * const nursery = kNurseryGate ? &threadNursery() : nullptr;
    // STRESS countdown — only meaningful when nursery != nullptr +
    // s_kGcStressBudget > 0.  Initialized at budget; decrements
    // every iteration; fires force-scavenge + reset on zero.
    uint32_t gcStressCountdown = s_kGcStressBudget;

    // 2026-05-17: internal exception barrier.  Any exception that
    // escapes the dispatch loop (from a primop body, from forceValue,
    // from any opcode handler) triggers clearBlackMarksOnException
    // BEFORE re-throwing.  Centralizes the cleanup that callers
    // (forceValue / callClosure / runOnExistingVm) used to do, letting
    // them tail-call dispatchLoop without holding their C-frame open
    // to catch.  Reverts Blackhole→Suspended on pushed thunk frames,
    // unwinds vm.frames / valueStack / withStack down to exitDepth,
    // clears CFF_FORCE_RETRY on the surviving top frame.  See
    // clearBlackMarksOnException's docstring for the protocol details.
    try {
    while (running) {
        // Phase C scavenge trigger.  Only inspected when the gate
        // is on (kNurseryGate covers env-var + exitDepth == 0).
        // The shouldScavenge() body is a small arithmetic compare;
        // under -O2 with the [[unlikely]] hint the whole branch
        // folds to a single conditional jump on the hot path.
        if (__builtin_expect(nursery != nullptr, 0)) [[unlikely]] {
            // STRESS countdown — gated by s_kGcStressBudget > 0.
            // Decrement every iteration; when it hits 0, force a
            // scavenge regardless of `shouldScavenge`.  Reset to
            // budget on fire.  When STRESS is off (budget == 0)
            // the counter stays at 0 and `stressFire` evaluates
            // false on every iteration — the branch is fully
            // predicted-not-taken on production builds.
            bool stressFire = false;
            if (s_kGcStressBudget > 0) {
                if (gcStressCountdown == 0)
                    gcStressCountdown = s_kGcStressBudget;
                else
                    --gcStressCountdown;
                stressFire = (gcStressCountdown == 0);
            }
            if (stressFire || nursery->shouldScavenge()) {
                // #705 N1-followup (2026-05-21): defer scavenge if a
                // NESTED VMState exists on the active stack.  When a
                // primop body creates a secondary VMState via
                // `runFunctionWithUpvalues`, the secondary's
                // dispatchLoop has exitDepth==0 on its own vm — so
                // this gate fires there too.  But the OUTER primop
                // body's C-stack locals (Values holding nursery
                // pointers passed as args, intermediate results,
                // etc.) are invisible to the scavenger — only
                // vm.valueStack/withStack/frames are walked.  If we
                // scavenge while the outer body is mid-execution,
                // those C-locals dangle, producing the
                // hello.outPath / hello.drvPath stale-callee crashes.
                //
                // The defer is conservative: scavenge only fires when
                // every active VMState on the thread is THIS vm.
                // Same-vm re-entries (forceValue → inner dispatchLoop)
                // push the same pointer multiple times — that's fine.
                bool nestedDistinct = false;
                for (VMState * vmp : activeVMStack()) {
                    if (vmp && vmp != &vm) { nestedDistinct = true; break; }
                }
                if (nestedDistinct) {
                    // Skip; next outer iteration with single-vm
                    // active stack will reclaim.  Cost: nursery may
                    // overshoot its threshold under primop chains
                    // that nest runFunctionWithUpvalues — bounded by
                    // NIX_V3_MAX_HEAP.  STRESS mode also has to
                    // defer here: forcing a scavenge under a nested
                    // VMState would corrupt the outer's C-locals.
                    // The countdown stays at 0, so the next outer
                    // iteration with single-vm active stack will
                    // fire.
                    static const bool s_dbg =
                        std::getenv("V3_DBG_NURSERY") != nullptr;
                    if (__builtin_expect(s_dbg, 0)) {
                        std::fprintf(stderr,
                            "[v3 nursery] DEFER scavenge — nested VMState "
                            "active (stack size=%zu, current=%p)\n",
                            activeVMStack().size(), (void*)&vm);
                    }
                    goto skip_scavenge;
                }
                // Sync ip into the frame so the scavenger walks a
                // consistent VM state.  ip is a per-iteration
                // running counter; valueStack/withStack/frames are
                // already source-of-truth.
                if (!vm.frames.empty()) vm.frames.back().ip = ip;
                // STRESS path uses forceScavenge (bypasses both the
                // `scavengeEnabled` env var AND the `shouldScavenge`
                // threshold).  Natural-frequency path stays on
                // `maybeScavenge` so production builds without
                // _SCAVENGE=1 still don't scavenge.
                const bool ran = stressFire
                    ? nursery->forceScavenge(vm)
                    : nursery->maybeScavenge(vm);
                if (ran) {
                    // Frame pointers may have been forwarded.  Re-
                    // read the dispatch locals from the top frame.
                    if (!vm.frames.empty()) {
                        auto & f = vm.frames.back();
                        cu        = f.cu;
                        closure   = f.closure;
                        stackBase = f.stackBaseOffset;
                        ip        = f.ip;
                    }
                }
            }
        }
        skip_scavenge:;
        // Stage 6 Day 6 — major mark-sweep trigger (replaces Day 3
        // Cheney scavenger per STAGE_6_CHENEY_FALSIFIED_2026-05-27
        // + GC_DESIGN_POST_CHENEY_2026-05-28).
        //
        // Gate: NIX_V3_MAJOR_GC=1 (default OFF).  When enabled, fires
        // runMajorMarkSweep when arena's active region grows past
        // NIX_V3_MAJOR_GC_THRESHOLD_MB (default 256 MB).
        //
        // Nested-VMState defer mirrors nursery scavenger logic: major
        // GC has the same safe-point constraint (C-locals in primop
        // bodies holding arena pointers would dangle if cells moved).
        // Flat MS does NOT move cells, so the dangling-C-local risk
        // is null — but the structural defer is retained for safety +
        // to ensure mark sees a consistent VM state.
        //
        // Trigger logic + dynamic threshold + nested-VMState defer
        // retained from Day 4 Cheney work (per GC_DESIGN_POST_CHENEY
        // §4.6 "what survives from the Cheney work").
        // Single source of truth: the centralized major-GC gate
        // (alloc.hh g_majorGcEnabled) — DEFAULT-ON 2026-06-04 (no-Boehm).
        // Previously a separate getenv here left the bookkeeping ON but
        // the TRIGGER OFF when only alloc.hh's default flipped.
        const bool s_majorGcEnabled = Arena::majorGcEnabled();
        // FP-4 Shape A: generational-major is active when NIX_V3_GEN_MAJOR is set
        // AND the nursery is on (nursery != nullptr).  It reuses the major-GC
        // safepoint block below, prepending a forceScavenge so the major mark
        // sees no nursery cells (M-3).
        const bool s_genMajor = g_genMajorEnabled && nursery != nullptr;
        // P-1 (CODEBASE_REVIEW_2026-06-11): test the CHEAP local `exitDepth==0`
        // FIRST.  The major-GC safepoint only ever fires in the OUTERMOST
        // dispatch loop, so nested dispatch loops (every callClosure2 per-
        // element re-entry, every primop callback) previously paid the
        // threadArena() TLS deref + s_majorGcThresholdBytes TLS + bytesAllocated()
        // per dispatched opcode for a check that can NEVER fire there.  Gating
        // on exitDepth==0 up front skips the whole block for nested loops; the
        // threshold/growth tunables are now file-scope (no per-opcode magic-
        // static guard).  Correctness-neutral: the firing condition is
        // unchanged (exitDepth==0 && bytesAllocated >= threshold && !nested).
        if ((s_majorGcEnabled || s_genMajor) && exitDepth == 0) {
            // Dynamic threshold (thread_local mutable state): starts at the
            // file-scope initial; after each scavenge raises to
            // `max(initial, post_scavenge_arena * growth)` so a scavenge whose
            // live-set ALONE exceeds the initial threshold doesn't re-fire on
            // the very next iteration (HNE 2026-05-27: 474 MB live, 256 MB
            // threshold → re-fire every tick, CPU pegged, no progress).
            static thread_local size_t s_majorGcThresholdBytes =
                g_majorGcInitialThresholdBytes;
            Arena & arena = threadArena();
            if (arena.bytesAllocated() >= s_majorGcThresholdBytes)
            {
                // Nested-VMState defer (same as nursery).
                bool nestedDistinct = false;
                for (VMState * vmp : activeVMStack()) {
                    if (vmp && vmp != &vm) {
                        nestedDistinct = true;
                        break;
                    }
                }
                if (!nestedDistinct) {
                    // Sync ip into the frame so the scavenger
                    // walks a consistent VM state.
                    if (!vm.frames.empty()) vm.frames.back().ip = ip;
                    // FP-4 Shape A: empty the nursery FIRST (promote all young
                    // survivors to tenured, single-region) so the major mark
                    // below sees no nursery-resident cells — the M-3 invariant
                    // that lets the major run safely under the nursery.
                    // forceScavenge forwards frames; the dispatch-local re-read
                    // after runMajorMarkSweep (below) picks up the final frames.
                    if (s_genMajor) nursery->forceScavenge(vm);
                    // R2.4d (2026-06-04) INLINE-CACHE INVALIDATION before
                    // the major GC.  attrSelectCache + recSlotCache hold
                    // raw Bindings* per call site; walkAllV3Roots does NOT
                    // walk them (the libc-resident CU slots are invisible
                    // to the precise walk + the C-stack scan), so a
                    // Bindings reachable ONLY via an IC would be swept (or
                    // its block munmapped) while the IC still points at it
                    // → use-after-free on the next IC hit (the #705 class;
                    // code-review findings #1/#6/#7/#8).  The caches are
                    // transient: clearing is always sound (they repopulate
                    // on the next lookup) AND lets the GC reclaim
                    // IC-pinned-only Bindings.  Simpler + safer than
                    // walking them as roots; fires only at the (infrequent)
                    // major-GC safepoint so the cold-cache cost is bounded.
                    // (attrSelectCache/recSlotCache are `mutable`, so this
                    // works through the registry's `const CompilationUnit*`.)
                    for (const CompilationUnit * icu : cuRegistry()) {
                        if (!icu) continue;
                        for (auto & ic : icu->attrSelectCache)
                            for (auto & e : ic.entries) e.bindings = nullptr;
                        for (auto & rc : icu->recSlotCache) rc.bindings = nullptr;
                    }
                    // M-1 (CODEBASE_REVIEW_2026-06-11): the Bindings::materialize
                    // memo is another raw-Bindings* side table that
                    // walkAllV3Roots does NOT walk — same UAF/aliasing hazard
                    // as the ICs above.  Clear it before the mark/sweep so no
                    // stale chain pointer survives a collection.
                    Bindings::clearMaterializeMemo();
                    const MajorGcResult gcr = runMajorMarkSweep(vm);
                    // Frame pointers may have been forwarded.
                    // Re-read dispatch locals.
                    if (!vm.frames.empty()) {
                        auto & f = vm.frames.back();
                        cu        = f.cu;
                        closure   = f.closure;
                        stackBase = f.stackBaseOffset;
                        ip        = f.ip;
                    }
                    // Raise threshold: don't re-fire until arena
                    // grows by `growth-factor × live_set`.
                    size_t liveSet = arena.bytesAllocated();
                    size_t nextThreshold =
                        static_cast<size_t>(liveSet * g_majorGcGrowth);
                    // PLAN_BEAT_TW_V2 §1.1b — adaptive backoff.  When the sweep
                    // returned < 5 % of the heap to libc, this workload's
                    // garbage is SCATTERED (flat MS can't whole-block-free it —
                    // firefox: 224 MB dead, 0 blocks freed; M5: GCs free ~17 MB
                    // yet cost ~6 s each).  Anchor the next threshold to the
                    // HEAP SIZE at this useless GC (×8) so one useless GC
                    // suppresses the rest of the eval's useless re-fires (M5
                    // 2 fires → 1; CPU −27 % on the cardano-node row, arena
                    // live unchanged — the suppressed GC freed ~0).
                    //
                    // HNE-safe by the yield key: HNE's GC frees ≥ 5 % (≈624 MB
                    // of whole-dead blocks) → freedLittle is false → the backoff
                    // never engages → HNE keeps collecting at the low threshold
                    // and its peak RSS stays capped.  (Rule 0: the sibling §1.1a
                    // 1 GB-initial-threshold raise was FALSIFIED by the HNE
                    // guard — +44.7 % RSS — because no single scalar threshold
                    // fits both a useful and a useless collector; the yield key
                    // is what distinguishes them.  firefox CPU win → §1.8.)
                    const bool freedLittle =
                        gcr.heapBytes == 0 || gcr.bytesFreed < gcr.heapBytes / 20;
                    if (freedLittle) {
                        size_t backoff = (gcr.heapBytes > (SIZE_MAX >> 3))
                            ? SIZE_MAX : gcr.heapBytes * 8;
                        if (backoff > nextThreshold) nextThreshold = backoff;
                    }
                    if (nextThreshold < g_majorGcInitialThresholdBytes)
                        nextThreshold = g_majorGcInitialThresholdBytes;
                    s_majorGcThresholdBytes = nextThreshold;
                }
            }
        }
        // Step 4 of post-Phase-3.8 plan (2026-05-29): periodic L(t)
        // live-fraction trace.  Gated NIX_V3_LIVE_TRACE_PERIODIC=<K>
        // (default OFF).  Same safepoint constraint as major-GC; runs
        // a transitive walk + records one CSV row each time arena
        // crosses the next K-MB boundary.
        //
        // Independent of major-GC gate: this measures L(t) for the
        // current code path, gate-OFF OR gate-ON.  Per
        // L_MEASUREMENT_GAP_2026-05-28 §5: closes the "L_end is the
        // only data point" methodology hole.
        //
        // Retirement: when L(t) is integrated into the bench harness
        // as a default-OFF metric, remove this hook + the env-gate.
        // P-6 (CODEBASE_REVIEW_2026-06-11): both PRE-decode default-off
        // diagnostic gates (periodic-live-trace + thunk-body-trace) sit behind
        // the ONE folded `kAnySlowGate` mask — the common path tests a single
        // bool here instead of two separate branch-predicted-not-taken tests.
        if (__builtin_expect(kAnySlowGate, 0)) [[unlikely]] {
        if (g_periodicLiveTrace) {   // T2: cached gate
            // De-gated from `exitDepth == 0` (2026-06-15).  The old gate
            // shared the major-GC safepoint constraint, so on deep evals
            // that stay in nested dispatch loops until the end (firefox.
            // drvPath, M5/cardano) it fired ~ONCE — the L(t) "time series"
            // degenerated to a single sample on exactly the workloads the
            // memory question is about (lode/WHY_V3_USES_MORE_MEMORY_2026
            // -06-14 §infra).
            //
            // Sampling at ANY depth is memory-SAFE here because the sample
            // is READ-ONLY: maybeSamplePeriodicLiveFraction runs a LiveTracer
            // transitive walk that builds its own visited-set — it sets no
            // mark bits, moves nothing, and frees nothing (unlike the GC,
            // whose depth>0 hazards are reclaim/relocation, not marking).
            // At a between-opcodes safepoint every live object is intact and
            // `walkAllV3Roots` already enumerates activeVMStack(), so all
            // active VMStates' precise roots are covered regardless of depth.
            //
            // ACCURACY: this is a precise-root LOWER BOUND — transient values
            // held only in primop C-locals below a nested dispatchLoop (e.g.
            // primFoldl's acc, a half-built mergeBindings result) are not in
            // any frame/valueStack and are omitted.  The fully-accurate
            // variant would drive the real marker's walkCStackConservative
            // (mark_sweep.cc) in a count-only / no-sweep mode — sound mid-eval
            // for the same read-only reason — see live_trace.cc for the
            // upgrade note.  For the gross live-vs-dead shape that informs the
            // generational-GC decision, the lower bound is sufficient and is
            // labelled as such in the CSV/plot.
            if (!vm.frames.empty()) vm.frames.back().ip = ip;
            maybeSamplePeriodicLiveFraction(vm);
        }
        // V3_DBG_TRACE_THUNK_BODY: print this instruction if the current
        // frame is a thunk frame matching the configured codeOffset/nUp.
        // Profile (sample on fib38) showed this branch alone consumed
        // ~10% of dispatchLoop time even though it's almost always
        // false.  Mark it unlikely so the compiler keeps the cold body
        // off the fast path and predicts the branch correctly.
        if (s_trace_env != nullptr && !vm.frames.empty()) {
            const auto & cur = vm.frames.back();
            if (cur.thunk && (cur.flags & CFF_THUNK_RETURN)
                && cur.thunk->nUpvalues == s_trace_nup)
            {
                const auto * d = cur.thunk->suspended.desc;
                if (d && d->codeOffset == s_trace_codeoff) {
                    Instruction peek = cu->code[ip];
                    Op pop_o = decodeOp(peek);
                    uint32_t pop_n = decodeOperand(peek);
                    // Fingerprint: pointer values of the first 3 upvalues
                    // — distinguishes different thunks that happen to share
                    // pointer (Boehm GC reuse) by their upvalue contents.
                    uint64_t fp0 = cur.thunk->tail[0].rawWord();
                    uint64_t fp1 = cur.thunk->nUpvalues > 1 ? cur.thunk->tail[1].rawWord() : 0;
                    std::fprintf(stderr,
                        "  TRACE thunk=%p state=%d ip=%u op=0x%02x operand=%u (cu=%p) fp=[%016llx,%016llx]\n",
                        (void*)cur.thunk, (int)cur.thunk->state, ip, (unsigned)pop_o, pop_n,
                        (void*)cu, (unsigned long long)fp0, (unsigned long long)fp1);
                }
            }
        }
        }  // end P-6 PRE-decode slow-gate cluster
        Instruction instr = cu->code[ip++];
        // P-6: decode the opcode up-front so all POST-decode default-off gates
        // (instr-count, limit poll, opcycles, opcounts) can share one mask test.
        Op op = decodeOp(instr);
        // P-6: POST-decode default-off diagnostic gates, all behind the one mask.
        if (__builtin_expect(kAnySlowGate, 0)) [[unlikely]] {
        if (kCountInstructions) {
            vm.nrInstructions++;
            allocStats().bytecodeInstructions++;
        }
        // Phase 1.6 (2026-05-18) — resource-limit polling.  When no
        // cap is configured (`limitsActive()` is false), the outer
        // branch elides everything; per-opcode overhead is one
        // branch-predicted-not-taken (≤1 cycle).  When caps ARE set,
        // a thread-local poll counter gates the actual getrusage /
        // steady_clock / atomic-OOM check to once per kPollInterval
        // (10000) opcodes — amortised ≤2ns per opcode.
        //
        // Gate: NIX_V3_MAX_HEAP / NIX_V3_MAX_CPU_TIME / NIX_V3_MAX_WALL_TIME.
        // Retire individual gates when bounded-memory / termination
        // / bounded-wall-time guarantees become structural (see
        // limits.cc).
        if (nix::v3::limitsActive()) {
            static thread_local uint32_t s_pollCounter = 0;
            if (__builtin_expect(++s_pollCounter >= nix::v3::kPollInterval, 0)) {
                s_pollCounter = 0;
                nix::v3::checkLimits();  // throws on cap exceed
            }
        }
        // (op already decoded above for the shared mask)
        // 2026-05-18 per-opcode profiling: bump under NIX_VM_OPCOUNTS=1.
        // P-2: gates are now file-scope (g_countOpcodes / g_countOpCycles) —
        // no per-dispatch magic-static guard.  Separate gate from NIX_VM_STATS
        // because the per-op increment adds one extra cache write per dispatch.
        // #786 OPCYCLES (2026-05-23) — g_countOpCycles samples the clock at
        // dispatch start and credits the elapsed time to the PREVIOUS opcode
        // (≈10-20 ns/dispatch; invariant per-op so relative comparisons hold).
        const bool s_countOpcodes  = g_countOpcodes;
        const bool s_countOpCycles = g_countOpCycles;
        if (s_countOpCycles) {
            // #790: read/write the FILE-SCOPE thread_local so the
            // dispatchLoop scope guard (see line ~2270) can save+
            // restore across nested-loop boundaries.
            uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (g_opcyclesPrevOp != 0xFF) {
                allocStats().opcycleNs[g_opcyclesPrevOp] += (ts - g_opcyclesPrevTs);
            }
            g_opcyclesPrevOp = static_cast<uint8_t>(op);
            g_opcyclesPrevTs = ts;
        }
        if (s_countOpcodes) {
            allocStats().opcodeCounts[static_cast<uint8_t>(op)]++;
            // #782 bigram tracking — only when NIX_VM_BIGRAMS=1
            // alongside NIX_VM_OPCOUNTS=1.  Identifies common
            // (prev_op, current_op) sequences for super-instruction
            // candidates.  Per-thread previous-op stored in
            // a function-local static (single-threaded VM; the per-
            // thread aliasing rules around static thread_local
            // would add bookkeeping for no benefit).
            static const bool s_countBigrams =
                std::getenv("NIX_VM_BIGRAMS") != nullptr;
            if (__builtin_expect(s_countBigrams, 0)) [[unlikely]] {
                static thread_local uint8_t prevOp = 0;
                static thread_local uint32_t prevSetSlot = ~0u;
                uint8_t opi = static_cast<uint8_t>(op);
                allocStats().bigramCounts[prevOp][opi]++;
                // #783-measure: track the SAME-SLOT subset of
                // SET_LOCAL -> GET_LOCAL.  Bigram counter tracks
                // opcode pairs only; fusion requires operand match
                // (write-then-read the same slot).  Decode operand
                // inline (cheap; only paid under the gate).
                uint32_t curOperand = decodeOperand(instr);
                if (prevOp == static_cast<uint8_t>(OP_SET_LOCAL)
                    && opi == static_cast<uint8_t>(OP_GET_LOCAL)
                    && curOperand == prevSetSlot)
                {
                    allocStats().bigramSetGetSameSlot++;
                }
                if (opi == static_cast<uint8_t>(OP_SET_LOCAL))
                    prevSetSlot = curOperand;
                else
                    prevSetSlot = ~0u;
                prevOp = opi;
            }
            // Step-1 (2026-06-04, BYTECODE_NGRAM_ANALYSIS §7) trigram
            // tracking — only when NIX_VM_TRIGRAMS=1 alongside
            // NIX_VM_OPCOUNTS=1.  Extends the bigram counter above to
            // (prevPrev, prev, curr) triples so the STATIC top n-gram
            // candidates (§4) can be confirmed under EXECUTION weighting
            // before any super-instruction / register-VM work.
            // Independent of NIX_VM_BIGRAMS so the heavier (hash-map)
            // trigram cost is only paid when explicitly requested.
            // Rolling state uses 0xFF sentinels so the first two
            // dispatches don't fabricate a triple; like the bigram
            // counter it does NOT reset at nested-dispatchLoop / RETURN
            // boundaries (the static tool breaks at OP_RETURN/OP_HALT,
            // but the dominant intra-function triples — GET_UPVALUE
            // REC_BINDING_SLOT_REF SET_LOCAL, GET_LOCAL ATTRS_REC_SET …
            // — dominate regardless of the small cross-frame noise, and
            // keeping the convention identical to the shipped bigram
            // counter keeps the static-vs-dynamic overlap comparison
            // apples-to-apples).
            static const bool s_countTrigrams =
                std::getenv("NIX_VM_TRIGRAMS") != nullptr;
            if (__builtin_expect(s_countTrigrams, 0)) [[unlikely]] {
                static thread_local uint8_t tgPrevPrev = 0xFF;
                static thread_local uint8_t tgPrev     = 0xFF;
                uint8_t opi = static_cast<uint8_t>(op);
                if (tgPrevPrev != 0xFF && tgPrev != 0xFF) {
                    uint32_t key = (uint32_t(tgPrevPrev) << 16)
                                 | (uint32_t(tgPrev) << 8)
                                 | uint32_t(opi);
                    allocStats().trigramCounts[key]++;
                }
                tgPrevPrev = tgPrev;
                tgPrev = opi;
            }
        }
        }  // end P-6 POST-decode slow-gate cluster
        uint32_t operand = decodeOperand(instr);

        // -Wswitch-enum: deliberately don't list reserved opcodes
        // (OP_NOP, OP_POP, OP_SWAP, OP_NEGATE, OP_BRANCH_TRUE, OP_POS)
        // in the case table -- they hit the default abort below by
        // design.  Previous comments listed them inline; this single
        // pragma block keeps them off the unhandled-enum diagnostic.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
        switch (op) {

        // --- Literals ---
        case OP_LIT_INT: {
            int32_t imm = decodeSignedOperand(instr);
            Value v; v.mkInt(imm); push(vm, v);
            break;
        }
        case OP_LIT_INT_BIG: { Value v; v.mkInt(cu->intConstants[operand]); push(vm, v); break; }
        case OP_LIT_FLOAT: {
            Value v; v.mkFloat(cu->floatConstants[operand]); push(vm, v); break;
        }
        case OP_LIT_STR: {
            Value v;
            v.mkString(cu->stringConstants[operand]->c_str());  // M-10: interned ptr
            push(vm, v);
            break;
        }
        case OP_LIT_PATH: {
            Value v;
            v.mkPath(cu->stringConstants[operand]->c_str());  // M-10: interned ptr
            push(vm, v);
            break;
        }
        case OP_LIT_TRUE:  push(vm, Value::vTrue);  break;
        case OP_LIT_FALSE: push(vm, Value::vFalse); break;
        case OP_LIT_NULL:  push(vm, Value::vNull);  break;

        // --- Locals / upvalues ---
        // Note: bounds checking on GET_LOCAL is omitted — the emit pass
        // + LambdaDescriptor::nLocals + the OP_CALL resize guarantee
        // every slot a function references has been pre-allocated.  A
        // bounds violation means the bytecode is corrupt; accept the
        // UB rather than pay for the check on every read.
        case OP_GET_LOCAL: {
            push(vm, vm.valueStack[stackBase + operand]);
            break;
        }
        case OP_GET_LOCAL2: {
            // Lever 1B-lite: fused two-slot push (operand = a<<12 | b).  Copy
            // both values BEFORE pushing — the first push() may reallocate
            // valueStack, invalidating a held reference to slot b.
            Value va = vm.valueStack[stackBase + (operand >> 12)];
            Value vb = vm.valueStack[stackBase + (operand & 0xFFF)];
            push(vm, va);
            push(vm, vb);
            break;
        }
        case OP_GET_LOCAL_FORCE: {
            // Superinstruction: GET_LOCAL + FORCE.  Push the slot value
            // and apply the FORCE fast path inline.  Hot path:
            // tag != Thunk/App/Slot -> just push.  Cold paths route
            // through op_force_slow.
            //
            // Profile-guided ordering: the diagnostic env-var checks
            // (V3_DBG_FORCE_SITE / V3_DBG_GETFORCE_TAG /
            //  NIX_V3_NO_GETFORCE_SUPER) are statically false in
            // production, so we only consult them after the hot
            // fast-path bails out.
            const Value & v = vm.valueStack[stackBase + operand];
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App && t != Tag::App3
                                 && t != Tag::Slot, 1)) {
                push(vm, v);
                break;
            }
            // Slow path (cold): diagnostics + the slow force.  Static
            // env-var checks live here so the fast path doesn't pay
            // the load + branch on every iteration.
            dbgLogForceSite(cu, ip - 1, &vm.valueStack[stackBase + operand]);
            dbgLogForceInsideX(vm, &vm.valueStack[stackBase + operand]);
            static const bool s_skipForce =
                std::getenv("NIX_V3_NO_GETFORCE_SUPER") != nullptr;
            if (__builtin_expect(s_skipForce, 0)) [[unlikely]] {
                push(vm, v);
                break;
            }
            {
                static const char * s_dbg_gflog =
                    std::getenv("V3_DBG_GETFORCE_TAG");
                if (__builtin_expect(s_dbg_gflog != nullptr, 0)) [[unlikely]] {
                    static const size_t depthFilter =
                        std::atoll(s_dbg_gflog);
                    if (vm.frames.size() >= depthFilter) {
                        std::fprintf(stderr,
                            "v3 GLF@d%zu slot=%u tag=%u ip=%u\n",
                            vm.frames.size(), operand, (unsigned)t, ip - 1);
                    }
                }
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_SET_LOCAL: {
            // SET keeps an auto-grow loop because some lower paths
            // (notably tryEval / inherit-from temp slots) write to a
            // slot that wasn't reserved by the function's nLocals
            // count — see eval-okay-tryeval-failed-thunk-reeval.
            // Fast path: target slot already exists below the value
            // we're about to pop.  Use `idx + 1 < size` to avoid the
            // unsigned-underflow trap when valueStack is empty (size-1
            // would wrap to SIZE_MAX and the bound check would always
            // succeed, dereferencing past the end).
            const size_t idx = stackBase + operand;
            if (__builtin_expect(idx + 1 < vm.valueStack.size(), 1)) {
                vm.valueStack[idx] = vm.valueStack.back();
                vm.valueStack.pop_back();
            } else {
                Value v = pop(vm);
                while (idx >= vm.valueStack.size())
                    vm.valueStack.push_back(Value{});
                vm.valueStack[idx] = v;
            }
            break;
        }
        case OP_SET_LOCAL_KEEP: {
            // Step-2 superinstruction: fused SET_LOCAL + adjacent same-slot
            // GET_LOCAL.  Store top into the slot WITHOUT popping (the
            // elided GET would have re-pushed it).  Only emitted for
            // reserved locals (slot < nLocals), always in range + below top.
            vm.valueStack[stackBase + operand] = vm.valueStack.back();
            break;
        }
        case OP_GET_UPVALUE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE: no closure context");
            if (operand >= closure->nUpvalues) {
                // #705 (2026-05-21) diagnostic: dump closure + frame
                // state to localize which lambda body is reading an
                // out-of-range upvalue.  Common signature when the
                // frame.closure is the WRONG closure (a different
                // lambda's body with fewer upvalues) post-scavenge.
                const LambdaDescriptor * d = closure->desc;
                const Nursery & nu = threadNursery();
                std::fprintf(stderr,
                    "v3 OP_GET_UPVALUE OOR: operand=%u nUpvalues=%u "
                    "closure=%p (nursery=%s) desc=%p desc.name=%.*s codeOff=%u "
                    "ip=%u frames=%zu\n",
                    (unsigned)operand, (unsigned)closure->nUpvalues,
                    (const void*)closure,
                    nu.contains(closure) ? "YES (stale)" : "no",
                    (const void*)d,
                    d ? (int)d->name.size() : 0,
                    d ? d->name.data() : "",
                    d ? d->codeOffset : 0u, (unsigned)(ip - 1),
                    vm.frames.size());
                // Dump top frames to see what desc the parent has.
                std::fprintf(stderr, "  top 5 frames:\n");
                for (size_t i = vm.frames.size(); i > 0 && i + 5 > vm.frames.size(); --i) {
                    const auto & fr = vm.frames[i - 1];
                    const LambdaDescriptor * fd = nullptr;
                    if (fr.thunk && (fr.thunk->state == ThunkState::Suspended
                                     || fr.thunk->state == ThunkState::Blackhole))
                        fd = fr.thunk->suspended.desc;
                    else if (fr.closure) fd = fr.closure->desc;
                    std::fprintf(stderr,
                        "    [%zu] closure=%p (ns=%s) thunk=%p (ns=%s) "
                        "ip=%u desc=%.*s\n",
                        i - 1, (const void*)fr.closure,
                        nu.contains(fr.closure) ? "Y" : "n",
                        (const void*)fr.thunk,
                        fr.thunk && nu.contains(fr.thunk) ? "Y" : "n",
                        fr.ip,
                        fd ? (int)fd->name.size() : 5,
                        fd ? fd->name.data() : "<?>");
                }
                // Search arena for refs to the stale closure pointer.
                if (nu.contains(closure)) {
                    Arena & arena = threadArena();
                    auto blocks = arena.blockRanges();
                    uintptr_t target = reinterpret_cast<uintptr_t>(closure);
                    size_t hits = 0;
                    std::fprintf(stderr, "  arena refs to stale closure:\n");
                    for (auto & blk : blocks) {
                        uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
                        uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
                        lo = (lo + 7) & ~uintptr_t{7};
                        for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
                            if (*reinterpret_cast<const uintptr_t *>(p) != target)
                                continue;
                            if (hits++ < 8) {
                                uint64_t prev = (p >= 8)
                                    ? *reinterpret_cast<const uint64_t *>(p - 8) : 0;
                                std::fprintf(stderr,
                                    "    @ %p (preceding word = 0x%llx tag=%d)\n",
                                    (void*)p, (unsigned long long)prev,
                                    (int)(prev & 0xff));
                            }
                        }
                    }
                    std::fprintf(stderr, "  total: %zu\n", hits);
                }
                std::fflush(stderr);
                throw std::runtime_error("v3 OP_GET_UPVALUE: index out of range");
            }
            push(vm, closureUpvalue(closure, operand));   // env-sharing: FAM or shared Env
            // Phase A5: frame-focused upvalue trace.  When
            // V3_DBG_SELECT_AT_CODEOFF=<codeoff> is set, log every
            // OP_GET_UPVALUE in matching frames.  Logs the tag of the
            // pushed value + chase-through-WHNF, so we can see if the
            // captured upvalue is what we expect.
            {
                static const char * s_atCo =
                    std::getenv("V3_DBG_SELECT_AT_CODEOFF");
                if (__builtin_expect(s_atCo != nullptr, 0)) {
                    uint32_t targetCo = static_cast<uint32_t>(std::atoi(s_atCo));
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure)
                        d = fr.closure->desc;
                    if (d && d->codeOffset == targetCo) {
                        const Value & top = vm.valueStack.back();
                        std::fprintf(stderr,
                            "v3 GET_UPVALUE@codeOff[%u] idx=%u tag=%u",
                            targetCo, (unsigned)operand, (unsigned)top.tag());
                        Value chase = top;
                        int hops = 0;
                        while (hops < 16) {
                            if (chase.tag() == Tag::Slot && chase.asSlot())
                                chase = *chase.asSlot();
                            else if (chase.tag() == Tag::Thunk
                                     && chase.asThunk()
                                     && chase.asThunk()->state == ThunkState::Evaluated)
                                chase = chase.asThunk()->evaluated;
                            else break;
                            ++hops;
                        }
                        std::fprintf(stderr,
                            " chase-tag=%u (hops=%d)",
                            (unsigned)chase.tag(), hops);
                        if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                            const auto & st = ir::globalSymbolTable();
                            uint32_t sz = chase.asAttrs()->size;
                            std::fprintf(stderr, " size=%u keys={", sz);
                            for (uint32_t k = 0; k < sz && k < 16; ++k) {
                                SymbolId nn =
                                    chase.asAttrs()->entries[k].name;
                                std::fprintf(stderr, "%s%s",
                                    k ? "," : "",
                                    nn < st.size() ? st[nn].c_str() : "?");
                            }
                            std::fprintf(stderr, "}");
                            if (const BindingsOrigin * o =
                                    lookupBindingsOrigin(chase.asAttrs())) {
                                const PosSnapshot * ps =
                                    resolvePosSnapshot(o->posHandle);
                                std::fprintf(stderr,
                                    " origin=%s@%s:%u",
                                    o->source ? o->source : "?",
                                    (ps && !ps->file.empty())
                                        ? ps->file.c_str() : "?",
                                    ps ? ps->line : 0u);
                            }
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            break;
        }
        case OP_GET_UPVALUE_FORCE: {
            if (!closure)
                throw std::runtime_error("v3 OP_GET_UPVALUE_FORCE: no closure context");
            // Hot path: tag != Thunk/App/Slot.  Diagnostics and the
            // NIX_V3_NO_GETFORCE_SUPER gate live below the bail-out so
            // they don't pay the load + branch on every iteration.
            Value v = closureUpvalue(closure, operand);   // env-sharing: FAM or shared Env
            Tag t = v.tag();
            if (__builtin_expect(t != Tag::Thunk && t != Tag::App && t != Tag::App3
                                 && t != Tag::Slot, 1)) {
                push(vm, v);
                break;
            }
            // V3_DBG_FORCE_SITE trace; see OP_GET_LOCAL_FORCE.
            dbgLogForceSite(cu, ip - 1,
                operand < closure->nUpvalues ? closureUpvaluePtr(closure, operand) : nullptr);
            // See OP_GET_LOCAL_FORCE — same NIX_V3_NO_GETFORCE_SUPER gate.
            static const bool s_skipForceUv =
                std::getenv("NIX_V3_NO_GETFORCE_SUPER") != nullptr;
            if (__builtin_expect(s_skipForceUv, 0)) [[unlikely]] {
                push(vm, v);
                break;
            }
            push(vm, v);
            goto op_force_slow;
        }
        case OP_DUP:  push(vm, top(vm)); break;
        // OP_POP: drop the top of the value stack.  Used by lower.cc's
        // builtins.seq fast-path (2026-05-17): emit `lowerExpr(x);
        // OP_FORCE; OP_POP; lowerExpr(y)` so x's force is done via
        // iterative OP_FORCE instead of the C-recursive OP_CALL_PRIMOP
        // arg-prep path.  Cheap: just pop_back().
        case OP_POP:  vm.valueStack.pop_back(); break;
        // OP_SWAP: bytecode value reserved (don't reuse for disk-cache
        // compatibility), but no current emit path produces it, so
        // dispatch falls through to default (abort).

        // --- Arithmetic ---
        // Int operations check for overflow via __builtin_*_overflow:
        // tree-walker raises an integer-overflow error, and v3 should
        // match.  Float operations have no such check (NaN/Inf semantics
        // mirror IEEE-754, same as tree-walker).
        case OP_ADD: {
            // Hot path mirror of OP_EQ/OP_LESS: in-place stack mutate on
            // int-int (the dominant case for the fib/ackermann/numeric-
            // loop shape) to avoid the pop+pop+push triple.  __builtin_
            // expect(int-int, 1) keeps the slow Float / mixed paths off
            // the inner-loop hot trace.
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            // #687 — TW phrasing (libexpr/eval.cc:2515):
            //   `integer overflow in adding <a> + <b>`
            // Pre-fix v3: `v3 OP_ADD: integer overflow` (no operands).
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t sum;
                if (__builtin_expect(__builtin_add_overflow(
                        top0.asInt(), top1.asInt(), &sum), 0))
                    throw std::runtime_error(
                        "integer overflow in adding "
                        + std::to_string(top0.asInt()) + " + "
                        + std::to_string(top1.asInt()));
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(sum);
                break;
            }
            // Slow path: Float / mixed Int-Float.
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.asFloat() + rhs.asFloat());
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.asInt()) + rhs.asFloat());
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.asFloat() + static_cast<double>(rhs.asInt()));
            else throw std::runtime_error("value is not a number");
            push(vm, r);
            break;
        }
        case OP_SUB: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            // #687 — TW pattern (libexpr/eval.cc:2515 family):
            //   `integer overflow in subtracting <a> - <b>`
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t diff;
                if (__builtin_expect(__builtin_sub_overflow(
                        top0.asInt(), top1.asInt(), &diff), 0))
                    throw std::runtime_error(
                        "integer overflow in subtracting "
                        + std::to_string(top0.asInt()) + " - "
                        + std::to_string(top1.asInt()));
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(diff);
                break;
            }
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.asFloat() - rhs.asFloat());
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.asInt()) - rhs.asFloat());
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.asFloat() - static_cast<double>(rhs.asInt()));
            else throw std::runtime_error("value is not a number");
            push(vm, r);
            break;
        }
        case OP_MUL: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            // #687 — TW pattern:
            //   `integer overflow in multiplying <a> * <b>`
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                int64_t prod;
                if (__builtin_expect(__builtin_mul_overflow(
                        top0.asInt(), top1.asInt(), &prod), 0))
                    throw std::runtime_error(
                        "integer overflow in multiplying "
                        + std::to_string(top0.asInt()) + " * "
                        + std::to_string(top1.asInt()));
                vm.valueStack.pop_back();
                vm.valueStack.back().mkInt(prod);
                break;
            }
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            if (lhs.isFloat() && rhs.isFloat())      r.mkFloat(lhs.asFloat() * rhs.asFloat());
            else if (lhs.isInt() && rhs.isFloat())   r.mkFloat(static_cast<double>(lhs.asInt()) * rhs.asFloat());
            else if (lhs.isFloat() && rhs.isInt())   r.mkFloat(lhs.asFloat() * static_cast<double>(rhs.asInt()));
            else throw std::runtime_error("value is not a number");
            push(vm, r);
            break;
        }
        case OP_DIV: {
            Value rhs = pop(vm), lhs = pop(vm);
            Value r;
            // #678 — match TW phrasing (libexpr/primops.cc:4703 +
            // implicit overflow / type-error sites): "division by
            // zero" / "integer overflow".  Drops "v3 OP_DIV:" debug
            // prefix.
            if (lhs.isInt() && rhs.isInt()) {
                if (rhs.asInt() == 0) throw std::runtime_error("division by zero");
                // INT64_MIN / -1 wraps around (mathematical result is
                // INT64_MAX + 1).  Match tree-walker by raising.
                if (lhs.asInt() == std::numeric_limits<int64_t>::min() && rhs.asInt() == -1)
                    throw std::runtime_error("integer overflow");
                r.mkInt(lhs.asInt() / rhs.asInt());
            } else if (lhs.isFloat() && rhs.isFloat()) {
                if (rhs.asFloat() == 0.0) throw std::runtime_error("division by zero");
                r.mkFloat(lhs.asFloat() / rhs.asFloat());
            } else if (lhs.isInt() && rhs.isFloat()) {
                if (rhs.asFloat() == 0.0) throw std::runtime_error("division by zero");
                r.mkFloat(static_cast<double>(lhs.asInt()) / rhs.asFloat());
            } else if (lhs.isFloat() && rhs.isInt()) {
                if (rhs.asInt() == 0) throw std::runtime_error("division by zero");
                r.mkFloat(lhs.asFloat() / static_cast<double>(rhs.asInt()));
            } else throw std::runtime_error(
                "value is not a number");
            push(vm, r);
            break;
        }
        // OP_NEGATE: bytecode value reserved (don't reuse for disk-
        // cache compatibility); never emitted by lowerExpr.  Negation
        // lowers as `0 - x` via OP_SUB.  Default-case abort catches a
        // stale CU.

        // --- Comparison ---
        // Inline fast-path for the int-int case (common: `n == 0`,
        // `n < 2` etc.).  In-place mutate the deeper slot to the bool
        // result and pop the top — no helper call, no Value temporaries.
        case OP_EQ:  {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool eq = top0.asInt() == top1.asInt();
                vm.valueStack.pop_back();
                vm.valueStack.back() = eq ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }
        case OP_NEQ: {
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool ne = top0.asInt() != top1.asInt();
                vm.valueStack.pop_back();
                vm.valueStack.back() = ne ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueEqual(vm, a, b) ? Value::vFalse : Value::vTrue; push(vm, r); break;
        }
        case OP_LESS:{
            Value & top1 = vm.valueStack.back();
            Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
            if (__builtin_expect(top0.isInt() && top1.isInt(), 1)) {
                bool lt = top0.asInt() < top1.asInt();
                vm.valueStack.pop_back();
                vm.valueStack.back() = lt ? Value::vTrue : Value::vFalse;
                break;
            }
            Value b = pop(vm), a = pop(vm); Value r; r = valueLess(vm, a, b) ? Value::vTrue : Value::vFalse; push(vm, r); break;
        }

        // --- Boolean / branches ---
        // All boolean opcodes force their operand: a function arg may be
        // a thunk whose evaluated value is the bool we need to branch on.
        // Without a force, `arg || y` would peek the thunk, fail the
        // isBool check, and incorrectly fall through into the rhs block.
        // A8 (2026-05-13) — boolean/branch opcodes now drive force
        // iteratively: when top-of-stack is non-WHNF, the case rewinds
        // `ip` to its own opcode, sets CFF_FORCE_RETRY on the current
        // frame, and `goto op_force_slow` to push a thunk-force frame.
        // dispatchLoop's main loop drains the force frame; on return
        // the same opcode re-runs with WHNF on top.  This replaces the
        // C-recursive `v = forceValue(vm, v)` pattern that grew the C
        // stack by one dispatchLoop frame per nested force.

        case OP_NOT: {
            Value & top = vm.valueStack.back();
            if (top.isThunk() || top.isAppLike()
                || top.tag() == Tag::Slot)
            {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            Value v = pop(vm);
            push(vm, isTrueValue(v) ? Value::vFalse : Value::vTrue);
            break;
        }

        case OP_AND_BRANCH: {
            // peek; if false -> jump (keep false); if true -> pop and fall through
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.isAppLike() || v.tag() == Tag::Slot) {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            if (v.isBool() && v.asInt() == 0) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_OR_BRANCH: {
            Value & v = vm.valueStack.back();
            if (v.isThunk() || v.isAppLike() || v.tag() == Tag::Slot) {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            if (v.isBool() && v.asInt() == 1) ip = operand;
            else                                 vm.valueStack.pop_back();
            break;
        }
        case OP_IMPL_BRANCH: {
            // If lhs false -> result is true; jump.  If lhs true -> pop, fall through.
            Value & top = vm.valueStack.back();
            if (top.isThunk() || top.isAppLike()
                || top.tag() == Tag::Slot)
            {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            Value v = pop(vm);
            if (v.isBool() && v.asInt() == 0) { push(vm, Value::vTrue); ip = operand; }
            break;
        }

        case OP_JUMP: ip = operand; break;
        case OP_BRANCH_FALSE: {
            Value & top = vm.valueStack.back();
            if (top.isThunk() || top.isAppLike()
                || top.tag() == Tag::Slot)
            {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            Value v = pop(vm);
            if (v.isBool() && v.asInt() == 0) ip = operand;
            break;
        }
        case OP_R_BRANCH_FALSE: {
            // reg-VM Phase 5: branch on regs[cond_slot] (a follow-up word);
            // operand = jump target.  Force the slot in place (writeback) if
            // non-WHNF, mirroring OP_BRANCH_FALSE — then re-enter.
            uint32_t condSlot = cu->code[ip];   // peek follow-up
            Value cval = vm.valueStack[stackBase + condSlot];
            Tag t = cval.tag();
            if (t == Tag::Thunk || t == Tag::App || t == Tag::App3
                || t == Tag::Slot) {
                push(vm, cval);
                CallFrame & frame = vm.frames.back();
                setForceWriteback(frame, static_cast<uint16_t>(condSlot));
                frame.flags |= CFF_FORCE_RETRY;
                ip = ip - 1;            // rewind to the OP_R_BRANCH_FALSE word
                goto op_force_slow;
            }
            ip++;                       // consume the cond_slot follow-up
            if (cval.isBool() && cval.asInt() == 0) ip = operand;
            break;
        }
        case OP_R_CALL: {
            // reg-VM Phase 5: register-addressed non-tail call.  See bytecode.hh.
            //   operand = dst slot ; follow-up word = (callee_slot<<12)|arg_slot
            uint32_t dst        = operand;
            uint32_t rdesc      = cu->code[ip];   // peek follow-up (consumed Phase 2)
            uint32_t calleeSlot = rdesc >> 12;
            uint32_t argSlot    = rdesc & 0xFFFu;
            // Phase 1: force the callee slot in place if it is a genuinely
            // non-WHNF indirection (Thunk / Slot), then re-execute — the A8
            // pattern (identical to OP_R_PRIMOP2's arg pre-force).  This makes
            // a thunk-callee resolve to its Closure on the iterative frame-push
            // path (Phase 2) rather than C-recursing through callClosure.
            // Tag::App / App3 are NOT forced here: a Tag::App is a partial
            // application (PAP) — WHNF for call purposes, exactly as OP_CALL's
            // own handler treats it (op_call_have_fun, not iter_force).  Forcing
            // a PAP would re-enter op_force_slow on an already-WHNF value and,
            // with the re-execute, spin forever (e.g. lib `concat = fold f ""`
            // applied as a `+` operand).  PAP callees fall through to Phase 2's
            // callClosure path, which applies them correctly.
            {
                Value callee = vm.valueStack[stackBase + calleeSlot];
                Tag ct = callee.tag();
                if (ct == Tag::Thunk || ct == Tag::Slot) {
                    push(vm, callee);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(calleeSlot));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;            // rewind to OP_R_CALL
                    goto op_force_slow;
                }
            }
            ip++;                           // Phase 2: consume the follow-up word
            {
                Value callee = vm.valueStack[stackBase + calleeSlot];
                Value argv   = vm.valueStack[stackBase + argSlot];  // lazy, unforced
                const Closure * cl = callee.tag() == Tag::Closure
                    ? callee.asClosure() : nullptr;
                if (cl && cl->desc && cl->desc->arity == 1
                    && cl->desc->selectorSym == 0 && !cl->desc->identityLambda) {
                    // Plain single-arg user closure: take the iterative
                    // frame-push path so deep recursion (fib, fold) stays on
                    // vm.frames, not the C stack.  Arm CFF_FORCE_WB=dst (NO
                    // CFF_FORCE_RETRY) so the callee's OP_RETURN drops the
                    // result into regs[dst] and we resume past this op;
                    // op_call_dispatch's frame push leaves the caller frame's
                    // flags untouched, so the armed writeback survives the call.
                    push(vm, callee);
                    push(vm, argv);
                    setForceWriteback(vm.frames.back(),
                                      static_cast<uint16_t>(dst));
                    goto op_call_dispatch;
                }
                // Any other callee shape — primop / selector- or identity-
                // lambda / __functor attrset / multi-arity PAP — is SHALLOW (no
                // deep user recursion), so run it synchronously via callClosure
                // and store the result.  This avoids threading the result
                // writeback through op_call_dispatch's many inline fast-path
                // exits.  (vm.valueStack may reallocate inside callClosure;
                // index by offset afterwards.)
                Value out = callClosure(vm, callee, argv);
                vm.valueStack[stackBase + dst] = out;
                break;
            }
        }
        case OP_R_STR_CONCAT2: {
            // reg-VM Phase 5: register-addressed binary `+` / 2-part concat.
            //   operand = dst slot ; follow-up = (forceStr<<24)|(a<<12)|b
            // See bytecode.hh.  Reuses the OP_STR_CONCAT body via op_str_concat.
            uint32_t dst   = operand;
            uint32_t fw    = cu->code[ip];          // peek follow-up (consume P2)
            uint32_t aSlot = (fw >> 12) & 0xFFFu;
            uint32_t bSlot =  fw        & 0xFFFu;
            // Phase 1: force each non-WHNF operand slot in place + re-execute
            // (the OP_R_PRIMOP2 arg-force pattern, matching STR_CONCAT's own
            // App/App3/Thunk force-scan).  After this the operands are WHNF, so
            // op_str_concat's force-scan is a no-op and never arms CFF_FORCE_WB
            // (which would clash with our result writeback) — and we avoid that
            // scan's `ip-1` re-exec, which would land on our follow-up word.
            for (uint32_t s : { aSlot, bSlot }) {
                Value v = vm.valueStack[stackBase + s];
                Tag t = v.tag();
                if (t == Tag::Thunk || t == Tag::App || t == Tag::App3) {
                    push(vm, v);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(s));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;                    // rewind to OP_R_STR_CONCAT2
                    goto op_force_slow;
                }
            }
            // Phase 2: both WHNF.  Push them, arm CFF_FORCE_WB=dst, synthesise
            // the STR_CONCAT operand (n=2 | forceStr) and reuse its body; at
            // str_concat_done applyForceWriteback drops the result into dst.
            {
                uint32_t forceStr = (fw >> 24) & 1u;
                ip++;                               // consume the follow-up word
                Value av = vm.valueStack[stackBase + aSlot];
                Value bv = vm.valueStack[stackBase + bSlot];
                push(vm, av);
                push(vm, bv);
                setForceWriteback(vm.frames.back(), static_cast<uint16_t>(dst));
                operand = (2u << 1) | forceStr;     // OP_STR_CONCAT decodes this
                goto op_str_concat;
            }
        }
        case OP_R_MOVE: {
            // reg-VM Phase 5: regs[dst] = regs[src].  operand=(dst<<12)|src.
            // Plain copy (no force) — see bytecode.hh.
            uint32_t dst = operand >> 12;
            uint32_t src = operand & 0xFFFu;
            vm.valueStack[stackBase + dst] = vm.valueStack[stackBase + src];
            break;
        }
        // OP_BRANCH_TRUE: bytecode value reserved; lowerer always emits
        // OP_BRANCH_FALSE with negated condition or OP_AND/OP_OR-shaped
        // branches.  Removed dispatch; default-case abort catches stale.

        // --- Closure / call / thunk ---
        case OP_MAKE_CLOSURE: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            // #530 lexical-with chain — second data word is the count
            // of with-target Values pushed BELOW the upvalue block on
            // the value stack.
            uint16_t nWiths = static_cast<uint16_t>(cu->code[ip++]);

            // IR Phase D (2026-05-18): closure-free lambda lifting.
            // When nUp==0 && nWiths==0, the descriptor's body has no
            // upvalues and no captured `with` (lowerer guarantees
            // lexicalWiths.empty() implies the body's WithLookup chain
            // doesn't reach any enclosing with).  Every invocation
            // would produce a semantically identical Closure; intern
            // a singleton in the descriptor and reuse it.
            //
            // Gate: NIX_V3_NO_LAMBDA_LIFT=1 disables the fast path
            // (falls through to plain Alloc::allocClosure for A/B).
            // Cached static so the env lookup happens once per
            // process.  Single-threaded VM — no atomics needed.
            static const bool s_noLift =
                std::getenv("NIX_V3_NO_LAMBDA_LIFT") != nullptr;
            if (__builtin_expect(nUp == 0 && nWiths == 0 && !s_noLift, 0)) {
                const LambdaDescriptor & desc = cu->lambdas[funcIdx];
                if (desc.cachedSingletonClosure) {
                    Value v;
                    v.mkClosure(desc.cachedSingletonClosure);
                    push(vm, v);
                    break;
                }
                // #705 (2026-05-20): tenured allocator MANDATORY.  The
                // returned pointer is stored in `desc.cachedSingletonClosure`
                // (a tenured field on LambdaDescriptor that's not a
                // scavenge root).  Routing this through `allocClosure`
                // (which can use the nursery) → SIGSEGV after the first
                // scavenge.  See allocClosureTenured docstring.
                Closure * c = Alloc::allocClosureTenured(0);
                V3_STATS_INC(closuresAllocated);
                c->desc = &desc;
                c->cu   = cu;
                c->nUpvalues = 0;
                // No upvalues to pop (nUp==0); no withs to pop (nWiths==0).
                // The body cannot reach any enclosing `with` (lowerer
                // computed lexicalWiths.empty()), so the absent
                // capturedWiths is safe regardless of the runtime
                // with-stack.  This intentionally differs from the
                // generic path's snapshotCurrentWiths() fallback —
                // that fallback exists for synthetic non-bytecode
                // Closures (primop bridges); bytecode-emitted
                // lambdas with nWiths==0 are guaranteed by the
                // lowerer to be with-independent.
                c->capturedWiths = nullptr;
                desc.cachedSingletonClosure = c;
                // Phase 3.7 (2026-05-28): register the cached pointer
                // so v3 mark phase keeps the closure alive across
                // arena sweeps.  Without this, the libc-resident
                // LambdaDescriptor field is invisible to the v3
                // walker; the closure gets freed; the next call to
                // this lambda reads a stale cached pointer → SIGSEGV.
                singletonClosureRegistry().push_back(&desc.cachedSingletonClosure);

                static const bool s_dbg =
                    std::getenv("V3_DBG_LAMBDA_LIFT") != nullptr;
                if (__builtin_expect(s_dbg, 0)) {
                    std::fprintf(stderr,
                        "v3 OP_MAKE_CLOSURE: intern %s "
                        "(funcIdx=%u codeOff=%u) → %p\n",
                        desc.name.empty() ? "<anon>" : desc.name.c_str(),
                        funcIdx, desc.codeOffset, (void *)c);
                }
                Value v;
                v.mkClosure(c);
                push(vm, v);
                break;
            }

            static const bool s_envSharing = []{
                if (std::getenv("NIX_V3_NO_ENV_SHARING")) return false;
                const char * e = std::getenv("NIX_V3_ENV_SHARING");
                if (e) return e[0] != '0';
                return true;
            }();
            const bool shareUpvalues = s_envSharing && nUp > 0;
            Closure * c = Alloc::allocClosure(shareUpvalues ? 0 : nUp);
            V3_STATS_INC(closuresAllocated);
            c->desc = &cu->lambdas[funcIdx];
            c->cu   = cu;
            c->nUpvalues = nUp;
            // Pop upvalues first (they sit on TOP of stack), then pop
            // the with-target block beneath.  Build capturedWiths
            // outermost-first by filling reverse into the ListVec.
            //
            // env-sharing (default ON, NIX_V3_NO_ENV_SHARING opts out): store
            // the upvalues in a shared, tenured Env (Closure::upvalEnv) rather than
            // the inline FAM.  The closure is allocated with a zero-length FAM
            // when shared; nUpvalues remains the logical count and
            // closureUpvalue() reads the Env.  This removes the bring-up path's
            // double storage (closure FAM plus Env).
            // Env-aware GC: scavenge walkClosure grays the Env; mark/evac/auditor +
            // closurePostConstructBarrier all branch on upvalEnv (gc.cc/mark_sweep.cc).
            if (__builtin_expect(shareUpvalues, 1)) {
                Env * env = Alloc::allocEnv(nUp);
                for (uint16_t i = nUp; i > 0; --i) env->values[i - 1] = pop(vm);
                c->upvalEnv = env;
            } else {
                for (uint16_t i = nUp; i > 0; --i) c->upvalues[i - 1] = pop(vm);
            }
            if (nWiths == 1) {
                // Day 13-15 (2026-05-29): singleton interning for the
                // overwhelmingly-dominant 1-element case (avg 1.04 on
                // HNE per T1_3_PAIRS_LISTS).  See helper at vm.cc top.
                Value w = pop(vm);
                c->capturedWiths = internOrAllocSingletonCapWiths(w);
            } else if (nWiths > 0) {
                ListVec * lws = Alloc::allocList(nWiths);
                for (uint16_t i = nWiths; i > 0; --i)
                    lws->elems[i - 1] = pop(vm);
                listPostConstructBarrier(lws);  // Phase D coverage
                c->capturedWiths = lws;
            } else {
                // No lexical `with` enclosing this lambda — fall back
                // to runtime-snapshot path for top-level cases like
                // primop bridges where the lowerer didn't see the
                // creation site (e.g., synthesized closures from
                // setNixEvalState glue).  Concrete v3-lowered code
                // will always set nWiths==0 only when there are
                // genuinely no enclosing `with` scopes, so the
                // snapshot returns nullptr too — safe overlap.
                c->capturedWiths = snapshotCurrentWiths(vm);
            }
            // #498 v2: log ALL super lambdas being made (V3_DBG_MAKE_SUPER_ALL).
            static const bool s_dbgMakeSuperAll =
                std::getenv("V3_DBG_MAKE_SUPER_ALL") != nullptr;
            if (__builtin_expect(s_dbgMakeSuperAll, 0)
                && c->desc && c->desc->name == "super") {
                std::fprintf(stderr,
                    "v3 OP_MAKE_CLOSURE super (codeOff=%u nUp=%u): cu=%p frames=%zu\n",
                    c->desc->codeOffset, (unsigned)nUp,
                    (void *)cu, vm.frames.size());
                if (!vm.frames.empty()) {
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure) d = fr.closure->desc;
                    std::fprintf(stderr,
                        "  maker frame: %s ip=%u flags=%u\n",
                        d && !d->name.empty() ? d->name.c_str()
                            : (d ? "<anon>" : "<root>"),
                        fr.ip, (unsigned)fr.flags);
                    // Dump the local[0] of the maker frame (= caller's arg
                    // for OP_CALL targets).
                    if (fr.stackBaseOffset < vm.valueStack.size()) {
                        Value lv = vm.valueStack[fr.stackBaseOffset];
                        Value chase = lv;
                        for (int hops = 0; hops < 4; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.asSlot())
                                chase = *chase.asSlot();
                            else if (chase.tag() == Tag::Thunk
                                     && chase.asThunk()
                                     && chase.asThunk()->state == ThunkState::Evaluated)
                                chase = chase.asThunk()->evaluated;
                            else break;
                        }
                        std::fprintf(stderr,
                            "  maker.local[0] tag=%d", (int)lv.tag());
                        if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                            std::fprintf(stderr, " -> attrs size=%u",
                                (unsigned)chase.asAttrs()->size);
                        } else {
                            std::fprintf(stderr, " -> chased.tag=%d",
                                (int)chase.tag());
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                std::fflush(stderr);
            }
            // #498: when the closure being made is named "super" with
            // 4 upvalues (matches all-packages.nix's failing inner
            // lambda), log the captured upvalues + the current frame.
            static const bool s_dbgMakeSuper =
                std::getenv("V3_DBG_MAKE_SUPER") != nullptr;
            if (__builtin_expect(s_dbgMakeSuper, 0)
                && c->desc && c->desc->name == "super"
                && nUp == 4) {
                auto chase = [](Value v, int hops) -> Value {
                    while (hops-- > 0) {
                        if (v.tag() == Tag::Slot && v.asSlot())
                            v = *v.asSlot();
                        else if (v.tag() == Tag::Thunk && v.asThunk()
                                 && v.asThunk()->state == ThunkState::Evaluated)
                            v = v.asThunk()->evaluated;
                        else break;
                    }
                    return v;
                };
                std::fprintf(stderr,
                    "v3 OP_MAKE_CLOSURE super: cu=%p desc.codeOffset=%u "
                    "frames=%zu\n",
                    (void *)cu, c->desc->codeOffset, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = closureUpvalue(c, i);
                    Value chased = chase(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.asAttrs()) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.asAttrs();
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                // Print all frames.
                for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                    const auto & fr = vm.frames[fi - 1];
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure) d = fr.closure->desc;
                    std::fprintf(stderr,
                        "  maker frame[%zu]: %s ip=%u flags=%u\n",
                        fi - 1,
                        d && !d->name.empty() ? d->name.c_str()
                            : (d ? "<anon>" : "<root>"),
                        fr.ip, (unsigned)fr.flags);
                }
                // Dump bytecode of the calling frame (res's thunk body) to
                // identify what immediately preceded the call to pkgs.
                if (vm.frames.size() >= 2) {
                    const auto & callerFr = vm.frames[vm.frames.size() - 2];
                    if (callerFr.cu) {
                        uint32_t lo = (callerFr.ip > 8) ? callerFr.ip - 8 : 0;
                        uint32_t hi = callerFr.ip + 4;
                        std::fprintf(stderr,
                            "  caller frame disasm cu=%p [%u..%u):\n",
                            (void*)callerFr.cu, lo, hi);
                        disassembleWindow(stderr, *callerFr.cu, lo, hi);
                        // Find caller's containing lambda
                        const auto & cuRef = *callerFr.cu;
                        uint32_t target = callerFr.ip;
                        uint32_t bestIdx = ~0u;
                        uint32_t bestOff = 0;
                        for (uint32_t li = 0; li < cuRef.lambdas.size(); ++li) {
                            uint32_t lo2 = cuRef.lambdas[li].codeOffset;
                            if (lo2 <= target && lo2 > bestOff) {
                                bestOff = lo2;
                                bestIdx = li;
                            }
                        }
                        if (bestIdx != ~0u) {
                            const auto & ld = cuRef.lambdas[bestIdx];
                            std::fprintf(stderr,
                                "  caller lambda[%u]: name=%s codeOffset=%u nUp=%u\n",
                                bestIdx,
                                ld.name.empty() ? "<anon>" : ld.name.c_str(),
                                ld.codeOffset, ld.nUpvalues);
                            // Print full body of caller's lambda (extended)
                            std::fprintf(stderr,
                                "  caller lambda body [%u..%u):\n",
                                ld.codeOffset, callerFr.ip + 30);
                            disassembleWindow(stderr, cuRef,
                                ld.codeOffset, callerFr.ip + 30);
                        }
                    }
                }
                // Also print the maker frame's locals (especially local[0],
                // which is the formal arg `pkgs` whose value should be
                // the fix-point but appears to be `{prev}`).
                if (!vm.frames.empty()) {
                    const auto & fr = vm.frames.back();
                    uint32_t nLoc = fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole)
                            ? fr.thunk->suspended.desc->nLocals
                        : (fr.closure ? fr.closure->desc->nLocals : 0);
                    std::fprintf(stderr,
                        "  maker frame.local[0..min(2,%u)]:\n", nLoc);
                    for (uint32_t li = 0; li < std::min(nLoc, 2u); ++li) {
                        Value lv = vm.valueStack[fr.stackBaseOffset + li];
                        Value chased = lv;
                        for (int hops = 0; hops < 4; ++hops) {
                            if (chased.tag() == Tag::Slot && chased.asSlot())
                                chased = *chased.asSlot();
                            else if (chased.tag() == Tag::Thunk
                                     && chased.asThunk()
                                     && chased.asThunk()->state == ThunkState::Evaluated)
                                chased = chased.asThunk()->evaluated;
                            else break;
                        }
                        std::fprintf(stderr,
                            "    local[%u]: tag=%d", li, (int)lv.tag());
                        if (chased.tag() == Tag::Attrs && chased.asAttrs()) {
                            const auto & st = ir::globalSymbolTable();
                            auto * b = chased.asAttrs();
                            std::fprintf(stderr,
                                " -> attrs size=%u {", (unsigned)b->size);
                            for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                                SymbolId nm = b->entries[k].name;
                                std::fprintf(stderr, "%s%s", k ? "," : "",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            if (b->size > 6) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "}");
                        } else {
                            std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
                std::fflush(stderr);
            }
            // Phase D coverage: post-construction barrier — if the new
            // Closure is tenured (nursery overflow / opt-out) and ANY
            // upvalue or capturedWiths is a nursery payload, this is
            // an inter-gen edge.  Dirty-list it so the next scavenge
            // walks it.  For nursery-resident closures, no-op.
            closurePostConstructBarrier(c);
            Value v; v.mkClosure(c); push(vm, v);
            // Phase A5: trace OP_MAKE_CLOSURE results whose body codeOff
            // matches V3_DBG_MAKE_CLO_AT_CODEOFF.  This catches the
            // wrong-closure-build bug — where the cpuName trace shows
            // darwinArch resolving to a closure whose body is at
            // inspect.nix:198:13 codeOff=1927 (the {family} thunk).  By
            // logging every MAKE_CLOSURE that produces a desc with
            // codeOff=<target>, we'll see WHICH bytecode site emitted
            // OP_MAKE_CLOSURE with the wrong funcIdx.
            {
                static const char * s_atCo =
                    std::getenv("V3_DBG_MAKE_CLO_AT_CODEOFF");
                if (__builtin_expect(s_atCo != nullptr, 0)) {
                    uint32_t targetCo = static_cast<uint32_t>(std::atoi(s_atCo));
                    if (c->desc && c->desc->codeOffset == targetCo) {
                        const PosSnapshot * dps =
                            resolvePosSnapshot(c->desc->posHandle);
                        std::fprintf(stderr,
                            "v3 MAKE_CLOSURE produced closure-ptr=%p "
                            "desc-codeOff=%u name='%s' pos=%s:%u:%u nUp=%u\n",
                            (void *)c,
                            targetCo,
                            !c->desc->name.empty() ? c->desc->name.c_str() : "<?>",
                            (dps && !dps->file.empty()) ? dps->file.c_str() : "<no-pos>",
                            dps ? dps->line : 0u, dps ? dps->column : 0u,
                            (unsigned)nUp);
                        // Caller frame info — who made this closure?
                        if (!vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const LambdaDescriptor * d2 = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d2 = fr.thunk->suspended.desc;
                            else if (fr.closure) d2 = fr.closure->desc;
                            const PosSnapshot * fps =
                                d2 ? resolvePosSnapshot(d2->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  maker frame: name=%s pos=%s:%u:%u "
                                "maker-codeOff=%u maker-cu=%p maker-funcIdx=%u\n",
                                d2 && !d2->name.empty() ? d2->name.c_str() : "<?>",
                                (fps && !fps->file.empty()) ? fps->file.c_str() : "<no-pos>",
                                fps ? fps->line : 0u, fps ? fps->column : 0u,
                                d2 ? d2->codeOffset : 0u,
                                (const void *)cu,
                                (unsigned)funcIdx);
                            // Disasm a window around the OP_MAKE_CLOSURE site
                            // so we can see what bytecode emitted it.
                            uint32_t opIp = ip - 3;  // operand+nUp+nWiths consumed already
                            uint32_t lo3 = opIp > 4 ? opIp - 4 : 0;
                            uint32_t hi3 = opIp + 4;
                            std::fprintf(stderr,
                                "  emit-site disasm [%u..%u):\n", lo3, hi3);
                            disassembleWindow(stderr, *cu, lo3, hi3);
                        }
                    }
                }
            }
            break;
        }
        case OP_MAKE_THUNK: {
            uint32_t funcIdx = operand;
            uint16_t nUp = static_cast<uint16_t>(cu->code[ip++]);
            // #530 lexical-with chain — see OP_MAKE_CLOSURE for the
            // protocol.  Second data word is the with-target count;
            // the with-targets sit BELOW the upvalues on the stack.
            uint16_t nWiths = static_cast<uint16_t>(cu->code[ip++]);
            // FP-2b: predict whether this thunk will capture a non-null with-list
            // and reserve the trailing tail slot iff so.  This EXACTLY matches
            // the capturedWiths logic below: nWiths>0 => an explicit lexical with;
            // else snapshotCurrentWiths(vm) is non-null iff the frame's with-stack
            // is non-empty (its `top<=base => null` test).  ~74-97% are false.
            const bool willHaveWiths = (nWiths > 0)
                || (vm.withStack.size()
                    > (vm.frames.empty() ? 0 : vm.frames.back().withStackBase));
            // env-sharing (default ON, NIX_V3_NO_ENV_SHARING opts out): store the thunk's
            // upvalues in a shared tenured Env (tail[0]=Env*) so the per-force
            // fakeClo shares it with NO upvalue copy (the forceValue lever).  See
            // OP_MAKE_CLOSURE for the gate retirement criterion.  Header stays
            // 24 B (ENV_SHARED flag in hasWithsSlot, not a new field).
            static const bool s_envSharingThunk = []{
                if (std::getenv("NIX_V3_NO_ENV_SHARING")) return false;
                const char * e = std::getenv("NIX_V3_ENV_SHARING");
                if (e) return e[0] != '0';
                return true;
            }();
            Thunk * t;
            Env * thunkEnv = nullptr;
            if (__builtin_expect(s_envSharingThunk && nUp > 0, 0)) {
                t = Alloc::allocThunkSuspendedShared(nUp, willHaveWiths);
                thunkEnv = Alloc::allocEnv(nUp);
                *reinterpret_cast<Env **>(&t->tail[0]) = thunkEnv;  // Env* @ tail[0]
            } else {
                t = Alloc::allocThunkSuspended(nUp, willHaveWiths);
            }
            // The "descriptor" we use is the LambdaDescriptor for the
            // referenced function (treated as 0-arg for thunks).
            // Reuse the LambdaDescriptor pointer through suspended.desc.
            t->suspended.desc = &cu->lambdas[funcIdx];
            // #733: thunksAllocated + per-descriptor allocCount serve
            // V3_DBG_ALLOC_DUMP / V3_DBG_FORCES / NIX_VM_STATS only —
            // gate the writes (cross-cache-line; unconditional cost
            // ~1-2 ns per alloc on hot paths).
            if (__builtin_expect(dbgForceStatsActive(), 0)) {
                V3_STATS_INC(thunksAllocated);
                ++cu->lambdas[funcIdx].allocCount;
            }
            if (__builtin_expect(g_dbgAllocDump, 0)) {
                cuRegistry().insert(cu);
                // A12 #583: periodic dump every 2M allocations since
                // atexit doesn't fire under SIGTERM (the timeout case
                // we're trying to debug).  Snapshot top-20 then continue.
                static std::atomic<uint64_t> s_allocSeq{0};
                uint64_t n = s_allocSeq.fetch_add(1, std::memory_order_relaxed) + 1;
                if ((n % 2000000) == 0) {
                    struct Row {
                        uint64_t alloc; uint64_t force; uint64_t call;
                        const LambdaDescriptor * d;
                    };
                    std::vector<Row> rows;
                    for (auto * cu2 : cuRegistry()) {
                        if (!cu2) continue;
                        for (const auto & ld : cu2->lambdas) {
                            if (ld.allocCount + ld.forceCount + ld.callCount < 100)
                                continue;
                            rows.push_back({ld.allocCount, ld.forceCount,
                                            ld.callCount, &ld});
                        }
                    }
                    auto emitTop = [&](const char * heading,
                                       auto keyFn) {
                        std::sort(rows.begin(), rows.end(),
                            [&](const Row & a, const Row & b) {
                                return keyFn(a) > keyFn(b);
                            });
                        size_t lim = std::min<size_t>(rows.size(), 15);
                        std::fprintf(stderr,
                            "\n  %s (top %zu):\n", heading, lim);
                        for (size_t i = 0; i < lim; ++i) {
                            const auto & r = rows[i];
                            if (keyFn(r) == 0) break;
                            const PosSnapshot * ps =
                                resolvePosSnapshot(r.d->posHandle);
                            const char * nm = r.d->name.empty()
                                ? "<anon>" : r.d->name.c_str();
                            if (ps && !ps->file.empty()) {
                                std::fprintf(stderr,
                                    "    alloc=%llu force=%llu call=%llu %s @ %s:%u:%u\n",
                                    (unsigned long long)r.alloc,
                                    (unsigned long long)r.force,
                                    (unsigned long long)r.call,
                                    nm,
                                    ps->file.c_str(), ps->line, ps->column);
                            } else {
                                std::fprintf(stderr,
                                    "    alloc=%llu force=%llu call=%llu %s @ <no-pos>\n",
                                    (unsigned long long)r.alloc,
                                    (unsigned long long)r.force,
                                    (unsigned long long)r.call, nm);
                            }
                        }
                    };
                    std::fprintf(stderr,
                        "\nv3 ALLOC_PERIODIC[%llu]: %zu lambdas active\n",
                        (unsigned long long)n, rows.size());
                    emitTop("by alloc",
                        [](const Row & r) -> uint64_t { return r.alloc; });
                    emitTop("by call",
                        [](const Row & r) -> uint64_t { return r.call; });
                    std::fflush(stderr);
                }
            }
            // FP-2a (2026-06-14): set the descriptor's owning-CU backpointer
            // instead of a per-thunk `suspended.cu` field.  `desc` was just set
            // to `&cu->lambdas[funcIdx]` (line ~4777), so the descriptor lives
            // in THIS cu's lambdas vector and `desc->cu = cu` is authoritative;
            // the store is idempotent (always the same value for a given desc).
            // thunkCU(t) reads it back == the exact former `suspended.cu`.
            t->suspended.desc->cu = cu;
            // V3_DBG_TRACE_THUNK_X -- track creation of every thunk into
            // a process-wide map (thunk_ptr -> (funcIdx, codeOff,
            // name, descPtr, cu)).  Consumed by the cycle-dump
            // diagnostic at OP_WITH_LOOKUP-cycle to verify whether the
            // thunk's current `suspended.desc` matches what it was at
            // creation time.  A mismatch is hard evidence of in-place
            // descriptor mutation (or thunk-pointer reuse).
            if (__builtin_expect(g_traceThunkX, 0)) {
                ThunkCreationInfo info{
                    funcIdx,
                    t->suspended.desc ? t->suspended.desc->codeOffset : 0u,
                    t->suspended.desc ? t->suspended.desc->name : std::string(),
                    t->suspended.desc,
                    cu};
                thunkCreationMap()[t] = info;
            }
            // V3_DBG_MK_THUNK_ANY -- log every OP_MAKE_THUNK with funcIdx,
            // descriptor name, codeOffset.  Use to verify (1) the
            // diagnostic codepath compiles in, (2) which fids are
            // actually being thunkified at runtime, and (3) whether a
            // "super"-named thunk is ever created via OP_MAKE_THUNK.
            //
            // Filtered by codeOffset to keep the log small: only prints
            // when codeOffset < 600 (== the early functions in the cu),
            // capturing the unusual codeOff=140 'super' thunk if it's
            // created here.
            static const bool s_dbgMkThunkAny =
                std::getenv("V3_DBG_MK_THUNK_ANY") != nullptr;
            if (__builtin_expect(s_dbgMkThunkAny, 0)) {
                static thread_local int s_n = 0;
                if (t->suspended.desc
                    && t->suspended.desc->name == "super") {
                    s_n++;
                    std::fprintf(stderr,
                        "MK_THUNK super[%d] cu=%p funcIdx=%u codeOff=%u "
                        "nUp=%u nWiths=%u arity=%u hasFormals=%u\n",
                        s_n, (const void *)cu, funcIdx,
                        t->suspended.desc->codeOffset,
                        nUp, nWiths,
                        t->suspended.desc->arity,
                        t->suspended.desc->hasFormals);
                }
            }
            // env-sharing: upvalues go into the shared Env, not the inline tail
            // (tail[0] holds the Env*).  Stack order is identical (upvalues on
            // top, withs below), so the withs-pop logic below is unaffected.
            if (thunkEnv) {
                for (uint16_t i = nUp; i > 0; --i) thunkEnv->values[i - 1] = pop(vm);
            } else {
                for (uint16_t i = nUp; i > 0; --i) t->tail[i - 1] = pop(vm);
            }
            if (__builtin_expect(g_dbgUpvalDup, 0) && nUp > 0 && !thunkEnv)
                recordUpvalDup(t, nUp);  // workstream H sizing probe
            // FP-2b: capturedWiths now lives in the reserved tail slot
            // (tail[nUpvalues], present iff willHaveWiths).  Each branch that
            // stores produces a non-null list AND implies willHaveWiths (so the
            // slot exists); the nWiths==0 && !willHaveWiths case stores nothing
            // (no slot — thunkCapturedWiths returns null, matching the old
            // snapshot-returns-null behaviour) and skips the snapshot alloc.
            if (nWiths == 1) {
                // Day 13-15 (2026-05-29): singleton interning — this
                // is the HEADLINE site per T1_3 (vm.cc:3831 in the
                // 05-27 numbering): 544 K allocs / 12.8 MB on HNE,
                // avg-size 1.04.  Almost all hit the singleton path.
                Value w = pop(vm);
                thunkSetCapturedWiths(t, internOrAllocSingletonCapWiths(w));
            } else if (nWiths > 0) {
                ListVec * lws = Alloc::allocList(nWiths);
                for (uint16_t i = nWiths; i > 0; --i)
                    lws->elems[i - 1] = pop(vm);
                listPostConstructBarrier(lws);  // Phase D coverage
                thunkSetCapturedWiths(t, lws);
            } else if (willHaveWiths) {
                // No lexical with-chain, but the frame's with-stack is non-empty
                // → snapshot is non-null and a slot was reserved.  (When
                // !willHaveWiths the snapshot would be null and no slot exists.)
                thunkSetCapturedWiths(t, snapshotCurrentWiths(vm));
            }
            // #498: trace MK_THUNK for thunks named "res" with nUp=4
            // (the all-packages.nix `let res = ...` thunk) and dump
            // captured upvalues to identify which freeVars[1] is.
            // Filter by upvalue[3] presence of 'conflictingAttrs' attr
            // to pin the all-packages.nix one (stage.nix's let has res
            // and conflictingAttrs).
            auto chaseFn = [](Value v, int hops) -> Value {
                while (hops-- > 0) {
                    if (v.tag() == Tag::Slot && v.asSlot())
                        v = *v.asSlot();
                    else if (v.tag() == Tag::Thunk && v.asThunk()
                             && v.asThunk()->state == ThunkState::Evaluated)
                        v = v.asThunk()->evaluated;
                    else break;
                }
                return v;
            };
            bool isStageRes = false;
            // env-sharing: a shared thunk's tail is [Env*]+[withs] (1-2 slots), so
            // tail[3] would be out of bounds — these #498 diagnostics read inline-
            // tail upvalues and are skipped under env-sharing (debug-only trace).
            if (!thunkEnv && t->suspended.desc && t->suspended.desc->name == "res"
                && nUp == 4) {
                Value uv3chased = chaseFn(t->tail[3], 4);
                if (uv3chased.tag() == Tag::Attrs && uv3chased.asAttrs()
                    && uv3chased.asAttrs()->size == 2) {
                    auto * b = uv3chased.asAttrs();
                    static const SymbolId conflictSym =
                        ir::globalInternSymbol("conflictingAttrs");
                    for (uint32_t k = 0; k < b->size; ++k) {
                        if (b->entries[k].name == conflictSym) {
                            isStageRes = true; break;
                        }
                    }
                }
            }
            // #498 diagnostic: trace prev-thunk MAKE_THUNK (nUp==2,
            // name=="prev") and dump captured upvalues to verify
            // tail[1] (= captured "final") IS extends.final's local[0]
            // at MAKE_THUNK time.
            static const bool s_dbgMakePrev =
                std::getenv("V3_DBG_MAKE_PREV") != nullptr;
            if (__builtin_expect(s_dbgMakePrev, 0) && !thunkEnv
                && t->suspended.desc && t->suspended.desc->name == "prev"
                && nUp == 2) {
                // Dump the maker frame's local[0] for comparison with
                // tail[1] (which should be a snapshot of local[0]).
                {
                    const auto & fr = vm.frames.back();
                    Value uv0 = vm.valueStack[fr.stackBaseOffset + 0];
                    Value chased = chaseFn(uv0, 4);
                    const LambdaDescriptor * desc = nullptr;
                    if (fr.closure) desc = fr.closure->desc;
                    else if (fr.thunk) desc = fr.thunk->suspended.desc;
                    std::fprintf(stderr,
                        "v3 OP_MAKE_THUNK prev MAKER %s codeOff=%u.local[0].tag=%d",
                        desc && !desc->name.empty() ? desc->name.c_str() : "?",
                        (unsigned)(desc ? desc->codeOffset : 0),
                        (int)uv0.tag());
                    if (chased.tag() == Tag::Attrs && chased.asAttrs()) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.asAttrs();
                        std::fprintf(stderr, " -> attrs size=%u {",
                            (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 4; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fprintf(stderr,
                    "v3 OP_MAKE_THUNK prev: thunk=%p frames=%zu\n",
                    (void *)t, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = t->tail[i];
                    Value chased = chaseFn(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.asAttrs()) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.asAttrs();
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else if (chased.tag() == Tag::Closure
                               && chased.asClosure()
                               && chased.asClosure()->desc) {
                        std::fprintf(stderr, " -> Closure name=%s",
                            chased.asClosure()->desc->name.c_str());
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fflush(stderr);
            }
            static const bool s_dbgMakeRes =
                std::getenv("V3_DBG_MAKE_RES") != nullptr;
            if (__builtin_expect(s_dbgMakeRes, 0) && isStageRes) {
                std::fprintf(stderr,
                    "v3 OP_MAKE_THUNK res (stage.nix): cu=%p thunk=%p "
                    "frames=%zu\n",
                    (void *)cu, (void *)t, vm.frames.size());
                for (uint16_t i = 0; i < nUp; ++i) {
                    Value uv = t->tail[i];
                    Value chased = chaseFn(uv, 4);
                    std::fprintf(stderr,
                        "  upvalue[%u]: tag=%d", i, (int)uv.tag());
                    if (chased.tag() == Tag::Attrs && chased.asAttrs()) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chased.asAttrs();
                        std::fprintf(stderr,
                            " -> attrs size=%u {", (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 6; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        if (b->size > 6) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chased.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fflush(stderr);
            }
            // Phase D coverage: post-construction barrier for OP_MAKE_THUNK.
            // If t is tenured (overflow / opt-out) AND tail[] or
            // suspended.capturedWiths carries nursery payload, dirty-list.
            thunkPostConstructBarrier(t);
            Value v;
            v.mkThunk(t);
            push(vm, v);
            break;
        }
        case OP_CALL: {
            op_call_dispatch:
            // A non-tail call resets the tail-iteration counter — any
            // subsequent runaway recursion is bounded against the
            // 5000-frame stack guard, not the tail-call counter.
            vm.tailCallCount = 0;
            Value arg = pop(vm), fun = pop(vm);
            // V3_DBG_FINAL_CALL=1: log Apply of extends's `final:`
            // lambda OR allPackages's `self:` outer lambda — tracing
            // the broader-thunkify upvalue bug at #498.  Cache the
            // env-var lookup as `static const bool` so OP_CALL — the
            // hottest opcode on fib/ackermann/lib.foldl' — doesn't
            // pay a libc getenv call per iteration.  Same pattern as
            // s_dbgOpCall below.
            static const bool s_dbgFinalCall =
                std::getenv("V3_DBG_FINAL_CALL") != nullptr;
            if (__builtin_expect(s_dbgFinalCall, 0)
                && fun.tag() == Tag::Closure && fun.asClosure()
                && fun.asClosure()->desc
                && (fun.asClosure()->desc->name == "final"
                    || fun.asClosure()->desc->name == "self"
                    || fun.asClosure()->desc->name == "rattrs"
                    || fun.asClosure()->desc->name == "overlay")) {
                Value chase = arg;
                int hops = 0;
                while (hops < 4) {
                    if (chase.tag() == Tag::Slot && chase.asSlot())
                        chase = *chase.asSlot();
                    else if (chase.tag() == Tag::Thunk && chase.asThunk()
                             && chase.asThunk()->state == ThunkState::Evaluated)
                        chase = chase.asThunk()->evaluated;
                    else break;
                    ++hops;
                }
                std::fprintf(stderr,
                    "v3 OP_CALL %s-lambda: arg.tag=%d",
                    fun.asClosure()->desc->name.c_str(),
                    (int)arg.tag());
                if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                    auto * b = chase.asAttrs();
                    std::fprintf(stderr, " -> attrs size=%u", b->size);
                } else {
                    std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                }
                std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
            }
            // V3_DBG_OP_CALL=1 logs every OP_CALL with the closure
            // name + arg shape.  Used to trace the broader-thunkify
            // upvalue bug.
            static const bool s_dbgOpCall =
                std::getenv("V3_DBG_OP_CALL") != nullptr;
            if (s_dbgOpCall) {
                const char * fname = "<?>";
                if (fun.tag() == Tag::Closure && fun.asClosure()
                    && fun.asClosure()->desc)
                    fname = fun.asClosure()->desc->name.c_str();
                int arg_tag = (int)arg.tag();
                int arg_size = -1;
                if (arg.tag() == Tag::Attrs && arg.asAttrs())
                    arg_size = arg.asAttrs()->size;
                std::fprintf(stderr,
                    "v3 OP_CALL: name=%s fun.tag=%d arg.tag=%d size=%d frames=%zu\n",
                    fname, (int)fun.tag(), arg_tag, arg_size,
                    vm.frames.size());
            }

            // Force `fun` if it's a deferred shape (Tag::App from lazy
            // primops like mapAttrs, or a Thunk that lazy attr access
            // produced).  Tree-walker's `callFunction` does the same up
            // front; mirroring it here keeps the rest of the dispatch
            // simple and avoids the OP_RETURN-chase cycle problem (where
            // chasing while the outer thunk is still Black trips
            // infinite-recursion).  Cheap on the hot path: one tag
            // check on already-WHNF callables.
            // Fast path: a Tag::Thunk in Evaluated state caches its
            // resolved value in `evaluated`.  Most calls under
            // thunkifyRecAttrSelect (Phase-5-off rec-attr access) hit
            // this case after the first force, so chasing one Thunk
            // hop inline saves a forceValue() call + its full chase
            // setup (depth guards, iteration bound, etc.).
            //
            // A12b (2026-05-17): when the one-hop fast path doesn't
            // resolve to WHNF, convert the deep force to iterative
            // (op_force_slow + writeback) instead of C-recursing into
            // forceValue.  This is the largest C-stack consumer on
            // deep nixpkgs eval graphs (stdenv.mkDerivation's chained
            // finalPackage / commonAttrs / overlappingArgs) and the
            // root cause of the depth=5000 SIGSEGV that affected
            // `(import <nixpkgs>{})`.  Each OP_CALL fun-force used to
            // grow the C-stack by ~14 KiB (forceValue + dispatchLoop
            // + their callees); after this conversion, the inner
            // force runs as a regular frame on vm.frames and the
            // outer OP_CALL re-runs on completion without C-recursion.
            //
            // To force the fun-slot (which is at top-1 after the
            // pop+pop above, but we've already extracted fun + arg
            // into locals), we push BOTH back to the stack and use
            // the writeback-slot pattern: fun ends up at top-2 after
            // pushing fun, arg, dup-of-fun.  The forced value
            // overwrites the original fun-slot on retry.
            if (Tag fT = fun.tag(); fT == Tag::Thunk) {
                Thunk * t = fun.asThunk();
                if (t->state == ThunkState::Evaluated) {
                    Value e = t->evaluated;
                    Tag eT = e.tag();
                    // If the evaluated value itself is in WHNF (the
                    // common case for rec-attr-thunk-of-Closure), we're
                    // done.  Else fall through to iterative force.
                    if (eT != Tag::Thunk && eT != Tag::App && eT != Tag::App3
                        && eT != Tag::Slot) {
                        fun = e;
                    } else if (isUnderappliedClosurePap(e)) {  // C-10: App OR App3
                        // eval/apply: a thunk that evaluated to an under-applied
                        // closure-PAP is WHNF — apply the next arg at
                        // op_call_have_fun (which now saturates App AND App3
                        // PAPs) rather than iter-forcing it (which loops forever
                        // since op_force_slow returns a PAP unchanged).
                        fun = e;
                        goto op_call_have_fun;
                    } else {
                        goto op_call_iter_force;
                    }
                } else {
                    goto op_call_iter_force;
                }
            } else if (fT == Tag::App || fT == Tag::App3 || fT == Tag::Slot) {
                // eval/apply (#3, gate-on only): an under-applied arity-N
                // closure represented as a Tag::App chain is already WHNF (a
                // partial application).  Do NOT iter-force it (that would try
                // to evaluate it as a deferred call and loop) — handle the
                // arg accumulation at op_call_have_fun.  Gate-off this never
                // fires: arity>1 closures exist only when NIX_V3_EVAL_APPLY
                // collapsed a curried chain, so `arity > depth` is false for
                // every ordinary single-arg-closure lazy-app.
                // C-10: route App AND App3 PAP callees to op_call_have_fun
                // (which now saturates both); the prior Tag::App-only walk left
                // an App3 PAP callee to iter-force and loop forever.
                if (isUnderappliedClosurePap(fun))
                    goto op_call_have_fun;
                goto op_call_iter_force;
            }
            goto op_call_have_fun;
            op_call_iter_force: {
                // Restore stack to [..., fun, arg] then push dup-of-fun
                // for op_force_slow.  Writeback target is the restored
                // fun slot (offset relative to stackBase).
                push(vm, fun);
                push(vm, arg);
                size_t funIdx = vm.valueStack.size() - 2;
                uint32_t off = static_cast<uint32_t>(funIdx - stackBase);
                if (__builtin_expect(off > 0xFFFFu, 0))
                    throw std::runtime_error(
                        "v3 OP_CALL: writeback slot offset too large");
                push(vm, fun);  // dup, will be popped by op_force_slow
                CallFrame & frame = vm.frames.back();
                setForceWriteback(frame, static_cast<uint16_t>(off));
                frame.flags |= CFF_FORCE_RETRY;
                ip = ip - 1;
                goto op_force_slow;
            }
            op_call_have_fun:;

            // PrimOp / PrimOpApp partial application.
            if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
                // Walk the PrimOpApp chain to find the root PrimOp and
                // collect the previously-applied args.
                Value cur = fun;
                size_t depth = 0;
                while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.asPair()->left; }
                if (!cur.isPrimOp())
                    throw std::runtime_error("v3 OP_CALL: PrimOpApp chain doesn't terminate in a PrimOp");
                const PrimOp * po = cur.asPrimOp();
                size_t totalArgs = depth + 1;
                if (totalArgs < po->arity) {
                    // Build a new PrimOpApp wrapping (fun, arg).
                    ValuePair * vp = Alloc::allocPair();
                    vp->left = fun;
                    vp->right = arg;
                    pairPostConstructBarrier(vp);  // Phase D
                    Value v;
                    v.mkPair(Tag::PrimOpApp, vp);
                    push(vm, v);
                    break;
                }
                if (totalArgs > po->arity)
                    throw std::runtime_error("v3 OP_CALL: too many args for primop");
                // Collect args in [arg_0, arg_1, ..., arg_{N-1}, arg] order.
                Value buf[8];
                if (po->arity > 8) throw std::runtime_error("v3 OP_CALL: primop arity > 8");
                buf[totalArgs - 1] = arg;
                Value chain = fun;
                for (size_t i = totalArgs - 1; i > 0; --i) {
                    buf[i - 1] = chain.asPair()->right;
                    chain = chain.asPair()->left;
                }
                vm.frames.back().ip = ip;
                // PrimOpApp accumulates args lazily — primops expect
                // WHNF, so force each here before invoking, EXCEPT for
                // args the primop has explicitly opted out of via its
                // `lazyArgs` bitmask (e.g., addErrorContext's value arg
                // — see lib/modules.nix's
                // `config = addErrorContext "..." config` cycle).
                //
                // Fast-path: skip forceValue function call if the arg
                // is already in WHNF (Tag is not Thunk/App/Slot).
                // CPU sample showed forceValue+424 (chase loop entry)
                // at 5% of CPU under THUNK_ALL on nixpkgs — most of
                // those are no-op calls for WHNF args.  Inline the
                // tag check saves CALL/RET + setup per skipped arg.
                for (uint32_t i = 0; i < po->arity; ++i) {
                    if (po->lazyArgs & (1u << i)) continue;
                    Tag at = buf[i].tag();
                    if (__builtin_expect(at == Tag::Thunk
                                         || at == Tag::App || at == Tag::App3
                                         || at == Tag::Slot, 0))
                        buf[i] = forceValue(vm, buf[i]);
                }
                bumpPrimOpCallCount(po);
                // Wire nixEvalState so primops that touch the
                // store / file system (toString-on-path, import,
                // path coercion) can reach the live nix::EvalState.
                // 2026-05-19 #665 root cause: this path is the
                // hot OP_CALL fall-through when the callee is a
                // PrimOp/PrimOpApp (e.g. the derivationStrict
                // bytecode wrapper's `builtins.toString args.X`
                // calls).  Without nixEvalState, toStringCoerceCtx
                // hits its `!state.nixEvalState` early-return and
                // produces raw source-tree paths instead of
                // copying them to /nix/store, causing every
                // derivation that interpolates a `${./X.sh}`-style
                // path to diverge from TW (drv hash mismatch
                // cascading through bash → stdenv → all packages).
                EvalState state; state.vm = &vm;
                state.nixEvalState = getNixEvalState();
                Value out;
                po->fn(state, buf, out);
                push(vm, out);
                break;
            }

            // (OP_CALL Bridge-thunk handler retired with the bridge
            //  apparatus — TW_VALUE_ERADICATION F4, 2026-06-02.)

            // eval/apply (#3, gate-on only): arity-N closure application via
            // Tag::App PAP accumulation — the closure analogue of the
            // PrimOpApp block above.  arity>1 closures exist ONLY when
            // NIX_V3_EVAL_APPLY collapsed a curried chain, so every branch
            // here is inert by default (papBase->desc->arity is 0/1).  A bare
            // arity-1 closure falls through to the normal single-arg entry.
            {
                // C-10 (CODEBASE_REVIEW_2026-06-11): walk a mixed App/App3 spine
                // to the leaf closure.  An App3 link carries TWO applied args
                // (right + third), so it contributes 2 to the depth — mirroring
                // isUnderappliedClosurePap and the force-spine path.  (Previously
                // this walked Tag::App only, so an App3 PAP callee left
                // papBase=null and fell through to "callee is not a closure".)
                const Closure * papBase = nullptr;
                size_t papDepth = 0;
                if (fun.tag() == Tag::Closure && fun.asClosure()) {
                    papBase = fun.asClosure();
                } else if (fun.isAppLike() && fun.asPair()) {
                    const Value * cur = &fun;
                    while ((cur->tag() == Tag::App || cur->tag() == Tag::App3)
                           && cur->asPair()) {
                        papDepth += (cur->tag() == Tag::App3) ? 2 : 1;
                        cur = &cur->asPair()->left;
                    }
                    if (cur->tag() == Tag::Closure && cur->asClosure())
                        papBase = cur->asClosure();
                }
                if (papBase && papBase->desc && papBase->desc->arity > 1) {
                    const uint8_t A = papBase->desc->arity;
                    const size_t total = papDepth + 1;
                    if (total < A) {
                        // Under-applied: extend the PAP chain with one App link
                        // carrying the new arg (correct for an App OR App3 base).
                        ValuePair * vp = Alloc::allocPair();
                        vp->left = fun; vp->right = arg;
                        pairPostConstructBarrier(vp);
                        Value v;
                        v.mkPair(Tag::App, vp);
                        push(vm, v);
                        break;
                    }
                    // Saturated (total == A).  Gather the spine's args in
                    // closure-slot order, then append the new OP_CALL arg last.
                    if (A > 16) throw std::runtime_error("v3 OP_CALL: arity > 16");
                    if (__builtin_expect(total > A, 0)) {
                        // OVER-APPLICATION (R2, 2026-06-12) — twin of the OP_TAIL_CALL
                        // site (see the full rationale there).  The leaf saturates at
                        // A and returns a function consuming the remaining args; replay
                        // the spine + new arg one-at-a-time via callClosure (mirrors the
                        // OP_CALL_N fallback below).  REPLACES `throw "PAP over-applied"`.
                        Value tmp2[16]; size_t nT = 0;
                        const Value * w2 = &fun;
                        while (w2->isAppLike() && w2->asPair()) {
                            const ValuePair * pp2 = w2->asPair();
                            if (w2->tag() == Tag::App3) {
                                tmp2[nT++] = pp2->third; tmp2[nT++] = pp2->right;
                            } else {
                                tmp2[nT++] = pp2->right;
                            }
                            w2 = &pp2->left;
                        }
                        Value f = *w2;  // leaf closure (spine bottom)
                        for (size_t i = nT; i > 0; --i) f = callClosure(vm, f, tmp2[i - 1]);
                        f = callClosure(vm, f, arg);
                        push(vm, f);
                        break;
                    }
                    Value argbuf[16];
                    // Walk the spine outermost-first, collecting args exactly as
                    // the force-spine path does (App3: third then right; App:
                    // right).  The closure-slot order is that collection
                    // reversed; the new OP_CALL arg occupies the last slot.
                    Value tmp[16]; size_t nTmp = 0;
                    const Value * w = &fun;
                    while (w->isAppLike() && w->asPair()) {
                        const ValuePair * p = w->asPair();
                        if (w->tag() == Tag::App3) {
                            tmp[nTmp++] = p->third; tmp[nTmp++] = p->right;
                        } else {
                            tmp[nTmp++] = p->right;
                        }
                        w = &p->left;
                    }
                    for (size_t i = 0; i < nTmp; ++i) argbuf[i] = tmp[nTmp - 1 - i];
                    argbuf[A - 1] = arg;
                    const LambdaDescriptor * d = papBase->desc;
                    if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                        throw std::runtime_error(
                            "v3 OP_CALL: stack overflow; call depth exceeded "
                            + std::to_string(kMaxCallDepth));
                    vm.frames.back().ip = ip;
                    size_t newBase = vm.valueStack.size();
                    vm.valueStack.resize(newBase + d->nLocals);
                    for (size_t i = 0; i < A; ++i)
                        vm.valueStack[newBase + i] = argbuf[i];
                    uint32_t newWithBase =
                        static_cast<uint32_t>(vm.withStack.size());
                    const CompilationUnit * calleeCu = papBase->cu ? papBase->cu : cu;
                    vm.frames.push_back(CallFrame{
                        .cu = calleeCu,
                        .closure = papBase,
                        .thunk = nullptr,
                        .ip = d->codeOffset,
                        .stackBaseOffset = static_cast<uint32_t>(newBase),
                        .withStackBase = newWithBase,
                        .flags = 0,
                    });
                    pushCapturedWiths(vm, papBase->capturedWiths);
                    ip = d->codeOffset;
                    cu  = calleeCu;
                    closure = papBase;
                    stackBase = newBase;
                    break;
                }
            }

            // __functor: applying an attrset that has a `__functor`
            // attribute calls `__functor self arg` per the standard
            // Nix protocol.  Push (functor, attrset, arg) and re-enter
            // OP_CALL twice to match the curried call sequence.
            if (fun.isAttrs()) {
                static const SymbolId functorId = ir::globalInternSymbol("__functor");
                if (!fun.asAttrs())
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                const Value * fn = fun.asAttrs()->lookup(functorId);
                if (!fn)
                    throw std::runtime_error("v3 OP_CALL: callee is an attrset without __functor");
                Value forced = forceValue(vm, *fn);
                // First apply functor to self (= the attrset).
                Value firstStep = callClosure(vm, forced, fun);
                // Then apply that result to the original arg.
                Value out = callClosure(vm, firstStep, arg);
                push(vm, out);
                break;
            }

            if (!fun.isClosure()) {
                static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
                if (dbg) {
                    std::fprintf(stderr,
                        "v3 OP_CALL: callee is not a closure tag=%u "
                        "frames=%zu callerIp=%u\n",
                        (unsigned)fun.tag(), vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = fr.thunk->suspended.desc;
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        std::fprintf(stderr,
                            "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                            i - 1,
                            desc && !desc->name.empty() ? desc->name.c_str()
                                : (desc ? "<anon>" : "<closure-body>"),
                            desc ? desc->codeOffset : 0,
                            fr.ip, (unsigned)fr.flags);
                    }
                    // Dump 32 instructions before/after the failing OP_CALL.
                    if (cu) {
                        uint32_t fip = ip > 0 ? ip - 1 : 0;
                        uint32_t lo = fip > 64 ? fip - 64 : 0;
                        uint32_t hi = fip + 16;
                        std::fprintf(stderr, "  current frame disasm [%u..%u):\n", lo, hi);
                        disassembleWindow(stderr, *cu, lo, hi);
                    }
                }
                throw std::runtime_error("v3 OP_CALL: callee is not a closure");
            }
            const Closure * callee = fun.asClosure();
            const LambdaDescriptor * desc = callee->desc;

            // #495 follow-on bisect: log OP_CALL post-force for
            // platform-named closures.  Used to trace the wrong-arg
            // capture in the broader-thunkify upvalue bug.  Also derefs
            // Tag::Slot args to show the slot's storage pointer and
            // the Value found there (chasing one indirection).  The
            // bug: arg=Tag::Slot(p), *p = Tag::Attrs{gcc, linux-kernel}
            // but should be the platform record `final` containing
            // isx86 etc.
            static const bool s_dbgOpCallPost =
                std::getenv("V3_DBG_OP_CALL_POST") != nullptr;
            if (__builtin_expect(s_dbgOpCallPost, 0)
                && desc && !desc->name.empty()
                && desc->name == "platform")
            {
                int arg_tag = (int)arg.tag();
                int arg_size = -1;
                if (arg.tag() == Tag::Attrs && arg.asAttrs())
                    arg_size = arg.asAttrs()->size;
                std::fprintf(stderr,
                    "v3 OP_CALL platform: callee=%p desc=%p arg.tag=%d size=%d "
                    "frames=%zu\n",
                    (void*)callee, (void*)desc, arg_tag, arg_size,
                    vm.frames.size());
                // Dereference Tag::Slot indirections (chase up to 4
                // hops to handle Slot→Slot rebinding) and print the
                // ultimate Value's tag + a few attr names.
                if (arg.tag() == Tag::Slot && arg.asSlot()) {
                    Value * p = arg.asSlot();
                    int hop = 0;
                    while (p && hop < 4) {
                        std::fprintf(stderr,
                            "  slot[%d] @ %p tag=%d", hop, (void*)p,
                            (int)p->tag());
                        if (p->tag() == Tag::Attrs && p->asAttrs()) {
                            const auto & st = ir::globalSymbolTable();
                            auto * b = p->asAttrs();
                            std::fprintf(stderr, " bindings=%p size=%u present=[",
                                (void*)b, (unsigned)b->size);
                            for (uint32_t i = 0; i < b->size && i < 10; ++i) {
                                SymbolId nm = b->entries[i].name;
                                std::fprintf(stderr, "%s%s",
                                    i == 0 ? "" : ",",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            std::fprintf(stderr, "]\n");
                            break;
                        }
                        if (p->tag() == Tag::Slot) {
                            std::fprintf(stderr, " → @ %p\n",
                                (void*)p->asSlot());
                            p = p->asSlot();
                            ++hop;
                            continue;
                        }
                        if (p->tag() == Tag::Thunk && p->asThunk()) {
                            auto * th = p->asThunk();
                            std::fprintf(stderr,
                                " thunk=%p state=%d",
                                (void*)th, (int)th->state);
                            // Chase Evaluated thunks one hop to see the
                            // cached value (the actual Bindings the
                            // closure body will see).
                            if (th->state == ThunkState::Evaluated) {
                                Value & ev = th->evaluated;
                                std::fprintf(stderr,
                                    " evaluated.tag=%d", (int)ev.tag());
                                if (ev.tag() == Tag::Attrs && ev.asAttrs()) {
                                    auto * b = ev.asAttrs();
                                    const auto & st = ir::globalSymbolTable();
                                    std::fprintf(stderr,
                                        " bindings=%p size=%u present=[",
                                        (void*)b, (unsigned)b->size);
                                    for (uint32_t i = 0; i < b->size && i < 10; ++i) {
                                        SymbolId nm = b->entries[i].name;
                                        std::fprintf(stderr, "%s%s",
                                            i == 0 ? "" : ",",
                                            nm < st.size() ? st[nm].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "]");
                                }
                            }
                            std::fprintf(stderr, "\n");
                        } else if (p->isAppLike()) {
                            std::fprintf(stderr,
                                p->tag() == Tag::App3 ? " app3\n" : " app\n");
                        } else {
                            std::fprintf(stderr, "\n");
                        }
                        break;
                    }
                }
            }

            // #495: native fix-point intrinsics fast path.  Recognised
            // at lower-time (lower.cc recogniseIntrinsic), evaluated in
            // v3 without TW round-trips.  Permanent optimization: the
            // bytecode for `lib.fix` etc. never runs; we dispatch
            // directly to a v3-native impl that mirrors the canonical
            // pure-Nix definition.
            //
            // Fix:  `f: let x = f x; in x`
            //   Allocate a heap-stable Value slot (Boehm-traced),
            //   make a Tag::Slot pointing at it, push slot as arg,
            //   call f(slot), store result into slot, return result.
            //   Knot-tying via slot mutation -- exactly what TW's
            //   `let x = f x; in x;` does via Env::values[].
            //
            // Caller invariant: arg is the lambda's `f` (a callable).
            //
            // OPT-IN via NIX_V3_INTRINSIC_DISPATCH=1.  Default OFF until
            // validated against full nixpkgs lib evaluation; the simple
            // case works (`fix ext` returns the right attrset) but
            // nixpkgs lib's fix-point chain (makeExtensible' + extends
            // chain) interacts in ways not yet diagnosed.
            static const bool s_intrinsicEnable =
                std::getenv("NIX_V3_INTRINSIC_DISPATCH") != nullptr;
            if (s_intrinsicEnable && __builtin_expect(
                    desc->intrinsicKind != LambdaDescriptor::Intrinsic::None,
                    0)) {
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::Fix) {
                    // Refuse native dispatch when the user's `f` is a
                    // Bridge thunk (a TW value bridged into v3): TW
                    // lambdas can't handle a v3 Tag::Slot as an arg.
                    // Fall through to the regular bytecode dispatch
                    // which already knows how to bridge across.
                    // (Bridge-thunk arg guard retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
                    allocStats().intrinsicFixCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic Fix dispatch [#%llu]: arg.tag=%d desc=%s\n",
                        (unsigned long long)allocStats().intrinsicFixCalls,
                        (int)arg.tag(),
                        desc->name.empty() ? "<anon>" : desc->name.c_str());
                    // Heap-allocate the slot storage (GC-traced).  Initial
                    // value is Tag::Uninitialized; populated by the body's
                    // result.  Tag::Slot wraps a Value*; reading through
                    // the slot during body eval re-reads from this heap
                    // location, so the body sees the in-progress result
                    // (TW-style knot-tying).
                    Value * slotStorage = Alloc::allocValue();
                    slotStorage->mkUninitialized();
                    Value slotV;
                    slotV.mkSlot(slotStorage);
                    // Save current ip on this frame so callClosure's
                    // re-entry into the dispatch loop can return cleanly.
                    vm.frames.back().ip = ip;
                    // Evaluate `f slotV`.  callClosure forces `fun` (the
                    // user-supplied f) and runs its body with the slot
                    // as arg.  The body may force the slot (chases via
                    // Tag::Slot deref); blackhole detection is per-thunk,
                    // not per-slot, so a self-referential `let x = f x;`
                    // shape works as long as f is sufficiently lazy
                    // (the standard Nix `lib.fix` precondition).
                    Value res = callClosure(vm, arg, slotV);
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic Fix: callClosure returned tag=%d\n",
                        (int)res.tag());
                    // Don't forceValue eagerly -- TW's `let x = f x; in x`
                    // returns whatever `f` returns (could be a thunk if f
                    // is lazy).  Eager force here can drive a Tag::Slot
                    // chase through the as-yet-uninitialised slot.
                    cellWrite(slotStorage, res, nullptr);  // PhD-6: barrier raw writeback
                    push(vm, res);
                    break;
                }
                // STG-13c (#509/#512): native dispatch for ExtendsBody.
                // chain[2] of `extends = overlay: f: final: <body>`.
                // body: `let prev = f final; in prev // overlay final prev`.
                //
                // Closure upvalues at indices `desc->intrinsicVar0`
                // (overlay) and `intrinsicVar1` (f).  arg = final.
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ExtendsBody
                    && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
                    && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues) {
                    allocStats().intrinsicExtendsCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    Value overlay = closureUpvalue(callee, (uint16_t)desc->intrinsicVar0);
                    Value f       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar1);
                    Value final_  = arg;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic ExtendsBody dispatch [#%llu]: "
                        "overlay.tag=%d f.tag=%d final.tag=%d\n",
                        (unsigned long long)allocStats().intrinsicExtendsCalls,
                        (int)overlay.tag(), (int)f.tag(), (int)final_.tag());
                    vm.frames.back().ip = ip;
                    // prev = f(final); force to attrs WHNF.
                    Value prev = callClosure(vm, f, final_);
                    prev = forceValue(vm, prev);
                    if (!prev.isAttrs() || !prev.asAttrs())
                        throw std::runtime_error(
                            "v3 intrinsic ExtendsBody: prev (= f final) didn't reduce to attrs");
                    // overlay_partial = overlay(final), then
                    // overlay_result = overlay_partial(prev); force to attrs.
                    Value overlay_partial = callClosure(vm, overlay, final_);
                    Value overlay_result  = callClosure(vm, overlay_partial, prev);
                    overlay_result = forceValue(vm, overlay_result);
                    if (!overlay_result.isAttrs() || !overlay_result.asAttrs())
                        throw std::runtime_error(
                            "v3 intrinsic ExtendsBody: overlay final prev didn't reduce to attrs");
                    Bindings * merged = mergeBindings(prev.asAttrs(),
                                                      overlay_result.asAttrs(),
                                                      MergeBindingsSite::ExtendsCallPrev);
                    Value res;
                    res.mkAttrs(merged);
                    push(vm, res);
                    break;
                }

                // STG-13c (#509/#512): native dispatch for ComposeBody.
                // chain[3] of `composeExtensions = f: g: final: prev: <body>`.
                // body: `let fApplied = f final prev; prev' = prev //
                // fApplied; in fApplied // g final prev'`.
                //
                // Closure upvalues: intrinsicVar0 (f), intrinsicVar1 (g),
                // intrinsicVar2 (final).  arg = prev.
                if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ComposeBody
                    && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
                    && desc->intrinsicVar2 >= 0
                    && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues
                    && (uint16_t)desc->intrinsicVar2 < callee->nUpvalues) {
                    allocStats().intrinsicComposeCalls++;
                    static const bool s_dbg =
                        std::getenv("V3_DBG_INTRINSIC") != nullptr;
                    Value f       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar0);
                    Value g       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar1);
                    Value final_  = closureUpvalue(callee, (uint16_t)desc->intrinsicVar2);
                    Value prev_   = arg;
                    if (s_dbg) std::fprintf(stderr,
                        "v3 intrinsic ComposeBody dispatch [#%llu]: "
                        "f.tag=%d g.tag=%d final.tag=%d prev.tag=%d\n",
                        (unsigned long long)allocStats().intrinsicComposeCalls,
                        (int)f.tag(), (int)g.tag(), (int)final_.tag(),
                        (int)prev_.tag());
                    vm.frames.back().ip = ip;
                    // fApplied = f final prev; force to attrs.
                    Value f_partial = callClosure(vm, f, final_);
                    Value fApplied  = callClosure(vm, f_partial, prev_);
                    fApplied = forceValue(vm, fApplied);
                    if (!fApplied.isAttrs() || !fApplied.asAttrs())
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: f final prev didn't reduce to attrs");
                    // prev' = prev // fApplied (force prev_ to attrs first).
                    Value prevForced = forceValue(vm, prev_);
                    if (!prevForced.isAttrs() || !prevForced.asAttrs())
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: prev didn't reduce to attrs");
                    Bindings * prevPrimeB = mergeBindings(prevForced.asAttrs(),
                                                          fApplied.asAttrs(),
                                                          MergeBindingsSite::ExtendsCallPrevPrime);
                    Value prevPrime;
                    prevPrime.mkAttrs(prevPrimeB);
                    // gApplied = g final prev'; force to attrs.
                    Value g_partial = callClosure(vm, g, final_);
                    Value gApplied  = callClosure(vm, g_partial, prevPrime);
                    gApplied = forceValue(vm, gApplied);
                    if (!gApplied.isAttrs() || !gApplied.asAttrs())
                        throw std::runtime_error(
                            "v3 intrinsic ComposeBody: g final prev' didn't reduce to attrs");
                    // result = fApplied // gApplied.
                    Bindings * merged = mergeBindings(fApplied.asAttrs(),
                                                       gApplied.asAttrs(),
                                                       MergeBindingsSite::ComposeCallApplied);
                    Value res;
                    res.mkAttrs(merged);
                    push(vm, res);
                    break;
                }

                // Other intrinsic kinds (Extends, ComposeExtensions, ...)
                // fall through to the regular dispatch path below.
                // Step 1's recogniseIntrinsic only sets Fix; future
                // commits add the rest.
            }

            // #424: selector-lambda fast path for `\x: x.f`.  Skips
            // frame allocation + dispatch -- force arg, project the
            // recorded SymbolId, push.  Detected at emit time
            // (emit.cc; sets desc->selectorSym).  Only fires for
            // arity-1 simple-arg lambdas with no upvalues; the
            // frame's withStack invariants are unaffected since we
            // never allocate one.
            if (__builtin_expect(desc->selectorSym != 0, 0)) {
                allocStats().selectorLambdaCalls++;
                Value sArg = arg;
                if (sArg.isThunk() || sArg.isAppLike()
                    || sArg.tag() == Tag::Slot) {
                    vm.frames.back().ip = ip;
                    sArg = forceValue(vm, sArg);
                }
                if (!sArg.isAttrs() || !sArg.asAttrs())
                    throw std::runtime_error(
                        "v3 selector lambda: arg not an attrset");
                // Binary-search the attrset (entries sorted ascending
                // by SymbolId).  Mirrors what OP_ATTRS_SELECT does
                // post-IC-miss; we don't have an IC slot here since
                // there's no allocated bytecode site for the projection.
                const Value * v = sArg.asAttrs()->lookup(
                    desc->selectorSym);
                if (!v)
                    throw std::runtime_error(
                        "v3 selector lambda: missing attr");
                push(vm, *v);
                break;
            }

            // Phase 1.2 identity-lambda fast path: body is `x: x`.
            // Push arg back, skip frame allocation entirely.  Matches
            // the selectorSym pattern above.  Detection in emit.cc.
            if (__builtin_expect(desc->identityLambda, 0)) {
                push(vm, arg);
                break;
            }

            // Closures from imported files own their own CompilationUnit;
            // when callee->cu differs, switch the dispatch loop to the
            // callee's bytecode/constant pools.  Falls back to the caller's
            // cu when the closure was made before cu-tracking landed.
            const CompilationUnit * calleeCu = callee->cu ? callee->cu : cu;

            // Formals validation: when a lambda has formals and no
            // ellipsis, every key in the param attrset must match a
            // declared formal name.  Tree-walker raises with the offending
            // attribute name; we mirror that message format.
            //
            // WC-38: tree-walker forces the arg attrset ALWAYS when the
            // callee has formals (eval.cc state.callFunction calls
            // forceAttrs on arg regardless of ellipsis).  v3 originally
            // skipped this when ellipsis is set, leaving the force to
            // each per-formal thunk (lower.cc:619-652).  This means
            // formal access in the body forces the arg once per access
            // — and crucially, *defers* the force until body execution.
            // For nixpkgs's chain `arg = autoArgs // userArgs` where
            // autoArgs = `intersectAttrs (functionArgs f) pkgs`, the
            // deferred force fires while pkgs (= lib.fix slot) is still
            // Black, causing the WC-38 `with`-lookup miss.
            //
            // NIX_V3_EAGER_ARG_FORCE=1 enables tree-walker semantics:
            // force the arg attrset at call time.  Default off until
            // validated against full lang + wc-laziness suites.
            if (desc->hasFormals) {
                // #681 — for ANY formals lambda (including ellipsis-
                // only), TW forces the arg and validates it's a set
                // (libexpr/eval.cc:1434 forceAttrs in callFunction).
                // Pre-fix v3 deferred the force for ellipsis lambdas,
                // and the type check fired only for already-WHNF
                // primitive args — Thunk-wrapped non-attrsets (e.g.
                // `({ ... }: 1) [ ]` where `[ ]` lowers through a
                // Thunk) slipped through and the body ran with the
                // wrong-typed arg, returning the wrong-vs-TW result.
                //
                // We can't skip the force entirely without breaking
                // the type check.  The original needForce gate was
                // motivated by nixpkgs perf concerns around forcing
                // huge attrset args before checking ellipsis — but
                // since attrsets dominate the formals call sites,
                // the force is usually a no-op (already WHNF).  The
                // remaining cost is a tag check on a cached value.
                static const bool s_eagerArgForce =
                    std::getenv("NIX_V3_EAGER_ARG_FORCE") != nullptr;
                bool needForce = true; (void)s_eagerArgForce;
                if (needForce) {
                    // STG-12 (#498) diagnostic: see what we're about to
                    // force at OP_CALL.  V3_DBG_OPCALL_FORCE=1 to enable.
                    static const bool s_dbg_callforce =
                        std::getenv("V3_DBG_OPCALL_FORCE") != nullptr;
                    if (s_dbg_callforce) {
                        Value chase = arg;
                        Thunk * blackOnFrames = nullptr;
                        for (int hops = 0; hops < 16; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.asSlot())
                                chase = *chase.asSlot();
                            else if (chase.tag() == Tag::Thunk
                                     && chase.asThunk()) {
                                Thunk * th = chase.asThunk();
                                if (th->state == ThunkState::Evaluated) {
                                    chase = th->evaluated;
                                } else if (th->state == ThunkState::Blackhole) {
                                    for (size_t i = 0; i < vm.frames.size(); ++i)
                                        if (vm.frames[i].thunk == th) {
                                            blackOnFrames = th; break;
                                        }
                                    break;
                                } else break;
                            } else break;
                        }
                        if (blackOnFrames) {
                            const auto & curFr = vm.frames.back();
                            const LambdaDescriptor * cd = nullptr;
                            if (curFr.thunk
                                && (curFr.thunk->state == ThunkState::Suspended
                                    || curFr.thunk->state == ThunkState::Blackhole))
                                cd = curFr.thunk->suspended.desc;
                            else if (curFr.closure) cd = curFr.closure->desc;
                            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
                            std::fprintf(stderr,
                                "v3 OP_CALL force-arg → BLACK arg-thunk=%s "
                                "callee=%s formals=%zu ellipsis=%d "
                                "from-frame=%s ip=%u nUpvalues=%u\n",
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                desc->name.empty() ? "<?>" : desc->name.c_str(),
                                desc->formals.size(),
                                (int)desc->ellipsis,
                                cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                                curFr.ip, (unsigned)desc->nUpvalues);
                            const auto & tbl = ir::globalSymbolTable();
                            std::fprintf(stderr, "  callee formals: ");
                            for (size_t i = 0; i < desc->formals.size() && i < 12; ++i) {
                                uint32_t nm = desc->formals[i].name;
                                std::fprintf(stderr, "%s%s", i ? "," : "",
                                    nm < tbl.size() ? tbl[nm].c_str() : "?");
                            }
                            if (desc->formals.size() > 12) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "\n");
                        }
                    }
                    Value forcedArg = forceValue(vm, arg);
                    // #680 — TW raises "expected a set but found <type>"
                    // when a formals-lambda is called with a non-attrset
                    // argument (libexpr/eval.cc:1434 forceAttrs).  Pre-fix
                    // v3 either silently produced a value (when the body
                    // didn't touch the formals — e.g. `({ ... }: 1) 42`)
                    // or surfaced a generic OP_ATTRS_SELECT error from
                    // the destructuring path.
                    if (!forcedArg.isAttrs()) {
                        auto typeWord = [](const Value & v) -> std::pair<const char *, const char *> {
                            Tag t = v.tag();
                            if (t == Tag::Int)    return {"an", "integer"};
                            if (t == Tag::Float)  return {"a",  "float"};
                            if (t == Tag::Bool)   return {"a",  "Boolean"};
                            if (t == Tag::Null)   return {"",   "null"};
                            if (t == Tag::String) return {"a",  "string"};
                            if (t == Tag::Path)   return {"a",  "path"};
                            if (t == Tag::List)   return {"a",  "list"};
                            if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                                return {"a", "function"};
                            return {"a", "value"};
                        };
                        auto [art, name] = typeWord(forcedArg);
                        std::string msg = "expected a set but found ";
                        if (*art) { msg += art; msg += ' '; }
                        msg += name;
                        // #691 — append `: <value>` matching TW.
                        msg += ": ";
                        msg += valueRepr(forcedArg);
                        throw std::runtime_error(msg);
                    }
                    if (forcedArg.asAttrs()) {
                        // #680 — emit TW's exact error phrasing
                        // (libexpr/eval.cc:1849 + extra-arg sibling) so
                        // user-visible formals errors don't expose
                        // "v3 OP_CALL: ..." debug naming.  Lambda name
                        // uses `contextualName` (set by lower.cc from
                        // ExprLambda::name) when present, else falls
                        // back to TW's literal "anonymous lambda".
                        std::string lambdaName = desc && !desc->contextualName.empty()
                            ? desc->contextualName
                            : std::string("anonymous lambda");
                        const Bindings * b = forcedArg.asAttrs();
                        const auto & tbl = ir::globalSymbolTable();
                        // #809 (2026-05-24): diagnostic gate.  When
                        // NIX_V3_PERMISSIVE_FORMALS=1, treat every
                        // lambda as ellipsis=1 (accept extra args
                        // silently).  Used to isolate whether the
                        // strict extra-arg check is the load-bearing
                        // blocker on haskell.nix-class workloads, or
                        // whether downstream over-forcing persists
                        // even after we let extra args through.
                        // OFF-by-default; semantically incorrect when
                        // ON (TW would also reject in pure strict
                        // mode).
                        static const bool s_permissiveFormals =
                            std::getenv("NIX_V3_PERMISSIVE_FORMALS") != nullptr;
                        auto hasFormal = [&](SymbolId name) noexcept {
                            size_t lo = 0, hi = desc->formals.size();
                            while (lo < hi) {
                                size_t mid = (lo + hi) >> 1;
                                SymbolId midName = desc->formals[mid].name;
                                if (midName == name) return true;
                                if (midName < name) lo = mid + 1;
                                else hi = mid;
                            }
                            return false;
                        };
                        // (a) Extra-arg check for non-ellipsis lambdas.
                        // #803 (2026-05-24): print FIRST (under
                        // V3_DBG_FORMALS_DIAG), THEN gate the throw under
                        // !PERMISSIVE_FORMALS.  This lets the trace fire
                        // under PERMISSIVE mode without killing the eval —
                        // captures the H10 divergent call across the long
                        // haskell.nix run.
                        if (!desc->ellipsis) {
                            b->forEach([&](const Bindings::Entry & entry) {
                                SymbolId name = entry.name;
                                bool found = hasFormal(name);
                                if (!found) {
                                    std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                                    // #802 Phase C diag: dump formals +
                                    // passed attrs to localize the haskell.nix
                                    // unexpected-arg-'git' divergence.
                                    static const bool s_dbgFormals =
                                        std::getenv("V3_DBG_FORMALS_DIAG") != nullptr;
                                    if (s_dbgFormals) {
                                        const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                                        // Find caller's frame for context.
                                        const PosSnapshot * callerPs = nullptr;
                                        std::string callerName = "?";
                                        if (vm.frames.size() >= 1) {
                                            CallFrame & cf = vm.frames.back();
                                            if (cf.closure && cf.closure->desc) {
                                                callerName = cf.closure->desc->name.empty()
                                                    ? cf.closure->desc->contextualName
                                                    : cf.closure->desc->name;
                                                callerPs = resolvePosSnapshot(
                                                    cf.closure->desc->posHandle);
                                            }
                                        }
                                        std::fprintf(stderr,
                                            "v3 FORMALS-DIAG "
                                            "callee=%p cu=%p name='%s' ctx='%s' "
                                            "callee_src=%s:%u:%u "
                                            "caller_name='%s' caller_src=%s:%u:%u "
                                            "unexpected='%s' ellipsis=0 "
                                            "passed_attrs=[",
                                            (const void *)desc,
                                            (const void *)(fun.asClosure() ? fun.asClosure()->cu : nullptr),
                                            desc->name.c_str(),
                                            desc->contextualName.c_str(),
                                            ps ? ps->file.c_str() : "?",
                                            ps ? ps->line : 0,
                                            ps ? ps->column : 0,
                                            callerName.c_str(),
                                            callerPs ? callerPs->file.c_str() : "?",
                                            callerPs ? callerPs->line : 0,
                                            callerPs ? callerPs->column : 0,
                                            nm.c_str());
                                        uint32_t printedAttrs = 0;
                                        b->forEach([&](const Bindings::Entry & e) {
                                            if (printedAttrs >= 24) return;
                                            SymbolId sk = e.name;
                                            std::string_view sn = (sk < tbl.size()) ? std::string_view(tbl[sk]) : "?";
                                            std::fprintf(stderr, "%s%.*s",
                                                printedAttrs ? "," : "", (int)sn.size(), sn.data());
                                            ++printedAttrs;
                                        });
                                        std::fprintf(stderr, "] formals=[");
                                        for (size_t k = 0; k < desc->formals.size() && k < 24; ++k) {
                                            SymbolId sk = desc->formals[k].name;
                                            std::string_view sn = (sk < tbl.size()) ? std::string_view(tbl[sk]) : "?";
                                            std::fprintf(stderr, "%s%.*s",
                                                k ? "," : "", (int)sn.size(), sn.data());
                                        }
                                        std::fprintf(stderr, "]\n");
                                        // #815 full callstack dump — trace back
                                        // to identify which expression is making
                                        // the call.
                                        std::fprintf(stderr,
                                            "v3 FORMALS-DIAG-STACK (%zu frames):\n",
                                            vm.frames.size());
                                        for (size_t fi = vm.frames.size();
                                             fi-- > 0 && fi >= (vm.frames.size() > 20
                                                 ? vm.frames.size() - 20 : 0); ) {
                                            const CallFrame & cfr = vm.frames[fi];
                                            const LambdaDescriptor * fd = nullptr;
                                            if (cfr.thunk
                                                && (cfr.thunk->state == ThunkState::Suspended
                                                    || cfr.thunk->state == ThunkState::Blackhole))
                                                fd = cfr.thunk->suspended.desc;
                                            else if (cfr.closure) fd = cfr.closure->desc;
                                            const PosSnapshot * fps =
                                                fd ? resolvePosSnapshot(fd->posHandle) : nullptr;
                                            std::fprintf(stderr,
                                                "  [%zu] name='%s' ctx='%s' src=%s:%u:%u ip=%u\n",
                                                fi,
                                                fd && !fd->name.empty()
                                                    ? fd->name.c_str() : "<?>",
                                                fd && !fd->contextualName.empty()
                                                    ? fd->contextualName.c_str() : "<?>",
                                                (fps && !fps->file.empty())
                                                    ? fps->file.c_str() : "<no-pos>",
                                                fps ? fps->line : 0u,
                                                fps ? fps->column : 0u,
                                                cfr.ip);
                                        }
                                        std::fflush(stderr);
                                    }
                                    if (!s_permissiveFormals)
                                        throw std::runtime_error(
                                            "function '" + lambdaName
                                            + "' called with unexpected argument '"
                                            + nm + "'");
                                    // PERMISSIVE: continue silently
                                    // — keep scanning further extras
                                    // for diagnostic but don't throw.
                                }
                            });
                        }
                        // (b) Missing-arg check: every formal without
                        // a default must be in the input bindings.  TW
                        // does this in eval.cc:1847.  Pre-#680 v3
                        // emitted the generic "attribute 'X' missing"
                        // from the formal-destructure OP_ATTRS_SELECT
                        // (post-#678 alignment); now we catch it earlier
                        // with the function-specific phrasing.
                        for (auto & f : desc->formals) {
                            if (f.hasDefault) continue;
                            if (!b->lookup(f.name)) {
                                std::string nm = (f.name < tbl.size()) ? tbl[f.name] : "?";
                                throw std::runtime_error(
                                    "function '" + lambdaName
                                    + "' called without required argument '"
                                    + nm + "'");
                            }
                        }
                    }
                    arg = forcedArg;
                }
            }

            // Max call-depth check — guards `(x: x x) (x: x x)` and
            // similar non-thunk-mediated infinite recursion.  Tree-walker
            // defaults to 5000; we match that via kMaxCallDepth (see
            // anonymous namespace at top of file).  Cheap O(1) check.
            if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                throw std::runtime_error("v3 OP_CALL: stack overflow; call depth exceeded "
                                          + std::to_string(kMaxCallDepth));

            vm.frames.back().ip = ip;

            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + desc->nLocals);
            vm.valueStack[newBase + 0] = arg;

            // #498 frame-entry diagnostic: log every OP_CALL closure entry
            // with name + local[0] shape.  V3_DBG_FRAME_ENTRY=<name> filters
            // by closure name (e.g. "final" or "self").
            {
                static const char * s_filter =
                    std::getenv("V3_DBG_FRAME_ENTRY");
                if (s_filter && desc && desc->name == s_filter) {
                    Value chase = arg;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.asSlot())
                            chase = *chase.asSlot();
                        else if (chase.tag() == Tag::Thunk && chase.asThunk()
                                 && chase.asThunk()->state == ThunkState::Evaluated)
                            chase = chase.asThunk()->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 FRAME_ENTRY OP_CALL %s codeOff=%u: local[0].tag=%d",
                        desc->name.c_str(), (unsigned)desc->codeOffset,
                        (int)arg.tag());
                    if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                        auto * b = chase.asAttrs();
                        std::fprintf(stderr, " -> attrs size=%u {",
                            b->size);
                        const auto & tbl = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 3; ++i) {
                            uint32_t nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s", i ? "," : "",
                                nm < tbl.size() ? tbl[nm].c_str() : "?");
                        }
                        if (b->size > 3) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }

            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            // #583 (2026-05-15): per-Closure call counter for V3_DBG_ALLOC_DUMP.
            // The OP_MAKE_THUNK side gives us alloc/force-of-thunk, but Closure
            // invocations go through OP_CALL — without this we can't see who
            // re-evaluates `final.isLinux` 1.6M times in nixpkgs hello.name.
            // Gated behind g_dbgAllocDump so the bump (and the dependent
            // register read) only fires when diagnostic mode is on.
            if (__builtin_expect(g_dbgAllocDump, 0) && desc) {
                uint64_t cnt = ++desc->callCount;
                // V3_DBG_HOT_CALLEE=<name>:<every_n>:<max_dumps>
                // dumps the frame stack each time the named callee's
                // callCount hits a multiple of every_n.  Used to find
                // who's driving the hello.name re-eval loop.
                static const char * s_hotCallee =
                    std::getenv("V3_DBG_HOT_CALLEE");
                if (__builtin_expect(s_hotCallee != nullptr, 0)
                    && !desc->name.empty())
                {
                    // Parse: name[:everyN[:maxDumps]]
                    static std::string s_targetName;
                    static uint64_t s_everyN = 100000;
                    static int s_maxDumps = 8;
                    static bool s_parsed = false;
                    if (!s_parsed) {
                        s_parsed = true;
                        std::string spec{s_hotCallee};
                        size_t c1 = spec.find(':');
                        s_targetName = spec.substr(0, c1);
                        if (c1 != std::string::npos) {
                            size_t c2 = spec.find(':', c1 + 1);
                            s_everyN = std::strtoull(
                                spec.substr(c1 + 1, c2 - c1 - 1).c_str(),
                                nullptr, 10);
                            if (c2 != std::string::npos)
                                s_maxDumps = (int)std::strtol(
                                    spec.substr(c2 + 1).c_str(),
                                    nullptr, 10);
                        }
                        if (s_everyN == 0) s_everyN = 100000;
                    }
                    if (desc->name == s_targetName
                        && (cnt % s_everyN) == 0
                        && s_maxDumps > 0)
                    {
                        --s_maxDumps;
                        std::fprintf(stderr,
                            "\nv3 HOT_CALLEE %s[cnt=%llu] frames=%zu:\n",
                            desc->name.c_str(),
                            (unsigned long long)cnt,
                            vm.frames.size());
                        size_t lim = vm.frames.size();
                        size_t depth = std::min<size_t>(lim, 16);
                        for (size_t i = lim; i > 0 && i + depth > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            const LambdaDescriptor * d2 = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d2 = fr.thunk->suspended.desc;
                            else if (fr.closure) d2 = fr.closure->desc;
                            const PosSnapshot * ps =
                                d2 ? resolvePosSnapshot(d2->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  [%zu] %s @ %s:%u:%u ip=%u\n",
                                i - 1,
                                d2 && !d2->name.empty() ? d2->name.c_str()
                                    : (d2 ? "<anon>" : "<?>"),
                                (ps && !ps->file.empty()) ? ps->file.c_str()
                                    : "<no-pos>",
                                ps ? ps->line : 0u, ps ? ps->column : 0u,
                                fr.ip);
                        }
                        std::fflush(stderr);
                    }
                }
            }
            // Push the new frame in a single move-construct: lets the
            // compiler initialize the trailing 40 bytes inline at the
            // back of the vector rather than emplace_back + 7 separate
            // field stores.  Frames are pre-reserved so push_back never
            // reallocates on the hot path.
            vm.frames.push_back(CallFrame{
                .cu = calleeCu,
                .closure = callee,
                .thunk = nullptr,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = 0,
            });
            pushCapturedWiths(vm, callee->capturedWiths);

            ip = desc->codeOffset;
            cu  = calleeCu;
            closure = callee;
            stackBase = newBase;
            break;
        }
        case OP_TAIL_CALL: {
            // Tail call: same semantics as OP_CALL but reuses the
            // current frame — no frame push.  Lets long recursive
            // chains run in O(1) frame stack space.
            //
            // Tail-iteration guard: catch infinite tail recursion
            // (`(x: x x) (x: x x)`) which the frame-stack limit
            // can't see because we don't grow the stack.  Tree-walker
            // catches it via C-stack overflow.  We bound at 10^7
            // iterations between frame-stack changes; ~99% headroom
            // over any real-world deep tail recursion.
            constexpr size_t kMaxTailCalls = 10'000'000;
            if (__builtin_expect(++vm.tailCallCount >= kMaxTailCalls, 0)) {
                vm.tailCallCount = 0;
                throw std::runtime_error("v3 OP_TAIL_CALL: tail-call iteration limit exceeded "
                                          + std::to_string(kMaxTailCalls)
                                          + " (likely infinite recursion)");
            }

            // Falls back to OP_CALL behaviour for non-closure callees
            // (primops, __functor, partial application) since those
            // need the full OP_CALL machinery.  We jump back into the
            // OP_CALL case via goto.
            Value arg = pop(vm), fun = pop(vm);
            // STG-12 diagnostic: when the topmost prev thunk dispatches
            // TAIL_CALL on a Black-chasing arg, log the fun's tag and
            // (if closure) name + hasFormals.
            static const bool s_dbgTcPre =
                std::getenv("V3_DBG_TC_PRE") != nullptr;
            if (__builtin_expect(s_dbgTcPre, 0)) {
                Value chase = arg;
                Thunk * blackOnFrames = nullptr;
                for (int hops = 0; hops < 16; ++hops) {
                    if (chase.tag() == Tag::Slot && chase.asSlot())
                        chase = *chase.asSlot();
                    else if (chase.tag() == Tag::Thunk && chase.asThunk()) {
                        Thunk * th = chase.asThunk();
                        if (th->state == ThunkState::Evaluated)
                            chase = th->evaluated;
                        else if (th->state == ThunkState::Blackhole) {
                            for (size_t i = 0; i < vm.frames.size(); ++i)
                                if (vm.frames[i].thunk == th) {
                                    blackOnFrames = th; break;
                                }
                            break;
                        } else break;
                    } else break;
                }
                if (blackOnFrames) {
                    const char * funName = "<?>";
                    int funIsClosure = (int)fun.isClosure();
                    int funHasFormals = -1;
                    int funEllipsis = -1;
                    int funThunkState = -1;
                    const char * thunkName = "";
                    if (fun.isClosure() && fun.asClosure()
                        && fun.asClosure()->desc) {
                        funName = fun.asClosure()->desc->name.c_str();
                        funHasFormals = (int)fun.asClosure()->desc->hasFormals;
                        funEllipsis = (int)fun.asClosure()->desc->ellipsis;
                    } else if (fun.tag() == Tag::Thunk && fun.asThunk()) {
                        funThunkState = (int)fun.asThunk()->state;
                        if (fun.asThunk()->state == ThunkState::Suspended
                            || fun.asThunk()->state == ThunkState::Blackhole) {
                            const auto * d = fun.asThunk()->suspended.desc;
                            if (d) thunkName = d->name.c_str();
                        }
                    }
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL pre-dispatch BLACK arg: "
                        "fun.tag=%d ptr=%p isClosure=%d name=%s hasFormals=%d ellipsis=%d "
                        "thunkState=%d thunkName=%s\n",
                        (int)fun.tag(), fun.asThunk(),
                        funIsClosure, funName,
                        funHasFormals, funEllipsis,
                        funThunkState, thunkName);
                }
            }
            // eval/apply (#3, gate-on only): a TAIL call of a multi-arity
            // closure or closure-PAP.  Resolve the leaf closure + collected-
            // arg depth for BOTH a bare arity-N closure (`(a:b:…) x` in tail
            // position — depth 0) and a Tag::App PAP chain (`go (i+1) next` —
            // depth ≥ 1).  Gate-off inert (arity>1 closures don't exist).
            {
                const Closure * tcBase = nullptr; size_t papDepth = 0;
                if (fun.tag() == Tag::Closure && fun.asClosure()) {
                    tcBase = fun.asClosure();
                } else if (fun.isAppLike() && fun.asPair()) {
                    // C-10 (CODEBASE_REVIEW_2026-06-11): walk App AND App3 spines
                    // (App3 carries two applied args → +2 depth), so an App3 PAP
                    // callee in tail position is handled rather than mishandled.
                    const Value * c0 = &fun;
                    while ((c0->tag() == Tag::App || c0->tag() == Tag::App3)
                           && c0->asPair()) {
                        papDepth += (c0->tag() == Tag::App3) ? 2 : 1;
                        c0 = &c0->asPair()->left;
                    }
                    if (c0->tag() == Tag::Closure && c0->asClosure())
                        tcBase = c0->asClosure();
                }
                if (tcBase && tcBase->desc && tcBase->desc->arity > 1) {
                    const uint8_t A = tcBase->desc->arity;
                    const size_t total = papDepth + 1;
                    if (total < A) {
                        // Under-applied in tail position: the result is a PAP
                        // (a value, NOT a tail call).  Build it, push it, and
                        // fall through to the OP_RETURN the tail-call peephole
                        // left right after this OP_TAIL_CALL (it rewrote the
                        // OP_CALL in place), which returns the PAP to the
                        // caller.  Without this, the frame would wrongly enter
                        // the arity-N body with too few args (leaving the
                        // unfilled param slots Uninitialized).
                        ValuePair * vp = Alloc::allocPair();
                        vp->left = fun; vp->right = arg;
                        pairPostConstructBarrier(vp);
                        Value v;
                        v.mkPair(Tag::App, vp);
                        push(vm, v);
                        break;  // ip already points at the trailing OP_RETURN
                    }
                    // Saturated (total == A — fun is a Tag::App PAP here, since
                    // a bare arity>1 closure with 1 arg is under-applied above):
                    // reuse the current frame (preserving O(1) tail recursion;
                    // the op_call_dispatch fallback below would PUSH a frame
                    // and overflow deep folds).  Mirrors the in-place retarget
                    // below (withStack reset + captured-withs).
                    if (A > 16) throw std::runtime_error("v3 OP_TAIL_CALL: arity > 16");
                    if (__builtin_expect(total > A, 0)) {
                        // OVER-APPLICATION (R2, 2026-06-12): the leaf closure's
                        // arity-A body returns a FUNCTION that consumes the
                        // remaining (total-A) args.  This arises when an App3 PAP
                        // packs two args at once over an arity-2 callback whose
                        // body is a function (e.g. `mapAttrs (n: v: <fn>) attrs`,
                        // the only App3 builders) and that PAP reaches a call site
                        // UNFORCED with a further arg (e.g. `map (g: g x) (attrValues
                        // (mapAttrs …))`).  An App-only spine can't reach depth≥A
                        // (it saturates at A), so only App3 triggers this.
                        //
                        // TW applies one arg at a time: the leaf saturates at A,
                        // yields the function, which then consumes the rest.  Replay
                        // the whole spine + the new arg through callClosure from the
                        // leaf closure (callClosure saturates then re-applies — byte-
                        // identical to `total` curried OP_CALLs; mirrors the
                        // OP_TAIL_CALL_N fallback below).  REPLACES the prior
                        // `throw "PAP over-applied"`, which was simply unimplemented
                        // over-application.
                        Value tmp2[16]; size_t nT = 0;
                        const Value * w2 = &fun;
                        while (w2->isAppLike() && w2->asPair()) {
                            const ValuePair * pp2 = w2->asPair();
                            if (w2->tag() == Tag::App3) {
                                tmp2[nT++] = pp2->third; tmp2[nT++] = pp2->right;
                            } else {
                                tmp2[nT++] = pp2->right;
                            }
                            w2 = &pp2->left;
                        }
                        Value f = *w2;  // leaf closure (spine bottom)
                        // tmp2 holds spine args newest-first; apply oldest-first.
                        for (size_t i = nT; i > 0; --i) f = callClosure(vm, f, tmp2[i - 1]);
                        f = callClosure(vm, f, arg);
                        push(vm, f);
                        break;  // trailing OP_RETURN returns f to the caller
                    }
                    Value argbuf[16];
                    // C-10: App3-aware spine gather (mirrors OP_CALL + the
                    // force-spine path: App3 → third then right; App → right;
                    // closure-slot order is the collection reversed, new arg last).
                    Value tmp[16]; size_t nTmp = 0;
                    const Value * w = &fun;
                    while (w->isAppLike() && w->asPair()) {
                        const ValuePair * p = w->asPair();
                        if (w->tag() == Tag::App3) {
                            tmp[nTmp++] = p->third; tmp[nTmp++] = p->right;
                        } else {
                            tmp[nTmp++] = p->right;
                        }
                        w = &p->left;
                    }
                    for (size_t i = 0; i < nTmp; ++i) argbuf[i] = tmp[nTmp - 1 - i];
                    argbuf[A - 1] = arg;
                    const LambdaDescriptor * d = tcBase->desc;
                    const CompilationUnit * baseCu = tcBase->cu ? tcBase->cu : cu;
                    vm.valueStack.resize(stackBase + d->nLocals);
                    for (size_t i = 0; i < A; ++i)
                        vm.valueStack[stackBase + i] = argbuf[i];
                    CallFrame & cur = vm.frames.back();
                    cur.cu = baseCu;
                    cur.closure = tcBase;
                    cur.ip = d->codeOffset;
                    if (vm.withStack.size() > cur.withStackBase)
                        vm.withStack.resize(cur.withStackBase);
                    cur.withStackBase = static_cast<uint32_t>(vm.withStack.size());
                    pushCapturedWiths(vm, tcBase->capturedWiths);
                    ip = d->codeOffset;
                    cu = baseCu;
                    closure = tcBase;
                    break;
                }
            }
            if (!fun.isClosure()) {
                // Push back and replay through OP_CALL.
                push(vm, fun);
                push(vm, arg);
                goto op_call_dispatch;
            }
            const Closure * tcCallee = fun.asClosure();
            // #705 (2026-05-21): defensive null-desc check (similar to
            // forceValue's STALE THUNK detection).  If tcCallee was a
            // stale nursery closure that got memset, desc reads as 0.
            // Catch here with diagnostic + abort so we can localize
            // the missed-root source.
            {
                const Nursery & nu = threadNursery();
                if (__builtin_expect(!tcCallee || !tcCallee->desc, 0)) {
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL: STALE callee suspected.\n"
                        "  tcCallee=%p in-nursery=%s desc=%p nUpvalues=%u\n"
                        "  ip=%u frames=%zu exitDepth=%zu\n",
                        (const void*)tcCallee,
                        tcCallee && nu.contains(tcCallee) ? "YES" : "no",
                        tcCallee ? (const void*)tcCallee->desc : nullptr,
                        tcCallee ? (unsigned)tcCallee->nUpvalues : 0,
                        (unsigned)(ip - 1), vm.frames.size(),
                        exitDepth);
                    if (tcCallee && nu.contains(tcCallee)) {
                        // Search arena for refs to this stale pointer.
                        Arena & arena = threadArena();
                        auto blocks = arena.blockRanges();
                        uintptr_t target = reinterpret_cast<uintptr_t>(tcCallee);
                        size_t hits = 0;
                        for (auto & blk : blocks) {
                            uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
                            uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
                            lo = (lo + 7) & ~uintptr_t{7};
                            for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
                                if (*reinterpret_cast<const uintptr_t *>(p) != target) continue;
                                if (hits++ < 4) {
                                    uint64_t prev = p >= 8
                                        ? *reinterpret_cast<const uint64_t *>(p - 8) : 0;
                                    std::fprintf(stderr,
                                        "  arena @ %p (prev word 0x%llx tag=%d)\n",
                                        (void*)p,
                                        (unsigned long long)prev,
                                        (int)(prev & 0xff));
                                }
                            }
                        }
                        std::fprintf(stderr, "  total arena refs to tcCallee: %zu\n", hits);
                    }
                    std::fflush(stderr);
                    throw std::runtime_error("v3 OP_TAIL_CALL: stale callee");
                }
            }
            const LambdaDescriptor * tcDesc = tcCallee->desc;
            const CompilationUnit * tcCalleeCu = tcCallee->cu ? tcCallee->cu : cu;

            // Same eager-arg-force as OP_CALL — see WC-38 explanation above.
            if (tcDesc->hasFormals) {
                // #680 — pre-force type check (mirror of OP_CALL site).
                if (arg.tag() != Tag::Attrs
                    && arg.tag() != Tag::Thunk
                    && !arg.isAppLike()
                    && arg.tag() != Tag::Slot)
                {
                    auto typeWord = [](const Value & v) -> std::pair<const char *, const char *> {
                        Tag t = v.tag();
                        if (t == Tag::Int)    return {"an", "integer"};
                        if (t == Tag::Float)  return {"a",  "float"};
                        if (t == Tag::Bool)   return {"a",  "Boolean"};
                        if (t == Tag::Null)   return {"",   "null"};
                        if (t == Tag::String) return {"a",  "string"};
                        if (t == Tag::Path)   return {"a",  "path"};
                        if (t == Tag::List)   return {"a",  "list"};
                        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                            return {"a", "function"};
                        return {"a", "value"};
                    };
                    auto [art, name] = typeWord(arg);
                    std::string msg = "expected a set but found ";
                    if (*art) { msg += art; msg += ' '; }
                    msg += name;
                    // #691 — append the value via shared valueRepr.
                    msg += ": ";
                    msg += valueRepr(arg);
                    throw std::runtime_error(msg);
                }
                static const bool s_eagerArgForce =
                    std::getenv("NIX_V3_EAGER_ARG_FORCE") != nullptr;
                bool needForce = !tcDesc->ellipsis || s_eagerArgForce;
                if (needForce) {
                    // STG-12 (#498) diagnostic: see what we're about to
                    // force.  V3_DBG_TAIL_FORCE=1 to enable.
                    static const bool s_dbg_tcforce =
                        std::getenv("V3_DBG_TAIL_FORCE") != nullptr;
                    if (s_dbg_tcforce) {
                        Value chase = arg;
                        Thunk * blackOnFrames = nullptr;
                        for (int hops = 0; hops < 16; ++hops) {
                            if (chase.tag() == Tag::Slot && chase.asSlot())
                                chase = *chase.asSlot();
                            else if (chase.tag() == Tag::Thunk
                                     && chase.asThunk()) {
                                Thunk * th = chase.asThunk();
                                if (th->state == ThunkState::Evaluated) {
                                    chase = th->evaluated;
                                } else if (th->state == ThunkState::Blackhole) {
                                    for (size_t i = 0; i < vm.frames.size(); ++i)
                                        if (vm.frames[i].thunk == th) {
                                            blackOnFrames = th; break;
                                        }
                                    break;
                                } else break;
                            } else break;
                        }
                        if (blackOnFrames) {
                            const auto & curFr = vm.frames.back();
                            const LambdaDescriptor * cd = nullptr;
                            if (curFr.thunk
                                && (curFr.thunk->state == ThunkState::Suspended
                                    || curFr.thunk->state == ThunkState::Blackhole))
                                cd = curFr.thunk->suspended.desc;
                            else if (curFr.closure) cd = curFr.closure->desc;
                            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
                            std::fprintf(stderr,
                                "v3 OP_TAIL_CALL force-arg → BLACK arg-thunk=%s "
                                "callee=%s formals.size=%zu ellipsis=%d "
                                "from-frame=%s ip=%u (cu=%p code-off=%u)\n",
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                tcDesc->name.empty() ? "<?>" : tcDesc->name.c_str(),
                                tcDesc->formals.size(),
                                (int)tcDesc->ellipsis,
                                cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                                curFr.ip, (const void *)curFr.cu,
                                cd ? cd->codeOffset : 0u);
                            // Print formals names.
                            const auto & tbl = ir::globalSymbolTable();
                            std::fprintf(stderr, "  callee formals: ");
                            for (size_t i = 0; i < tcDesc->formals.size() && i < 8; ++i) {
                                uint32_t nm = tcDesc->formals[i].name;
                                std::fprintf(stderr, "%s%s", i ? "," : "",
                                    nm < tbl.size() ? tbl[nm].c_str() : "?");
                            }
                            if (tcDesc->formals.size() > 8) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "\n");
                        }
                    }
                    Value forcedArg = forceValue(vm, arg);
                    // #680 — TAIL_CALL path mirror of OP_CALL formals
                    // validation.  Type-check arg is a set; emit TW's
                    // exact phrasing for extra/missing args.
                    if (!forcedArg.isAttrs()) {
                        auto typeWord = [](const Value & v) -> std::pair<const char *, const char *> {
                            Tag t = v.tag();
                            if (t == Tag::Int)    return {"an", "integer"};
                            if (t == Tag::Float)  return {"a",  "float"};
                            if (t == Tag::Bool)   return {"a",  "Boolean"};
                            if (t == Tag::Null)   return {"",   "null"};
                            if (t == Tag::String) return {"a",  "string"};
                            if (t == Tag::Path)   return {"a",  "path"};
                            if (t == Tag::List)   return {"a",  "list"};
                            if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                                return {"a", "function"};
                            return {"a", "value"};
                        };
                        auto [art, name] = typeWord(forcedArg);
                        std::string msg = "expected a set but found ";
                        if (*art) { msg += art; msg += ' '; }
                        msg += name;
                        // #691 — append `: <value>` matching TW.
                        msg += ": ";
                        msg += valueRepr(forcedArg);
                        throw std::runtime_error(msg);
                    }
                    if (forcedArg.asAttrs()) {
                        std::string lambdaName = tcDesc && !tcDesc->contextualName.empty()
                            ? tcDesc->contextualName
                            : std::string("anonymous lambda");
                        const Bindings * b = forcedArg.asAttrs();
                        // #825 Phase C SPIKE: materialise Chain — see
                        // the matching note in the OP_CALL formals path
                        // above (~line 4895).
                        if (b && b->isChain()) b = b->materialize();
                        const auto & tbl = ir::globalSymbolTable();
                        // #809: same NIX_V3_PERMISSIVE_FORMALS gate as
                        // the call-site path above.  Both OP_CALL and
                        // OP_TAIL_CALL hit this check; both must obey
                        // the gate or the diagnostic isn't honest.
                        static const bool s_permissiveFormalsTC =
                            std::getenv("NIX_V3_PERMISSIVE_FORMALS") != nullptr;
                        if (!tcDesc->ellipsis && !s_permissiveFormalsTC) {
                            for (uint32_t i = 0; i < b->size; ++i) {
                                SymbolId name = b->entries[i].name;
                                bool found = false;
                                for (auto & f : tcDesc->formals)
                                    if (f.name == name) { found = true; break; }
                                if (!found) {
                                    std::string nm = (name < tbl.size()) ? tbl[name] : "?";
                                    throw std::runtime_error(
                                        "function '" + lambdaName
                                        + "' called with unexpected argument '"
                                        + nm + "'");
                                }
                            }
                        }
                        for (auto & f : tcDesc->formals) {
                            if (f.hasDefault) continue;
                            bool found = false;
                            for (uint32_t i = 0; i < b->size; ++i)
                                if (b->entries[i].name == f.name) { found = true; break; }
                            if (!found) {
                                std::string nm = (f.name < tbl.size()) ? tbl[f.name] : "?";
                                throw std::runtime_error(
                                    "function '" + lambdaName
                                    + "' called without required argument '"
                                    + nm + "'");
                            }
                        }
                    }
                    arg = forcedArg;
                }
            }

            // Reuse the current frame: shrink valueStack down to our
            // stackBase, then resize for the callee's locals.  The
            // outer-frame's stackBaseOffset and CallFrame stay put;
            // we just retarget cu/closure/ip and overwrite locals.
            vm.valueStack.resize(stackBase + tcDesc->nLocals);
            vm.valueStack[stackBase + 0] = arg;

            // #498 frame-entry diagnostic for OP_TAIL_CALL.
            {
                static const char * s_filter =
                    std::getenv("V3_DBG_FRAME_ENTRY");
                if (s_filter && tcDesc && tcDesc->name == s_filter) {
                    Value chase = arg;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.asSlot())
                            chase = *chase.asSlot();
                        else if (chase.tag() == Tag::Thunk && chase.asThunk()
                                 && chase.asThunk()->state == ThunkState::Evaluated)
                            chase = chase.asThunk()->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 FRAME_ENTRY OP_TAIL_CALL %s codeOff=%u: local[0].tag=%d",
                        tcDesc->name.c_str(),
                        (unsigned)tcDesc->codeOffset, (int)arg.tag());
                    if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                        auto * b = chase.asAttrs();
                        std::fprintf(stderr, " -> attrs size=%u {",
                            b->size);
                        const auto & tbl = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 4; ++i) {
                            uint32_t nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s", i ? "," : "",
                                nm < tbl.size() ? tbl[nm].c_str() : "?");
                        }
                        if (b->size > 4) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }

            // Update the existing frame in place (don't push a new one).
            CallFrame & cur = vm.frames.back();
            // V3_DBG_STORE_PREVSTAGE: trace when OP_TAIL_CALL retargets
            // a frame that has CFF_THUNK_RETURN — this is the path that
            // can corrupt thunk evaluated values (WC-37 hypothesis).
            {
                static const bool s_dbg_tc =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_tc && (cur.flags & CFF_THUNK_RETURN) && cur.thunk) {
                    std::fprintf(stderr,
                        "v3 OP_TAIL_CALL on thunk-frame: thunk %p nUp=%u "
                        "old_cu=%p old_ip=%u -> new_cu=%p new_ip=%u "
                        "callee=%s nUp=%u\n",
                        (void*)cur.thunk,
                        (unsigned)cur.thunk->nUpvalues,
                        (void*)cur.cu, cur.ip,
                        (void*)tcCalleeCu, tcDesc->codeOffset,
                        !tcDesc->name.empty() ? tcDesc->name.c_str() : "<anon>",
                        tcDesc->nUpvalues);
                }
            }
            cur.cu = tcCalleeCu;
            // Phase A5 (RCA 2026-05-11): when a CFF_THUNK_RETURN frame's
            // body tail-calls into a real closure, cur.closure is
            // replaced with the callee's REAL closure pointer.  The
            // original fakeClo is no longer referenced; Boehm GC will
            // reclaim it.  At OP_RETURN, the recycle path WOULD try to
            // pool cur.closure — but the sentinel check in
            // recycleFakeClo (kFakeCloMagic in _pad) rejects real
            // closures, preventing the cell-stored-closure corruption
            // described in lode/RCA_FAMILY_DIVERGENCE_ROOTCAUSE_2026-05-11.md.
            cur.closure = tcCallee;
            // thunk stays whatever it was — if we're inside a thunk
            // re-entry frame, the thunk should still be set when
            // we eventually OP_RETURN.
            cur.ip = tcDesc->codeOffset;
            // stackBaseOffset is unchanged: we reuse the same
            // operand-stack window.
            //
            // Phase-13 review HIGH-1 fix: the callee gets its OWN
            // with-scope, so we MUST truncate the with-stack and
            // reset withStackBase to the new size *before* pushing
            // the callee's captured withs.  Previously we kept the
            // outer's withs on the stack, which leaked names from
            // the caller's `with` chain into the tail-callee's
            // OP_WITH_LOOKUP scope (cross-closure tail call).  Self-
            // recursive TC was unaffected because the captures match.
            if (vm.withStack.size() > cur.withStackBase)
                vm.withStack.resize(cur.withStackBase);
            cur.withStackBase = static_cast<uint32_t>(vm.withStack.size());

            // Push the callee's captured-withs on top of the now-
            // truncated with-stack.  They get popped at OP_RETURN
            // since withStackBase tracks the new floor.
            pushCapturedWiths(vm, tcCallee->capturedWiths);

            ip = tcDesc->codeOffset;
            cu = tcCalleeCu;
            closure = tcCallee;
            // stackBase unchanged.
            break;
        }
        case OP_CALL_N: {
            // eval/apply call-SITE optimization (lever B): saturated n-arg
            // call.  Stack: fun then a0..a_{n-1} (a_{n-1} on top).  When fun
            // is a bare closure of arity == n, enter its body with the n args
            // in slots 0..n-1 DIRECTLY — no intermediate PAP (Tag::App) pair
            // per partial application (the dominant per-iteration allocation
            // in every bytecode fold/loop).  Any other shape (thunk/slot/PAP
            // fun, under/over-arity, primop, __functor, non-closure) falls
            // back to applying the args one at a time via callClosure, which
            // is byte-for-byte identical to n curried OP_CALLs.
            vm.tailCallCount = 0;
            const uint32_t n = operand;
            Value argbuf[16];
            for (uint32_t i = n; i > 0; --i) argbuf[i - 1] = pop(vm);
            Value fun = pop(vm);
            // Resolve a Slot/Thunk fun to WHNF so the fast path sees the
            // closure (e.g. `go` via REC_BINDING_SLOT_REF is a Tag::Slot;
            // a recursive callee may be a Thunk).  A Tag::App PAP is left for
            // the fallback, which accumulates it correctly via callClosure.
            if (fun.tag() == Tag::Slot || fun.tag() == Tag::Thunk)
                fun = forceValue(vm, fun);
            if (fun.tag() == Tag::Closure && fun.asClosure()
                && fun.asClosure()->desc
                && fun.asClosure()->desc->arity == n) {
                const Closure * c = fun.asClosure();
                const LambdaDescriptor * d = c->desc;
                if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                    throw std::runtime_error(
                        "v3 OP_CALL_N: stack overflow; call depth exceeded "
                        + std::to_string(kMaxCallDepth));
                vm.frames.back().ip = ip;
                size_t newBase = vm.valueStack.size();
                vm.valueStack.resize(newBase + d->nLocals);
                for (uint32_t i = 0; i < n; ++i)
                    vm.valueStack[newBase + i] = argbuf[i];
                uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
                const CompilationUnit * calleeCu = c->cu ? c->cu : cu;
                vm.frames.push_back(CallFrame{
                    .cu = calleeCu,
                    .closure = c,
                    .thunk = nullptr,
                    .ip = d->codeOffset,
                    .stackBaseOffset = static_cast<uint32_t>(newBase),
                    .withStackBase = newWithBase,
                    .flags = 0,
                });
                pushCapturedWiths(vm, c->capturedWiths);
                ip = d->codeOffset;
                cu = calleeCu;
                closure = c;
                stackBase = newBase;
                break;
            }
            // Fallback: n curried applications (callClosure resolves thunk/
            // slot/PAP/primop/functor fun and builds PAPs for under-arity).
            Value f = fun;
            for (uint32_t i = 0; i < n; ++i)
                f = callClosure(vm, f, argbuf[i]);
            push(vm, f);
            break;
        }
        case OP_TAIL_CALL_N: {
            // Tail variant of OP_CALL_N: a saturated arity-n closure REUSES
            // the current frame (O(1) tail recursion + no PAP) — mirrors
            // OP_TAIL_CALL's in-place retarget.  Non-saturated shapes build
            // the value via callClosure and fall through to the trailing
            // OP_RETURN the coalescer/peephole left after this op.
            constexpr size_t kMaxTailCallsN = 10'000'000;
            if (__builtin_expect(++vm.tailCallCount >= kMaxTailCallsN, 0)) {
                vm.tailCallCount = 0;
                throw std::runtime_error("v3 OP_TAIL_CALL_N: tail-call iteration limit exceeded "
                                          + std::to_string(kMaxTailCallsN)
                                          + " (likely infinite recursion)");
            }
            const uint32_t n = operand;
            Value argbuf[16];
            for (uint32_t i = n; i > 0; --i) argbuf[i - 1] = pop(vm);
            Value fun = pop(vm);
            // Resolve Slot/Thunk fun to WHNF (see OP_CALL_N) — critical here:
            // the recursive tail callee (`go`) is a Tag::Slot, and without
            // this the fast frame-reuse path is missed, so the fallback's
            // C-recursive callClosure grows the frame stack and overflows on
            // deep tail recursion (the whole point of the tail variant).
            if (fun.tag() == Tag::Slot || fun.tag() == Tag::Thunk)
                fun = forceValue(vm, fun);
            if (fun.tag() == Tag::Closure && fun.asClosure()
                && fun.asClosure()->desc
                && fun.asClosure()->desc->arity == n) {
                const Closure * c = fun.asClosure();
                const LambdaDescriptor * d = c->desc;
                const CompilationUnit * baseCu = c->cu ? c->cu : cu;
                vm.valueStack.resize(stackBase + d->nLocals);
                for (uint32_t i = 0; i < n; ++i)
                    vm.valueStack[stackBase + i] = argbuf[i];
                CallFrame & cur = vm.frames.back();
                cur.cu = baseCu;
                cur.closure = c;
                cur.ip = d->codeOffset;
                if (vm.withStack.size() > cur.withStackBase)
                    vm.withStack.resize(cur.withStackBase);
                cur.withStackBase = static_cast<uint32_t>(vm.withStack.size());
                pushCapturedWiths(vm, c->capturedWiths);
                ip = d->codeOffset;
                cu = baseCu;
                closure = c;
                break;
            }
            // Non-saturated: build the value, push it, fall through to the
            // trailing OP_RETURN (which returns it to our caller).
            Value f = fun;
            for (uint32_t i = 0; i < n; ++i)
                f = callClosure(vm, f, argbuf[i]);
            push(vm, f);
            break;
        }
        case OP_R_RETURN:   // reg-VM Phase 5: retVal from a slot (see below)
        case OP_RETURN: {
            // #787 (2026-05-23) per-phase breakdown for #786 OPCYCLES
            // OP_RETURN-dominates finding.  Gated by env var; three
            // markers (pre-pop / pre-thunk / post-everything) credit
            // deltas to per-phase accumulators.  Per-marker overhead:
            // ~10-20 ns clock read; total = 60 ns/return × 775K =
            // ~46 ms overhead under the gate.
            static const bool s_dbgRetBd =
                std::getenv("NIX_V3_DBG_RETURN_BREAKDOWN") != nullptr;
            uint64_t retBdT0 = 0;
            if (__builtin_expect(s_dbgRetBd, 0)) [[unlikely]] {
                retBdT0 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            // Reset the tail-call counter — once we return out of a
            // tail-recursive burst, subsequent tail calls in a
            // different chain start fresh.
            vm.tailCallCount = 0;
            // reg-VM Phase 5: OP_R_RETURN reads the result from slot `operand`
            // (no GET_LOCAL + pop); OP_RETURN pops it off the operand stack.
            // Everything below is shared — retVal is copied before the frame
            // teardown (resize(fStackBase)) drops the slot region.
            Value retVal = (op == OP_R_RETURN)
                ? vm.valueStack[stackBase + operand]
                : pop(vm);
            // Phase A5: trace EVERY OP_RETURN whose frame's codeOff
            // matches V3_DBG_RETURN_AT_CODEOFF.  Logs retVal's tag +
            // (for closures) the closure-body codeOff so we can see
            // exactly what value a target thunk's body produces.
            {
                static const char * s_atCo =
                    std::getenv("V3_DBG_RETURN_AT_CODEOFF");
                if (__builtin_expect(s_atCo != nullptr, 0)) {
                    uint32_t targetCo = static_cast<uint32_t>(std::atoi(s_atCo));
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure) d = fr.closure->desc;
                    if (d && d->codeOffset == targetCo) {
                        const PosSnapshot * dps =
                            resolvePosSnapshot(d->posHandle);
                        std::fprintf(stderr,
                            "v3 RETURN@codeOff[%u] name=%s pos=%s:%u:%u "
                            "ip=%u retVal-tag=%u flags=0x%x thunk=%p",
                            targetCo,
                            !d->name.empty() ? d->name.c_str() : "<?>",
                            (dps && !dps->file.empty()) ? dps->file.c_str() : "<no-pos>",
                            dps ? dps->line : 0u, dps ? dps->column : 0u,
                            ip - 1, (unsigned)retVal.tag(),
                            (unsigned)fr.flags, (const void *)fr.thunk);
                        if (retVal.tag() == Tag::Closure
                            && retVal.asClosure()
                            && retVal.asClosure()->desc) {
                            auto * cd = retVal.asClosure()->desc;
                            const PosSnapshot * cps =
                                resolvePosSnapshot(cd->posHandle);
                            std::fprintf(stderr,
                                " retVal-closure=%s@%s:%u:%u codeOff=%u nUp=%u",
                                !cd->name.empty() ? cd->name.c_str() : "<?>",
                                (cps && !cps->file.empty()) ? cps->file.c_str() : "<no-pos>",
                                cps ? cps->line : 0u, cps ? cps->column : 0u,
                                cd->codeOffset,
                                retVal.asClosure()->nUpvalues);
                        } else if (retVal.tag() == Tag::Attrs
                                   && retVal.asAttrs()) {
                            std::fprintf(stderr, " retVal-attrs-size=%u",
                                (unsigned)retVal.asAttrs()->size);
                        }
                        std::fprintf(stderr, "\n");
                        // Dump the thunk's body bytecode + descriptor info
                        // (the thunk's source location and the bytecode
                        // around the OP_RETURN site).
                        if (cu) {
                            uint32_t bodyStart = d->codeOffset;
                            uint32_t hi = ip + 2;
                            std::fprintf(stderr,
                                "  body disasm [%u..%u):\n", bodyStart, hi);
                            disassembleWindow(stderr, *cu, bodyStart, hi);
                        }
                    }
                }
            }
            // Phase A4 (RCA 2026-05-11): trace returns whose retVal is
            // a size-1 attrs matching V3_DBG_RETURN_KEY (e.g. "family").
            // Used to localize WHICH thunk's body produces the {family}
            // value that ends up at the cpuName cell.
            {
                static const char * s_dbgRetK =
                    std::getenv("V3_DBG_RETURN_KEY");
                if (__builtin_expect(s_dbgRetK != nullptr, 0)
                    && retVal.tag() == Tag::Attrs
                    && retVal.asAttrs()
                    && retVal.asAttrs()->size == 1) {
                    const auto & st = ir::globalSymbolTable();
                    SymbolId nm = retVal.asAttrs()->entries[0].name;
                    const char * nmStr =
                        nm < st.size() ? st[nm].c_str() : "?";
                    if (std::strcmp(nmStr, s_dbgRetK) == 0) {
                        const auto & fr = vm.frames.back();
                        const LambdaDescriptor * d = nullptr;
                        if (fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole))
                            d = fr.thunk->suspended.desc;
                        else if (fr.closure) d = fr.closure->desc;
                        const PosSnapshot * fps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_RETURN with retVal={%s} (size=1) "
                            "from frame: name=%s pos=%s:%u:%u codeOff=%u "
                            "thunk=%p flags=0x%x ip=%u\n",
                            nmStr,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (fps && !fps->file.empty())
                                ? fps->file.c_str() : "<no-pos>",
                            fps ? fps->line : 0u,
                            fps ? fps->column : 0u,
                            d ? d->codeOffset : 0u,
                            (const void *)fr.thunk,
                            (unsigned)fr.flags,
                            ip - 1);
                        if (const BindingsOrigin * o =
                                lookupBindingsOrigin(retVal.asAttrs())) {
                            const PosSnapshot * ops =
                                resolvePosSnapshot(o->posHandle);
                            std::fprintf(stderr,
                                "  retVal-origin=%s@%s:%u\n",
                                o->source ? o->source : "?",
                                (ops && !ops->file.empty())
                                    ? ops->file.c_str() : "?",
                                ops ? ops->line : 0u);
                        }
                    }
                }
            }
            // Capture only the fields we need across the pop_back —
            // copying the whole CallFrame is the per-recursion-call
            // hot path on fib/ack benchmarks.
            const CallFrame & frRef = vm.frames.back();
            const uint32_t fStackBase    = frRef.stackBaseOffset;
            const uint32_t fWithBase     = frRef.withStackBase;
            const uint32_t fFlags        = frRef.flags;
            Thunk *        fThunk        = frRef.thunk;
            // #558 Phase 4: capture the closure pointer too so we can
            // recycle the fakeClo back to the pool below.  Only the
            // CFF_THUNK_RETURN frames' closures are synthesized
            // fakeClos eligible for pooling; OP_CALL frames' closures
            // are owned by the caller's Value graph and must NOT be
            // recycled.  Casting away const here is safe: the pool
            // restores fakeClos to a clean state on recycle and the
            // VM owns them once popped from the frame stack.
            Closure *      fClosure      = const_cast<Closure *>(frRef.closure);
            vm.valueStack.resize(fStackBase);
            vm.withStack.resize(fWithBase);
            vm.frames.pop_back();
            CallFrame fr;  // referenced by name later — only thunk + flags matter.
            fr.flags = fFlags;
            fr.thunk = fThunk;
            // #787 breakdown mark T1: after frame state pop (pre-pop work).
            uint64_t retBdT1 = 0;
            if (__builtin_expect(s_dbgRetBd, 0)) [[unlikely]] {
                retBdT1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                allocStats().opReturnPrePopNs += (retBdT1 - retBdT0);
            }
            // NIX_TRACE_EVAL: W event for OP_RETURN of any
            // CFF_THUNK_RETURN frame.  Pairs with the F emitted at
            // OP_FORCE's or forceValue's frame push.  Uses the popped
            // closure's desc (fClosure->desc) which is identical to
            // the suspended.desc that produced the F.  Cheap when
            // disabled (one FILE* null check).
            if (__builtin_expect(nix::evalTrace::enabled(), 0)
                && (fFlags & CFF_THUNK_RETURN) && fClosure && fClosure->desc) {
                const PosSnapshot * ps =
                    resolvePosSnapshot(fClosure->desc->posHandle);
                std::string posStr =
                    (ps && !ps->file.empty())
                        ? nix::evalTrace::formatPos(ps->file, ps->line, ps->column)
                        : std::string("<no-pos>");
                nix::evalTrace::leaveWhnf(posStr, v3ValueTypeName(retVal));
            }
            // EXIT_GC_SPIRAL Day 6-8 wire-back (2026-05-29): recycle
            // the popped fakeClo back to the closure pool.  Only
            // CFF_THUNK_RETURN frames synthesized fakeClos via
            // `allocFakeClo` (with kFakeCloMagic in _pad);
            // `recycleFakeClo` rejects non-magic closures, so a stray
            // non-fakeClo closure here is a safe no-op.
            //
            // Must happen AFTER fClosure->desc is last read (above
            // trace block); recycleFakeClo zeros upvalues + sets
            // capturedWiths=nullptr, leaving desc/cu stale until next
            // pop.  fClosure is unused beyond this point in OP_RETURN.
            if ((fFlags & CFF_THUNK_RETURN) && fClosure) {
                Alloc::recycleFakeClo(fClosure);
            }
            if (fFlags & CFF_THUNK_RETURN) {
                // Chase Evaluated chains so the thunk caches the
                // ultimate WHNF and not an intermediate thunk.
                //
                // Tag::App is intentionally NOT chased here: chasing
                // would call callClosure while `fr.thunk` is still
                // Blackhole, and any transitive force of fr.thunk in
                // the App's body would trip "infinite recursion".  The
                // App is left in `evaluated`; downstream consumers
                // (OP_CALL, OP_FORCE, callClosure) all force-on-receive
                // and chase Apps through forceValue's own loop, by
                // which time `fr.thunk->state` is Evaluated and any
                // re-entry just reads the cached App and chases it
                // again (idempotent — the App's left/right don't
                // change).
                {
                    // Diagnostic: trace the chase chain for OP_RETURN
                    // self-cycle root-cause analysis.  Gated on
                    // V3_DBG_RET_CHASE=1.
                    static const bool s_dbgRetChase =
                        std::getenv("V3_DBG_RET_CHASE") != nullptr;
                    if (s_dbgRetChase && retVal.isThunk() && fr.thunk) {
                        Value chase = retVal;
                        std::fprintf(stderr,
                            "v3 OP_RETURN chase: fr.thunk=%p initial=%p\n",
                            (void *)fr.thunk,
                            (void *)retVal.asThunk());
                        int hops = 0;
                        while (chase.isThunk() && hops < 32) {
                            Thunk * th = chase.asThunk();
                            const auto * d = (th->state == ThunkState::Suspended
                                              || th->state == ThunkState::Blackhole)
                                ? th->suspended.desc : nullptr;
                            const PosSnapshot * ps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  hop=%d thunk=%p state=%d name='%s' pos=%s:%u:%u%s\n",
                                hops, (void *)th, (int)th->state,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u,
                                ps ? ps->column : 0u,
                                th == fr.thunk ? " <-- SELF" : "");
                            if (th->state != ThunkState::Evaluated) break;
                            chase = th->evaluated;
                            ++hops;
                        }
                        std::fflush(stderr);
                    }
                }
                while (retVal.isThunk() && retVal.asThunk()->state == ThunkState::Evaluated)
                    retVal = retVal.asThunk()->evaluated;
                // Self-reference detection: `let x = x; in x` makes the
                // thunk's body return the thunk itself (the chase above
                // can't catch this since we hit a Blackhole-state thunk
                // which isn't ThunkState::Evaluated until we're about
                // to assign).  Storing self into evaluated would make
                // subsequent forceValue calls spin forever in the
                // chase loop above.  Match tree-walker by raising.
                if (retVal.isThunk() && retVal.asThunk() == fr.thunk) {
                    // gate: V3_DBG_RETURN_SELF — log OP_RETURN self-cycle
                    // cases (thunk-body returns its own Thunk*).  Retire
                    // when STG indirect-chain recovery is replaced by
                    // a deterministic cycle-handling architecture
                    // (action plan Phase 2).
                    static const bool s_dbgReturnSelf =
                        std::getenv("V3_DBG_RETURN_SELF") != nullptr;
                    if (__builtin_expect(s_dbgReturnSelf, 0)) {
                        const LambdaDescriptor * d = fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole)
                            ? fr.thunk->suspended.desc : nullptr;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_RETURN self-cycle: thunk=%p name='%s' pos=%s:%u:%u state=%d retTag=%d\n",
                            (void *)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                            ps ? ps->line : 0u,
                            ps ? ps->column : 0u,
                            (int)fr.thunk->state,
                            (int)retVal.tag());
                    }
                    // #558 (2026-05-10) STG indirect-chain recovery:
                    //
                    // 1. If THIS thunk has a registered partial Bindings
                    //    (its body's REC_INIT_TAIL fired earlier — the
                    //    STG "reached WHNF" point), recover with that.
                    // 2. Otherwise, return vBlackhole (the deferred-
                    //    value marker).  The chain from retVal's thunk
                    //    back to self traversed Evaluated indirections
                    //    that resolved circularly.  Returning
                    //    vBlackhole lets the consumer see "value not
                    //    yet known" and propagate that lazily — same
                    //    protocol v3 uses for cross-stack Black thunk
                    //    accesses (see line ~7355).
                    //
                    // Rationale: the original throw assumed self-return
                    // was a real `let x = x; in x;` infinite-loop.
                    // Under STG-style partial-Bindings recovery, a
                    // Black thunk's "value" is the partial Bindings
                    // (or vBlackhole until WHNF reached); a chase
                    // resolving back to self via indirections is NOT
                    // a true loop — it's the chain unwinding through
                    // a self-reference that's still mid-construction.
                    // #558 Phase 3.3: partial-Bindings recovery
                    // retired.  Defer the self-cycle via vBlackhole.
                    static const bool s_dbgVBHProd =
                        std::getenv("V3_DBG_VBH_PROD") != nullptr;
                    if (s_dbgVBHProd) {
                        const auto * d = (fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole))
                            ? fr.thunk->suspended.desc : nullptr;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_RETURN→vBlackhole defer: thunk=%p name='%s' pos=%s:%u:%u\n",
                            (void *)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                            ps ? ps->line : 0u,
                            ps ? ps->column : 0u);
                    }
                    retVal = Value::vBlackhole;
                }
                // V3_DBG_STORE_PREVSTAGE: trace any thunk that gets
                // evaluated to a Closure whose desc is "prevStage" and
                // codeOffset 3099, nUp=0 — used to isolate WC-37.
                {
                    static const bool s_dbg_pv =
                        std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                    if (s_dbg_pv && retVal.tag() == Tag::Closure
                        && retVal.asClosure()
                        && retVal.asClosure()->desc
                        && retVal.asClosure()->nUpvalues == 0
                        && retVal.asClosure()->desc->name == "prevStage")
                    {
                        const auto * d = fr.thunk->suspended.desc;
                        std::fprintf(stderr,
                            "v3 OP_RETURN: storing prevStage(nUp=0) into thunk "
                            "%p desc=%s codeOffset=%u nUp=%u; cu=%p ip=%u\n",
                            (void*)fr.thunk,
                            d && !d->name.empty() ? d->name.c_str()
                                : (d ? "<anon>" : "<no-desc>"),
                            d ? d->codeOffset : 0,
                            (unsigned)fr.thunk->nUpvalues,
                            (void*)cu,
                            ip - 1);
                        // Dump frame stack to help locate caller.
                        size_t lim2 = vm.frames.size();
                        for (size_t i = lim2; i > 0 && i + 6 > lim2; --i) {
                            const auto & fr2 = vm.frames[i - 1];
                            const LambdaDescriptor * d2 = nullptr;
                            if (fr2.thunk) d2 = fr2.thunk->suspended.desc;
                            else if (fr2.closure) d2 = fr2.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u cu=%p\n",
                                i - 1,
                                d2 && !d2->name.empty() ? d2->name.c_str()
                                    : (d2 ? "<anon>" : "<closure-body>"),
                                d2 ? d2->codeOffset : 0, fr2.ip,
                                (unsigned)fr2.flags, (void*)fr2.cu);
                        }
                        // Dump bytecode around ip-1 in the popped frame's cu
                        // — verifies that ip-1 is actually OP_RETURN.
                        if (cu && ip > 1) {
                            uint32_t lo = ip > 6 ? ip - 6 : 0;
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr, "  popped frame cu=%p disasm [%u..%u):\n",
                                (void*)cu, lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm the descriptor's codeOffset region
                            // — this is what the body SHOULD have started at.
                            if (d && d->codeOffset != ip - 1) {
                                std::fprintf(stderr,
                                    "  desc.codeOffset=%u disasm [%u..%u):\n",
                                    d->codeOffset, d->codeOffset, d->codeOffset + 200);
                                disassembleWindow(stderr, *cu,
                                    d->codeOffset, d->codeOffset + 200);
                            }
                            // Find the lambda whose codeOffset is closest
                            // to (ip-1), going backwards.  Tells us which
                            // function we actually returned from.
                            uint32_t target_off = ip - 1;
                            uint32_t best_idx = ~0u;
                            uint32_t best_off = 0;
                            for (uint32_t li = 0; li < cu->lambdas.size(); ++li) {
                                uint32_t lo2 = cu->lambdas[li].codeOffset;
                                if (lo2 <= target_off && lo2 > best_off) {
                                    best_off = lo2;
                                    best_idx = li;
                                }
                            }
                            if (best_idx != ~0u) {
                                const auto & ld = cu->lambdas[best_idx];
                                std::fprintf(stderr,
                                    "  ip-1=%u falls inside lambdas[%u]"
                                    " (name=%s codeOffset=%u nUp=%u nLocals=%u)\n",
                                    target_off, best_idx,
                                    !ld.name.empty() ? ld.name.c_str() : "<anon>",
                                    ld.codeOffset, ld.nUpvalues, ld.nLocals);
                            }
                            // Also dump the thunk pointer's `tail` (its
                            // upvalues) so we can identify which specific
                            // thunk instance.
                            std::fprintf(stderr,
                                "  fr.thunk->tail upvalues (nUp=%u):\n",
                                (unsigned)fr.thunk->nUpvalues);
                            for (uint16_t ui = 0; ui < fr.thunk->nUpvalues && ui < 8; ++ui) {
                                const Value & uv = fr.thunk->tail[ui];
                                std::fprintf(stderr,
                                    "    [%u] tag=%u\n", ui, (unsigned)uv.tag());
                            }
                        }
                    }
                }
                // EVAL-COMP §4.4 / §8.2: drop upvalue references on
                // evaluation.  Once the thunk's body has returned, its
                // upvalues are no longer needed -- the cached
                // `evaluated` value is the only useful state.  Zeroing
                // tail[] lets Boehm reclaim transitive references that
                // would otherwise be retained for the thunk's
                // lifetime.  This is the GHC selector-thunk pattern
                // generalised: every Nix thunk gets selector-thunk
                // memory behaviour.  Particularly important for
                // `let pkgs = import <nixpkgs> {}; in pkgs.foo.bar`
                // patterns where pkgs holds a giant attrset that's
                // otherwise pinned by every per-attr selector thunk.
                {
                    Thunk * t = fr.thunk;
                    // env-sharing: the upvalues live in the shared Env (tail[0]
                    // is the Env*, tail size is 1-2 — NOT nUpvalues slots).  The
                    // Suspended/Blackhole→Evaluated transition just below makes
                    // thunkScanSize header-only, so the Env (and its upvalues)
                    // drops out of the GC graph automatically — no per-slot clear,
                    // which would corrupt tail[0] and run off the end.  Skipping
                    // it also keeps tail[0] a valid Env* for the whole
                    // Suspended/Blackhole lifetime (the GC walkers rely on that).
                    if (!thunkEnvShared(t)) {
                        for (uint16_t ui = 0; ui < t->nUpvalues; ++ui)
                            t->tail[ui] = Value{};
                    }
                    // Don't reset nUpvalues -- the FAM size was set at
                    // alloc time; reusing the slot would require the
                    // count.  Leaving it preserves alloc-time
                    // invariants (Bridge thunks etc.).
                }
                fr.thunk->state = ThunkState::Evaluated;
                thunkSetEvaluated(fr.thunk, retVal);  // Phase D barrier
                // STG-8 (#498): cell update.  If this thunk was stored
                // at a heap-stable cell (recorded at OP_ATTRS_REC_SET
                // time), overwrite the cell's contents with the body's
                // final result.  This mirrors tree-walker's in-place
                // `forceValue` update — slots / sub-thunks that
                // captured a Tag::Slot pointing at the cell now read
                // the result via single deref, and foreign VMState
                // observers stop seeing the leaked Black thunk.
                //
                // Read-and-clear: we want the write to fire exactly
                // once per cell binding.  Idempotent on re-entry
                // (cell becomes nullptr after first OP_RETURN).
                if (Value * cell = fr.thunk->cell) {
                    cellOwnRecordWrite(cell, fr.thunk,
                                        "OP_RETURN/CFF_THUNK_RETURN");
                    cellTraceWrite(cell, fr.thunk, retVal,
                                    "OP_RETURN/CFF_THUNK_RETURN");
                    // Phase D Step 3 batch D: cellWrite routes nursery
                    // payloads to standaloneCellRoots.  M-8
                    // (CODEBASE_REVIEW_2026-06-11): the stored Thunk::
                    // cellContainer was removed; pass nullptr so this single
                    // cell write is tracked via the standalone-cell registry
                    // (sufficient — only this one cell changed; scavenge walks
                    // standaloneCellRoots).  Zero default-path cost vs deriving
                    // the owning Bindings on the (default-on) OP_RETURN path
                    // for a default-off nursery feature.
                    cellWrite(cell, retVal, nullptr);
                    fr.thunk->cell = nullptr;
                }
                // M-8: #558 shapeCell final-value publish removed (field gone).
                // #558 Phase 3.3 (2026-05-12): partial-Bindings
                // infrastructure retired.  No publishes fire so the
                // registry stays empty; nothing to erase at OP_RETURN.

                // Phase D Step 12 (2026-05-21): closure-pool retired.
                // Previously the just-popped fakeClo would be
                // returned to a thread-local pool here for
                // reuse on the next OP_FORCE Thunk dispatch (and
                // similar).  Replaced by nursery-based allocation
                // (Alloc::allocClosure → nurseryOrArena) — fresh
                // Closures land in the nursery and get reclaimed by
                // scavenge when no longer referenced.  V3_DBG_RECYCLE_*
                // diagnostics removed along with the pool.

                // WC-38: the legacy "return-chain push" -- eagerly
                // forcing the next thunk if the outer's body returned
                // a Suspended thunk -- has been removed.  forceValue's
                // chase loop already resolves the chain on the
                // consumer's pull, and OP_RETURN's caller-resume path
                // re-runs op_force_slow when CFF_FORCE_RETRY is set.
            }
            // #787 breakdown mark T2: after CFF_THUNK_RETURN logic.
            // Credit thunk-eval branch delta to thunkEvalNs; counter
            // distinguishes thunk vs call returns so consumers can
            // average correctly.
            uint64_t retBdT2 = 0;
            if (__builtin_expect(s_dbgRetBd, 0)) [[unlikely]] {
                retBdT2 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (fFlags & CFF_THUNK_RETURN) {
                    allocStats().opReturnThunkEvalNs += (retBdT2 - retBdT1);
                    allocStats().opReturnThunkCalls++;
                } else {
                    allocStats().opReturnCallCalls++;
                }
            }
            if (vm.frames.size() == exitDepth) {
                // #787 breakdown — final-exit returns also bump
                // postEvalNs for the small "frames-size-check + exit"
                // path so the buckets sum cleanly.
                if (__builtin_expect(s_dbgRetBd, 0)) [[unlikely]] {
                    uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    allocStats().opReturnPostEvalNs += (now - retBdT2);
                }
                finalResult = retVal;
                running = false;
                break;
            }
            {
                // WC-38: GHC STG-style force-retry.  If the caller frame
                // was marked CFF_FORCE_RETRY (set by OP_FORCE /
                // OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE before
                // pushing the now-popped thunk frame) AND retVal is
                // still a Thunk/App (= the body returned a forwarding
                // pointer to another unforced value), re-enter
                // op_force_slow to drive the chain.
                //
                // ALSO (WC-38 part 2 / "early publish"): if the popped
                // frame was a CLOSURE call frame (NOT CFF_THUNK_RETURN)
                // and the caller frame is a thunk frame in Black state,
                // EARLY-PUBLISH retVal to the caller's thunk.evaluated.
                // This mirrors tree-walker's `v.mkAttrs(...)` writing
                // to the slot DURING expr->eval (not at body return),
                // so sub-thunks captured-with that fire DURING the
                // outer's body see the published value rather than
                // hitting the Black state and throwing.
                CallFrame & caller = vm.frames.back();
                cu = caller.cu;
                ip = caller.ip;
                closure = caller.closure;
                stackBase = caller.stackBaseOffset;

                // Early publish: only fires when popped frame was a
                // closure call (not a thunk frame), retVal is fully
                // resolved (non-thunk/app), and caller is a Black
                // thunk frame.  Idempotent — the eventual OP_RETURN
                // of the caller's thunk frame will overwrite with the
                // FINAL retVal.
                //
                // Disabled by default — set NIX_V3_EARLY_PUBLISH=1
                // to enable.  Currently doesn't fully fix WC-38 but
                // is kept for experimentation.
                static const bool s_early_publish =
                    std::getenv("NIX_V3_EARLY_PUBLISH") != nullptr;
                if (s_early_publish
                    && !(fFlags & CFF_THUNK_RETURN)
                    && (caller.flags & CFF_THUNK_RETURN)
                    && caller.thunk
                    && caller.thunk->state == ThunkState::Blackhole
                    && retVal.tag() != Tag::Thunk
                    && !retVal.isAppLike()
                    && retVal.tag() != Tag::Blackhole)
                {
                    caller.thunk->state = ThunkState::Evaluated;
                    thunkSetEvaluated(caller.thunk, retVal);  // Phase D barrier
                }

                bool retry = (caller.flags & CFF_FORCE_RETRY)
                    && (retVal.tag() == Tag::Thunk
                        || retVal.isAppLike()
                        || retVal.tag() == Tag::Slot);
                // Clear the retry flag — it's a one-shot per
                // OP_FORCE.  The next OP_FORCE will re-set it.
                caller.flags &= ~CFF_FORCE_RETRY;
                // STG-11 (#498): diagnostic — log the retry value's
                // shape so we can see what's about to be re-forced.
                // V3_DBG_RETRY=1 dumps each retry's retVal tag + chase.
                // V3_DBG_RETRY_BLACK=1 only logs when the retry's
                // chain would hit a Black thunk (= the cycle source).
                static const bool s_dbgRetry =
                    std::getenv("V3_DBG_RETRY") != nullptr;
                static const bool s_dbgRetryBlack =
                    std::getenv("V3_DBG_RETRY_BLACK") != nullptr;
                if (retry && (s_dbgRetry || s_dbgRetryBlack))
                {
                    Value chase = retVal;
                    int hops = 0;
                    Thunk * retryThunk = nullptr;
                    Thunk * blackHit = nullptr;
                    while (hops < 16) {
                        if (chase.tag() == Tag::Slot && chase.asSlot()) {
                            chase = *chase.asSlot();
                        } else if (chase.tag() == Tag::Thunk
                                   && chase.asThunk()) {
                            Thunk * th = chase.asThunk();
                            if (th->state == ThunkState::Evaluated) {
                                chase = th->evaluated;
                            } else if (th->state == ThunkState::Blackhole) {
                                blackHit = th;
                                break;
                            } else {
                                break;
                            }
                        } else break;
                        ++hops;
                    }
                    if (retVal.tag() == Tag::Thunk
                        && retVal.asThunk())
                        retryThunk = retVal.asThunk();
                    else if (retVal.tag() == Tag::Slot
                             && retVal.asSlot()
                             && retVal.asSlot()->tag() == Tag::Thunk)
                        retryThunk = retVal.asSlot()->asThunk();
                    bool onlyBlack = s_dbgRetryBlack;
                    if (!onlyBlack || blackHit) {
                        std::fprintf(stderr,
                            "v3 OP_RETURN retry: retVal.tag=%d chase.tag=%d hops=%d",
                            (int)retVal.tag(), (int)chase.tag(), hops);
                        if (blackHit) {
                            const LambdaDescriptor * bd =
                                blackHit->suspended.desc;
                            std::fprintf(stderr,
                                " BLACK thunk=%p name=%s code=%u",
                                (void *)blackHit,
                                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                                bd ? bd->codeOffset : 0);
                        }
                        if (retryThunk) {
                            const LambdaDescriptor * d =
                                retryThunk->state == ThunkState::Suspended
                                || retryThunk->state == ThunkState::Blackhole
                                    ? retryThunk->suspended.desc : nullptr;
                            std::fprintf(stderr,
                                " retryThunk=%p state=%d name=%s",
                                (void *)retryThunk,
                                (int)retryThunk->state,
                                d && !d->name.empty() ? d->name.c_str() : "<?>");
                        }
                        // Dump caller frame name so we know where the
                        // retry fires from.
                        const LambdaDescriptor * cd = nullptr;
                        if (caller.thunk
                            && (caller.thunk->state == ThunkState::Suspended
                                || caller.thunk->state == ThunkState::Blackhole))
                            cd = caller.thunk->suspended.desc;
                        else if (caller.closure)
                            cd = caller.closure->desc;
                        std::fprintf(stderr,
                            " caller.name=%s frames=%zu\n",
                            cd && !cd->name.empty() ? cd->name.c_str() : "<?>",
                            vm.frames.size());
                    }
                }
                push(vm, retVal);
                // A8: iterative-force writeback.  When the caller set a
                // writeback slot before goto op_force_slow, write the
                // forced retVal back into the original arg slot and
                // suppress the retry chain — the opcode that initiated
                // the force will re-scan its args on re-entry.
                if (applyForceWriteback(vm))
                    retry = false;
                if (retry)
                    goto op_force_slow;
            }
            // #787 breakdown mark T3: end of case (post-eval).
            if (__builtin_expect(s_dbgRetBd, 0)) [[unlikely]] {
                uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                allocStats().opReturnPostEvalNs += (now - retBdT2);
            }
            break;
        }
        case OP_FORCE: {
            // Fast path: peek at the top of the stack.  The vast majority
            // of OP_FORCE calls hit values already in WHNF (Int / Bool /
            // String / Attrs / List / Closure / Path / Null / Float /
            // PrimOp / PrimOpApp).  Skip the pop+push for those.
            // Diagnostics live BELOW the bail-out so the fast path
            // doesn't compute the address argument when disabled.
            {
                Value & topRef = vm.valueStack.back();
                Tag t = topRef.tag();
                if (t != Tag::Thunk && t != Tag::App && t != Tag::App3
                    && t != Tag::Slot) break;
            }
            // V3_DBG_FORCE_SITE trace; see dbgLogForceSite().
            dbgLogForceSite(cu, ip - 1,
                vm.valueStack.empty() ? nullptr : &vm.valueStack.back());
            dbgLogForceInsideX(vm,
                vm.valueStack.empty() ? nullptr : &vm.valueStack.back());
            // Slow path: shared with OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE
            // which push the value first and then jump here.
            op_force_slow:
            Value v = pop(vm);
            // Chase Evaluated chains, deref Tag::Slot, and resolve
            // Tag::App deferred calls (used by mapAttrs et al. for
            // lazy entries).
            //
            // Same iteration bound as forceValue() to detect
            // SECD-style indirection cycles (`let x = x; in x` after
            // the Phase 5 slot-pointer rewrite).  See forceValue
            // comment for rationale.
            {
            int forceChaseIters = 0;
            // #558 Phase 4 follow-up: path compression for the
            // OP_FORCE chase loop — mirror of forceValue's chase.
            // Records up to kCompressMax Evaluated thunks; after the
            // chase resolves to a stable WHNF, write that value into
            // each recorded `t->evaluated` so future forces hit in
            // O(1).  Opt-out: NIX_V3_NO_PATH_COMPRESS=1.
            static const bool s_opForceNoCompress =
                std::getenv("NIX_V3_NO_PATH_COMPRESS") != nullptr;
            constexpr int kOpForceCompressMax = 16;
            Thunk * opForceCompressChain[kOpForceCompressMax];
            int opForceCompressCount = 0;
            // #757 ring-buffer of recent chase steps for opcode-level
            // diagnostic at the chase-iter limit firing.  Gated by
            // V3_DBG_CHASE matching forceValue's ring.
            static const bool s_dbg_opf_chase = std::getenv("V3_DBG_CHASE") != nullptr;
            constexpr int kOpfRingSize = 32;
            Tag opfRingTag[kOpfRingSize] = {};
            void * opfRingPtr[kOpfRingSize] = {};
            int opfRingState[kOpfRingSize] = {};
            int opfRingIdx = 0;
            while (true) {
                if (__builtin_expect(s_dbg_opf_chase, 0)) {
                    opfRingTag[opfRingIdx % kOpfRingSize] = v.tag();
                    void * p = nullptr;
                    int st = -1;
                    if (v.tag() == Tag::Thunk) {
                        p = v.asThunk();
                        if (v.asThunk()) st = (int)v.asThunk()->state;
                    } else if (v.tag() == Tag::Slot) p = v.asSlot();
                    else if (v.isAppLike())         p = v.asPair();
                    opfRingPtr[opfRingIdx % kOpfRingSize] = p;
                    opfRingState[opfRingIdx % kOpfRingSize] = st;
                    opfRingIdx++;
                }
                if (__builtin_expect(++forceChaseIters > kMaxIndirectionChase, 0)) {
                    // #680 — match TW phrasing
                    // (libexpr/eval.cc:2594 InfiniteRecursionError).
                    if (s_dbg_opf_chase) {
                        std::fprintf(stderr, "v3 OP_FORCE chase-cycle limit %d hit; last %d steps:\n",
                                     kMaxIndirectionChase, kOpfRingSize);
                        int start = opfRingIdx > kOpfRingSize ? opfRingIdx - kOpfRingSize : 0;
                        for (int i = start; i < opfRingIdx; ++i) {
                            int slot = i % kOpfRingSize;
                            std::fprintf(stderr,
                                "  step[%d]: tag=%d ptr=%p", i,
                                (int)opfRingTag[slot], opfRingPtr[slot]);
                            if (opfRingTag[slot] == Tag::Thunk && opfRingPtr[slot]) {
                                std::fprintf(stderr, " state=%d", opfRingState[slot]);
                                if (opfRingState[slot] == (int)ThunkState::Evaluated) {
                                    auto * t = static_cast<Thunk *>(opfRingPtr[slot]);
                                    std::fprintf(stderr, " evaluated.tag=%d", (int)t->evaluated.tag());
                                }
                            }
                            std::fprintf(stderr, "\n");
                        }
                        // Frame stack
                        std::fprintf(stderr, "v3 frame stack (top 16):\n");
                        size_t lim = vm.frames.size();
                        for (size_t i = lim; i > 0 && i + 16 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            const PosSnapshot * ps = d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr, "  frame[%zu]: %s ip=%u thunk=%p pos=%s:%u:%u\n",
                                i - 1,
                                d && !d->name.empty() ? d->name.c_str() : "<anon>",
                                fr.ip, (void*)fr.thunk,
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u, ps ? ps->column : 0u);
                        }
                    }
                    throw std::runtime_error("infinite recursion encountered");
                }
                if (v.tag() == Tag::Slot) {
                    Value * p = v.asSlot();
                    if (!p) throw std::runtime_error(
                        "v3 OP_FORCE: null slot pointer");
                    v = *p;
                    continue;
                }
                if (v.isAppLike()) {
                    // eval/apply (#3): an under-applied closure-PAP is WHNF —
                    // a partial application, NOT a deferred call.  Stop the
                    // chase here with v as the result (don't walk+apply it,
                    // which would enter the arity-N body with too few args).
                    if (isUnderappliedClosurePap(v)) break;
                    // REVIEW MED-18: walk the App spine iteratively
                    // to find the leaf function + collected args.
                    // Pre-fix recursed through forceValue per App level
                    // (`left = forceValue(vm, left)`), blowing C-stack
                    // on long mapAttrs / map chains in nixpkgs.  Now we
                    // chase down the left side without recursion,
                    // accumulating rights into a small vector, then
                    // apply once.  The leaf force call is non-App, so
                    // any forceValue recursion bottoms out at the leaf
                    // rather than at every App level.
                    //
                    // 2026-05-18: App-result memoization.  See the
                    // matching comment in forceValue's Tag::App handler
                    // (~line 9257).  Memo-hit fast path returns the
                    // cached result; cold path computes + stores back.
                    //
                    // 2026-05-30 (EXIT_GC_SPIRAL Day 4 option A): the
                    // 3-arg variant Tag::App3 now uses `pair->third` for
                    // arg2 and KEEPS `pair->evaluated` as the memoization
                    // sink — same shape as Tag::App.  Both pair tags
                    // therefore memoize identically; the prior Day 9-11
                    // attempt that overloaded `evaluated` for arg2 lost
                    // memoization and caused HNE +1644 MB / +4× wall
                    // regression (#696 pattern).  Separating the slots
                    // preserves App-result memo on hot mapAttrs entries.
                    bool outerIsAppLike = v.isAppLike();
                    ValuePair * outerPair = outerIsAppLike ? v.asPair() : nullptr;
                    if (__builtin_expect(outerIsAppLike
                        && outerPair
                        && outerPair->evaluated.tag() != Tag::Uninitialized, 1))
                    {
                        v = outerPair->evaluated;
                        continue;
                    }
                    // P-4 (CODEBASE_REVIEW_2026-06-11): collect the spine's
                    // args into an INLINE small buffer (observed depth ≤4) to
                    // avoid a per-force heap std::vector alloc on this cold
                    // (non-memoized) App-force path; overflow to a heap vector
                    // for deep spines.  Stack-local → re-entrant-safe
                    // (op_force_slow re-enters via callClosure below).
                    constexpr size_t kInlineRights = 16;
                    Value inlineRights[kInlineRights];
                    std::vector<Value> overflowRights;
                    size_t nRights = 0;
                    auto pushRight = [&](const Value & val) {
                        if (nRights < kInlineRights) inlineRights[nRights] = val;
                        else overflowRights.push_back(val);
                        ++nRights;
                    };
                    auto rightAt = [&](size_t i) -> const Value & {
                        return i < kInlineRights ? inlineRights[i]
                                                 : overflowRights[i - kInlineRights];
                    };
                    while (v.isAppLike()) {
                        ValuePair * p = v.asPair();
                        if (v.tag() == Tag::App3)
                            pushRight(p->third);   // arg2 lives in `third` now
                        pushRight(p->right);
                        v = p->left;
                    }
                    vm.frames.back().ip = ip;
                    if (v.tag() == Tag::Slot
                        || v.tag() == Tag::Thunk
                        || v.isAppLike())
                        v = forceValue(vm, v);
                    // Apply rights in source order (we collected
                    // outermost-first while walking; reverse on apply).
                    //
                    // Phase 1.2 (2026-05-16): identity-lambda fast path.
                    // `id (id (id ... 0))` x N otherwise pushes N frames
                    // via callClosure → dispatchLoop → OP_GET_LOCAL_FORCE
                    // → OP_FORCE → callClosure (each id's body forces
                    // its arg, triggering the next nested OP_FORCE), and
                    // hits the kMaxCallDepth=5000 guard around N=5000.
                    // For identity lambdas the body is `x: x`; the arg
                    // substitutes directly with no frame setup needed.
                    // This collapses the App spine to a tight loop
                    // inside this OP_FORCE handler.
                    for (size_t i = nRights; i > 0; --i) {
                        if (v.tag() == Tag::Closure
                            && v.asClosure()
                            && v.asClosure()->desc
                            && v.asClosure()->desc->identityLambda)
                        {
                            v = rightAt(i - 1);
                            continue;
                        }
                        v = callClosure(vm, v, rightAt(i - 1));
                    }
                    // 2026-05-30: App-result memoization writeback for
                    // both Tag::App AND Tag::App3 (since App3 now has a
                    // separate `third` slot for arg2 and `evaluated`
                    // stays as the memo sink).
                    if (outerIsAppLike && outerPair) {
                        Tag rt = v.tag();
                        if (rt != Tag::Thunk && rt != Tag::App && rt != Tag::App3
                            && rt != Tag::Slot
                            && rt != Tag::Uninitialized && rt != Tag::Blackhole)
                            pairSetEvaluated(outerPair, v);  // Phase D barrier
                    }
                    continue;
                }
                if (!v.isThunk()) break;
                if (v.asThunk()->state == ThunkState::Evaluated) {
                    if (!s_opForceNoCompress
                        && opForceCompressCount < kOpForceCompressMax)
                        opForceCompressChain[opForceCompressCount++] =
                            v.asThunk();
                    v = v.asThunk()->evaluated;
                    continue;
                }
                break;
            }
            // Path compression writeback: only on a stable WHNF.
            if (opForceCompressCount > 0
                && v.tag() != Tag::Thunk
                && v.tag() != Tag::Slot
                && !v.isAppLike()
                && v.tag() != Tag::Blackhole)
            {
                for (int i = 0; i < opForceCompressCount; ++i)
                    thunkSetEvaluated(opForceCompressChain[i], v);  // Phase D barrier
            }
            } // end forceChaseIters scope
            if (!v.isThunk()) {
                push(vm, v);
                // A8: apply writeback if the opcode that initiated this
                // force set a writeback slot.  No-op when off==FORCE_WB_NONE.
                applyForceWriteback(vm);
                // C-11 (CODEBASE_REVIEW_2026-06-11): the force resolved
                // SYNCHRONOUSLY (the chase reached WHNF inline — no thunk frame
                // was pushed, so OP_RETURN's retry-clear at vm.cc:7085 never
                // runs for this force).  CFF_FORCE_RETRY is a one-shot per
                // force; applyForceWriteback clears the WB bits but NOT RETRY,
                // and for opcodes that armed RETRY WITHOUT a writeback slot
                // (OP_HEAD/TAIL/LENGTH/AND_BRANCH/…) it returns false and leaves
                // RETRY set.  A stale RETRY then spuriously forces this frame's
                // NEXT legitimate lazy return (the open libsForQt5 deferral
                // signature: v3 forces `inherit (pkgs) lib` while pkgs is BLACK,
                // where TW never enters that thunk).  Clear it now — the opcode
                // re-arms a fresh RETRY if it still needs to force another arg.
                if (!vm.frames.empty())
                    vm.frames.back().flags &= ~CFF_FORCE_RETRY;
                break;
            }
            Thunk * t = v.asThunk();
            if (t->state == ThunkState::Blackhole) {
                // WC-17.1 diagnostic: dump the v3 frame stack with
                // function names + IP deltas when V3_DBG_OPCYCLE=1.
                // The `name` field on LambdaDescriptor (populated by
                // emit() from ir::Function::name) lets us correlate
                // cycle frames back to source-level rec-attrset attr
                // names — invaluable for diagnosing the closure-bridge
                // cycle without a full bytecode disassembler.
                static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
                if (s_dbg) {
                    auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t fip) -> std::string {
                        const LambdaDescriptor * desc = nullptr;
                        if (th) desc = th->suspended.desc;
                        else if (cl) desc = cl->desc;
                        if (!desc) return "<closure-body>";
                        char buf[256];
                        std::snprintf(buf, sizeof buf,
                            "%s code=[%u..) nUp=%u nLocals=%u",
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            desc->codeOffset, desc->nUpvalues, desc->nLocals);
                        return buf;
                    };
                    std::fprintf(stderr,
                        "v3 OP_FORCE Black thunk=%p frames=%zu callerIp=%u\n",
                        (void*)t, vm.frames.size(), ip - 1);
                    size_t lim = vm.frames.size();
                    for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                        const auto & fr = vm.frames[i - 1];
                        std::fprintf(stderr,
                            "  frame[%zu]: %s flags=%u ip=%u thunk=%p\n",
                            i - 1, frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                            (unsigned)fr.flags, fr.ip, (void*)fr.thunk);
                    }
                    // WC-32 disassembler: when V3_DBG_OPCYCLE_DISASM=1,
                    // also dump 8 instructions surrounding each frame's ip.
                    static const bool s_dbg_disasm =
                        std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                    if (s_dbg_disasm) {
                        // Expanded: dump top 12 frames (was 8) for WC-38
                        // investigation.
                        for (size_t i = lim; i > 0 && i + 12 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            uint32_t fip = fr.ip;
                            uint32_t lo = fip > 16 ? fip - 16 : 0;
                            uint32_t hi = fip + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] disasm [%u..%u):\n",
                                i - 1, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                        // Also dump the prologue of each frame's lambda
                        // (where the body STARTS) for context.
                        std::fprintf(stderr, "  --- frame prologues ---\n");
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            if (!fr.cu) continue;
                            const LambdaDescriptor * desc = nullptr;
                            if (fr.thunk)
                                desc = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                desc = fr.closure->desc;
                            if (!desc) continue;
                            uint32_t prologueStart = desc->codeOffset;
                            uint32_t prologueEnd = prologueStart + 16;
                            std::fprintf(stderr,
                                "  frame[%zu] prologue [%u..%u):\n",
                                i - 1, prologueStart, prologueEnd);
                            disassembleWindow(stderr, *fr.cu, prologueStart, prologueEnd);
                        }
                    }
                }
                // #466 error-as-value (GHC-style mkBlackHole) — see
                // forceValue's matching block for full rationale.
                {
                    static const bool s_blackholeAsValue =
                        std::getenv("NIX_V3_NO_BLACKHOLE_AS_VALUE") == nullptr;
                    if (s_blackholeAsValue) {
                        bool onMyFrames = false;
                        for (size_t i = 0; i < vm.frames.size(); ++i) {
                            if (vm.frames[i].thunk == t) {
                                onMyFrames = true; break;
                            }
                        }
                        if (!onMyFrames) {
                            push(vm, Value::vBlackhole);
                            applyForceWriteback(vm);
                            break;
                        }
                    }
                }
                // #680 — match TW phrasing
                // (libexpr/eval.cc:2594 InfiniteRecursionError).
                throw BlackholeError("infinite recursion encountered");
            }
            // (WC-10 OP_FORCE Bridge-thunk handler retired with the bridge
            //  apparatus — TW_VALUE_ERADICATION F4, 2026-06-02.)
            // Suspended: blackhole and run.
            // We treat suspended.desc as a LambdaDescriptor* (see OP_MAKE_THUNK).
            const LambdaDescriptor * desc = t->suspended.desc;
            // Phase 13 instrumentation: bump per-thunk + per-descriptor +
            // global counters at the Suspended → Blackhole gate.  Each
            // thunk should transition exactly once per lifetime, so
            // `t->forces` should never grow past 1 unless something
            // re-suspends a previously-blackholed thunk.  Per-descriptor
            // count tells us how many thunks share the same body
            // (over-allocation indicator: when 1 expression yields N
            // thunks because the binding it captures isn't shared).
            // #733: per-Thunk + per-LambdaDescriptor + global force
            // stats — gate behind dbgForceStatsActive (see comment near
            // dbgForceStatsActive in this file).  desc->forceCount and
            // allocStats().thunksForced are the contended writes.
            if (__builtin_expect(dbgForceStatsActive(), 0)) {
                ++t->forces;
                ++desc->forceCount;
                ++allocStats().thunksForced;
            }
            // #558 (2026-05-11) Focused trace: log when a thunk with a
            // specific name is forced for the first time.  Used to
            // diagnose v3-specific eager forces vs TW.  Set
            // V3_DBG_FORCE_NAME=libsForQt5 to trace.  Also matches
            // by file:line if name doesn't match — set V3_DBG_FORCE_POS=8390
            // to match by line number.
            {
                static const char * s_focusName =
                    std::getenv("V3_DBG_FORCE_NAME");
                static const char * s_focusPos =
                    std::getenv("V3_DBG_FORCE_POS");
                // V3_DBG_FORCE_FILE: optional file-name substring filter
                // (combine with V3_DBG_FORCE_POS to match by file+line).
                // E.g., V3_DBG_FORCE_FILE=darwin/default.nix V3_DBG_FORCE_POS=232.
                static const char * s_focusFile =
                    std::getenv("V3_DBG_FORCE_FILE");
                bool nameMatch = s_focusName && desc
                    && desc->name == s_focusName;
                bool posMatch = false;
                if (s_focusPos && desc && desc->posHandle) {
                    const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                    if (ps) {
                        uint32_t want = std::strtoul(s_focusPos, nullptr, 10);
                        bool lineOk = ps->line == want;
                        bool fileOk = !s_focusFile
                            || (ps->file.find(s_focusFile) != std::string::npos);
                        if (lineOk && fileOk) posMatch = true;
                    }
                }
                if (__builtin_expect((nameMatch || posMatch)
                                     && desc && desc->forceCount == 1, 0)) {
                    if (true)
                    {
                        const PosSnapshot * ps =
                            resolvePosSnapshot(desc->posHandle);
                        std::fprintf(stderr,
                            "v3 FORCE-NAME-FIRST: '%s' thunk=%p pos=%s:%u:%u frames=%zu\n",
                            desc->name.c_str(), (void *)t,
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                            ps ? ps->line : 0u, ps ? ps->column : 0u,
                            vm.frames.size());
                        size_t lim = vm.frames.size();
                        for (size_t fi = lim; fi > 0 && fi + 20 > lim; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                d = fr.closure->desc;
                            const PosSnapshot * ps2 =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "  [%zu] %s ip=%u flags=%u %s:%u:%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                fr.ip, (unsigned)fr.flags,
                                (ps2 && !ps2->file.empty()) ? ps2->file.c_str() : "?",
                                ps2 ? ps2->line : 0u, ps2 ? ps2->column : 0u);
                        }
                        std::fflush(stderr);
                    }
                }
            }
            // Phase 13: live periodic stats dump.  When V3_DBG_FORCES is
            // set, every N millionth force emits a one-line snapshot to
            // stderr.  Lets us watch a runaway eval without waiting for
            // atexit (which doesn't fire under SIGKILL / SIGXCPU).
            // Default N = 10M; override via V3_DBG_FORCE_STRIDE.
            {
                static const bool s_periodic =
                    std::getenv("V3_DBG_FORCES") != nullptr;
                if (__builtin_expect(s_periodic, 0)) {
                    static const uint64_t s_stride = []() -> uint64_t {
                        const char * e = std::getenv("V3_DBG_FORCE_STRIDE");
                        return e ? std::strtoull(e, nullptr, 10)
                                 : uint64_t(10) * 1000 * 1000;
                    }();
                    auto & a = allocStats();
                    if (s_stride && (a.thunksForced % s_stride) == 0) {
                        const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                        char posBuf[256] = "";
                        if (ps && !ps->file.empty())
                            std::snprintf(posBuf, sizeof posBuf,
                                " at=%s:%u:%u", ps->file.c_str(),
                                ps->line, ps->column);
                        std::fprintf(stderr,
                            "v3 PROGRESS: forced=%llu allocated=%llu "
                            "ratio=%.3f frames=%zu arena=%lluMB hot=%s/%llu%s\n",
                            (unsigned long long)a.thunksForced,
                            (unsigned long long)a.thunksAllocated,
                            a.thunksAllocated
                                ? double(a.thunksForced) / double(a.thunksAllocated)
                                : 0.0,
                            vm.frames.size(),
                            (unsigned long long)(threadArena().bytesAllocated() >> 20),
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            (unsigned long long)desc->forceCount,
                            posBuf);
                        // #548c (2026-05-10): if V3_DBG_ALLOC_DUMP is
                        // also set, list the top-10 descriptors by
                        // (alloc + force) right here — atexit doesn't
                        // fire on `timeout` SIGKILL, so emitting at
                        // each progress tick guarantees we capture the
                        // hot pattern before the run terminates.
                        if (g_dbgAllocDump) {
                            struct R { uint64_t a, f; const LambdaDescriptor * d; };
                            std::vector<R> rows;
                            for (auto * cui : cuRegistry()) {
                                if (!cui) continue;
                                for (const auto & ld : cui->lambdas) {
                                    if (ld.allocCount + ld.forceCount < 1000)
                                        continue;
                                    rows.push_back({ld.allocCount, ld.forceCount, &ld});
                                }
                            }
                            std::sort(rows.begin(), rows.end(),
                                [](const R & x, const R & y) {
                                    return (x.a + x.f) > (y.a + y.f);
                                });
                            size_t lim = std::min<size_t>(rows.size(), 10);
                            std::fprintf(stderr,
                                "  top-10 hot descriptors:\n");
                            for (size_t i = 0; i < lim; ++i) {
                                const auto & r = rows[i];
                                const PosSnapshot * pps = resolvePosSnapshot(r.d->posHandle);
                                char b[256];
                                if (pps && !pps->file.empty())
                                    std::snprintf(b, sizeof b,
                                        "%s:%u:%u",
                                        pps->file.c_str(), pps->line, pps->column);
                                else
                                    std::snprintf(b, sizeof b,
                                        "<no-pos> codeOff=%u",
                                        r.d->codeOffset);
                                std::fprintf(stderr,
                                    "    a=%llu f=%llu %s @ %s\n",
                                    (unsigned long long)r.a,
                                    (unsigned long long)r.f,
                                    !r.d->name.empty() ? r.d->name.c_str() : "<anon>",
                                    b);
                            }
                        }
                    }
                }
            }
            // Synthesize a closure-like view for OP_GET_UPVALUE: we set
            // `closure` to a fresh Closure crafted from the thunk
            // tail.  Phase D Step 12 (2026-05-21): retired the
            // fakeClo / closure-pool sentinel infrastructure.  Pool
            // reuse saved a hand-rolled allocation, but the Cheney
            // nursery (`NIX_V3_NURSERY=1`) provides real generational
            // reclamation: fresh closures land in nursery, scavenge
            // collects unreferenced ones at next cycle.  Pool was
            // load-bearing only before nursery + Phase D landed.
            // EXIT_GC_SPIRAL Day 6-8 wire-back (2026-05-29): use the
            // fakeClo pool (`allocFakeClo`) instead of fresh
            // `allocClosure`.  The pool is gated by
            // `NIX_V3_NO_CLOSURE_POOL=1` (opt-OUT; pool default-on).
            Closure * fakeClo = Alloc::allocFakeClo(t->nUpvalues);
            // Phase A5 RCA: alarm when fakeClo's pre-overwrite desc is
            // a "real" closure body (i.e., codeOff != 0 and name not
            // empty).  A recycled-fakeClo pool would only set desc to a
            // thunk-body — never to an OP_MAKE_CLOSURE-created closure.
            // If we ever see a recycled fakeClo whose existing desc was
            // for a real closure (e.g., codeOff=2863), the pool has
            // returned a pointer that's STILL alive in the cell.
            {
                static const char * s_atFC =
                    std::getenv("V3_DBG_FAKECLO_AT_PTR");
                if (__builtin_expect(s_atFC != nullptr, 0)) {
                    void * target = nullptr;
                    sscanf(s_atFC, "%p", &target);
                    if (fakeClo == target) {
                        const LambdaDescriptor * preDesc = fakeClo->desc;
                        std::fprintf(stderr,
                            "v3 FAKECLO@%p: OVERWRITE desc pre=%p new=%p"
                            " (pre-codeOff=%u new-codeOff=%u nUp=%u)\n",
                            (void *)fakeClo,
                            (const void *)preDesc,
                            (const void *)desc,
                            preDesc ? preDesc->codeOffset : 0,
                            desc ? desc->codeOffset : 0,
                            (unsigned)t->nUpvalues);
                    }
                }
            }
            fakeClo->desc = desc;
            fakeClo->nUpvalues = t->nUpvalues;
            fakeClo->capturedWiths = thunkCapturedWiths(t);  // FP-2b: was suspended.capturedWiths
            fakeClo->cu = thunkCU(t);  // FP-2a: was t->suspended.cu
            // env-sharing: share the thunk's Env directly (NO per-force upvalue
            // copy — this is the forceValue lever).  allocFakeClo reset upvalEnv to
            // null; poison the unused inline FAM so the brute-scanner sees no stale
            // pointers there (walkClosure skips the FAM when upvalEnv is set).
            if (Env * te = thunkUpvalEnv(t)) {
                fakeClo->upvalEnv = te;
                for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i].mkUninitialized();
            } else {
                for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];
            }
            // Phase D coverage: fakeClo's upvalues now mirror t->tail[].
            // If the fakeClo itself is tenured (pool may return tenured)
            // and t->tail[] carries nursery payloads, dirty-list.
            closurePostConstructBarrier(fakeClo);

            ListVec * thunkWiths = thunkCapturedWiths(t);  // FP-2b: was suspended.capturedWiths
            const CompilationUnit * thunkCu = thunkCU(t);  // FP-2a: was t->suspended.cu
            if (!thunkCu) thunkCu = cu;                     // ...?: cu fallback preserved

            // Same call-depth guard as OP_CALL — catches blackhole-style
            // recursion that doesn't go through OP_CALL (e.g. `let x = x;
            // in x`, where every reference to x re-enters via OP_FORCE).
            if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
                throw std::runtime_error("v3 OP_FORCE: stack overflow; call depth exceeded "
                                          + std::to_string(kMaxCallDepth));

            // REVIEW §3: window between `t->state = Blackhole` and the
            // frame-push could leak orphan Black thunks if any step in
            // between threw bad_alloc (valueStack.resize, withStack
            // reads, position-pool accesses).  Snapshot prior state so
            // the catch path can revert.  Single thunk per OP_FORCE so
            // the snapshot is one ThunkState.
            ThunkState priorState = t->state;
            t->state = ThunkState::Blackhole;

            // ip on caller frame must be saved BEFORE the resize too,
            // so the catch path can leave it unchanged-but-correct.
            uint32_t priorCallerIp = vm.frames.back().ip;
            uint32_t priorCallerFlags = vm.frames.back().flags;
            vm.frames.back().ip = ip;
            // WC-38: mark the caller frame for force-retry. When the
            // pushed thunk's body returns, OP_RETURN's caller-resume
            // path will re-enter op_force_slow if retVal is still a
            // Thunk/App.  This replaces the over-eager OP_RETURN
            // chain push with GHC STG-style consumer-driven chase.
            vm.frames.back().flags |= CFF_FORCE_RETRY;

            size_t newBase = vm.valueStack.size();
            uint32_t newWithBase;
            try {
                vm.valueStack.resize(newBase + desc->nLocals);
                newWithBase = static_cast<uint32_t>(vm.withStack.size());
            } catch (...) {
                // Revert: thunk back to Suspended, caller frame back
                // to its prior ip/flags.  Re-throw -- the v3 force
                // hook / outer eval will catch and route via
                // phaseBFailureCount or fallbackToTreeWalker.
                t->state = priorState;
                vm.frames.back().ip = priorCallerIp;
                vm.frames.back().flags = priorCallerFlags;
                throw;
            }

            // V3_DBG_STORE_PREVSTAGE: trace OP_FORCE pushes for thunks
            // with nUp=5 to verify their codeOffset before body runs.
            {
                static const bool s_dbg_force =
                    std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
                if (s_dbg_force && t->nUpvalues == 5) {
                    std::fprintf(stderr,
                        "v3 OP_FORCE: pushing thunk %p desc=%s codeOffset=%u "
                        "nUp=%u cu=%p\n",
                        (void*)t,
                        !desc->name.empty() ? desc->name.c_str() : "<anon>",
                        desc->codeOffset, (unsigned)t->nUpvalues,
                        (void*)thunkCu);
                }
            }
            // WC-38: V3_DBG_FORCE_TRACE=DEPTH — log every OP_FORCE
            // pushed at frame-stack depth >= DEPTH.  Used to identify
            // the eager-force divergence between v3 and tree-walker.
            // Frame depth filter avoids spam — only deep forces inside
            // pkgs's body are interesting.
            {
                static const char * s_dbg_force_trace =
                    std::getenv("V3_DBG_FORCE_TRACE");
                if (s_dbg_force_trace) {
                    static const size_t depthFilter =
                        std::atoll(s_dbg_force_trace);
                    if (vm.frames.size() >= depthFilter) {
                        // Resolve source position for direct trace-diff
                        // against tree-walker's TW_DBG_FORCE output.
                        const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                        if (ps && !ps->file.empty()) {
                            std::fprintf(stderr,
                                "v3 FORCE: %s:%u:%u\n",
                                ps->file.c_str(), ps->line, ps->column);
                        } else {
                            std::fprintf(stderr,
                                "v3 FORCE: <?nopos> name=%s codeOff=%u\n",
                                !desc->name.empty() ? desc->name.c_str() : "<anon>",
                                desc->codeOffset);
                        }
                    }
                }
            }

            // NIX_TRACE_EVAL: F event for OP_FORCE's CFF_THUNK_RETURN
            // frame push.  Paired W is emitted by OP_RETURN's
            // CFF_THUNK_RETURN branch when this frame pops.
            // Conditional-cheap (single FILE* null check when disabled).
            if (__builtin_expect(nix::evalTrace::enabled(), 0)) {
                const PosSnapshot * ps = resolvePosSnapshot(desc->posHandle);
                std::string posStr;
                if (ps && !ps->file.empty()) {
                    posStr = nix::evalTrace::formatPos(ps->file, ps->line, ps->column);
                } else {
                    // 2026-05-18 cc-wrapper bisection: when verbose-no-pos
                    // is requested, decorate <no-pos> with desc name +
                    // thunk pointer so the cross-evaluator diff can
                    // disambiguate which synthesized thunk we're forcing.
                    static const bool s_verboseNoPos =
                        std::getenv("NIX_TRACE_EVAL_VERBOSE_NOPOS") != nullptr;
                    if (s_verboseNoPos) {
                        char buf[128];
                        std::snprintf(buf, sizeof buf,
                            "<no-pos:opforce name=%.40s t=%p>",
                            desc->name.empty() ? "<?>" : desc->name.c_str(),
                            (void *)t);
                        posStr = buf;
                    } else {
                        posStr = "<no-pos>";
                    }
                }
                nix::evalTrace::enterForce(posStr);
            }
            // V3_DBG_HOT_FORCE: count total dispatches + unique thunk
            // pointers at the configured source position suffix.
            hotForceCheck(t);
            // TT-1 falsifier (2026-06-19, NIX_V3_DBG_THUNK_BODYSIZE): histogram
            // forced-thunk body sizes (code-words from codeOffset to the first
            // body terminator) to size the trivial-thunk inline-force lever —
            // what % of forced thunks have a tiny (≤2-4-word) body that could
            // run inline, skipping this frame-push + dispatchLoop re-entry?
            {
                static const bool s_bs = std::getenv("NIX_V3_DBG_THUNK_BODYSIZE") != nullptr;
                if (__builtin_expect(s_bs, 0)) {
                    struct Hist {
                        uint64_t b[6] = {}; uint64_t total = 0;
                        ~Hist() { std::fprintf(stderr,
                            "v3 TT-1 forced-thunk body-size (words to terminator): "
                            "1-2=%llu 3-4=%llu 5-8=%llu 9-16=%llu 17-32=%llu 33+=%llu total=%llu "
                            "(trivial[1-4]=%.1f%%)\n",
                            (unsigned long long)b[0],(unsigned long long)b[1],(unsigned long long)b[2],
                            (unsigned long long)b[3],(unsigned long long)b[4],(unsigned long long)b[5],
                            (unsigned long long)total,
                            total? 100.0*(b[0]+b[1])/total : 0.0); }
                    };
                    static Hist hist;
                    static std::unordered_map<uint64_t,uint32_t> cache;
                    uint64_t key = (reinterpret_cast<uintptr_t>(thunkCu) << 24) ^ desc->codeOffset;
                    auto it = cache.find(key);
                    uint32_t words;
                    if (it != cache.end()) words = it->second;
                    else {
                        words = 0;
                        for (uint32_t p = desc->codeOffset;
                             p < thunkCu->code.size() && words < 100000; ++p) {
                            Op o = decodeOp(thunkCu->code[p]); ++words;
                            if (o==OP_RETURN||o==OP_R_RETURN||o==OP_HALT
                                ||o==OP_TAIL_CALL||o==OP_TAIL_CALL_N) break;
                        }
                        cache[key] = words;
                    }
                    int bk = words<=2?0 : words<=4?1 : words<=8?2 : words<=16?3 : words<=32?4 : 5;
                    hist.b[bk]++; hist.total++;
                }
            }
            vm.frames.push_back(CallFrame{
                .cu = thunkCu,
                .closure = fakeClo,
                .thunk = t,
                .ip = desc->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = CFF_THUNK_RETURN,
            });
            pushCapturedWiths(vm, thunkWiths);
            cu = thunkCu;

            ip = desc->codeOffset;
            closure = fakeClo;
            stackBase = newBase;
            break;
        }

        // --- Lists ---
        case OP_LIST_INIT: {
            uint32_t n = operand;
            // Empty list: skip the alloc, push the singleton.  Common
            // for default formals (`xs ? []`) and branch results.
            if (n == 0) {
                push(vm, Value::vEmptyList);
                break;
            }
            ListVec * l = Alloc::allocList(n);
            V3_STATS_INC(listsAllocated);
            for (uint32_t i = n; i > 0; --i) l->elems[i - 1] = pop(vm);
            listPostConstructBarrier(l);  // Phase D coverage (OP_LIST_INIT)
            Value v;
            v.mkList(l);
            push(vm, v);
            break;
        }
        case OP_LIST_CONCAT: {
            // Force-on-receive: lazy values (Tag::App from mapAttrs/
            // map/zipAttrsWith, Tag::Thunk from chained AttrSelects)
            // must be forced before shape-checking.  See WC-35.
            // A8: iterative writeback-force for the two args.  Stack:
            // [..., lhs, rhs] (rhs on top).
            {
                size_t topIdx = vm.valueStack.size() - 1;
                Value & rhsRef = vm.valueStack[topIdx];
                Value & lhsRef = vm.valueStack[topIdx - 1];
                if (needsForce(rhsRef)) {  // C-8/9/10
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
                if (needsForce(lhsRef)) {  // C-8/9/10
                    uint32_t off = static_cast<uint32_t>((topIdx - 1) - stackBase);
                    if (__builtin_expect(off > 0xFFFFu, 0))
                        throw std::runtime_error(
                            "v3 OP_LIST_CONCAT: writeback slot offset too large");
                    push(vm, lhsRef);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(off));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;
                    goto op_force_slow;
                }
            }
            Value rhs = pop(vm), lhs = pop(vm);
            if (!lhs.isList() || !rhs.isList())
                throw std::runtime_error("v3 OP_LIST_CONCAT: not lists");
            uint32_t n = lhs.asList()->size + rhs.asList()->size;
            ListVec * out = Alloc::allocList(n);
            V3_STATS_INC(listsAllocated);
            uint32_t k = 0;
            for (uint32_t i = 0; i < lhs.asList()->size; ++i) out->elems[k++] = lhs.asList()->elems[i];
            for (uint32_t i = 0; i < rhs.asList()->size; ++i) out->elems[k++] = rhs.asList()->elems[i];
            listPostConstructBarrier(out);  // Phase D coverage (OP_LIST_CONCAT)
            Value v;
            v.mkList(out);
            push(vm, v);
            break;
        }

        // --- Attrsets ---
        case OP_ATTRS_INIT: {
            uint32_t n = operand;
            // Empty attrset: skip the alloc entirely, push the singleton.
            // Real-world Nix code creates many empty attrsets (default
            // formal `... ? {}`, branch results etc.) — not allocating
            // them is cheap and reduces GC pressure.
            if (n == 0) {
                push(vm, Value::vEmptyAttrs);
                break;
            }
            // REVIEW MED-6: build entries directly into either a stack
            // buffer (most attrsets are small) or a heap fallback when
            // n exceeds kSmall.  Pre-fix used four std::vectors per
            // call (names/poses/values/entries); now zero allocations
            // for n <= kSmall and one for the heap fallback.
            struct Entry { SymbolId name; Value value; uint32_t pos; };
            constexpr uint32_t kSmall = 16;
            Entry smallBuf[kSmall];
            std::vector<Entry> bigBuf;
            Entry * entries;
            if (n <= kSmall) {
                entries = smallBuf;
            } else {
                bigBuf.resize(n);
                entries = bigBuf.data();
            }
            // Each entry is (SymbolId, PosIdx) inlined as 2 code words
            // followed by n popped values.  PosIdx feeds the per-attr
            // position side-table backing builtins.unsafeGetAttrPos.
            for (uint32_t i = 0; i < n; ++i) {
                entries[i].name = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                entries[i].pos  = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * n;
            for (uint32_t i = n; i > 0; --i) entries[i - 1].value = pop(vm);
            // Sort by name; duplicates become adjacent.
            std::sort(entries, entries + n,
                      [](const Entry & a, const Entry & b) { return a.name < b.name; });
            for (uint32_t i = 1; i < n; ++i) {
                if (entries[i].name == entries[i - 1].name) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = entries[i].name;
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT: attribute '" + s +
                                              "' already defined");
                }
            }
            Bindings * b = Alloc::allocBindings(n);
            V3_STATS_INC(attrsetsAllocated);
            for (uint32_t i = 0; i < n; ++i) {
                b->entries[i].name = entries[i].name;
                b->entries[i].pos  = entries[i].pos;  // #752 inline
                bindingsSetValue(b, i, entries[i].value);  // Phase D barrier
            }
            // Phase A1: origin tracking (NIX_V3_DBG_BINDINGS_ORIGIN=1).
            // Use the first entry's posHandle as a representative source
            // location — the attrset literal's `{` is unattributed at IR
            // level, but entry positions are close enough to localize.
            recordBindingsOrigin(b, n > 0 ? entries[0].pos : 0, "OP_ATTRS_INIT");
            Value v;
            v.mkAttrs(b);

            push(vm, v);
            break;
        }
        case OP_ATTRS_INIT_DYN: {
            uint32_t nStatic = (operand >> 12) & 0xFFFu;
            uint32_t nDyn    = operand & 0xFFFu;
            // Stack layout (bottom-up): [static values...][dyn name+value pairs...].
            // Inline layout: nStatic*(name, pos) pairs followed by nDyn pos words.
            // Build directly into one scratch entry array; null dynamic names are
            // skipped by compacting `nEntries`.
            constexpr uint32_t kSmall = 16;
            Bindings::Entry smallBuf[kSmall];
            std::vector<Bindings::Entry> bigBuf;
            const uint32_t maxEntries = nStatic + nDyn;
            Bindings::Entry * entries;
            if (maxEntries <= kSmall) {
                entries = smallBuf;
            } else {
                bigBuf.resize(maxEntries);
                entries = bigBuf.data();
            }
            for (uint32_t i = 0; i < nStatic; ++i) {
                entries[i].name = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                entries[i].pos  = cu->code[ip + 2 * i + 1];
            }
            ip += 2 * nStatic;
            const uint32_t dynPosBase = ip;
            ip += nDyn;

            uint32_t nEntries = nStatic;
            for (uint32_t i = nDyn; i > 0; --i) {
                Value valV  = pop(vm);
                Value nameV = pop(vm);
                // null-named dynamic attrs are silently dropped — Nix
                // semantics so things like `{ ${if cond then "k" else null}
                // = v; }` work as a conditional add.
                if (nameV.isNull()) continue;
                if (!nameV.isString())
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: dynamic name must be a string");
                // #685 — TW rejects dynamic attr names with string
                // context (libexpr/eval.cc:2826 forceStringNoCtx).
                requireNoStringContextRuntime(nameV, "OP_ATTRS_INIT_DYN");
                // Use the global symbol table — IDs from any CU stay
                // consistent so attrset lookups across CUs work.
                SymbolId id = ir::globalInternSymbol(nameV.asString());
                entries[nEntries].name = id;
                entries[nEntries].pos  = cu->code[dynPosBase + i - 1];
                entries[nEntries].value = valV;
                ++nEntries;
            }
            for (uint32_t i = nStatic; i > 0; --i)
                entries[i - 1].value = pop(vm);

            std::sort(entries, entries + nEntries,
                      [](const Bindings::Entry & a, const Bindings::Entry & b) {
                          return a.name < b.name;
                      });
            // Dup-attr detection: after sort, duplicates are adjacent.
            for (uint32_t i = 1; i < nEntries; ++i) {
                if (entries[i].name == entries[i - 1].name) {
                    const auto & tbl = ir::globalSymbolTable();
                    SymbolId nm = entries[i].name;
                    std::string s = (nm < tbl.size()) ? tbl[nm] : "?";
                    throw std::runtime_error("v3 OP_ATTRS_INIT_DYN: attribute '" + s +
                                              "' already defined");
                }
            }
            Bindings * b = Alloc::allocBindings(nEntries);
            V3_STATS_INC(attrsetsAllocated);
            for (uint32_t i = 0; i < nEntries; ++i) {
                b->entries[i].name = entries[i].name;
                b->entries[i].pos  = entries[i].pos;  // #752 inline
                bindingsSetValue(b, i, entries[i].value);  // Phase D barrier
            }
            // Phase A1: origin tracking.
            recordBindingsOrigin(b,
                nEntries == 0 ? 0 : entries[0].pos,
                "OP_ATTRS_INIT_DYN");
            Value v;
            v.mkAttrs(b);

            push(vm, v);
            break;
        }
        case OP_ATTRS_REC_INIT: {
            // #498 diagnostic: log local[0] of the current frame at
            // OP_ATTRS_REC_INIT for "final"-named maker frames.
            static const bool s_dbgRecInitLocal0 =
                std::getenv("V3_DBG_REC_INIT_LOCAL0") != nullptr;
            if (__builtin_expect(s_dbgRecInitLocal0, 0)) {
                const auto & fr = vm.frames.back();
                const LambdaDescriptor * d = nullptr;
                if (fr.closure) d = fr.closure->desc;
                else if (fr.thunk) d = fr.thunk->suspended.desc;
                if (d && d->name == "final") {
                    Value v = vm.valueStack[fr.stackBaseOffset + 0];
                    Value chase = v;
                    int hops = 0;
                    while (hops < 4) {
                        if (chase.tag() == Tag::Slot && chase.asSlot())
                            chase = *chase.asSlot();
                        else if (chase.tag() == Tag::Thunk && chase.asThunk()
                                 && chase.asThunk()->state == ThunkState::Evaluated)
                            chase = chase.asThunk()->evaluated;
                        else break;
                        ++hops;
                    }
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_REC_INIT in final codeOff=%u: local[0].tag=%d",
                        (unsigned)d->codeOffset, (int)v.tag());
                    if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                        const auto & st = ir::globalSymbolTable();
                        auto * b = chase.asAttrs();
                        std::fprintf(stderr, " -> attrs size=%u {",
                            (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 4; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s", k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        std::fprintf(stderr, "}");
                    } else {
                        std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
                    }
                    std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
                }
            }
            // Allocate a Bindings(n) with placeholder values; values
            // are written later by OP_ATTRS_REC_SET[slot].  Names come
            // pre-sorted from emit (LetRec emit sorts entries by
            // SymbolId before writing the data words and rewrites the
            // REC_SET operand to the sorted slot).  Each entry is
            // (SymbolId, PosIdx) — the PosIdx feeds the per-attr
            // position side-table.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            V3_STATS_INC(attrsetsAllocated);
            uint32_t firstPos = 0;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].pos  = ps;  // #752 inline
                b->entries[i].value.mkNull();
                if (i == 0) firstPos = ps;
            }
            ip += 2 * n;
            // Phase A1: origin tracking.  Rec attrsets have stable
            // identity (they're written to via OP_ATTRS_REC_SET); their
            // origin is the same place across the rec-init/rec-set
            // sequence.
            recordBindingsOrigin(b, firstPos, "OP_ATTRS_REC_INIT");
            Value v;
            v.mkAttrs(b);
            // OP_ATTRS_REC_INIT is the ONLY callsite with isRecInit=true.
            // The rec attrset's Bindings* registered here is the same
            // pointer that subsequent OP_ATTRS_REC_SET writes into, so
            // self-reference recovery via partialBindings observes the
            // entries as they are filled in.

            // M-8 (CODEBASE_REVIEW_2026-06-11): the #558 Phase 1.5 shapeCell
            // publish (and its VM-13 NIX_V3_CELL_EVERYWHERE gate) was REMOVED
            // here along with the Thunk::shapeCell field.  The experiment was
            // default-off, so production never published — byte-identical.
            push(vm, v);
            break;
        }
        case OP_ATTRS_LET_REC_INIT: {
            // Bytecode-identical body to OP_ATTRS_REC_INIT (allocate a
            // placeholder rec-attrset with the n trailing (name, pos)
            // pairs; entries are filled in by following OP_ATTRS_REC_SET
            // ops sharing the same n-slot layout).
            //
            // KEY DIFFERENCE: no publishToNearestBlackThunkFrame.  The
            // emitter selects this opcode for `let ... in body` shapes
            // (lowerLet -> lowerLetRecCapture with hasBody=true), where
            // the rec-attrset is INTERMEDIATE state -- the surrounding
            // thunk's eventual return value is `body`'s evaluation,
            // NOT the recAttrs.  Publishing the placeholder recAttrs
            // (or its in-progress fill) to the surrounding Black thunk
            // sets thunk->state=Evaluated with a wrong-shape value,
            // which later participates in with-scope lookups and
            // produces "name X not found in with-scope" errors when
            // the with-source dereferences to that wrong shape.
            //
            // Specifically surfaces in lib.extends's body
            //   `final: let prev = f final; in prev // overlay final prev`
            // where `let prev = ...` ran inside lib.fix's `x`-thunk
            // body and published `{prev}` (size 1) onto `x`'s thunk.
            // Inner closures that captured `pkgs = self = final = x`
            // then saw `{prev}` instead of the full pkgs attrset and
            // failed to resolve `with pkgs; callPackage`.  See
            // CALLPACKAGE_BUG_2026-05-09.md for the full analysis.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            V3_STATS_INC(attrsetsAllocated);
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].pos  = ps;  // #752 inline
                b->entries[i].value.mkNull();
            }
            ip += 2 * n;
            Value v;
            v.mkAttrs(b);
            push(vm, v);
            break;
        }
        case OP_ATTRS_REC_INIT_TAIL: {
            // #558 (2026-05-10): tail-return-AttrSet variant.
            // Bytecode-identical to OP_ATTRS_REC_INIT (allocates a
            // Bindings(n) with placeholder values, n trailing (name,
            // pos) pairs in the same layout) BUT registers the partial
            // Bindings with EVERY thunk frame on the call stack —
            // Black AND Suspended — using FIRST-WINS semantics.
            //
            // Emitted by the lowerer when the AttrSet IR's
            // `isFunctionReturn` flag is true — i.e. this AttrSet IS
            // the function body's tail-return value.  Outer thunks
            // currently waiting for this function's return are
            // therefore conceptually waiting for THIS AttrSet's
            // value, so registering with all of them lets `with self;`
            // / `with pkgs;`-style lookups find the in-progress
            // entries via the partial-Bindings peek path
            // (vm.cc:withLookup).
            //
            // See bytecode.hh OP_ATTRS_REC_INIT_TAIL doc + ir.hh
            // AttrSet::isFunctionReturn doc for the complete design
            // rationale.
            uint32_t n = operand;
            Bindings * b = Alloc::allocBindings(n);
            V3_STATS_INC(attrsetsAllocated);
            uint32_t firstPosTail = 0;
            for (uint32_t i = 0; i < n; ++i) {
                SymbolId nm = static_cast<SymbolId>(cu->code[ip + 2 * i]);
                uint32_t ps = cu->code[ip + 2 * i + 1];
                b->entries[i].name = nm;
                b->entries[i].pos  = ps;  // #752 inline
                b->entries[i].value.mkNull();
                if (i == 0) firstPosTail = ps;
            }
            ip += 2 * n;
            // Phase A1: origin tracking.  This is the tail-return rec
            // init — the load-bearing one for cell-update-everywhere.
            // Identifying which Nix source line emits this lets us
            // localize which `rec { ... }` is being observed mid-state.
            recordBindingsOrigin(b, firstPosTail, "OP_ATTRS_REC_INIT_TAIL");
            Value v;
            v.mkAttrs(b);

            // M-8 (CODEBASE_REVIEW_2026-06-11): #558 shapeCell publish removed
            // (default-off experiment; field gone — see closure.hh).
            push(vm, v);
            break;
        }
        case OP_APPLY_OVERRIDES: {
            // Peek the attrset on top of stack.  If it has __overrides,
            // force it and merge each (name, value) into the rec attrs:
            //   - Names already present are overwritten in place (so
            //     OP_ATTRS_SELECT inside the rec body sees the new value).
            //   - New names cause a Bindings grow + re-sort so the result
            //     attrset visible to the outer scope contains them.
            //   This matches tree-walker semantics.
            Value & top = vm.valueStack.back();
            if (!top.isAttrs() || !top.asAttrs()) break;
            static const SymbolId ovId = ir::globalInternSymbol("__overrides");
            const Value * ovRaw = top.asAttrs()->lookup(ovId);
            if (!ovRaw) break;
            Value ov = forceValue(vm, *ovRaw);
            // Tree-walker raises if __overrides is present but not an
            // attrset; v3 silently ignored.
            if (!ov.isAttrs())
                throw std::runtime_error("v3 OP_APPLY_OVERRIDES: __overrides must be an attrset");
            if (!ov.asAttrs()) break;
            auto * dst = top.asAttrs();
            // PLAN_BEAT_TW_V2 §1.7 (correctness, from the chain-SELECT RCA):
            // `dst->lookup(k)` below is CHAIN-AWARE — for a Chain Bindings it
            // walks overlay then parent layers — and the matched slot is then
            // mutated in place via `const_cast`.  If `dst` is ever a Chain
            // whose match lands in a SHARED parent layer, that write corrupts
            // every sibling chain pointing at the same parent (the C-1
            // stale-KEEP class: src/libexpr attr-set.hh layered Bindings are
            // shared across all of nixpkgs).  OP_ATTRS_REC_INIT builds Sorted
            // Bindings today so this is latent, but guard it now: if `dst` is a
            // Chain, privatize a flat Sorted copy and operate on THAT, so an
            // in-place override can never leak into a shared parent.
            if (dst->isChain()) {
                const Bindings * flat = dst->materialize();
                Bindings * priv = Alloc::allocBindings(flat->size);
                V3_STATS_INC(attrsetsAllocated);
                for (uint32_t i = 0; i < flat->size; ++i)
                    bindingsSetEntry(priv, i, flat->entries[i]);  // Phase D
                top.mkAttrs(priv);   // replace the chain on the stack
                dst = priv;          // overrides now write into our private copy
            }
            const auto * src = ov.asAttrs();
            // First pass: overwrite existing entries; collect names to add.
            std::vector<std::pair<SymbolId, Value>> toAdd;
            for (uint32_t i = 0; i < src->size; ++i) {
                SymbolId k = src->entries[i].name;
                const Value * existing = dst->lookup(k);
                if (existing) {
                    // Mutate in place via const_cast — `lookup` returns a
                    // pointer to the actual storage and we own this Bindings.
                    const_cast<Value &>(*existing) = src->entries[i].value;
                } else {
                    toAdd.emplace_back(k, src->entries[i].value);
                }
            }
            if (!toAdd.empty()) {
                Bindings * grown = Alloc::allocBindings(dst->size + toAdd.size());
                V3_STATS_INC(attrsetsAllocated);
                std::vector<std::pair<SymbolId, Value>> all;
                all.reserve(dst->size + toAdd.size());
                for (uint32_t i = 0; i < dst->size; ++i)
                    all.emplace_back(dst->entries[i].name, dst->entries[i].value);
                for (auto & e : toAdd) all.push_back(e);
                std::sort(all.begin(), all.end(),
                    [](auto & a, auto & b) { return a.first < b.first; });
                for (size_t i = 0; i < all.size(); ++i) {
                    grown->entries[i].name  = all[i].first;
                    bindingsSetValue(grown, static_cast<uint32_t>(i),
                                     all[i].second);  // Phase D barrier
                }
                top.mkAttrs(grown);
            }
            break;
        }
        case OP_ATTRS_SELECT: {
            // (A8 Bridge attr peek retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
            // A8: iterative force — when top is non-WHNF, rewind to
            // OP_ATTRS_SELECT and goto op_force_slow.  Replaces the
            // C-recursive `attrs = forceValue(vm, attrs)` below.
            {
                Value & topRef = vm.valueStack.back();
                if (topRef.isAppLike()
                    || topRef.tag() == Tag::Thunk
                    || topRef.tag() == Tag::Slot)
                {
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
            }
            Value attrs = pop(vm);
            //
            // #558 (2026-05-10) partial-Bindings peek for OP_ATTRS_SELECT:
            // when the source is a Black thunk in mid-construction (the
            // canonical lib.fix `let x = f x; in x` shape, where x is
            // currently being forced and an inner `self.X` access tries
            // to select through it), peek the partial-Bindings registry
            // chain BEFORE forcing.  If the chain has an entry for the
            // looked-up name, return it — avoids the BlackholeError that
            // forceValue would throw on the Black thunk.
            //
            // Mirror of the OP_WITH_LOOKUP partial-Bindings peek path
            // (vm.cc:withLookup) but for direct Select access.  Both
            // paths share the same registry chain (populated by
            // OP_ATTRS_REC_INIT_TAIL via publishToAllThunkFrames).
            // #558 (2026-05-10) Chase Evaluated thunks to find a
            // potentially-Black target.  When `attrs` is a thunk that
            // was Evaluated to another thunk (e.g., the inherit-from
            // cache thunk's `evaluated` was set to a recovered
            // Tag::Thunk for the outer Black fix-point), the chain
            // peek path SHOULD fire on the chased target.  Without
            // this chase, we'd see Tag::Attrs (from STG WHNF's
            // recovery in the cache thunk's body) which doesn't have
            // all the chain layers' keys.
            {
                Value chase = attrs;
                int hops = 0;
                while (hops < 8
                       && chase.isThunk()
                       && chase.asThunk()
                       && chase.asThunk()->state == ThunkState::Evaluated
                       && chase.asThunk()->evaluated.isThunk())
                {
                    chase = chase.asThunk()->evaluated;
                    ++hops;
                }
            }
            // A8: force is handled at case entry (iterative).  By here
            // `attrs` is WHNF.
            // Phase A3: when SELECT operates on a 1-entry attrset
            // matching V3_DBG_SELECT_PATTERN (e.g. "family"), dump the
            // source position + selecting attribute name.  Used to
            // catch the moment v3 reaches SELECT on a `{family}` shape
            // that TW would have skipped.
            {
                static const char * s_dbgSelP =
                    std::getenv("V3_DBG_SELECT_PATTERN");
                if (__builtin_expect(s_dbgSelP != nullptr, 0)) {
                    if (attrs.isAttrs() && attrs.asAttrs()
                        && attrs.asAttrs()->size == 1) {
                        const auto & st = ir::globalSymbolTable();
                        SymbolId nm = attrs.asAttrs()->entries[0].name;
                        const char * nmStr =
                            nm < st.size() ? st[nm].c_str() : "?";
                        if (std::strcmp(nmStr, s_dbgSelP) == 0) {
                            SymbolId sym = static_cast<SymbolId>(operand);
                            const char * symStr =
                                sym < st.size() ? st[sym].c_str() : "?";
                            std::fprintf(stderr,
                                "v3 OP_ATTRS_SELECT on {%s} (size=1) "
                                "selecting '%s' [ptr=%p]",
                                nmStr, symStr,
                                (const void *)attrs.asAttrs());
                            if (const BindingsOrigin * o =
                                    lookupBindingsOrigin(attrs.asAttrs())) {
                                const PosSnapshot * ps =
                                    resolvePosSnapshot(o->posHandle);
                                std::fprintf(stderr,
                                    " bindings-origin=%s@%s:%u",
                                    o->source ? o->source : "?",
                                    (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                    ps ? ps->line : 0u);
                            }
                            if (!vm.frames.empty()) {
                                const auto & fr = vm.frames.back();
                                const LambdaDescriptor * d = nullptr;
                                if (fr.thunk
                                    && (fr.thunk->state == ThunkState::Suspended
                                        || fr.thunk->state == ThunkState::Blackhole))
                                    d = fr.thunk->suspended.desc;
                                else if (fr.closure) d = fr.closure->desc;
                                const PosSnapshot * fps =
                                    d ? resolvePosSnapshot(d->posHandle) : nullptr;
                                std::fprintf(stderr,
                                    " from %s@%s:%u (ip=%u)",
                                    d && !d->name.empty() ? d->name.c_str() : "<?>",
                                    (fps && !fps->file.empty()) ? fps->file.c_str() : "?",
                                    fps ? fps->line : 0u, ip - 1);
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            if (!attrs.isAttrs()) {
                // #558 (2026-05-10) diagnostic: log tag + symbol + frame
                // chain when a Select fails on a non-attrs value.  Used
                // to root-cause the v3-vs-TW divergence on nixpkgs.
                static const bool s_dbgSelFail =
                    std::getenv("V3_DBG_SELECT_FAIL") != nullptr;
                if (s_dbgSelFail) {
                    const auto & st = ir::globalSymbolTable();
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_SELECT: not an attrset (tag=%d) "
                        "looking up '%s' (frames=%zu)\n",
                        (int)attrs.tag(),
                        operand < st.size() ? st[operand].c_str() : "?",
                        vm.frames.size());
                    size_t lim = vm.frames.size();
                    for (size_t fi = lim; fi > 0 && fi + 12 > lim; --fi) {
                        const auto & frD = vm.frames[fi - 1];
                        const LambdaDescriptor * d = nullptr;
                        if (frD.thunk
                            && (frD.thunk->state == ThunkState::Suspended
                                || frD.thunk->state == ThunkState::Blackhole))
                            d = frD.thunk->suspended.desc;
                        else if (frD.closure)
                            d = frD.closure->desc;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        // For closure frames, peek at slot 0 (the lambda
                        // param) — when it's a string, print its value.
                        // This is the cheapest way to tell "envKeyValue
                        // called with k=<which attr name>" without
                        // instrumenting the wrapper itself.
                        //
                        // If slot 0 is a Thunk, peek into the thunk's
                        // body for a known-value (the v3 thunk may
                        // already be in Evaluated state with the result
                        // cached, in which case .value is the forced
                        // string).  We do NOT call forceValue here —
                        // we're in the middle of an exception throw
                        // path, recursion is dangerous.
                        char argBuf[128] = "";
                        auto formatValue = [&](const Value & v) {
                            if (v.tag() == Tag::String && v.asString()) {
                                std::snprintf(argBuf, sizeof(argBuf),
                                              " arg0=\"%.96s\"", v.asString());
                            } else if (v.tag() == Tag::Int) {
                                std::snprintf(argBuf, sizeof(argBuf),
                                              " arg0=%lld", (long long)v.asInt());
                            } else if (v.tag() == Tag::Bool) {
                                std::snprintf(argBuf, sizeof(argBuf),
                                              " arg0=%s",
                                              v.asInt() == 1 ? "true" : "false");
                            } else if (v.tag() == Tag::Null) {
                                std::snprintf(argBuf, sizeof(argBuf), " arg0=null");
                            } else {
                                std::snprintf(argBuf, sizeof(argBuf),
                                              " arg0=<tag=%d>", (int)v.tag());
                            }
                        };
                        if (frD.closure
                            && frD.stackBaseOffset < vm.valueStack.size())
                        {
                            const Value & v0 = vm.valueStack[frD.stackBaseOffset];
                            if (v0.tag() == Tag::Thunk && v0.asThunk()
                                && v0.asThunk()->state == ThunkState::Evaluated)
                            {
                                formatValue(v0.asThunk()->evaluated);
                            } else {
                                formatValue(v0);
                            }
                        }
                        std::fprintf(stderr,
                            "  [%zu] %s ip=%u thunk=%p flags=%u %s:%u:%u%s\n",
                            fi - 1,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            frD.ip,
                            (void *)frD.thunk,
                            (unsigned)frD.flags,
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                            ps ? ps->line : 0u,
                            ps ? ps->column : 0u,
                            argBuf);
                    }
                    std::fflush(stderr);
                }
                throw std::runtime_error("v3 OP_ATTRS_SELECT: not an attrset");
            }
            uint32_t icIdx = cu->code[ip++];
            // REVIEW §3: IC entries key on (Bindings* shape pointer +
            // slot index + sym).  Safe because Bindings::entries is a
            // FAM allocated alongside Bindings -- the pointer to a
            // specific entry doesn't move once the Bindings is built.
            // OP_APPLY_OVERRIDES grows the entries vector via realloc
            // (see vm.cc:2301+), but that's a different Bindings* so
            // the IC entry doesn't alias.  If a future op were to
            // mutate an existing Bindings in-place (resize entries[]),
            // every cached entry pointer would dangle -- update this
            // comment to add the assertion.
            auto & ic = cu->attrSelectCache[icIdx];
            // Phase 13.3: non-const so we can write back the resolved
            // value of a Tag::App entry — mapAttrs et al. install lazy
            // App(App(fn,name),val) entries that, without memoization,
            // re-apply the function on every access.
            auto * b = attrs.asAttrs();

            // Lever A (MEMORY_REPRESENTATION §6) — chain-aware SELECT,
            // NO materialise().  The IC fast path and the slow-path
            // binary search below both assume Sorted: they index
            // `b->entries[]` linearly.  For a Chain Bindings the local
            // `entries[]` holds only the overlay; parent entries are
            // reachable by walking the layers.  The previous spike
            // materialised (O(N log N) copy per SELECT) which defeated
            // the chain's whole purpose — every `o.attr` re-flattened
            // the base, so chain-on measured NEUTRAL (§5).
            //
            // CRITICAL writeback rule (the python3-env / structuredAttrs
            // bug, bisected 2026-06-07): the App-like force-writeback
            // (`forceWriteTarget = found`) memoises the forced value INTO
            // the entry.  That is only safe when the entry lives in the
            // LEAF overlay — a per-chain fresh copy (bindingsSetEntry in
            // mergeBindings).  A hit in a PARENT layer is in the SHARED
            // base (every `base // oN` chain aliases it); writing a forced
            // value there contaminates all sibling chains → "OP_CALL:
            // callee is not a closure" deep inside derivationStrict, and a
            // degenerate constant drv hash.  The materialise path never
            // hits this because it copies entries first.  So: leaf hit →
            // writeback as before; parent hit → force a COPY, no writeback.
            // Lever A (MEMORY_REPRESENTATION §6) — chain SELECT MATERIALISES.
            // The IC + binary search below assume a flat Sorted entries[].
            // materialize() is memoised, so a given chain flattens at most
            // once and shares that copy with derivationStrict's own
            // materialise of the same attrset.  The big memory win comes from
            // composition/construction NOT copying the base on every `//`
            // (firefox 932→667 MB, −265 MB), NOT from a lookup-only SELECT.
            //
            // DEFERRED OPTIMISATION (2026-06-07): a chain-aware SELECT that
            // walks layers via lookup (no copy) was implemented and measured
            // a further firefox −132 MB (→535 MB), BUT introduced a subtle
            // shared-state contamination — forcing/memoising a value reached
            // through a SHARED parent layer corrupted sibling chains rooted at
            // the same base (manifested as `OP_CALL: callee is not a closure`
            // deep in cargo/git derivationStrict, bisected to this site).
            // materialize() avoids it by copying entries first (isolating any
            // writeback).  Revisit only with a precise shared-Pair / writeback
            // analysis; not worth the correctness risk for the extra 132 MB.
            if (__builtin_expect(b && b->isChain(), 0)) {
                // WS-A step 3 — chain-SELECT L1 (NIX_V3_CHAIN_LOOKUP_SELECT):
                // walk the chain layers instead of materialising a flat copy.
                // Leaf-overlay hit → memoizing KEEP writeback (this chain's own
                // copy, never shared).  Parent-layer hit → push WITHOUT a
                // writeback (the App/Thunk self-memoises at its own level; only
                // slot-flattening is lost, and nothing is written into the
                // SHARED parent — the 2026-06-07 C-1 corruption mechanism).
                // v1 skips the inline cache for chain operands.  A miss falls
                // through to materialize() so the existing missing-attr error
                // path is reused unchanged.
                if (g_chainLookupSelect) {
                    const Bindings * ownerLayer = nullptr;
                    Value * lslot = nullptr;
                    for (const Bindings * L = b; L; L = L->parent) {
                        if (const Value * v =
                                L->lookupLocal(static_cast<SymbolId>(operand))) {
                            lslot = const_cast<Value *>(v);
                            ownerLayer = L;
                            break;
                        }
                    }
                    if (lslot) {
                        const bool leafHit = (ownerLayer == b);
                        Value & s = *lslot;
                        if (leafHit && s.isAppLike()
                            && !isUnderappliedClosurePap(s)) {
                            // Safe memoizing writeback into the leaf overlay.
                            push(vm, s);
                            CallFrame & f = vm.frames.back();
                            armKeepBeltCheck(f);  // C-1 belt
                            f.forceWriteTarget = lslot;
                            f.flags |= CFF_FORCE_WB_PTR_KEEP | CFF_FORCE_RETRY;
                            if (__builtin_expect(g_sharedWbDetect, 0))
                                armedWritebackValue()[f.forceWriteTarget] =
                                    *f.forceWriteTarget;  // provenance
                            f.ip = ip;
                            goto op_force_slow;
                        }
                        // Parent hit, PAP, plain Thunk, or WHNF: push, no
                        // writeback (the consumer force-on-receives).
                        push(vm, s);
                        break;
                    }
                    // miss → fall through to the materialize/error path.
                }
                b = const_cast<Bindings *>(b->materialize());
            }
            // V3_DBG_PREHOOK diagnostic: log every ATTRS_SELECT preHook
            // attempt with what value it returns.  Used to localize WC-37.
            static const bool s_dbg_prehook = std::getenv("V3_DBG_PREHOOK") != nullptr;
            if (s_dbg_prehook) {
                static const SymbolId preHookSym = ir::globalInternSymbol("preHook");
                if (operand == preHookSym) {
                    static int call_n = 0;
                    ++call_n;
                    std::fprintf(stderr,
                        "v3 ATTRS_SELECT preHook (#%d): bindings=%p size=%u attrs:\n",
                        call_n, (void*)b, b ? b->size : 0);
                    if (b) {
                        const auto & st = ir::globalSymbolTable();
                        for (uint32_t i = 0; i < b->size && i < 30; ++i) {
                            SymbolId nm = b->entries[i].name;
                            const Value & vv = b->entries[i].value;
                            Tag vtag = vv.tag();
                            std::fprintf(stderr, "  [%u] %s tag=%u",
                                i, nm < st.size() ? st[nm].c_str() : "?",
                                (unsigned)vtag);
                            if (vtag == Tag::Thunk && vv.asThunk()) {
                                Thunk * t = vv.asThunk();
                                const LambdaDescriptor * d = nullptr;
                                if (t->state == ThunkState::Suspended)
                                    d = t->suspended.desc;
                                std::fprintf(stderr, " state=%d nUp=%u",
                                    (int)t->state, (unsigned)t->nUpvalues);
                                if (d)
                                    std::fprintf(stderr, " %s [%u..)",
                                        !d->name.empty() ? d->name.c_str() : "<anon>",
                                        d->codeOffset);
                                // For preHook entry [0], also dump the
                                // thunk's body bytecode + upvalue tags.
                                if (i == 0 && t->state == ThunkState::Suspended && d) {
                                    std::fprintf(stderr, "\n    body [%u..%u):\n",
                                        d->codeOffset, d->codeOffset + 200);
                                    const CompilationUnit * tcu = thunkCU(t);  // FP-2a
                                    if (tcu)
                                        disassembleWindow(stderr, *tcu,
                                            d->codeOffset, d->codeOffset + 200);
                                    // Dump the FUNCTION DESCRIPTORS of every
                                    // MAKE_THUNK target in this body — names
                                    // like "recref-X" tell us what each
                                    // upvalue resolves to in source.
                                    if (tcu) {
                                        const auto & cu2 = *tcu;
                                        std::fprintf(stderr, "    referenced functions:\n");
                                        for (uint32_t cur = d->codeOffset;
                                             cur < d->codeOffset + 80 && cur < cu2.code.size(); ) {
                                            Op op = decodeOp(cu2.code[cur]);
                                            uint32_t operand = decodeOperand(cu2.code[cur]);
                                            if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                                if (operand < cu2.lambdas.size()) {
                                                    const auto & d3 = cu2.lambdas[operand];
                                                    std::fprintf(stderr,
                                                        "      [%u] -> fn[%u] (%s, nUp=%u, code=[%u..))\n",
                                                        cur, operand,
                                                        !d3.name.empty() ? d3.name.c_str() : "<anon>",
                                                        d3.nUpvalues, d3.codeOffset);
                                                    // Also dump the body of recref- thunks
                                                    if (d3.name.find("recref-") == 0) {
                                                        std::fprintf(stderr, "        body:\n");
                                                        disassembleWindow(stderr, cu2,
                                                            d3.codeOffset, d3.codeOffset + 6);
                                                    }
                                                }
                                                // #530: encode word + nUpvalues + nWithTargets.
                                                cur += 3;
                                            } else {
                                                cur++;
                                            }
                                        }
                                    }
                                    std::fprintf(stderr, "    upvalues:\n");
                                    for (uint16_t u = 0; u < t->nUpvalues && u < 8; ++u) {
                                        const Value & uv = t->tail[u];
                                        Tag ut = uv.tag();
                                        std::fprintf(stderr, "      [%u] tag=%u", u, (unsigned)ut);
                                        if (ut == Tag::Closure && uv.asClosure()
                                            && uv.asClosure()->desc) {
                                            auto * cd = uv.asClosure()->desc;
                                            std::fprintf(stderr, " closure=%s [%u..) nUp=%u",
                                                !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                cd->codeOffset, uv.asClosure()->nUpvalues);
                                        } else if (ut == Tag::Thunk && uv.asThunk()) {
                                            Thunk * ut2 = uv.asThunk();
                                            std::fprintf(stderr, " state=%d nUp=%u",
                                                (int)ut2->state, (unsigned)ut2->nUpvalues);
                                            if (ut2->state == ThunkState::Suspended) {
                                                auto * d2 = ut2->suspended.desc;
                                                if (d2)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d2->name.empty() ? d2->name.c_str() : "<anon>",
                                                        d2->codeOffset);
                                            } else if (ut2->state == ThunkState::Evaluated) {
                                                Tag et = ut2->evaluated.tag();
                                                std::fprintf(stderr, " EVAL=tag%u", (unsigned)et);
                                                if (et == Tag::Closure && ut2->evaluated.asClosure()
                                                    && ut2->evaluated.asClosure()->desc) {
                                                    auto * cd = ut2->evaluated.asClosure()->desc;
                                                    std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                        !cd->name.empty() ? cd->name.c_str() : "<anon>",
                                                        cd->codeOffset, ut2->evaluated.asClosure()->nUpvalues);
                                                }
                                            }
                                        } else if (ut == Tag::Attrs && uv.asAttrs()) {
                                            auto * b2 = uv.asAttrs();
                                            std::fprintf(stderr, " attrs size=%u {", b2->size);
                                            const auto & st2 = ir::globalSymbolTable();
                                            for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                                SymbolId nm = b2->entries[k].name;
                                                std::fprintf(stderr, "%s%s",
                                                    k ? "," : "",
                                                    nm < st2.size() ? st2[nm].c_str() : "?");
                                            }
                                            std::fprintf(stderr, "}");
                                        }
                                        std::fprintf(stderr, "\n");
                                    }
                                }
                                if (t->state == ThunkState::Evaluated) {
                                    Tag etag = t->evaluated.tag();
                                    std::fprintf(stderr, " EVAL=tag%u", (unsigned)etag);
                                    if (etag == Tag::Closure && t->evaluated.asClosure()
                                        && t->evaluated.asClosure()->desc) {
                                        auto * ed = t->evaluated.asClosure()->desc;
                                        std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                            !ed->name.empty() ? ed->name.c_str() : "<anon>",
                                            ed->codeOffset,
                                            t->evaluated.asClosure()->nUpvalues);
                                    } else if (etag == Tag::String && t->evaluated.asString()) {
                                        std::fprintf(stderr, "(\"%.40s\")",
                                            t->evaluated.asString());
                                    }
                                }
                            } else if (vtag == Tag::Closure && vv.asClosure()
                                && vv.asClosure()->desc) {
                                auto * d = vv.asClosure()->desc;
                                std::fprintf(stderr, " %s [%u..) nUp=%u",
                                    !d->name.empty() ? d->name.c_str() : "<anon>",
                                    d->codeOffset, vv.asClosure()->nUpvalues);
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            // EVAL-COMP §8.1: 4-way polymorphic IC fast path.  Walk
            // the small entries array; on hit, read the cached slot
            // directly.  Hit at any way is O(kWays) compares, vs.
            // O(log n) binary search on miss.
            //
            // V3_DBG_NO_IC disables the fast-path -- bisect aid for
            // suspected IC corruption.
            static const bool s_no_ic = std::getenv("V3_DBG_NO_IC") != nullptr;
            uint32_t hitSlot = UINT32_MAX;
            if (!s_no_ic) {
                for (int w = 0; w < cu->attrSelectCache[icIdx].kWays; ++w) {
                    auto & e = ic.entries[w];
                    if (e.bindings == b
                        && e.slot < b->size
                        && b->entries[e.slot].name == static_cast<SymbolId>(operand))
                    {
                        hitSlot = e.slot;
                        break;
                    }
                }
            }
            if (hitSlot != UINT32_MAX) {
                Value & slot = b->entries[hitSlot].value;
                // 2026-05-17: iterative force + memoizing writeback for
                // mapAttrs/genList App entries.  Previously this site
                // C-recursed via `forceValue(vm, slot)` (1.2 KB +
                // dispatchLoop's 1.6 KB per level).  Replaced with the
                // CFF_FORCE_WB_PTR_KEEP protocol: push, mark slot as
                // writeback target, goto op_force_slow.  The retry chain
                // chases through Suspended thunks via vm.frames pushes
                // (no extra C-recursion); on WHNF, applyForceWriteback
                // memoizes slot=forced and leaves the value on the stack
                // for the IC handler's natural continuation.
                // eval/apply (#3): an under-applied closure-PAP (Tag::App chain
                // bottoming in a Closure with arity>depth) is ALREADY WHNF — a
                // partial application.  Forcing it is a no-op AND the memoizing
                // writeback below would saturate it to its result type and poison
                // the (often shared) entry — the 2026-06-11 python3
                // `passthru.pythonAtLeast` PAP→Bool corruption: the slot mutated
                // Thunk→App(PAP)→Bool, so a later `pythonAtLeast "3.14"` did
                // `OP_CALL` on a Bool ("callee is not a closure") and native
                // derivationStrict fell back to /v3-fake-store/, diverging the
                // drvPath of every package that depends on python3.  Push the PAP
                // directly (no force, no writeback) — mirrors OP_FORCE (vm.cc:7161).
                if (__builtin_expect(slot.isAppLike(), 0)
                    && !isUnderappliedClosurePap(slot)) {
                    push(vm, slot);
                    CallFrame & f = vm.frames.back();
                    armKeepBeltCheck(f);  // C-1 belt
                    f.forceWriteTarget = &slot;
                    f.flags |= CFF_FORCE_WB_PTR_KEEP | CFF_FORCE_RETRY;
                    if (__builtin_expect(g_sharedWbDetect, 0))
                        armedWritebackValue()[f.forceWriteTarget] = *f.forceWriteTarget;  // WS-A step 3 provenance
                    f.ip = ip;  // resume past the IC handler on success
                    goto op_force_slow;
                }
                push(vm, slot);
                // Phase A4 diagnostic mirror (IC fast path).
                // Chases through Tag::Thunk(Evaluated) + Tag::Slot to
                // find size-1 attrs results.  Without the chase, we miss
                // cases where SELECT pushes a still-thunk that later
                // resolves to the matching attrs.
                {
                    static const char * s_dbgSelR =
                        std::getenv("V3_DBG_SELECT_RESULT");
                    if (__builtin_expect(s_dbgSelR != nullptr, 0)) {
                        Value chase = vm.valueStack.back();
                        int hops = 0;
                        while (hops < 8) {
                            if (chase.tag() == Tag::Slot && chase.asSlot()) {
                                chase = *chase.asSlot();
                            } else if (chase.tag() == Tag::Thunk && chase.asThunk()
                                       && chase.asThunk()->state == ThunkState::Evaluated) {
                                chase = chase.asThunk()->evaluated;
                            } else break;
                            ++hops;
                        }
                        if (chase.tag() == Tag::Attrs && chase.asAttrs()
                            && chase.asAttrs()->size == 1) {
                            const auto & st = ir::globalSymbolTable();
                            SymbolId rnm = chase.asAttrs()->entries[0].name;
                            const char * rnmStr = rnm < st.size() ? st[rnm].c_str() : "?";
                            if (std::strcmp(rnmStr, s_dbgSelR) == 0) {
                                SymbolId selSym = static_cast<SymbolId>(operand);
                                const char * selStr = selSym < st.size() ? st[selSym].c_str() : "?";
                                std::fprintf(stderr,
                                    "v3 SELECT(IC) returned chase-to {%s} (chase-hops=%d) "
                                    "when selecting '%s' from bindings ptr=%p size=%u; "
                                    "pushed-tag=%u\n",
                                    rnmStr, hops, selStr, (const void *)b,
                                    (unsigned)b->size,
                                    (unsigned)vm.valueStack.back().tag());
                                if (const BindingsOrigin * o =
                                        lookupBindingsOrigin(b)) {
                                    const PosSnapshot * ps =
                                        resolvePosSnapshot(o->posHandle);
                                    std::fprintf(stderr,
                                        "  source-bindings-origin=%s@%s:%u\n",
                                        o->source ? o->source : "?",
                                        (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                        ps ? ps->line : 0u);
                                }
                                if (const BindingsOrigin * o2 =
                                        lookupBindingsOrigin(chase.asAttrs())) {
                                    const PosSnapshot * ps =
                                        resolvePosSnapshot(o2->posHandle);
                                    std::fprintf(stderr,
                                        "  chased-result-origin=%s@%s:%u\n",
                                        o2->source ? o2->source : "?",
                                        (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                        ps ? ps->line : 0u);
                                }
                                // Dump current frame so we know which
                                // lambda/thunk is reading the bad slot.
                                if (!vm.frames.empty()) {
                                    const auto & fr = vm.frames.back();
                                    const LambdaDescriptor * d = nullptr;
                                    if (fr.thunk
                                        && (fr.thunk->state == ThunkState::Suspended
                                            || fr.thunk->state == ThunkState::Blackhole))
                                        d = fr.thunk->suspended.desc;
                                    else if (fr.closure) d = fr.closure->desc;
                                    const PosSnapshot * fps =
                                        d ? resolvePosSnapshot(d->posHandle) : nullptr;
                                    std::fprintf(stderr,
                                        "  reading-frame: name=%s pos=%s:%u:%u codeOff=%u thunk=%p\n",
                                        d && !d->name.empty() ? d->name.c_str() : "<?>",
                                        (fps && !fps->file.empty()) ? fps->file.c_str() : "<?>",
                                        fps ? fps->line : 0u, fps ? fps->column : 0u,
                                        d ? d->codeOffset : 0u,
                                        (const void *)fr.thunk);
                                }
                            }
                        }
                    }
                }
            } else {
                // Manual binary search inlined to also recover the
                // matched slot index, so we can install in the cache.
                uint32_t lo = 0, hi = b->size;
                while (lo < hi) {
                    uint32_t mid = (lo + hi) >> 1;
                    SymbolId midName = b->entries[mid].name;
                    if (midName == static_cast<SymbolId>(operand)) { lo = mid; break; }
                    if (midName < static_cast<SymbolId>(operand)) lo = mid + 1; else hi = mid;
                }
                if (lo >= b->size || b->entries[lo].name != static_cast<SymbolId>(operand)) {
                    // WC-21 diagnostic: dump requested attr + present
                    // attr names to help root-cause closure-bridge
                    // attr-shape divergences.  Off by default.
                    static const bool dbg = std::getenv("V3_DBG_ATTRS_SELECT") != nullptr;
                    if (dbg) {
                        auto & symTab = ir::globalSymbolTable();
                        SymbolId want = static_cast<SymbolId>(operand);
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_SELECT miss: want sid=%u name=\"%s\" "
                            "bindings=%p size=%u ip=%u present=[",
                            (unsigned)want,
                            want < symTab.size() ? symTab[want].c_str() : "?",
                            (void*)b, (unsigned)b->size, (unsigned)ip);
                        for (uint32_t i = 0; i < b->size && i < 20; ++i) {
                            SymbolId nm = b->entries[i].name;
                            std::fprintf(stderr, "%s%s",
                                i ? "," : "",
                                nm < symTab.size() ? symTab[nm].c_str() : "?");
                        }
                        if (b->size > 20) std::fprintf(stderr, ",...");
                        std::fprintf(stderr, "]\n");
                        // Frame stack so we can identify WHICH function
                        // emitted this OP_ATTRS_SELECT.
                        std::fprintf(stderr, "  frame stack size=%zu (top first):\n",
                            vm.frames.size());
                        for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk && fr.thunk->state == ThunkState::Blackhole)
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure)
                                d = fr.closure->desc;
                            std::fprintf(stderr,
                                "    [%zu] %s ip=%u thunk=%p closure=%p flags=%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                fr.ip,
                                (void*)fr.thunk, (void*)fr.closure,
                                (unsigned)fr.flags);
                        }
                        if (cu) {
                            uint32_t lo = ip > 16 ? ip - 16 : 0;
                            uint32_t hi = ip + 8;
                            std::fprintf(stderr,
                                "  failing-frame disasm [%u..%u):\n", lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                        }
                        std::fflush(stderr);
                    }
                    // Include the missing attr name in the error so
                    // catch-and-fallback paths (primDerivationStrict) can
                    // surface a more useful diagnostic without the
                    // V3_DBG_ATTRS_SELECT env var.
                    {
                        const auto & symTab = ir::globalSymbolTable();
                        SymbolId want = static_cast<SymbolId>(operand);
                        std::string nm = want < symTab.size()
                            ? symTab[want]
                            : std::string("<sid=") + std::to_string(want) + ">";
                        // #678 — match TW's exact phrasing
                        // (libexpr/eval.cc:1680): `attribute '<name>'
                        // missing`.  Use EvalError so it groups with
                        // TW's MissingAttribute family (which derives
                        // from EvalError).
                        throw std::runtime_error(
                            "attribute '" + nm + "' missing");
                    }
                }
                // Install at the next eviction slot (round-robin).
                auto & evicted = ic.entries[ic.evictIdx];
                evicted.bindings = b;
                evicted.slot     = lo;
                ic.evictIdx = (ic.evictIdx + 1)
                    % CompilationUnit::AttrSelectIC::kWays;
                Value & slot = b->entries[lo].value;
                // 2026-05-17: iterative force + memoizing writeback
                // (IC install path).  Mirror of the IC HIT path above —
                // see comment there for rationale (incl. the 2026-06-11
                // under-applied-PAP writeback-poisoning guard).
                if (__builtin_expect(slot.isAppLike(), 0)
                    && !isUnderappliedClosurePap(slot)) {
                    push(vm, slot);
                    CallFrame & f = vm.frames.back();
                    armKeepBeltCheck(f);  // C-1 belt
                    f.forceWriteTarget = &slot;
                    f.flags |= CFF_FORCE_WB_PTR_KEEP | CFF_FORCE_RETRY;
                    if (__builtin_expect(g_sharedWbDetect, 0))
                        armedWritebackValue()[f.forceWriteTarget] = *f.forceWriteTarget;  // WS-A step 3 provenance
                    f.ip = ip;
                    goto op_force_slow;
                }
                push(vm, slot);
                // Phase A4: chase Tag::Slot / Tag::Thunk(Evaluated) so
                // we catch results that *resolve* to {family} after
                // chasing.
                {
                    static const char * s_dbgSelR_chase =
                        std::getenv("V3_DBG_SELECT_RESULT");
                    if (__builtin_expect(s_dbgSelR_chase != nullptr, 0)) {
                        Value chase = vm.valueStack.back();
                        int hops = 0;
                        while (hops < 8) {
                            if (chase.tag() == Tag::Slot && chase.asSlot()) {
                                chase = *chase.asSlot();
                            } else if (chase.tag() == Tag::Thunk
                                       && chase.asThunk()
                                       && chase.asThunk()->state == ThunkState::Evaluated) {
                                chase = chase.asThunk()->evaluated;
                            } else break;
                            ++hops;
                        }
                        if (chase.tag() == Tag::Attrs && chase.asAttrs()
                            && chase.asAttrs()->size == 1) {
                            const auto & st = ir::globalSymbolTable();
                            SymbolId rnm = chase.asAttrs()->entries[0].name;
                            const char * rnmStr = rnm < st.size() ? st[rnm].c_str() : "?";
                            if (std::strcmp(rnmStr, s_dbgSelR_chase) == 0) {
                                SymbolId selSym = static_cast<SymbolId>(operand);
                                const char * selStr = selSym < st.size() ? st[selSym].c_str() : "?";
                                std::fprintf(stderr,
                                    "v3 SELECT(slow) returned chase-to {%s} (hops=%d) "
                                    "selecting '%s' from bindings ptr=%p size=%u; pushed-tag=%u\n",
                                    rnmStr, hops, selStr, (const void *)b,
                                    (unsigned)b->size,
                                    (unsigned)vm.valueStack.back().tag());
                                if (const BindingsOrigin * o =
                                        lookupBindingsOrigin(b)) {
                                    const PosSnapshot * ps =
                                        resolvePosSnapshot(o->posHandle);
                                    std::fprintf(stderr,
                                        "  source-origin=%s@%s:%u\n",
                                        o->source ? o->source : "?",
                                        (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                        ps ? ps->line : 0u);
                                }
                                if (const BindingsOrigin * o2 =
                                        lookupBindingsOrigin(chase.asAttrs())) {
                                    const PosSnapshot * ps =
                                        resolvePosSnapshot(o2->posHandle);
                                    std::fprintf(stderr,
                                        "  chased-origin=%s@%s:%u\n",
                                        o2->source ? o2->source : "?",
                                        (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                        ps ? ps->line : 0u);
                                }
                                if (!vm.frames.empty()) {
                                    const auto & fr = vm.frames.back();
                                    const LambdaDescriptor * d = nullptr;
                                    if (fr.thunk
                                        && (fr.thunk->state == ThunkState::Suspended
                                            || fr.thunk->state == ThunkState::Blackhole))
                                        d = fr.thunk->suspended.desc;
                                    else if (fr.closure) d = fr.closure->desc;
                                    const PosSnapshot * fps =
                                        d ? resolvePosSnapshot(d->posHandle) : nullptr;
                                    std::fprintf(stderr,
                                        "  reading-frame: name=%s pos=%s:%u:%u codeOff=%u\n",
                                        d && !d->name.empty() ? d->name.c_str() : "<?>",
                                        (fps && !fps->file.empty()) ? fps->file.c_str() : "<?>",
                                        fps ? fps->line : 0u, fps ? fps->column : 0u,
                                        d ? d->codeOffset : 0u);
                                }
                            }
                        }
                    }
                }
                // Phase A3 (original): when the SELECT immediate result
                // is a size-1 attrs matching V3_DBG_SELECT_RESULT, dump
                // it.  Kept for back-compat; the chase-version above
                // is more permissive.
                {
                    static const char * s_dbgSelR =
                        std::getenv("V3_DBG_SELECT_RESULT");
                    if (__builtin_expect(s_dbgSelR != nullptr, 0)) {
                        const Value & top_val = vm.valueStack.back();
                        if (top_val.isAttrs() && top_val.asAttrs()
                            && top_val.asAttrs()->size == 1) {
                            const auto & st = ir::globalSymbolTable();
                            SymbolId rnm = top_val.asAttrs()->entries[0].name;
                            const char * rnmStr = rnm < st.size() ? st[rnm].c_str() : "?";
                            if (std::strcmp(rnmStr, s_dbgSelR) == 0) {
                                SymbolId selSym = static_cast<SymbolId>(operand);
                                const char * selStr = selSym < st.size() ? st[selSym].c_str() : "?";
                                std::fprintf(stderr,
                                    "v3 SELECT returned {%s} (size=1) when selecting '%s' "
                                    "from bindings ptr=%p size=%u: {",
                                    rnmStr, selStr, (const void *)b, (unsigned)b->size);
                                for (uint32_t k = 0; k < b->size && k < 16; ++k) {
                                    SymbolId nm = b->entries[k].name;
                                    std::fprintf(stderr, "%s%s",
                                        k ? "," : "",
                                        nm < st.size() ? st[nm].c_str() : "?");
                                }
                                std::fprintf(stderr, "}");
                                if (const BindingsOrigin * o =
                                        lookupBindingsOrigin(b)) {
                                    const PosSnapshot * ps =
                                        resolvePosSnapshot(o->posHandle);
                                    std::fprintf(stderr,
                                        " bindings-origin=%s@%s:%u",
                                        o->source ? o->source : "?",
                                        (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                        ps ? ps->line : 0u);
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                    }
                }
            }
            // Phase A5 (RCA 2026-05-11): frame-focused unconditional SELECT
            // trace.  When V3_DBG_SELECT_AT_CODEOFF=<codeoff> is set, dump
            // EVERY OP_ATTRS_SELECT executed in any frame whose thunk's
            // descriptor has the matching codeOffset.  This is the
            // narrowest possible scope — only fires inside the specific
            // body being investigated.  Logs source bindings shape, the
            // result on top of stack (with chase), and the SELECT operand
            // name.  Used to localise the cpuName thunk's SELECT that
            // produces {family}.
            {
                static const char * s_atCo =
                    std::getenv("V3_DBG_SELECT_AT_CODEOFF");
                if (__builtin_expect(s_atCo != nullptr, 0)) {
                    uint32_t targetCo = static_cast<uint32_t>(std::atoi(s_atCo));
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure)
                        d = fr.closure->desc;
                    if (d && d->codeOffset == targetCo) {
                        const auto & st = ir::globalSymbolTable();
                        SymbolId selSym = static_cast<SymbolId>(operand);
                        const char * selStr =
                            selSym < st.size() ? st[selSym].c_str() : "?";
                        const Value & top = vm.valueStack.back();
                        std::fprintf(stderr,
                            "v3 SELECT@codeOff[%u] sym='%s' ip=%u "
                            "src-bindings=%p src-size=%u src-keys={",
                            targetCo, selStr, ip - 2,
                            (const void *)b, (unsigned)b->size);
                        for (uint32_t k = 0; k < b->size && k < 16; ++k) {
                            SymbolId nm = b->entries[k].name;
                            std::fprintf(stderr, "%s%s",
                                k ? "," : "",
                                nm < st.size() ? st[nm].c_str() : "?");
                        }
                        std::fprintf(stderr, "} pushed-tag=%u",
                            (unsigned)top.tag());
                        // Walk chase for the pushed value to find the
                        // ultimate WHNF.
                        Value chase = top;
                        int hops = 0;
                        while (hops < 16) {
                            if (chase.tag() == Tag::Slot && chase.asSlot())
                                chase = *chase.asSlot();
                            else if (chase.tag() == Tag::Thunk
                                     && chase.asThunk()
                                     && chase.asThunk()->state == ThunkState::Evaluated)
                                chase = chase.asThunk()->evaluated;
                            else break;
                            ++hops;
                        }
                        std::fprintf(stderr,
                            " chase-tag=%u (hops=%d)",
                            (unsigned)chase.tag(), hops);
                        if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                            std::fprintf(stderr, " chase-attrs-size=%u chase-keys={",
                                chase.asAttrs()->size);
                            uint32_t sz = chase.asAttrs()->size;
                            for (uint32_t k = 0; k < sz && k < 16; ++k) {
                                SymbolId nm =
                                    chase.asAttrs()->entries[k].name;
                                std::fprintf(stderr, "%s%s",
                                    k ? "," : "",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            std::fprintf(stderr, "}");
                            if (const BindingsOrigin * o2 =
                                    lookupBindingsOrigin(chase.asAttrs())) {
                                const PosSnapshot * ps =
                                    resolvePosSnapshot(o2->posHandle);
                                std::fprintf(stderr,
                                    " chase-attrs-origin=%s@%s:%u",
                                    o2->source ? o2->source : "?",
                                    (ps && !ps->file.empty())
                                        ? ps->file.c_str() : "?",
                                    ps ? ps->line : 0u);
                            }
                        } else if (chase.tag() == Tag::String
                                   && chase.asString()) {
                            std::fprintf(stderr,
                                " chase-str=\"%.40s\"", chase.asString());
                        } else if (chase.tag() == Tag::Thunk
                                   && chase.asThunk()) {
                            std::fprintf(stderr,
                                " chase-thunk=%p state=%d",
                                (void *)chase.asThunk(),
                                (int)chase.asThunk()->state);
                        }
                        if (const BindingsOrigin * o =
                                lookupBindingsOrigin(b)) {
                            const PosSnapshot * ps =
                                resolvePosSnapshot(o->posHandle);
                            std::fprintf(stderr,
                                " src-origin=%s@%s:%u",
                                o->source ? o->source : "?",
                                (ps && !ps->file.empty())
                                    ? ps->file.c_str() : "?",
                                ps ? ps->line : 0u);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            break;
        }
        case OP_ATTRS_SELECT_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            // Force lazy `name` too — attrs.${dynKey} where dynKey is
            // `formal.cpu` (now lazy via mapAttrs Tag::App entries) was
            // landing in OP_ATTRS_SELECT_DYN with name still in App form
            // and tripping `not a string`.
            if (name.isAppLike() || name.tag() == Tag::Thunk || name.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            if (attrs.isAppLike() || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!name.isString() || !attrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_SELECT_DYN: type error");
            // #685 — dynamic-attr-name forceStringNoCtx mirror.
            requireNoStringContextRuntime(name, "OP_ATTRS_SELECT_DYN");
            // Intern via the global table so the SymbolId matches the
            // ones the attrset's bindings were built with.
            SymbolId id = ir::globalInternSymbol(name.asString());
            Bindings * dynB = attrs.asAttrs();
            // WS-A step 3 — chain-SELECT L1 for the dynamic-name path (mirror
            // of OP_ATTRS_SELECT): walk the chain layers (no materialize) when
            // NIX_V3_CHAIN_LOOKUP_SELECT, tracking whether the hit is in the
            // LEAF overlay.  `dynLeafSafe` gates the memoizing writeback below:
            // a parent-layer hit pushes without arming (no shared-parent write).
            Value * found = nullptr;
            bool dynLeafSafe = true;
            if (__builtin_expect(dynB->isChain(), 0)) {
                if (g_chainLookupSelect) {
                    for (const Bindings * L = dynB; L; L = L->parent) {
                        if (const Value * v = L->lookupLocal(id)) {
                            found = const_cast<Value *>(v);
                            dynLeafSafe = (L == dynB);
                            break;
                        }
                    }
                } else {
                    dynB = const_cast<Bindings *>(dynB->materialize());
                    found = dynB->lookup(id);
                }
            } else {
                found = dynB->lookup(id);
            }
            if (!found)
                // #678 — match TW phrasing
                // (libexpr/eval.cc:1680): `attribute '<name>'
                // missing`.  Use EvalError so it groups with TW's
                // family.
                throw std::runtime_error(
                    "attribute '" + std::string(name.asString())
                    + "' missing");
            // Phase 13.3 mapAttrs memo (dynamic-name path).  2026-05-17:
            // mirror OP_ATTRS_SELECT_IC's iterative force + memoizing
            // writeback (CFF_FORCE_WB_PTR_KEEP) — see the comment at
            // OP_ATTRS_SELECT_IC's site (vm.cc:6075-ish) for protocol
            // details.  Was C-recursive forceValue here; now iterative
            // via op_force_slow + slot writeback.
            // 2026-06-11: skip the memoizing writeback for an under-applied
            // closure-PAP — it is already WHNF; forcing+memoizing it saturates
            // it to its result type and poisons the (shared) entry (the python3
            // passthru.pythonAtLeast / firefox optionalString PAP->result bug).
            // Mirrors OP_FORCE (vm.cc:7161) + the OP_ATTRS_SELECT sites above.
            if (__builtin_expect(found->isAppLike(), 0)
                && !isUnderappliedClosurePap(*found)
                && dynLeafSafe) {   // WS-A step 3: no writeback into a shared parent
                push(vm, *found);
                CallFrame & f = vm.frames.back();
                armKeepBeltCheck(f);  // C-1 belt
                f.forceWriteTarget = found;
                f.flags |= CFF_FORCE_WB_PTR_KEEP | CFF_FORCE_RETRY;
                if (__builtin_expect(g_sharedWbDetect, 0))
                    armedWritebackValue()[f.forceWriteTarget] = *f.forceWriteTarget;  // WS-A step 3 provenance
                f.ip = ip;
                goto op_force_slow;
            }
            push(vm, *found);
            break;
        }
        case OP_ATTRS_HAS: {
            Value attrs = pop(vm);
            // (#458 A.4 Bridge attr-has peek retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
            if (attrs.isAppLike() || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            bool hasIt = (attrs.isAttrs() && attrs.asAttrs()->has(operand));
            // #558 (2026-05-12): V3_DBG_ATTRS_HAS_KEY filter — trace
            // every OP_ATTRS_HAS that matches a target SymbolId.  Used
            // to verify the hypothesis that v3's partial-Bindings
            // recovery makes `pkg.passthru.isFromBootstrapFiles or
            // false` flip vs. TW.  Set to the SymbolId or name to
            // filter (we just match on the name string via the global
            // symbol table).
            {
                static const char * s_dbgKey =
                    std::getenv("V3_DBG_ATTRS_HAS_KEY");
                if (__builtin_expect(s_dbgKey != nullptr, 0)) [[unlikely]] {
                    const auto & st = ir::globalSymbolTable();
                    SymbolId sid = static_cast<SymbolId>(operand);
                    const char * nm = (sid < st.size()) ? st[sid].c_str() : "?";
                    if (std::strcmp(nm, s_dbgKey) == 0) {
                        Bindings * b = attrs.isAttrs()
                            ? attrs.asAttrs() : nullptr;
                        // Caller frame pos for context.
                        const LambdaDescriptor * dC = nullptr;
                        if (!vm.frames.empty()) {
                            const auto & cfr = vm.frames.back();
                            if (cfr.thunk
                                && (cfr.thunk->state == ThunkState::Suspended
                                    || cfr.thunk->state == ThunkState::Blackhole))
                                dC = cfr.thunk->suspended.desc;
                            else if (cfr.closure) dC = cfr.closure->desc;
                        }
                        const PosSnapshot * psC =
                            dC ? resolvePosSnapshot(dC->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_HAS '%s' result=%s bindings=%p size=%u "
                            "caller='%s' pos=%s:%u:%u\n",
                            nm, hasIt ? "TRUE" : "FALSE",
                            (void *)b, b ? b->size : 0,
                            dC && !dC->name.empty() ? dC->name.c_str() : "<?>",
                            (psC && !psC->file.empty()) ? psC->file.c_str() : "<no-pos>",
                            psC ? psC->line : 0u,
                            psC ? psC->column : 0u);
                    }
                }
            }
            push(vm, hasIt ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_HAS_DYN: {
            Value name = pop(vm), attrs = pop(vm);
            if (name.isAppLike() || name.tag() == Tag::Thunk || name.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                name = forceValue(vm, name);
            }
            // (#458 A.4 dyn Bridge attr-has peek retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
            if (attrs.isAppLike() || attrs.tag() == Tag::Thunk || attrs.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                attrs = forceValue(vm, attrs);
            }
            if (!name.isString() || !attrs.isAttrs()) { push(vm, Value::vFalse); break; }
            // #685 — dynamic-attr-name forceStringNoCtx mirror.  Note:
            // TW also throws here when the name has context (the
            // `?` operator forces its dynamic name via the same
            // evalDynamicAttrs path).  We throw rather than return
            // false to match TW.
            requireNoStringContextRuntime(name, "OP_ATTRS_HAS_DYN");
            SymbolId id = ir::globalInternSymbol(name.asString());
            push(vm, attrs.asAttrs()->has(id)
                ? Value::vTrue : Value::vFalse);
            break;
        }
        case OP_ATTRS_UPDATE: {
            // A8: iterative writeback-force for the two args.  Stack:
            // [..., lhs, rhs] (rhs on top).
            {
                size_t topIdx = vm.valueStack.size() - 1;
                Value & rhsRef = vm.valueStack[topIdx];
                Value & lhsRef = vm.valueStack[topIdx - 1];
                if (needsForce(rhsRef)) {  // C-8/9/10
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
                if (needsForce(lhsRef)) {  // C-8/9/10
                    uint32_t off = static_cast<uint32_t>((topIdx - 1) - stackBase);
                    if (__builtin_expect(off > 0xFFFFu, 0))
                        throw std::runtime_error(
                            "v3 OP_ATTRS_UPDATE: writeback slot offset too large");
                    push(vm, lhsRef);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(off));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;
                    goto op_force_slow;
                }
            }
            Value rhs = pop(vm), lhs = pop(vm);
            // #558 Phase 3.3: Tag::Thunk Blackhole collapse retired.
            if (!lhs.isAttrs() || !rhs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_UPDATE: not attrsets");
            Bindings * out = mergeBindings(lhs.asAttrs(), rhs.asAttrs(),
                                           MergeBindingsSite::AttrsUpdate);
            V3_STATS_INC(attrsetsAllocated);
            Value v;
            v.mkAttrs(out);

            push(vm, v);
            break;
        }
        case OP_ATTRS_UPDATE_TAIL: {
            // #558 (2026-05-10): tail-return // operation.  Same as
            // OP_ATTRS_UPDATE but additionally publishes the merged
            // Bindings to all THUNK_RETURN frames via
            // publishToAllThunkFrames.  STG analog of "constructor
            // allocation reaches WHNF" — when a function's tail
            // expression is `lhs // rhs`, the merged Bindings IS the
            // function's WHNF (and transitively, every tail-call
            // ancestor's).  The most-correct partial-WHNF approximation
            // for nested fix-points (lib.fix's `let x = f x; in x`
            // with f producing a // chain in tail position).
            // A8: iterative writeback-force for the two args.  Stack:
            // [..., lhs, rhs] (rhs on top).
            {
                size_t topIdx = vm.valueStack.size() - 1;
                Value & rhsRef = vm.valueStack[topIdx];
                Value & lhsRef = vm.valueStack[topIdx - 1];
                if (needsForce(rhsRef)) {  // C-8/9/10
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
                if (needsForce(lhsRef)) {  // C-8/9/10
                    uint32_t off = static_cast<uint32_t>((topIdx - 1) - stackBase);
                    if (__builtin_expect(off > 0xFFFFu, 0))
                        throw std::runtime_error(
                            "v3 OP_ATTRS_UPDATE_TAIL: writeback slot offset too large");
                    push(vm, lhsRef);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(off));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;
                    goto op_force_slow;
                }
            }
            Value rhs = pop(vm), lhs = pop(vm);
            // #558: if forceValue deferred (returned Tag::Thunk Black
            // with chain), collapse to chain.back() for the merge.
            // // semantics need a Bindings; the chain peek approach
            // doesn't apply to // operands directly.  Approximation:
            // #558 Phase 3.3: Tag::Thunk Blackhole collapse retired.
            if (!lhs.isAttrs() || !rhs.isAttrs()) {
                static const bool s_dbg =
                    std::getenv("V3_DBG_UPDATE_FAIL") != nullptr;
                if (s_dbg) {
                    std::fprintf(stderr,
                        "v3 OP_ATTRS_UPDATE_TAIL: not attrsets lhs.tag=%d rhs.tag=%d\n",
                        (int)lhs.tag(), (int)rhs.tag());
                    if (lhs.isThunk() && lhs.asThunk())
                        std::fprintf(stderr,
                            "  lhs thunk=%p state=%d\n",
                            (void *)lhs.asThunk(),
                            (int)lhs.asThunk()->state);
                    if (rhs.isThunk() && rhs.asThunk())
                        std::fprintf(stderr,
                            "  rhs thunk=%p state=%d\n",
                            (void *)rhs.asThunk(),
                            (int)rhs.asThunk()->state);
                }
                throw std::runtime_error("v3 OP_ATTRS_UPDATE_TAIL: not attrsets");
            }
            Bindings * out = mergeBindings(lhs.asAttrs(), rhs.asAttrs(),
                                           MergeBindingsSite::AttrsUpdateTail);
            V3_STATS_INC(attrsetsAllocated);
            Value v;
            v.mkAttrs(out);

            // M-8 (CODEBASE_REVIEW_2026-06-11): #558 shapeCell publish removed
            // (default-off experiment; field gone — see closure.hh).
            push(vm, v);
            break;
        }

        // --- With ---
        case OP_WITH_PUSH: {
            Value v = pop(vm);
            // #498 diagnostic: trace OP_WITH_PUSH that pushes a 1-attr
            // attrset whose only attr is "prev" — the bisect symptom
            // that surfaces under broader thunkify.  Logs the pushing
            // frame, ip, and chase through Tag::Slot/Tag::Thunk.
            static const bool s_dbgWithPushPrev =
                std::getenv("V3_DBG_WITH_PUSH_PREV") != nullptr;
            if (__builtin_expect(s_dbgWithPushPrev, 0)) {
                Value chase = v;
                int hops = 0;
                while (hops < 4) {
                    if (chase.tag() == Tag::Slot && chase.asSlot()) {
                        chase = *chase.asSlot();
                    } else if (chase.tag() == Tag::Thunk
                               && chase.asThunk()
                               && chase.asThunk()->state == ThunkState::Evaluated) {
                        chase = chase.asThunk()->evaluated;
                    } else break;
                    ++hops;
                }
                if (chase.tag() == Tag::Attrs && chase.asAttrs()
                    && chase.asAttrs()->size == 1) {
                    SymbolId nm = chase.asAttrs()->entries[0].name;
                    const auto & st = ir::globalSymbolTable();
                    std::string s = nm < st.size() ? st[nm] : "<?>";
                    if (s == "prev") {
                        std::fprintf(stderr,
                            "v3 OP_WITH_PUSH {prev}: cu=%p ip=%u frames=%zu\n",
                            (void *)cu, ip - 1, vm.frames.size());
                        // Dump bytecode window around the push.
                        if (cu) {
                            uint32_t lo = (ip > 16) ? ip - 16 : 0;
                            uint32_t hi = ip + 8;
                            std::fprintf(stderr,
                                "  pushing-frame disasm [%u..%u):\n",
                                lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Find the LambdaDescriptor whose codeOffset
                            // is closest BELOW the failing IP -- the
                            // function whose body contains this push.
                            uint32_t target = ip - 1;
                            uint32_t bestIdx = ~0u;
                            uint32_t bestOff = 0;
                            for (uint32_t li = 0; li < cu->lambdas.size(); ++li) {
                                uint32_t lo2 = cu->lambdas[li].codeOffset;
                                if (lo2 <= target && lo2 > bestOff) {
                                    bestOff = lo2;
                                    bestIdx = li;
                                }
                            }
                            if (bestIdx != ~0u) {
                                const auto & ld = cu->lambdas[bestIdx];
                                std::fprintf(stderr,
                                    "  containing lambdas[%u]: "
                                    "codeOffset=%u nUp=%u nLocals=%u name=%s\n",
                                    bestIdx, ld.codeOffset,
                                    ld.nUpvalues, ld.nLocals,
                                    ld.name.empty() ? "<anon>"
                                                    : ld.name.c_str());
                                // Dump full body of that lambda
                                uint32_t bodyLo = ld.codeOffset;
                                uint32_t bodyHi = ip + 8;
                                std::fprintf(stderr,
                                    "  containing lambda body [%u..%u):\n",
                                    bodyLo, bodyHi);
                                disassembleWindow(stderr, *cu, bodyLo, bodyHi);
                            }
                        }
                        for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                            const auto & fr = vm.frames[fi - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s ip=%u flags=%u\n",
                                fi - 1,
                                d && !d->name.empty() ? d->name.c_str()
                                    : (d ? "<anon>" : "<root>"),
                                fr.ip, (unsigned)fr.flags);
                        }
                        std::fflush(stderr);
                    }
                }
            }
            vm.withStack.push_back(v);
            break;
        }
        case OP_WITH_POP:  vm.withStack.pop_back(); break;
        case OP_REC_SLOT_PUBLISH: {
            // #458 step 1/6 — heap-stable rec-attrset slot publish.
            //
            // Peek the rec-attrset Tag::Attrs at top of stack (built
            // by the OP_ATTRS_REC_INIT immediately preceding), allocate
            // a fresh GC-managed Value*, copy the Tag::Attrs INTO that
            // slot, and push a Tag::Slot pointing at it ON TOP.  The
            // slot is heap-stable for the lifetime of any closure
            // capturing the Tag::Slot via its freeVars vector (Boehm
            // GC handles reachability automatically).
            //
            // Why peek-and-copy instead of move: the rec-attrset Value
            // payload is a Bindings* — copying the Value is cheap and
            // the Bindings is already heap-allocated, so OP_ATTRS_REC_SET
            // mutations to its entries[] are visible through both the
            // original Tag::Attrs (still on the stack, used by the rest
            // of the LetRec emit) and the slot's Tag::Attrs (captured
            // by inner closures).
            if (vm.valueStack.empty()) {
                throw std::runtime_error(
                    "v3 OP_REC_SLOT_PUBLISH: empty operand stack");
            }
            const Value & top = vm.valueStack.back();
            if (!top.isAttrs()) {
                throw std::runtime_error(
                    "v3 OP_REC_SLOT_PUBLISH: top of stack is not Tag::Attrs");
            }
            Value * heapSlot = Alloc::allocValue();
            cellWrite(heapSlot, top, nullptr);  // PhD-6: barrier raw fresh-cell init
            Value slotRef;
            slotRef.mkSlot(heapSlot);
            push(vm, slotRef);
            break;
        }
        case OP_THUNK_SET_LOCAL_THROUGH_CELL: {
            // STG-14b (#516/#517): pop a Tag::Thunk, allocate a heap
            // cell holding it, attach the cell as the thunk's
            // OP_RETURN-update target, and write a Tag::Slot{cell}
            // into the local at [slot:24].
            //
            // Used by emit.cc:594-601 for hidden-from-expr thunks
            // (the `inherit (X // Y) ...` lowering's inheritFromExpr
            // thunks).  Before this opcode, hidden thunks were stored
            // as Tag::Thunk in their slots; per-attr thunks captured
            // them via emitVarRef, holding a stale Black thunk pointer
            // when the hidden was forced mid-construction (the
            // STG_KEEP_HOOKS hang on `(import <nixpkgs> {}).lib`).
            //
            // After this opcode, the slot holds Tag::Slot{cell}; the
            // cell initially contains the Tag::Thunk and -- on the
            // hidden thunk's OP_RETURN -- gets `*cell = retVal`
            // applied (vm.cc:3003 cell-update path).  Captures
            // observing the slot deref through the cell to the
            // Evaluated value.
            if (vm.valueStack.empty()) {
                throw std::runtime_error(
                    "v3 OP_THUNK_SET_LOCAL_THROUGH_CELL: empty operand stack");
            }
            Value top = vm.valueStack.back();
            vm.valueStack.pop_back();
            if (top.tag() != Tag::Thunk || !top.asThunk()) {
                throw std::runtime_error(
                    "v3 OP_THUNK_SET_LOCAL_THROUGH_CELL: top of stack is not Tag::Thunk");
            }
            // Heap-stable cell: holds the thunk Value initially; the
            // thunk's OP_RETURN cell-update will overwrite it with
            // the evaluated value.
            Value * cell = Alloc::allocValue();
            cellWrite(cell, top, nullptr);  // PhD-6: barrier raw fresh-cell init
            // Attach cell to the thunk so OP_RETURN's CFF_THUNK_RETURN
            // handler at vm.cc:3003 fires `*cell = retVal`.  Only
            // attach if the thunk is fresh (Suspended with no cell
            // yet); otherwise we'd clobber an existing cell binding
            // (e.g., from OP_ATTRS_REC_SET).  Fresh OP_MAKE_THUNK
            // produces Suspended with cell == nullptr by construction
            // (alloc.hh:317-348), so this is the expected branch.
            if (top.asThunk()->state == ThunkState::Suspended
                && top.asThunk()->cell == nullptr) {
                top.asThunk()->cell = cell;
                cellOwnRecordSet(cell, top.asThunk(),
                                  "OP_THUNK_SET_LOCAL_THROUGH_CELL");
            }
            // Slot pointing at cell -- captures see the slot, deref
            // resolves through cell to the (eventually Evaluated) value.
            Value slotRef;
            slotRef.mkSlot(cell);
            uint16_t slot = static_cast<uint16_t>(operand);
            if (stackBase + slot >= vm.valueStack.size()) {
                vm.valueStack.resize(stackBase + slot + 1);
            }
            vm.valueStack[stackBase + slot] = slotRef;
            break;
        }
        case OP_REC_BINDING_SLOT_REF: {
            // Pop a Tag::Attrs (forced earlier), look up the entry by
            // SymbolId in operand, push a Tag::Slot Value pointing at
            // `&entries[i].value`.  Heap-stable because Bindings live
            // on the v3 heap (Alloc::allocBindings), not on the
            // value-stack.
            // A8: iterative force at case entry.
            {
                Value & topRef = vm.valueStack.back();
                if (topRef.isAppLike()
                    || topRef.tag() == Tag::Thunk
                    || topRef.tag() == Tag::Slot)
                {
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
            }
            Value attrs = pop(vm);
            // #437: count and tag-distribute OP_REC_BINDING_SLOT_REF fires.
            {
                static const bool s_dbg_p5 =
                    std::getenv("V3_DBG_P5") != nullptr;
                if (__builtin_expect(s_dbg_p5, 0)) [[unlikely]] {
                    static thread_local uint64_t total = 0;
                    static thread_local uint64_t byTag[16] = {0};
                    static thread_local uint64_t bridgeFires = 0;
                    static thread_local uint64_t logged = 0;
                    ++total;
                    Tag at = attrs.tag();
                    if ((unsigned)at < 16) byTag[(unsigned)at]++;
                    bool isBridge = false;  // (TW_VALUE_ERADICATION F4, 2026-06-02)
                    if (isBridge) ++bridgeFires;
                    // Print first 5 cases & every power of 10 thereafter.
                    if (logged < 5
                        || (total > 10 && (total & (total - 1)) == 0)) {
                        const auto & tbl = ir::globalSymbolTable();
                        SymbolId sym = static_cast<SymbolId>(operand);
                        std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                        std::fprintf(stderr,
                            "v3 P5#%llu: tag=%d sym='%s' frames=%zu bridges=%llu byTag=[",
                            (unsigned long long)total, (int)at, nm.c_str(),
                            vm.frames.size(),
                            (unsigned long long)bridgeFires);
                        for (int i = 0; i < 16; ++i)
                            if (byTag[i]) std::fprintf(stderr, "%d:%llu,", i,
                                (unsigned long long)byTag[i]);
                        std::fprintf(stderr, "]\n");
                        ++logged;
                    }
                }
            }
            // A8: force handled at case entry — attrs is WHNF here.
            if (!attrs.isAttrs() || !attrs.asAttrs()) {
                throw std::runtime_error(
                    "v3 OP_REC_BINDING_SLOT_REF: source is not a forced attrset");
            }
            SymbolId sym = static_cast<SymbolId>(operand);
            // #779 Schema 10: per-call-site IC.  The icIdx follow-up
            // word indexes cu->recSlotCache.  Bindings* identity is
            // sufficient (entries[] is allocated as a FAM alongside
            // Bindings; the pointer doesn't move).  Mutations come
            // via OP_APPLY_OVERRIDES which produces a NEW Bindings*,
            // so cache entries can't dangle to stale entries[].
            uint32_t icIdx = cu->code[ip++];
            Bindings * b = attrs.asAttrs();
            Value * found = nullptr;
            {
                auto & ic = cu->recSlotCache[icIdx];
                if (__builtin_expect(ic.bindings == b
                        && ic.slot < b->size
                        && b->entries[ic.slot].name == sym, 1)) {
                    // IC hit — direct entry access.  C-3
                    // (CODEBASE_REVIEW_2026-06-11): validate the cached slot's
                    // NAME (and bounds), not just Bindings* identity.  Under
                    // default-ON major GC a freed+reused Bindings address can
                    // alias the cached pointer with a different name layout,
                    // silently returning a wrong-named slot (the address-reuse
                    // hazard the safepoint IC-invalidation defends; this is
                    // belt-and-suspenders, matching OP_ATTRS_SELECT's IC which
                    // already name-validates).  On mismatch → binary search.
                    found = &b->entries[ic.slot].value;
                } else {
                    // IC miss (cold, shape change, or stale alias).  Binary
                    // search by SymbolId then install.
                    uint32_t lo = 0, hi = b->size;
                    uint32_t slotIdx = 0;
                    while (lo < hi) {
                        uint32_t mid = (lo + hi) >> 1;
                        SymbolId midName = b->entries[mid].name;
                        if (midName == sym) {
                            found = &b->entries[mid].value;
                            slotIdx = mid;
                            break;
                        }
                        if (midName < sym) lo = mid + 1;
                        else               hi = mid;
                    }
                    if (found) {
                        ic.bindings = b;
                        ic.slot     = slotIdx;
                    }
                }
            }
            if (!found) {
                const auto & tbl = ir::globalSymbolTable();
                std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                throw std::runtime_error(
                    "v3 OP_REC_BINDING_SLOT_REF: name '" + nm
                    + "' not found in source attrset");
            }
            // #558 (2026-05-10) diagnostic: V3_DBG_SLOT_REF=1 shows
            // what slot was selected and its current value's tag.
            {
                static const bool s_dbgSlotRef =
                    std::getenv("V3_DBG_SLOT_REF") != nullptr;
                if (__builtin_expect(s_dbgSlotRef, 0)) {
                    const auto & tbl = ir::globalSymbolTable();
                    std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                    if (found->isThunk() && found->asThunk()) {
                        Thunk * t = found->asThunk();
                        const auto * d =
                            (t->state == ThunkState::Suspended
                             || t->state == ThunkState::Blackhole)
                            ? t->suspended.desc : nullptr;
                        const PosSnapshot * ps =
                            d ? resolvePosSnapshot(d->posHandle) : nullptr;
                        std::fprintf(stderr,
                            "v3 SLOT_REF: bindings=%p size=%u sym='%s' "
                            "slot-tag=Thunk thunk=%p state=%d desc-name='%s' pos=%s:%u:%u\n",
                            (void *)b, b->size, nm.c_str(),
                            (void *)t, (int)t->state,
                            d && !d->name.empty() ? d->name.c_str() : "<?>",
                            (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                            ps ? ps->line : 0u, ps ? ps->column : 0u);
                    } else {
                        std::fprintf(stderr,
                            "v3 SLOT_REF: bindings=%p size=%u sym='%s' slot-tag=%d\n",
                            (void *)b, b->size, nm.c_str(), (int)found->tag());
                    }
                }
            }
            // #437 diagnostic: track repeated slot derefs on the same
            // (bindings, sym) pair within a single eval.  Under
            // NIX_V3_INLINE_REC_SLOT, the inline path lacks the Thunk
            // identity that pins recursion via Blackhole; if cardano-
            // node hits this opcode N>>1 times for the same key, the
            // hypothesis from the agent analysis is confirmed.  Gated
            // on V3_DBG_INLINE_REC=1; thread-local state is fine since
            // the VM is single-threaded.  Reports when re-entry count
            // for a key crosses 4.
            {
                static const bool s_dbg_inline_rec =
                    std::getenv("V3_DBG_INLINE_REC") != nullptr;
                if (__builtin_expect(s_dbg_inline_rec, 0)) [[unlikely]] {
                    static thread_local std::unordered_map<
                        uint64_t, uint32_t> reentries;
                    uint64_t key = (reinterpret_cast<uint64_t>(b) << 24)
                        ^ static_cast<uint64_t>(sym);
                    auto & cnt = reentries[key];
                    ++cnt;
                    if (cnt > 4 && (cnt & (cnt - 1)) == 0) {
                        const auto & tbl = ir::globalSymbolTable();
                        std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                        std::fprintf(stderr,
                            "v3 inline-rec re-entry #%u: bindings=%p sym='%s' frames=%zu ip=%u\n",
                            cnt, (void *)b, nm.c_str(),
                            vm.frames.size(), ip - 1);
                    }
                }
            }
            Value v;
            v.mkSlot(found);
            push(vm, v);
            // Phase A5: frame-focused unconditional SLOT_REF trace.  When
            // V3_DBG_SELECT_AT_CODEOFF=<codeoff> is set, dump every
            // OP_REC_BINDING_SLOT_REF executed in any frame whose thunk's
            // descriptor has the matching codeOffset.  Logs the slot's
            // current contents (and chase-through-WHNF for indirect cases).
            {
                static const char * s_atCo =
                    std::getenv("V3_DBG_SELECT_AT_CODEOFF");
                if (__builtin_expect(s_atCo != nullptr, 0)) {
                    uint32_t targetCo = static_cast<uint32_t>(std::atoi(s_atCo));
                    const auto & fr = vm.frames.back();
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk
                        && (fr.thunk->state == ThunkState::Suspended
                            || fr.thunk->state == ThunkState::Blackhole))
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure)
                        d = fr.closure->desc;
                    if (d && d->codeOffset == targetCo) {
                        const auto & st = ir::globalSymbolTable();
                        std::string nm = (sym < st.size()) ? st[sym] : "?";
                        Tag slotTag = found->tag();
                        std::fprintf(stderr,
                            "v3 SLOT_REF@codeOff[%u] sym='%s' "
                            "src-bindings=%p src-size=%u slot=%p slot-tag=%u",
                            targetCo, nm.c_str(),
                            (void *)b, (unsigned)b->size,
                            (void *)found, (unsigned)slotTag);
                        // Chase slot contents to WHNF (read-only inspect).
                        Value cur = *found;
                        int hops = 0;
                        while (hops < 16) {
                            if (cur.tag() == Tag::Slot && cur.asSlot())
                                cur = *cur.asSlot();
                            else if (cur.tag() == Tag::Thunk
                                     && cur.asThunk()
                                     && cur.asThunk()->state == ThunkState::Evaluated)
                                cur = cur.asThunk()->evaluated;
                            else break;
                            ++hops;
                        }
                        std::fprintf(stderr,
                            " chase-tag=%u (hops=%d)",
                            (unsigned)cur.tag(), hops);
                        if (cur.tag() == Tag::Attrs && cur.asAttrs()) {
                            uint32_t sz = cur.asAttrs()->size;
                            std::fprintf(stderr, " size=%u keys={", sz);
                            for (uint32_t k = 0; k < sz && k < 16; ++k) {
                                SymbolId nn =
                                    cur.asAttrs()->entries[k].name;
                                std::fprintf(stderr, "%s%s",
                                    k ? "," : "",
                                    nn < st.size() ? st[nn].c_str() : "?");
                            }
                            std::fprintf(stderr, "}");
                            if (const BindingsOrigin * o2 =
                                    lookupBindingsOrigin(cur.asAttrs())) {
                                const PosSnapshot * ps =
                                    resolvePosSnapshot(o2->posHandle);
                                std::fprintf(stderr,
                                    " origin=%s@%s:%u",
                                    o2->source ? o2->source : "?",
                                    (ps && !ps->file.empty())
                                        ? ps->file.c_str() : "?",
                                    ps ? ps->line : 0u);
                            }
                        } else if (cur.tag() == Tag::String && cur.asString()) {
                            std::fprintf(stderr,
                                " str=\"%.40s\"", cur.asString());
                        } else if (cur.tag() == Tag::Thunk && cur.asThunk()) {
                            std::fprintf(stderr,
                                " thunk=%p state=%d",
                                (void *)cur.asThunk(),
                                (int)cur.asThunk()->state);
                            // Dump the thunk's body source position so
                            // we can tell whether the wrapped lambda is
                            // what we expect.  Important for catching
                            // upvalue mis-wires: e.g., the cpuName
                            // thunk's darwinArch slot pointing at the
                            // wrong closure body.
                            auto * tt = cur.asThunk();
                            const LambdaDescriptor * dd =
                                (tt->state == ThunkState::Suspended
                                 || tt->state == ThunkState::Blackhole)
                                ? tt->suspended.desc : nullptr;
                            const PosSnapshot * pps =
                                dd ? resolvePosSnapshot(dd->posHandle) : nullptr;
                            std::fprintf(stderr,
                                " thunk-body=%s@%s:%u:%u codeOff=%u",
                                dd && !dd->name.empty() ? dd->name.c_str() : "<?>",
                                (pps && !pps->file.empty()) ? pps->file.c_str() : "<no-pos>",
                                pps ? pps->line : 0u, pps ? pps->column : 0u,
                                dd ? dd->codeOffset : 0u);
                        } else if (cur.tag() == Tag::Closure
                                   && cur.asClosure()
                                   && cur.asClosure()->desc) {
                            auto * dd = cur.asClosure()->desc;
                            const PosSnapshot * pps =
                                resolvePosSnapshot(dd->posHandle);
                            std::fprintf(stderr,
                                " closure-ptr=%p desc=%p closure-body=%s@%s:%u:%u codeOff=%u nUp=%u",
                                (const void *)cur.asClosure(),
                                (const void *)dd,
                                !dd->name.empty() ? dd->name.c_str() : "<?>",
                                (pps && !pps->file.empty()) ? pps->file.c_str() : "<no-pos>",
                                pps ? pps->line : 0u, pps ? pps->column : 0u,
                                dd->codeOffset,
                                cur.asClosure()->nUpvalues);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            break;
        }
        case OP_GET_UPVALUE_REC_BINDING: {
            // §2(b) superinstruction (NEXT_STEPS_2026-06-05): the fused form
            // of `OP_GET_UPVALUE idx ; OP_REC_BINDING_SLOT_REF sym ; <icIdx>`.
            // Reads the captured rec-attrset upvalue directly — no
            // intermediate stack push/pop and one fewer dispatch — then runs
            // the identical recSlotCache IC lookup and pushes the Tag::Slot.
            if (!closure)
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING: no closure context");
            SymbolId sym      = static_cast<SymbolId>(operand);
            uint32_t upvalIdx = cu->code[ip++];
            uint32_t icIdx    = cu->code[ip++];
            if (upvalIdx >= closure->nUpvalues)
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING: upvalue index out of range");
            Value attrs = closureUpvalue(closure, upvalIdx);
            // Force to WHNF if the rec-attrset isn't materialised yet
            // (Slot/Thunk/App).  forceValue runs a nested eval, but `closure`
            // and `cu` are heap-stable across it and `attrs` is a C-stack
            // local (a conservative-GC root), so no pointer is invalidated.
            // Mirrors OP_CALL_N's inline force; for the steady recursion case
            // (fib's `fib` self-ref) attrs is already Tag::Attrs and this is
            // a no-op.
            if (attrs.isAppLike()
                || attrs.tag() == Tag::Thunk
                || attrs.tag() == Tag::Slot)
                attrs = forceValue(vm, attrs);
            if (!attrs.isAttrs() || !attrs.asAttrs())
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING: source is not a forced attrset");
            Bindings * b = attrs.asAttrs();
            Value * found = nullptr;
            {
                auto & ic = cu->recSlotCache[icIdx];
                // C-3: name + bounds check, not just Bindings* identity —
                // guards against a freed+reused address aliasing the IC under
                // default-ON GC.
                if (__builtin_expect(ic.bindings == b
                        && ic.slot < b->size
                        && b->entries[ic.slot].name == sym, 1)) {
                    found = &b->entries[ic.slot].value;
                } else {
                    uint32_t lo = 0, hi = b->size, slotIdx = 0;
                    while (lo < hi) {
                        uint32_t mid = (lo + hi) >> 1;
                        SymbolId midName = b->entries[mid].name;
                        if (midName == sym) {
                            found = &b->entries[mid].value;
                            slotIdx = mid;
                            break;
                        }
                        if (midName < sym) lo = mid + 1;
                        else               hi = mid;
                    }
                    if (found) { ic.bindings = b; ic.slot = slotIdx; }
                }
            }
            if (!found) {
                const auto & tbl = ir::globalSymbolTable();
                std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING: name '" + nm
                    + "' not found in source attrset");
            }
            Value v;
            v.mkSlot(found);
            push(vm, v);
            break;
        }
        case OP_GET_UPVALUE_REC_BINDING_SLOT: {
            // reg-VM Phase 5 (item 5a): identical resolution to
            // OP_GET_UPVALUE_REC_BINDING, but writes the resulting Tag::Slot
            // into regs[dst] instead of pushing — dropping the SET that
            // materialised the recursive-self callee for R_CALL.  See
            // bytecode.hh.  operand=sym; follow-ups=[dst, upvalIdx, icIdx].
            if (!closure)
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING_SLOT: no closure context");
            SymbolId sym      = static_cast<SymbolId>(operand);
            uint32_t dst      = cu->code[ip++];
            uint32_t upvalIdx = cu->code[ip++];
            uint32_t icIdx    = cu->code[ip++];
            if (upvalIdx >= closure->nUpvalues)
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING_SLOT: upvalue index out of range");
            Value attrs = closureUpvalue(closure, upvalIdx);
            if (attrs.isAppLike()
                || attrs.tag() == Tag::Thunk
                || attrs.tag() == Tag::Slot)
                attrs = forceValue(vm, attrs);
            if (!attrs.isAttrs() || !attrs.asAttrs())
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING_SLOT: source is not a forced attrset");
            Bindings * b = attrs.asAttrs();
            Value * found = nullptr;
            {
                auto & ic = cu->recSlotCache[icIdx];
                // C-3: name + bounds check, not just Bindings* identity —
                // guards against a freed+reused address aliasing the IC under
                // default-ON GC.
                if (__builtin_expect(ic.bindings == b
                        && ic.slot < b->size
                        && b->entries[ic.slot].name == sym, 1)) {
                    found = &b->entries[ic.slot].value;
                } else {
                    uint32_t lo = 0, hi = b->size, slotIdx = 0;
                    while (lo < hi) {
                        uint32_t mid = (lo + hi) >> 1;
                        SymbolId midName = b->entries[mid].name;
                        if (midName == sym) {
                            found = &b->entries[mid].value;
                            slotIdx = mid;
                            break;
                        }
                        if (midName < sym) lo = mid + 1;
                        else               hi = mid;
                    }
                    if (found) { ic.bindings = b; ic.slot = slotIdx; }
                }
            }
            if (!found) {
                const auto & tbl = ir::globalSymbolTable();
                std::string nm = (sym < tbl.size()) ? tbl[sym] : "?";
                throw std::runtime_error(
                    "v3 OP_GET_UPVALUE_REC_BINDING_SLOT: name '" + nm
                    + "' not found in source attrset");
            }
            vm.valueStack[stackBase + dst].mkSlot(found);
            break;
        }
        case OP_WITH_LOOKUP: {
            // Sync local ip into the top frame BEFORE withLookup may
            // throw — otherwise the cycle dump's frame[top].ip is
            // stale (still showing the value last written at
            // OP_FORCE / OP_CALL push time, which is often the body
            // start) and the disasm window misses the failing
            // OP_WITH_LOOKUP itself.  Cheap on the hot path: one
            // store per OP_WITH_LOOKUP, only adds a memory write
            // ahead of an opcode that already does an STL hashmap
            // lookup.
            vm.frames.back().ip = ip;
            push(vm, withLookup(vm, static_cast<SymbolId>(operand)));
            break;
        }

        // --- Strings / pos / assert ---
        case OP_STR_CONCAT: {
            // reg-VM Phase 5 entry: OP_R_STR_CONCAT2 sets `operand` to the
            // synthesised STR_CONCAT operand ((2<<1)|forceStr), pushes its two
            // (already-WHNF) slot operands, arms CFF_FORCE_WB=dst, and jumps
            // here — reusing this entire body.  At str_concat_done the armed
            // writeback drops the result into the dst slot; a normal STR_CONCAT
            // has no writeback armed, so the result stays on the stack.
            op_str_concat:
            uint32_t n = operand >> 1;
            bool forceStr = (operand & 1u) != 0;
            // Ultra-fast path: 2 ints with no forceStr — covers every
            // arithmetic `a + b` over ints, which is the dominant case
            // on compute-bound benchmarks like fib.  Skip the small[]
            // setup, the loop, and the per-part type checks.  Match
            // tree-walker by raising on overflow.
            if (!forceStr && n == 2) {
                Value & top1 = vm.valueStack.back();
                Value & top0 = vm.valueStack[vm.valueStack.size() - 2];
                if (top0.isInt() && top1.isInt()) {
                    int64_t sum;
                    // #687 — TW phrasing (libexpr/eval.cc:2515):
                    //   `integer overflow in adding <a> + <b>`
                    if (__builtin_add_overflow(top0.asInt(), top1.asInt(), &sum))
                        throw std::runtime_error(
                            "integer overflow in adding "
                            + std::to_string(top0.asInt()) + " + "
                            + std::to_string(top1.asInt()));
                    vm.valueStack.pop_back();
                    vm.valueStack.back().mkInt(sum);
                    // reg-VM: honour CFF_FORCE_WB inline — can't `goto
                    // str_concat_done` from here (it would jump over the
                    // `overflow` vector's initialiser below).
                    applyForceWriteback(vm);
                    break;
                }
            }
            // Hot path on every Nix-level `a + b` (which the parser
            // lowers to ConcatStrings).  Avoid allocating a heap
            // vector for the common 2-part case — most ConcatStrings
            // expressions are exactly two operands.
            constexpr uint32_t kSmall = 8;
            Value small[kSmall];
            std::vector<Value> overflow;
            Value * parts = small;
            if (n > kSmall) {
                overflow.resize(n);
                parts = overflow.data();
            }
            // A8: iterative writeback-force for all `n` parts.  Stack:
            // [..., part0, part1, ..., part(n-1)] (part(n-1) on top).
            // Scan in place WITHOUT popping; on first non-WHNF, set up
            // writeback for that slot and goto op_force_slow.  On
            // re-entry the opcode rescans.  Note: tag check ignores
            // Tag::Slot for backwards compat with the prior loop, but
            // Slot values should be rare here (they'd appear only if
            // lower emitted an unforced Slot into a string operand).
            {
                size_t argBase = vm.valueStack.size() - n;
                for (uint32_t k = 0; k < n; ++k) {
                    Value & p = vm.valueStack[argBase + k];
                    Tag t = p.tag();
                    // C-8/9/10: exclude under-applied closure-PAPs (WHNF) —
                    // they'd loop the handshake forever; let them reach the
                    // "cannot coerce a function" path. (Slot deliberately
                    // ignored here, per the comment above.)
                    if ((t == Tag::App || t == Tag::App3 || t == Tag::Thunk)
                        && !isUnderappliedClosurePap(p)) {
                        uint32_t off = static_cast<uint32_t>((argBase + k) - stackBase);
                        if (__builtin_expect(off > 0xFFFFu, 0))
                            throw std::runtime_error(
                                "v3 OP_STR_CONCAT: writeback slot offset too large");
                        push(vm, p);
                        CallFrame & frame = vm.frames.back();
                        setForceWriteback(frame, static_cast<uint16_t>(off));
                        frame.flags |= CFF_FORCE_RETRY;
                        ip = ip - 1;
                        goto op_force_slow;
                    }
                }
            }
            for (uint32_t i = n; i > 0; --i) parts[i - 1] = pop(vm);
            // V3_DBG_STRCONCAT: when a Closure leaks into STR_CONCAT
            // (which happens when v3's eval-order divergence forces a
            // function value where tree-walker keeps it lazy), dump
            // the call stack to localise the source.
            {
                static const bool s_dbg = std::getenv("V3_DBG_STRCONCAT") != nullptr;
                if (s_dbg) {
                    bool hasUncoercible = false;
                    for (uint32_t i = 0; i < n; ++i) {
                        Tag t = parts[i].tag();
                        if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp || t == Tag::List)
                            { hasUncoercible = true; break; }
                    }
                    if (hasUncoercible) {
                        std::fprintf(stderr,
                            "v3 OP_STR_CONCAT pre-trace tags=[");
                        for (uint32_t i = 0; i < n; ++i)
                            std::fprintf(stderr, "%s%u", i ? "," : "", (unsigned)parts[i].tag());
                        std::fprintf(stderr, "] forceStr=%d frames=%zu callerIp=%u\n",
                            forceStr ? 1 : 0, vm.frames.size(), ip - 1);
                        // Dump current frame's upvalues / closure context.
                        if (!vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const Closure * cl = fr.closure;
                            const Thunk * th = fr.thunk;
                            uint16_t nUp = 0;
                            if (cl) nUp = cl->nUpvalues;
                            else if (th) nUp = th->nUpvalues;
                            std::fprintf(stderr,
                                "  current-frame nUpvalues=%u\n", nUp);
                            for (uint16_t i = 0; i < nUp && i < 8; ++i) {
                                Value uv = cl ? closureUpvalue(cl, i)
                                    : (thunkUpvalEnv(th) ? thunkUpvalEnv(th)->values[i] : th->tail[i]);
                                std::fprintf(stderr,
                                    "    upvalue[%u] tag=%u",
                                    i, (unsigned)uv.tag());
                                if (uv.tag() == Tag::Closure
                                    && uv.asClosure()
                                    && uv.asClosure()->desc) {
                                    std::fprintf(stderr, " closure=%s nUp=%u",
                                        !uv.asClosure()->desc->name.empty()
                                            ? uv.asClosure()->desc->name.c_str()
                                            : "<anon>",
                                        uv.asClosure()->nUpvalues);
                                } else if (uv.tag() == Tag::Attrs
                                           && uv.asAttrs()) {
                                    auto * b2 = uv.asAttrs();
                                    std::fprintf(stderr, " attrs size=%u {",
                                        (unsigned)b2->size);
                                    const auto & st2 = ir::globalSymbolTable();
                                    for (uint32_t k = 0; k < b2->size && k < 30; ++k) {
                                        SymbolId nm = b2->entries[k].name;
                                        std::fprintf(stderr, "%s%s",
                                            k ? "," : "",
                                            nm < st2.size() ? st2[nm].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "}");
                                    // For each attrs upvalue, also dump
                                    // tag of `preHook` slot (the bug
                                    // chases this attribute specifically).
                                    static const SymbolId preHookSym2 =
                                        ir::globalInternSymbol("preHook");
                                    const Value * ph = b2->lookup(preHookSym2);
                                    if (ph) {
                                        Tag pht = ph->tag();
                                        std::fprintf(stderr,
                                            " preHook=tag%u", (unsigned)pht);
                                        if (pht == Tag::Closure
                                            && ph->asClosure()
                                            && ph->asClosure()->desc) {
                                            auto * d3 = ph->asClosure()->desc;
                                            std::fprintf(stderr, "(%s [%u..) nUp=%u)",
                                                !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                d3->codeOffset,
                                                ph->asClosure()->nUpvalues);
                                        } else if (pht == Tag::Thunk && ph->asThunk()) {
                                            Thunk * pt = ph->asThunk();
                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                (int)pt->state, (unsigned)pt->nUpvalues);
                                            if (pt->state == ThunkState::Suspended) {
                                                auto * d3 = pt->suspended.desc;
                                                if (d3)
                                                    std::fprintf(stderr, " %s [%u..)",
                                                        !d3->name.empty() ? d3->name.c_str() : "<anon>",
                                                        d3->codeOffset);
                                            } else if (pt->state == ThunkState::Evaluated) {
                                                std::fprintf(stderr, " EVAL=tag%u",
                                                    (unsigned)pt->evaluated.tag());
                                                // Recurse one level deep
                                                if (pt->evaluated.tag() == Tag::Thunk
                                                    && pt->evaluated.asThunk()) {
                                                    Thunk * pt2 = pt->evaluated.asThunk();
                                                    std::fprintf(stderr, "(state=%d nUp=%u",
                                                        (int)pt2->state, (unsigned)pt2->nUpvalues);
                                                    if (pt2->state == ThunkState::Suspended) {
                                                        auto * d4 = pt2->suspended.desc;
                                                        if (d4)
                                                            std::fprintf(stderr, " %s [%u..)",
                                                                !d4->name.empty() ? d4->name.c_str() : "<anon>",
                                                                d4->codeOffset);
                                                    } else if (pt2->state == ThunkState::Evaluated) {
                                                        std::fprintf(stderr, " EVAL=tag%u",
                                                            (unsigned)pt2->evaluated.tag());
                                                        if (pt2->evaluated.tag() == Tag::Closure
                                                            && pt2->evaluated.asClosure()
                                                            && pt2->evaluated.asClosure()->desc) {
                                                            auto * d5 = pt2->evaluated.asClosure()->desc;
                                                            std::fprintf(stderr, "(closure=%s [%u..) nUp=%u)",
                                                                !d5->name.empty() ? d5->name.c_str() : "<anon>",
                                                                d5->codeOffset,
                                                                pt2->evaluated.asClosure()->nUpvalues);
                                                        } else if (pt2->evaluated.tag() == Tag::Thunk
                                                            && pt2->evaluated.asThunk()) {
                                                            Thunk * pt3 = pt2->evaluated.asThunk();
                                                            std::fprintf(stderr, "(state=%d nUp=%u",
                                                                (int)pt3->state, (unsigned)pt3->nUpvalues);
                                                            if (pt3->state == ThunkState::Suspended) {
                                                                auto * d6 = pt3->suspended.desc;
                                                                if (d6)
                                                                    std::fprintf(stderr, " %s [%u..)",
                                                                        !d6->name.empty() ? d6->name.c_str() : "<anon>",
                                                                        d6->codeOffset);
                                                            }
                                                            std::fprintf(stderr, ")");
                                                        }
                                                    }
                                                    std::fprintf(stderr, ")");
                                                }
                                            }
                                            std::fprintf(stderr, ")");
                                        }
                                    }
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                        size_t lim = vm.frames.size();
                        for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                            const auto & fr = vm.frames[i - 1];
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            std::fprintf(stderr,
                                "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                                i - 1,
                                d && !d->name.empty() ? d->name.c_str()
                                    : (d ? "<anon>" : "<closure-body>"),
                                d ? d->codeOffset : 0, fr.ip,
                                (unsigned)fr.flags);
                        }
                        // Disasm from the frame's prologue to the failing
                        // OP_STR_CONCAT — full body lets us trace slot
                        // assignments back to their source.
                        if (cu && !vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk) d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            uint32_t lo = d ? d->codeOffset : (ip > 32 ? ip - 32 : 0);
                            uint32_t hi = ip + 4;
                            std::fprintf(stderr,
                                "  current frame disasm [%u..%u) (prologue→ip):\n", lo, hi);
                            disassembleWindow(stderr, *cu, lo, hi);
                            // Also disasm functions referenced by MAKE_THUNK
                            // / MAKE_CLOSURE in the prologue — these are
                            // the inner thunks that produce slot values.
                            std::fprintf(stderr, "  --- referenced functions ---\n");
                            for (uint32_t cur = lo; cur < hi - 1; ) {
                                Op op = decodeOp(cu->code[cur]);
                                uint32_t operand = decodeOperand(cu->code[cur]);
                                if (op == OP_MAKE_THUNK || op == OP_MAKE_CLOSURE) {
                                    if (operand < cu->lambdas.size()) {
                                        const LambdaDescriptor & d2 = cu->lambdas[operand];
                                        uint32_t flo = d2.codeOffset;
                                        uint32_t fhi = flo + 24;
                                        std::fprintf(stderr,
                                            "  fn[%u] (%s, nUp=%u) [%u..%u):\n",
                                            operand,
                                            !d2.name.empty() ? d2.name.c_str() : "<anon>",
                                            d2.nUpvalues, flo, fhi);
                                        disassembleWindow(stderr, *cu, flo, fhi);
                                    }
                                    // #530: op + nUp + nWiths data.
                                    cur += 3;
                                } else {
                                    cur++;
                                }
                            }
                        }
                    }
                }
            }

            // nix `+` semantics: if forceString=false and the first operand
            // is numeric (Int/Float), perform arithmetic addition; otherwise
            // do string concatenation.  forceString=true (e.g. "${foo}")
            // always coerces to string.
            if (!forceStr && n > 0 && (parts[0].isInt() || parts[0].isFloat())) {
                bool allInt = true;
                for (uint32_t i = 0; i < n; ++i) if (!parts[i].isInt()) { allInt = false; break; }
                Value r;
                if (allInt) {
                    int64_t sum = 0;
                    for (uint32_t i = 0; i < n; ++i) {
                        // #678 — drop "v3 OP_STR_CONCAT:" debug
                        // prefix; match TW phrasing (libexpr/eval.cc
                        // emits "integer overflow" via primOps add).
                        if (__builtin_add_overflow(sum, parts[i].asInt(), &sum))
                            throw std::runtime_error("integer overflow");
                    }
                    r.mkInt(sum);
                } else {
                    double sum = 0.0;
                    // #678 — TW emits "cannot add <type> to <kind>"
                    // where <kind> is "integer" or "float" depending
                    // on the FIRST operand (libexpr/eval.cc:2525-
                    // 2535).  parts[0] entered this branch because
                    // it was Int or Float — distinguish here.
                    const bool firstIsFloat = parts[0].isFloat();
                    for (uint32_t i = 0; i < n; ++i) {
                        const Value & p = parts[i];
                        if (p.isInt())   sum += static_cast<double>(p.asInt());
                        else if (p.isFloat()) sum += p.asFloat();
                        else {
                            auto typeName = [](const Value & v) -> const char * {
                                if (v.isString()) return "a string";
                                if (v.isPath())   return "a path";
                                if (v.isBool())   return "a Boolean";
                                if (v.isList())   return "a list";
                                if (v.isAttrs())  return "a set";
                                if (v.isNull())   return "null";
                                return "a value";
                            };
                            throw std::runtime_error(
                                std::string("cannot add ") + typeName(p)
                                + (firstIsFloat ? " to a float" : " to an integer"));
                        }
                    }
                    r.mkFloat(sum);
                }
                push(vm, r);
                goto str_concat_done;   // reg-VM: honour CFF_FORCE_WB
            }

            // Blackhole propagation: if any part is a propagating
            // Blackhole sentinel (cross-VM cycle marker), the whole
            // concat result is a Blackhole.  Match the
            // blackhole-as-value semantics established for OP_FORCE
            // and forceValue's foreign-VM Black case (vm.cc:~4290 /
            // ~8643): rather than throwing here (which would break
            // tryEval and the fall-back paths that expect typed
            // propagation), push vBlackhole and break.
            for (uint32_t i = 0; i < n; ++i) {
                if (parts[i].tag() == Tag::Blackhole) {
                    push(vm, Value::vBlackhole);
                    goto str_concat_done;
                }
            }
            {
            std::string out;
            // Accumulate string contexts from all parts.  Path parts
            // produce a fresh Opaque entry (the store path of the
            // copied content); String parts inherit any context their
            // payload buffer was tagged with.  Attrset parts coerce
            // via __toString/outPath like before — we treat the
            // resulting string identically.
            std::vector<std::string> ctxAccum;
            auto addCtx = [&](const std::vector<std::string> * v) {
                if (!v) return;
                for (auto & s : *v) ctxAccum.push_back(s);
            };
            for (uint32_t i = 0; i < n; ++i) {
                // Attrset coercion: mirror tree-walker's
                // `tryAttrsToString` + `coerceToString` recursion.
                // Used to interpolate derivation values and any attrset
                // that has `__toString self` or `outPath` (which itself
                // may be a string, a path, or yet another attrset that
                // needs further coercion — nixpkgs's `pkgs.hello`
                // resolves via outPath → derivation outPath → string).
                //
                // Unwind into parts[i] in-place so the existing String /
                // Path / coerceToString branches below handle the
                // resulting primitive.  Depth limit 8 matches tree-
                // walker's implicit recursion depth; deeper chains are
                // pathological and surface a clearer error than a stack
                // overflow.
                //
                // C-4b/«slot» (CODEBASE_REVIEW_2026-06-11): deref a Tag::Slot
                // part FIRST.  A Slot (v3-only transparent pointer into a
                // tenured cell — *slot is the value) may point at a derivation
                // attrset; without this it is NOT isAttrs(), so it skips the
                // __toString/outPath unwind below and falls straight to
                // coerceToString, which (after its own Slot-deref) errors
                // "cannot coerce a set" on the derivation — the firefox≡ghc98
                // .drvPath residual.  Resolving the Slot here lets the unwind
                // coerce the derivation via outPath, exactly as TW does.
                while (parts[i].tag() == Tag::Slot && parts[i].asSlot())
                    parts[i] = *parts[i].asSlot();
                // R1 (HNE typeOf .hello, 2026-06-12): force a Thunk/App/App3 part
                // to WHNF here — TW's coerceToString forces its operand first, and
                // both the isAttrs unwind below and coerceToString (which has no VM
                // to force with) require a resolved value.  The pre-force handshake
                // above forces every ORIGINAL part, but one the __toString/outPath
                // unwind produces — or one the opcode-level op_force_slow RETRY path
                // left Suspended (state=0) — can still arrive unforced; the
                // forceValue HELPER fully evaluates where the RETRY handshake no-ops.
                // Skip under-applied closure-PAPs (WHNF functions → "cannot coerce a
                // function", matching TW).  Re-deref any Slot the force exposes so
                // the isAttrs unwind sees a derivation attrset.
                if ((parts[i].tag() == Tag::Thunk || parts[i].tag() == Tag::App
                     || parts[i].tag() == Tag::App3)
                    && !isUnderappliedClosurePap(parts[i])) {
                    parts[i] = forceValue(vm, parts[i]);
                    while (parts[i].tag() == Tag::Slot && parts[i].asSlot())
                        parts[i] = *parts[i].asSlot();
                    // If forceValue still leaves a non-WHNF here it is a genuine
                    // deeper bug, not a missed force; coerceToString below raises
                    // the (informative) "cannot coerce a thunk" — no masking.
                }
                if (parts[i].isAttrs() && parts[i].asAttrs()) {
                    static const SymbolId tsId  = ir::globalInternSymbol("__toString");
                    static const SymbolId outId = ir::globalInternSymbol("outPath");
                    int depth = 0;
                    while (parts[i].isAttrs() && parts[i].asAttrs() && depth < 8) {
                        Bindings * b = parts[i].asAttrs();
                        if (auto * fn = b->lookup(tsId)) {
                            Value forced = forceValue(vm, *fn);
                            parts[i] = callClosure(vm, forced, parts[i]);
                            parts[i] = forceValue(vm, parts[i]);
                            ++depth;
                            continue;
                        }
                        if (auto * op = b->lookup(outId)) {
                            parts[i] = forceValue(vm, *op);
                            ++depth;
                            continue;
                        }
                        // V3_DBG_STRCONCAT: dump the attr names for an
                        // attrset that has neither __toString nor outPath
                        // — narrows the source of v3-specific
                        // eval-order divergence that surfaces a coerce
                        // attempt TW would never reach.
                        static const bool s_dbgUnc =
                            std::getenv("V3_DBG_STRCONCAT") != nullptr;
                        if (s_dbgUnc) {
                            const auto & st = ir::globalSymbolTable();
                            std::fprintf(stderr,
                                "v3 STR_CONCAT: attrs missing __toString/outPath "
                                "(ptr=%p size=%u): {",
                                (const void*)b, (unsigned)b->size);
                            for (uint32_t k = 0; k < b->size && k < 16; ++k) {
                                SymbolId nm = b->entries[k].name;
                                std::fprintf(stderr, "%s%s",
                                    k ? "," : "",
                                    nm < st.size() ? st[nm].c_str() : "?");
                            }
                            if (b->size > 16) std::fprintf(stderr, ",...");
                            std::fprintf(stderr, "}\n");

                            // Phase A2 (RCA 2026-05-11): bindings-origin
                            // lookup.  When NIX_V3_DBG_BINDINGS_ORIGIN=1
                            // is set, dump where this Bindings was
                            // allocated (source file:line + alloc kind).
                            // Without origin, we know the symptom but
                            // not whose Bindings is misbehaving.
                            if (const BindingsOrigin * orig =
                                    lookupBindingsOrigin(b)) {
                                const PosSnapshot * ops =
                                    resolvePosSnapshot(orig->posHandle);
                                std::fprintf(stderr,
                                    "  bindings origin: source=%s pos=%s:%u:%u\n",
                                    orig->source ? orig->source : "<?>",
                                    (ops && !ops->file.empty())
                                        ? ops->file.c_str()
                                        : "<no-pos>",
                                    ops ? ops->line : 0u,
                                    ops ? ops->column : 0u);
                            } else {
                                std::fprintf(stderr,
                                    "  bindings origin: <not recorded — "
                                    "NIX_V3_DBG_BINDINGS_ORIGIN=1 to enable>\n");
                            }

                            // Source position chain — which nixpkgs
                            // call site triggers this?  Walk the frame
                            // stack and dump every frame's lambda name
                            // + position so the failing source line
                            // can be located.
                            std::fprintf(stderr,
                                "  STR_CONCAT call: n=%u forceStr=%d ip=%u\n",
                                (unsigned)n, forceStr ? 1 : 0, ip - 1);
                            size_t depth = vm.frames.size();
                            size_t lo = depth > 16 ? depth - 16 : 0;
                            for (size_t fi = depth; fi-- > lo; ) {
                                const auto & fr = vm.frames[fi];
                                const LambdaDescriptor * d = nullptr;
                                if (fr.thunk
                                    && (fr.thunk->state == ThunkState::Suspended
                                        || fr.thunk->state == ThunkState::Blackhole))
                                    d = fr.thunk->suspended.desc;
                                else if (fr.closure) d = fr.closure->desc;
                                const PosSnapshot * ps = d
                                    ? resolvePosSnapshot(d->posHandle)
                                    : nullptr;
                                std::fprintf(stderr,
                                    "  fr[%zu]: name=%s pos=%s:%u:%u ip=%u flags=0x%x%s\n",
                                    fi,
                                    d && !d->name.empty() ? d->name.c_str() : "<anon>",
                                    (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                    ps ? ps->line : 0u,
                                    ps ? ps->column : 0u,
                                    fr.ip, (unsigned)fr.flags,
                                    fr.thunk ? " THUNK" : "");
                            }
                            // Also dump the i-th part's tag context —
                            // useful when more than one operand is on
                            // the stack to identify which is the bad
                            // attrset.
                            std::fprintf(stderr,
                                "  failing part index=%u of %u operands; "
                                "other operand tags=[", i, n);
                            for (uint32_t k = 0; k < n; ++k) {
                                if (k == i) std::fprintf(stderr, "%s%u←", k?",":"", (unsigned)parts[k].tag());
                                else std::fprintf(stderr, "%s%u", k?",":"", (unsigned)parts[k].tag());
                            }
                            std::fprintf(stderr, "]\n");

                            // Phase A2 (RCA 2026-05-11): dump the
                            // top-frame's locals so we can see what
                            // value `cpu` (or whichever local feeds the
                            // failing operand) actually holds.  For
                            // tripleFromSystem the formals destructure
                            // into local slots [0..N]; the first slots
                            // are cpu/vendor/kernel/abi.
                            if (depth > 0) {
                                const auto & topFr = vm.frames.back();
                                size_t base = topFr.stackBaseOffset;
                                size_t numLocals = vm.valueStack.size() - base;
                                if (numLocals > 8) numLocals = 8;
                                std::fprintf(stderr,
                                    "  top-frame locals (base=%zu, %zu shown):\n",
                                    base, numLocals);
                                for (size_t li = 0; li < numLocals; ++li) {
                                    const Value & lv = vm.valueStack[base + li];
                                    Tag t = lv.tag();
                                    std::fprintf(stderr,
                                        "    local[%zu]: tag=%u", li, (unsigned)t);
                                    if (t == Tag::Attrs && lv.asAttrs()) {
                                        auto * lb = lv.asAttrs();
                                        std::fprintf(stderr, " attrs ptr=%p size=%u {",
                                            (const void*)lb, (unsigned)lb->size);
                                        for (uint32_t kk = 0; kk < lb->size && kk < 8; ++kk) {
                                            SymbolId nm = lb->entries[kk].name;
                                            std::fprintf(stderr, "%s%s", kk?",":"",
                                                nm < st.size() ? st[nm].c_str() : "?");
                                        }
                                        if (lb->size > 8) std::fprintf(stderr, ",...");
                                        std::fprintf(stderr, "}");
                                        if (const BindingsOrigin * o2 =
                                                lookupBindingsOrigin(lb)) {
                                            const PosSnapshot * ops2 =
                                                resolvePosSnapshot(o2->posHandle);
                                            std::fprintf(stderr,
                                                " origin=%s@%s:%u",
                                                o2->source ? o2->source : "?",
                                                (ops2 && !ops2->file.empty())
                                                    ? ops2->file.c_str() : "?",
                                                ops2 ? ops2->line : 0u);
                                        }
                                    } else if (t == Tag::String && lv.asString()) {
                                        std::fprintf(stderr, " str=\"%.40s\"",
                                            lv.asString());
                                    } else if (t == Tag::Slot && lv.asSlot()) {
                                        std::fprintf(stderr,
                                            " slot->tag=%u",
                                            (unsigned)lv.asSlot()->tag());
                                    } else if (t == Tag::Thunk && lv.asThunk()) {
                                        std::fprintf(stderr,
                                            " thunk-state=%d",
                                            (int)lv.asThunk()->state);
                                    }
                                    std::fprintf(stderr, "\n");
                                }
                            }
                        }
                        break;  // no __toString, no outPath — fall through to coerceToString error
                    }
                }
                const Value & p = parts[i];
                if (p.isString())
                    addCtx(lookupStringContextEntries(p.asString()));
                if (p.isPath() && forceStr) {
                    // coerceToString will copy this path to the store and
                    // produce its `/nix/store/...` representation; tag the
                    // resulting string with that store path as an Opaque
                    // context entry.  Encoded form is the StorePath's
                    // basename (`<hash>-<name>`) — what
                    // NixStringContextElem::to_string()/parse roundtrip.
                    // Exceptions propagate: tree-walker raises on missing
                    // paths during interpolation, so v3 must too.
                    if (auto * ns = getNixEvalState())
                        ctxAccum.push_back(ffi::coercePathToStoreName(
                            *ns, p.asPath() ? p.asPath() : ""));
                }
                out.append(coerceToString(p, forceStr));
            }
            // Path + string semantics: when the first operand is a Path
            // and we're in plain `+` mode (not interpolation), the result
            // is a Path (lexically normalized), not a String.  Required
            // by `dirOf p + ""` and by string-test concat patterns like
            // `/foo/bar + "/../xyzzy/."` which must collapse to /foo/xyzzy.
            bool resultIsPath = !forceStr && n > 0 && parts[0].isPath();
            if (resultIsPath) {
                std::string normalized =
                    std::filesystem::path(out).lexically_normal().string();
                // lexically_normal leaves a trailing "/." for inputs
                // like "/a/b/." — strip it so output matches Nix.
                while (normalized.size() > 1 && normalized.back() == '/')
                    normalized.pop_back();
                out = std::move(normalized);
            }
            // CRIT-4: arena allocation for long-lived string/path
            // payload (was std::malloc + leak).
            char * buf = Alloc::allocChars(out.size() + 1);
            std::memcpy(buf, out.data(), out.size());
            buf[out.size()] = '\0';
            Value v;
            if (resultIsPath) {
                v.mkPath(buf);
            } else {
                v.mkString(buf);
                // De-duplicate context entries (a sorted-unique pass) and
                // record on the new buffer.  Empty input → no entry left.
                if (!ctxAccum.empty()) {
                    std::sort(ctxAccum.begin(), ctxAccum.end());
                    ctxAccum.erase(std::unique(ctxAccum.begin(), ctxAccum.end()),
                                   ctxAccum.end());
                    setStringContextEntries(buf, std::move(ctxAccum));
                }
            }
            push(vm, v);
            }   // close scope for the Blackhole-propagation early-exit
        str_concat_done:
            // reg-VM Phase 5: if OP_R_STR_CONCAT2 armed CFF_FORCE_WB, drop the
            // result (top of stack) into the dst slot and pop; a normal
            // STR_CONCAT has nothing armed, so this is a no-op and the result
            // stays on the operand stack.
            applyForceWriteback(vm);
            break;
        }
        case OP_ASSERT: {
            // 2026-05-19 #667: like OP_NOT / OP_AND_BRANCH / OP_BRANCH_FALSE,
            // OP_ASSERT must force its operand before the bool check.
            // Pre-fix v3 popped raw and called isTrueValue, which throws
            // "v3: expected bool" on Thunk/App/Slot.  Caught while
            // investigating gtk3.drvPath (an assertion in nixpkgs's
            // mkDerivation chain — typically `assert lib.assertMsg ...;`
            // — landed on a Slot pointing to a Thunk during the
            // derivation-attr iteration of primConcatLists).  The other
            // bool-consuming opcodes already handle this via the
            // CFF_FORCE_RETRY iterative-force protocol; OP_ASSERT was
            // simply missed.
            Value & top = vm.valueStack.back();
            if (top.isThunk() || top.isAppLike()
                || top.tag() == Tag::Slot)
            {
                ip = ip - 1;
                vm.frames.back().flags |= CFF_FORCE_RETRY;
                goto op_force_slow;
            }
            Value c = pop(vm);
            // #678 — drop "v3 OP_ASSERT:" debug prefix.  TW emits
            // `assertion '<exprStr>' failed` with the source text of
            // the asserted expression (libexpr/eval.cc:2222).  v3's
            // bytecode discards the assertion AST at lower time so we
            // can't reconstruct exprStr without threading it through
            // a new IR field — for now emit the bare "assertion
            // failed" message.  TODO: add CU-side string table for
            // assertion exprStrs and reference by index in OP_ASSERT.
            if (!isTrueValue(c)) throw AssertionError("assertion failed");
            break;
        }
        // OP_POS: bytecode value reserved; never emitted (lowerExpr
        // skips ExprPos in v3).  Removed dispatch; default-case abort
        // catches stale.

        case OP_IFD_PROBE: {
            // #736 (2026-05-21) Zero stack effect; emitted by emit.cc
            // right before an IFD-class primop call.  See bytecode.hh's
            // OP_IFD_PROBE / IfdProbeKind documentation.
            //
            // The probe MUST be cheap: a single bounded-array bump.
            // The primop call that follows does the actual IFD work
            // (synchronously via realisePath today; future S2/S4
            // layers will hook here to batch / cache instead).
            //
            // Bounds-check the kind against ifdProbeCount[]'s 16 slots
            // — emit.cc guarantees kind ∈ [1, kIfdProbeKindCount), but
            // a defensive check costs nothing here.
            uint8_t kind = static_cast<uint8_t>(operand);
            if (__builtin_expect(kind < 16, 1)) {
                ++allocStats().ifdProbeCount[kind];
            }
            break;
        }

        case OP_LIT_PRIMOP: {
            const PrimOp * po = cu->primops[operand];
            // A12b T0b: bytecode-primop replacement.  If `po` has a
            // registered bytecode-closure replacement (installed via
            // `installBytecodePrimop`), push that Closure Value instead
            // of a Tag::PrimOp Value.  Subsequent OP_CALL sees a
            // Closure (or Bridge-wrapped Closure) and dispatches it
            // iteratively, never reaching the C-recursive
            // primop-arg-force path.
            if (const Value * repl = lookupPrimopReplacement(po)) {
                push(vm, *repl);
                break;
            }
            Value v;
            v.mkPrimOp(po);
            push(vm, v);
            break;
        }

        case OP_LIT_BUILTINS: {
            push(vm, getBuiltinsValue());
            break;
        }

        case OP_CALL_PRIMOP: {
            uint32_t nArgs = operand;
            // A8 (2026-05-13): iterative-force pattern.  Read poIdx by
            // peek (NOT `ip++`) so a rewind cleanly re-enters this case
            // after an inner force completes.  We advance past poIdx
            // only once all strict args are confirmed WHNF.
            uint32_t poIdx = cu->code[ip];
            const PrimOp * po = cu->primops[poIdx];
            if (nArgs > 8) throw std::runtime_error("v3 OP_CALL_PRIMOP: arity > 8 not supported");
            // Args occupy the top `nArgs` slots of valueStack, with arg0
            // at the deepest and arg(N-1) at the top.  Scan them in
            // place — peek without popping — so we can rewind and
            // re-enter cleanly if a non-WHNF strict arg needs forcing.
            // A8 supersedes the previous C-recursive
            // `args[i] = forceValue(vm, args[i])` loop, which burned one
            // C-stack frame per nested force and overflowed at ~3000
            // levels deep on nixpkgs.
            {
                size_t argBase = vm.valueStack.size() - nArgs;
                for (uint32_t k = 0; k < nArgs; ++k) {
                    if (po->lazyArgs & (1u << k)) continue;
                    Value & a = vm.valueStack[argBase + k];
                    // #455 (2026-06-10): an under-applied closure-PAP (a Tag::App
                    // /App3 spine bottoming out in a closure that still needs more
                    // args) is ALREADY WHNF — `OP_FORCE` returns it unchanged
                    // (isUnderappliedClosurePap → break).  Without excluding it the
                    // rewind-and-force handshake below re-scans, sees the same
                    // App/App3, rewinds, and loops forever.  Repro:
                    // `map (f "x") xs` where `f` is a partially-applied sibling of
                    // a SEPARATELY-IMPORTED rec module builds arg0 as such a PAP.
                    // A PAP is a value; the primop (e.g. map) applies it per element
                    // exactly as the inline case does.  Saturated/over-applied apps
                    // are NOT under-applied PAPs, so they still take the force path.
                    // C-8 (CODEBASE_REVIEW_2026-06-11): the prior guard only matched
                    // Tag::App, so an App3 PAP fell into the force handshake and
                    // looped forever; needsForce() excludes BOTH App and App3 PAPs.
                    // See lode/RCA_455_VNATIVE_2026-06-10.md.
                    if (needsForce(a)) {
                        // Set up writeback: duplicate the unforced
                        // value to top-of-stack, encode the original
                        // slot's offset (relative to stackBase) in the
                        // upper 16 bits of the caller frame's flags,
                        // rewind ip to OP_CALL_PRIMOP, set
                        // CFF_FORCE_RETRY, and goto op_force_slow.
                        // After the force completes (synchronously via
                        // chase, or via OP_RETURN of the thunk body),
                        // applyForceWriteback writes the forced result
                        // into the arg slot and the opcode re-enters
                        // for another scan.
                        uint32_t off = static_cast<uint32_t>((argBase + k) - stackBase);
                        // Defensive: 16-bit slot offset capacity.
                        if (__builtin_expect(off > 0xFFFFu, 0))
                            throw std::runtime_error(
                                "v3 OP_CALL_PRIMOP: writeback slot offset too large");
                        push(vm, a);
                        CallFrame & frame = vm.frames.back();
                        setForceWriteback(frame, static_cast<uint16_t>(off));
                        frame.flags |= CFF_FORCE_RETRY;
                        ip = ip - 1;  // rewind to re-enter this OP_CALL_PRIMOP
                        goto op_force_slow;
                    }
                }
                // A8 phase 2 (2026-05-13): deepForceList pre-pass.
                // For each arg flagged DEEP_FORCE_LIST, the outer list
                // is WHNF (phase 1 above) but its elements are still
                // lazy.  Walk the list and iteratively force any
                // non-WHNF element via writeback to its ListVec
                // storage slot (CFF_FORCE_WB_PTR + forceWriteTarget).
                // The primop body's later `forceValue` calls on these
                // elements hit Evaluated thunks → no C-recursion.
                // Eliminates the recursive `forceValue` chain that
                // primConcatLists / primMap / primFoldl' had built up
                // (3500+ levels on nixpkgs derivation construction).
                static const bool s_noDeepForce =
                    std::getenv("NIX_V3_NO_DEEP_FORCE") != nullptr;
                if (po->deepForceList && !s_noDeepForce) {
                    // LISTTOATTRS_QUADRATIC fix: resume the element scan from
                    // the frame cursor instead of re-scanning the forced
                    // prefix on every re-entry (was O(n²)).  Cursor encodes
                    // (argK << 28) | elemI; 0 = fresh.  The element at the
                    // resume point was just forced (now WHNF), so the inner
                    // loop re-checks it O(1) and advances.
                    uint32_t resumeK = vm.frames.back().deepForceCursor >> 28;
                    uint32_t resumeI = vm.frames.back().deepForceCursor & 0x0FFFFFFFu;
                    for (uint32_t k = resumeK; k < nArgs; ++k) {
                        if (!(po->deepForceList & (1u << k))) continue;
                        Value & a = vm.valueStack[argBase + k];
                        if (!a.isList() || !a.asList()) continue;
                        ListVec * list = a.asList();
                        // GC_AUDIT_ROUND_2 Round 1 #6 (LATENT, documented):
                        // `frame.forceWriteTarget = &e` where `e` is
                        // `list->elems[i]` pinches a pointer into a
                        // potentially nursery-resident ListVec.  If
                        // scavenge fires during the force chain, the
                        // ListVec is forwarded tenured and the pointer
                        // becomes stale; the scavenger walks the Value
                        // at `*forceWriteTarget` (gc.cc postScavengeAudit
                        // mirror) but does NOT update the pointer when
                        // its underlying container moves.  Current state
                        // (per audit): silent memoization loss only;
                        // applyForceWriteback writes WHNF into freed
                        // nursery bytes, the next loop iteration re-forces
                        // (correctness preserved by re-derivation), and
                        // the stale-write target is dead memory.
                        //
                        // Attempted fix (`if (listInNursery) target=null`)
                        // produced an infinite loop because the unforced
                        // element is never replaced; without a per-
                        // primop iteration limit the scan re-enters
                        // forever.  Correct fix requires either:
                        //   (a) extending scavenger to track and update
                        //       container-relative pointers, OR
                        //   (b) promoting the list to tenured before
                        //       deepForceList runs, OR
                        //   (c) frame-encoded (listPtr, index) writeback
                        //       descriptor that the scavenger rewrites.
                        // Deferred until measurement shows the silent
                        // loss meaningfully impacts perf or until a
                        // crash path surfaces.
                        uint32_t startI = (k == resumeK) ? resumeI : 0;
                        for (uint32_t i = startI; i < list->size; ++i) {
                            Value & e = list->elems[i];
                            // C-8 (CODEBASE_REVIEW_2026-06-11): needsForce
                            // excludes under-applied closure-PAPs (e.g. lib.pipe's
                            // function list fed to foldl') — a PAP is already WHNF,
                            // so forcing it is a no-op and re-arming the handshake
                            // on it would loop forever.
                            if (needsForce(e)) {
                                push(vm, e);
                                CallFrame & frame = vm.frames.back();
                                frame.forceWriteTarget = &e;
                                frame.flags |= CFF_FORCE_WB_PTR
                                             | CFF_FORCE_RETRY;
                                // Record resume point so re-entry skips the
                                // already-forced prefix (O(n²) → O(n)).
                                frame.deepForceCursor = (k << 28) | i;
                                ip = ip - 1;  // re-enter OP_CALL_PRIMOP
                                goto op_force_slow;
                            }
                        }
                    }
                    // All deep args fully forced — reset the cursor so the
                    // next OP_CALL_PRIMOP on this frame starts fresh.
                    vm.frames.back().deepForceCursor = 0;
                }
            }
            // All strict args are WHNF.  Advance past poIdx and call.
            ip++;
            // Profiling counter (gated on NIX_VM_STATS at process exit).
            // The bump is unconditional — the primop dispatch already
            // does substantially more work, so the cost is invisible.
            bumpPrimOpCallCount(po);
            Value args[8];
            for (uint32_t i = nArgs; i > 0; --i) args[i - 1] = pop(vm);
            // Save current frame state in case the primop calls back
            // into the VM via callClosure().
            vm.frames.back().ip = ip;
            // Wire the EvalState to this VM so callback primops can
            // re-enter the dispatcher; also propagate the (optional)
            // nix EvalState so primops like `import` can parse files.
            EvalState state;
            state.vm = &vm;
            state.nixEvalState = getNixEvalState();
            Value out;
            // #788 (2026-05-23) per-primop wall-clock attribution.
            // Gated by NIX_VM_PRIMOP_TIME=1; ~20 ns overhead per
            // primop call when enabled.  Captures INCLUSIVE time
            // (the primop body + any nested forceValue / callClosure
            // dispatches).  Identifies which specific primop is
            // the wall-clock lever within the ~11s OP_CALL_PRIMOP
            // inclusive bucket exposed by #786/#790 OPCYCLES.
            static const bool s_primopTime =
                std::getenv("NIX_VM_PRIMOP_TIME") != nullptr;
            uint64_t poStartNs = 0;
            if (__builtin_expect(s_primopTime, 0)) [[unlikely]] {
                poStartNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }
            po->fn(state, args, out);
            if (__builtin_expect(s_primopTime, 0)) [[unlikely]] {
                uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                bumpPrimOpNanos(po, now - poStartNs);
            }
            push(vm, out);
            break;
        }
        case OP_R_PRIMOP2: {
            // Register VM Phase 1: 3-address binary primop.  regs[dst] =
            // po(arg0, arg1), operands read directly from slots (or inline
            // immediates), result written to a dst slot — no operand stack.
            // Strict non-WHNF slot args are forced via A8 writeback-to-slot,
            // mirroring OP_CALL_PRIMOP (peek follow-up words; advance only
            // once all strict args are WHNF so a force-rewind re-enters
            // cleanly).
            const PrimOp * po = cu->primops[operand];
            uint32_t dst    = cu->code[ip];        // peek word1
            uint32_t descAB = cu->code[ip + 1];    // peek word2
            uint32_t desc[2] = { descAB >> 16, descAB & 0xFFFFu };
            // Force scan over strict slot args.
            for (uint32_t k = 0; k < 2; ++k) {
                if (desc[k] & 0x8000u) continue;           // inline immediate
                if (po->lazyArgs & (1u << k)) continue;    // lazy arg
                uint32_t slot = desc[k] & 0x7FFFu;
                Value & a = vm.valueStack[stackBase + slot];
                if (needsForce(a)) {  // C-8/9/10: PAP is WHNF — don't re-arm
                    push(vm, a);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(slot));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;  // rewind to the OP_R_PRIMOP2 word
                    goto op_force_slow;
                }
            }
            // All strict args WHNF.  Advance past word1, word2 and call.
            ip += 2;
            bumpPrimOpCallCount(po);
            Value args[2];
            for (uint32_t k = 0; k < 2; ++k) {
                if (desc[k] & 0x8000u) {
                    int32_t v = static_cast<int32_t>(desc[k] & 0x7FFFu);
                    if (v & 0x4000) v -= 0x8000;          // sign-extend 15-bit
                    args[k].mkInt(v);
                } else {
                    args[k] = vm.valueStack[stackBase + (desc[k] & 0x7FFFu)];
                }
            }
            vm.frames.back().ip = ip;
            EvalState state;
            state.vm = &vm;
            state.nixEvalState = getNixEvalState();
            Value out;
            po->fn(state, args, out);
            // Write result to dst slot (auto-grow, mirrors OP_SET_LOCAL).
            {
                size_t idx = stackBase + dst;
                while (idx >= vm.valueStack.size())
                    vm.valueStack.push_back(Value{});
                vm.valueStack[idx] = out;
            }
            break;
        }

        // ---- #428 fast-path primop opcodes ----------------------------
        // Each opcode is a bug-compatible inline of the corresponding C
        // primop (primops.cc): same forcing, same throws, same return
        // shape.  Emitted by the lowerer in lieu of OP_CALL_PRIMOP when
        // the called primop is one of the targeted ones; the primop
        // itself stays registered for first-class uses.

        // -Werror=switch-enum on the outer dispatch makes a single
        // multi-tag inner switch awkward (requires every enum value
        // listed), so each predicate gets its own self-contained
        // case body using a small lambda to share the force-then-test
        // pattern.
        //
        // #493: forceValue may break early on Tag::Thunk in Bridge
        // state when the underlying TW Value is a Function (the #456
        // chase-break: forceBridgeThunk would re-wrap as another
        // Bridge ad infinitum).  Result: v stays Tag::Thunk even
        // though the WHNF type is whatever TW says.  The naive
        // predicate `v.isClosure() || ...` answers false for these
        // bridged-TW-function values, mis-routing nixpkgs loadModule's
        // `if isFunction m then ... else import m` to import.  Peek
        // through Bridge thunks for the TW ValueType.  Cheap; only
        // fires on the Bridge case.
        // (TW_VALUE_ERADICATION F4, 2026-06-02): no Bridge thunks → always fallback.
        #define V3_BRIDGE_PEEK_OR(v, twTypePred, fallback) (fallback)
        // A8: iterative force.  On non-WHNF top, rewind ip, set
        // CFF_FORCE_RETRY, and goto op_force_slow — the opcode re-enters
        // with WHNF on top.  No C-recursion through forceValue.
        // A8: iterative force.  On non-WHNF top, rewind ip, set
        // CFF_FORCE_RETRY, and goto op_force_slow.  An under-applied
        // closure-PAP (Tag::App/App3 leaf-Closure with arity>depth) is
        // ALREADY WHNF — forcing it is a no-op, so sending it to
        // op_force_slow and then re-testing isAppLike() spins forever
        // (2026-06-10: this was the haskell.nix `lib.isFunction` hang —
        // `builtins.isFunction ((a: b: …) 1)` looped, since eval/apply
        // collapses curried lambdas so the partial call is a Tag::App PAP).
        // Exclude PAPs from the force-retry; the predExpr below must then
        // classify them correctly (a PAP is a `lambda`).
        #define V3_IS_OP(op_name, predExpr) \
            case op_name: { \
                Value & topRef = vm.valueStack.back(); \
                if ((topRef.isThunk() || topRef.isAppLike() \
                    || topRef.tag() == Tag::Slot) \
                    && !isUnderappliedClosurePap(topRef)) { \
                    ip = ip - 1; \
                    vm.frames.back().flags |= CFF_FORCE_RETRY; \
                    goto op_force_slow; \
                } \
                Value v = pop(vm); \
                push(vm, (predExpr) ? Value::vTrue : Value::vFalse); \
                break; \
            }
        V3_IS_OP(OP_IS_NULL,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Null,    v.isNull()))
        V3_IS_OP(OP_IS_BOOL,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Bool,    v.isBool()))
        V3_IS_OP(OP_IS_INT,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Int,     v.isInt()))
        V3_IS_OP(OP_IS_FLOAT,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Float,   v.isFloat()))
        V3_IS_OP(OP_IS_STRING,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::String,  v.isString()))
        V3_IS_OP(OP_IS_PATH,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Path,    v.isPath()))
        V3_IS_OP(OP_IS_LIST,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::List,    v.isList()))
        V3_IS_OP(OP_IS_ATTRS,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Attrs,   v.isAttrs()))
        V3_IS_OP(OP_IS_FUNCTION,
            V3_BRIDGE_PEEK_OR(v, tt == ffi::TwType::Function,
                v.isClosure() || v.isPrimOp() || v.tag() == Tag::PrimOpApp
                || isUnderappliedClosurePap(v)))
        #undef V3_IS_OP
        #undef V3_BRIDGE_PEEK_OR

        case OP_HEAD: {
            // Mirror primHead in primops.cc:272-278.
            // A8: iterative force.
            {
                Value & topRef = vm.valueStack.back();
                if (needsForce(topRef)) {  // C-8/9/10: a PAP is WHNF — fall
                    // through to the type-error path below (never re-arm the
                    // handshake on a PAP, which would loop forever).
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
            }
            Value v = pop(vm);
            // #678 — match TW phrasing (libexpr/primops.cc:3892).
            // TW distinguishes "not a list" (type error) from "empty
            // list" (call-with-empty error); v3 collapses both into a
            // single runtime_error.  For now match the empty-list
            // message exactly (the more common case); type-mismatch
            // falls under the same string.
            if (!v.isList() || !v.asList() || v.asList()->size == 0)
                throw std::runtime_error(
                    "'builtins.head' called on an empty list");
            push(vm, v.asList()->elems[0]);
            break;
        }

        case OP_TAIL: {
            // Mirror primTail in primops.cc:280-292.
            // A8: iterative force.
            {
                Value & topRef = vm.valueStack.back();
                if (needsForce(topRef)) {  // C-8/9/10: a PAP is WHNF — fall
                    // through to the type-error path below (never re-arm the
                    // handshake on a PAP, which would loop forever).
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
            }
            Value v = pop(vm);
            // #678 — match TW phrasing (libexpr/primops.cc:3919).
            if (!v.isList() || !v.asList() || v.asList()->size == 0)
                throw std::runtime_error(
                    "'builtins.tail' called on an empty list");
            uint32_t n = v.asList()->size;
            ListVec * out_l = Alloc::allocList(n - 1);
            V3_STATS_INC(listsAllocated);
            for (uint32_t i = 1; i < n; ++i)
                out_l->elems[i - 1] = v.asList()->elems[i];
            listPostConstructBarrier(out_l);  // Phase D coverage (OP_TAIL)
            Value r;
            r.mkList(out_l);
            push(vm, r);
            break;
        }

        case OP_LENGTH: {
            // Mirror primLength in primops.cc:262-270.  Handles list
            // OR string; throws otherwise with the same message.
            // A8: iterative force.
            {
                Value & topRef = vm.valueStack.back();
                if (needsForce(topRef)) {  // C-8/9/10: a PAP is WHNF — fall
                    // through to the type-error path below (never re-arm the
                    // handshake on a PAP, which would loop forever).
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
            }
            Value v = pop(vm);
            // #680 — TW's `builtins.length` only accepts lists
            // (libexpr/primops.cc:4109).  Pre-fix v3 also accepted
            // strings as a "bonus" — silent semantic divergence.
            // TW's forceList raises `expected a list but found a
            // <type>: <value>` (libexpr/eval.cc:1444 family).
            if (!v.isList()) {
                const char * art = "a";
                const char * name = "value";
                Tag t = v.tag();
                if (t == Tag::Int)        { art = "an"; name = "integer"; }
                else if (t == Tag::Float) { art = "a";  name = "float"; }
                else if (t == Tag::Bool)  { art = "a";  name = "Boolean"; }
                else if (t == Tag::Null)  { art = "";   name = "null"; }
                else if (t == Tag::String){ art = "a";  name = "string"; }
                else if (t == Tag::Path)  { art = "a";  name = "path"; }
                else if (t == Tag::Attrs) { art = "a";  name = "set"; }
                else if (t == Tag::Closure || t == Tag::PrimOp || t == Tag::PrimOpApp)
                                          { art = "a";  name = "function"; }
                std::string msg = "expected a list but found ";
                if (*art) { msg += art; msg += ' '; }
                msg += name;
                // #691 — append `: <value>` matching TW.
                msg += ": ";
                msg += valueRepr(v);
                throw std::runtime_error(msg);
            }
            Value r; r.mkInt(v.asList() ? v.asList()->size : 0);
            push(vm, r);
            break;
        }

        case OP_ELEM_AT: {
            // Mirror primElemAt in primops.cc:294-303.  Stack: [..., lst, idx]
            // (idx on top).  A8: iterative writeback-force for the two args.
            {
                size_t topIdx = vm.valueStack.size() - 1;
                Value & idxRef = vm.valueStack[topIdx];
                Value & lstRef = vm.valueStack[topIdx - 1];
                if (needsForce(idxRef)) {  // C-8/9/10
                    ip = ip - 1;
                    vm.frames.back().flags |= CFF_FORCE_RETRY;
                    goto op_force_slow;
                }
                if (needsForce(lstRef)) {  // C-8/9/10
                    // Writeback for the deeper slot.  Slot offset =
                    // (topIdx - 1) - stackBase.
                    uint32_t off = static_cast<uint32_t>((topIdx - 1) - stackBase);
                    if (__builtin_expect(off > 0xFFFFu, 0))
                        throw std::runtime_error(
                            "v3 OP_ELEM_AT: writeback slot offset too large");
                    push(vm, lstRef);
                    CallFrame & frame = vm.frames.back();
                    setForceWriteback(frame, static_cast<uint16_t>(off));
                    frame.flags |= CFF_FORCE_RETRY;
                    ip = ip - 1;
                    goto op_force_slow;
                }
            }
            Value idx = pop(vm);
            Value lst = pop(vm);
            if (!lst.isList() || !idx.isInt())
                throw std::runtime_error(
                    "value is not a list while a list was expected");
            uint32_t n = lst.asList() ? lst.asList()->size : 0;
            if (idx.asInt() < 0 || static_cast<uint64_t>(idx.asInt()) >= n)
                // #678 — match TW phrasing
                // (libexpr/primops.cc:3869).
                throw std::runtime_error(
                    "'builtins.elemAt' called with index "
                    + std::to_string(idx.asInt())
                    + " on a list of size " + std::to_string(n));
            push(vm, lst.asList()->elems[idx.asInt()]);
            break;
        }

        case OP_ATTRS_REC_SET: {
            uint32_t i = operand;
            Value v = pop(vm);
            // Peek at the rec bindings (top of stack now) and write into entry i.
            Value & recAttrs = top(vm);
            if (!recAttrs.isAttrs())
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: top is not an attrset");
            if (!recAttrs.asAttrs() || i >= recAttrs.asAttrs()->size)
                throw std::runtime_error("v3 OP_ATTRS_REC_SET: index out of range");
            // Phase A3 (RCA 2026-05-11): trace SETs that write into a
            // slot whose name matches V3_DBG_REC_SET_NAME.  Used to
            // localize cross-binding contamination — e.g., what value
            // is written into the `cpuName` slot of tripleFromSystem's
            // let-block.
            {
                static const char * s_dbgRecSetName =
                    std::getenv("V3_DBG_REC_SET_NAME");
                if (__builtin_expect(s_dbgRecSetName != nullptr, 0)) {
                    auto * b = recAttrs.asAttrs();
                    SymbolId nm = b->entries[i].name;
                    const auto & st = ir::globalSymbolTable();
                    const char * nmStr =
                        nm < st.size() ? st[nm].c_str() : "?";
                    if (std::strcmp(nmStr, s_dbgRecSetName) == 0) {
                        std::fprintf(stderr,
                            "v3 OP_ATTRS_REC_SET slot[%u]=%s on bindings ptr=%p"
                            " (slot-storage=%p, size=%u) <- value tag=%u",
                            i, nmStr, (const void *)b,
                            (const void *)&b->entries[i].value,
                            (unsigned)b->size, (unsigned)v.tag());
                        if (v.isThunk() && v.asThunk())
                            std::fprintf(stderr, " thunk-ptr=%p",
                                (const void *)v.asThunk());
                        if (v.isAttrs() && v.asAttrs()) {
                            auto * vb = v.asAttrs();
                            std::fprintf(stderr, " attrs size=%u {",
                                (unsigned)vb->size);
                            for (uint32_t k = 0; k < vb->size && k < 6; ++k) {
                                SymbolId nm2 = vb->entries[k].name;
                                std::fprintf(stderr, "%s%s", k ? "," : "",
                                    nm2 < st.size() ? st[nm2].c_str() : "?");
                            }
                            std::fprintf(stderr, "}");
                            if (const BindingsOrigin * o =
                                    lookupBindingsOrigin(vb)) {
                                const PosSnapshot * ps =
                                    resolvePosSnapshot(o->posHandle);
                                std::fprintf(stderr,
                                    " value-origin=%s@%s:%u",
                                    o->source ? o->source : "?",
                                    (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                    ps ? ps->line : 0u);
                            }
                        } else if (v.isThunk() && v.asThunk()) {
                            Thunk * t = v.asThunk();
                            const LambdaDescriptor * td =
                                t->state == ThunkState::Suspended
                                    ? t->suspended.desc : nullptr;
                            std::fprintf(stderr,
                                " thunk-state=%d nUp=%u codeOff=%u",
                                (int)t->state, (unsigned)t->nUpvalues,
                                td ? td->codeOffset : 0u);
                            // Also dump the body bytecode (40 words) so
                            // we can sanity-check the lowerer emitted
                            // the right ops for `cpu.name` etc.
                            if (const CompilationUnit * tcu = thunkCU(t); td && tcu) {  // FP-2a
                                std::fprintf(stderr, "\n      body disasm:\n");
                                disassembleWindow(stderr, *tcu,
                                    td->codeOffset, td->codeOffset + 40);
                            }
                            // Dump up to 6 upvalues with their tags + (for
                            // Attrs) their shape & origin.
                            for (uint16_t ui = 0; ui < t->nUpvalues && ui < 6; ++ui) {
                                const Value & uv = t->tail[ui];
                                std::fprintf(stderr, "\n      up[%u] tag=%u",
                                    ui, (unsigned)uv.tag());
                                if (uv.tag() == Tag::Attrs && uv.asAttrs()) {
                                    auto * ub = uv.asAttrs();
                                    std::fprintf(stderr, " ptr=%p size=%u {",
                                        (const void *)ub, (unsigned)ub->size);
                                    for (uint32_t k = 0; k < ub->size && k < 6; ++k) {
                                        SymbolId nm3 = ub->entries[k].name;
                                        std::fprintf(stderr, "%s%s", k?",":"",
                                            nm3 < st.size() ? st[nm3].c_str() : "?");
                                    }
                                    std::fprintf(stderr, "}");
                                    if (const BindingsOrigin * o3 =
                                            lookupBindingsOrigin(ub)) {
                                        const PosSnapshot * ps =
                                            resolvePosSnapshot(o3->posHandle);
                                        std::fprintf(stderr,
                                            " origin=%s@%s:%u",
                                            o3->source ? o3->source : "?",
                                            (ps && !ps->file.empty()) ? ps->file.c_str() : "?",
                                            ps ? ps->line : 0u);
                                    }
                                } else if (uv.tag() == Tag::Slot && uv.asSlot()) {
                                    std::fprintf(stderr, " slot->tag=%u",
                                        (unsigned)uv.asSlot()->tag());
                                } else if (uv.tag() == Tag::Thunk && uv.asThunk()) {
                                    std::fprintf(stderr,
                                        " thunk-state=%d", (int)uv.asThunk()->state);
                                } else if (uv.isString()) {
                                    std::fprintf(stderr, " str=\"%.20s\"", uv.asString());
                                }
                            }
                        } else if (v.isString()) {
                            std::fprintf(stderr, " str=\"%.40s\"",
                                v.asString());
                        }
                        // Current frame's position.
                        if (!vm.frames.empty()) {
                            const auto & fr = vm.frames.back();
                            const LambdaDescriptor * d = nullptr;
                            if (fr.thunk
                                && (fr.thunk->state == ThunkState::Suspended
                                    || fr.thunk->state == ThunkState::Blackhole))
                                d = fr.thunk->suspended.desc;
                            else if (fr.closure) d = fr.closure->desc;
                            const PosSnapshot * fps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                " from %s@%s:%u (ip=%u)",
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (fps && !fps->file.empty()) ? fps->file.c_str() : "?",
                                fps ? fps->line : 0u, ip - 1);
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            bindingsSetValue(recAttrs.asAttrs(), i, v);  // Phase D barrier
            // STG-8 (#498): if this entry's value is a Suspended thunk
            // (the common case from the LetRec emit's per-attr thunks),
            // record &entries[i].value as the thunk's heap-stable cell.
            // OP_RETURN's CFF_THUNK_RETURN handler will write the body's
            // result back to *cell at completion, mirroring TW's in-
            // place forceValue update.  Sub-thunks captured-with a
            // Tag::Slot to this cell then read the result via single
            // deref instead of the legacy `Slot -> Thunk -> Evaluated`
            // chase, AND foreign-VM observers (cross-VMState fresh
            // VMStates the eval/call hooks spawn) stop seeing a leaked
            // Black thunk after the body completes -- the cell holds
            // the final value.
            //
            // We only set cell when the entry IS a fresh Suspended
            // thunk (avoid clobbering a previously-set cell from a
            // shared thunk, and skip non-thunk entries entirely).
            if (v.isThunk() && v.asThunk()
                && v.asThunk()->state == ThunkState::Suspended
                && v.asThunk()->cell == nullptr)
            {
                Value * cellTarget =
                    &recAttrs.asAttrs()->entries[i].value;
                v.asThunk()->cell = cellTarget;
                // M-8 (CODEBASE_REVIEW_2026-06-11): the owning-Bindings record
                // (Thunk::cellContainer) was removed.  The GC marker now
                // DERIVES the container from `cell` via findContainingCellStart
                // (mark_sweep walkThunk), and the OP_RETURN barrier tracks the
                // single cell write via the standalone-cell registry — so this
                // store is no longer needed.
                cellOwnRecordSet(cellTarget, v.asThunk(),
                                  "OP_ATTRS_REC_SET");
            }
            break;
        }

        case OP_HALT: {
            // Defensive: chase Tag::Thunk/App/Slot before exiting so the
            // caller never receives an unforced value if a future bytecode
            // change drops the trailing OP_FORCE.  Today the entry-function
            // code emitter inserts that force, so this is a tail-handling
            // safety net rather than a hot path.  REVIEW MED-3.
            finalResult = pop(vm);
            if (finalResult.tag() == Tag::Thunk
                || finalResult.isAppLike()
                || finalResult.tag() == Tag::Slot) {
                vm.frames.back().ip = ip;
                finalResult = forceValue(vm, finalResult);
            }
            running = false;
            break;
        }

        default:
            std::fprintf(stderr, "v3 VM: unhandled opcode 0x%02x at ip=%u\n",
                static_cast<int>(op), ip - 1);
            std::abort();
        }
#pragma clang diagnostic pop
    }
    } catch (...) {
        // 2026-05-17 exception barrier — see comment at try { above.
        // Cleanup is best-effort; if it throws (e.g. partial-bindings
        // access on a freed thunk pointer), swallow so the original
        // exception propagates undisturbed.
        try {
            clearBlackMarksOnException(vm, exitDepth);
        } catch (...) {}
        throw;
    }

    return finalResult;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

// WC-5: clear Black marks on any thunk frames currently in the VM.
// Used as the exception-recovery hook around dispatchLoop calls.
// Tree-walker's mkFailed stores the exception and re-throws; we
// take the cheaper-but-still-correct path of reverting Black to
// Suspended so the next force re-runs (idempotent throws then
// re-throw the same error).
static void clearBlackMarksOnException(VMState & vm, size_t exitDepth)
{
    for (size_t i = vm.frames.size(); i > exitDepth; --i) {
        auto & fr = vm.frames[i - 1];
        if ((fr.flags & CFF_THUNK_RETURN) && fr.thunk
            && fr.thunk->state == ThunkState::Blackhole) {
            fr.thunk->state = ThunkState::Suspended;
        }
    }
    // WC-37: also unwind the leftover frames pushed by the failed
    // dispatchLoop call.  Without this, a thunk frame that was on
    // the stack when the exception was thrown remains as a "ghost
    // frame" — when a subsequent OP_RETURN in an outer dispatchLoop
    // pops vm.frames.back(), it pops the ghost frame, finds
    // CFF_THUNK_RETURN + a thunk pointer, and stores whatever was on
    // the value stack into that thunk's evaluated slot.  In the
    // pure-VM nixpkgs case this corrupts the +chain preHook thunk
    // (codeOffset=1346, nUp=5) by storing a closure (e.g.
    // bintoolsPackages's body's MAKE_CLOSURE result) where a string
    // was expected.  Resize valueStack & withStack to the FIRST
    // popped frame's bases (= the stack/with depth when that frame
    // was pushed), preserving everything below.
    if (vm.frames.size() > exitDepth) {
        const auto & firstPopped = vm.frames[exitDepth];
        uint32_t targetStackBase = firstPopped.stackBaseOffset;
        uint32_t targetWithBase  = firstPopped.withStackBase;
        vm.frames.resize(exitDepth);
        if (vm.valueStack.size() > targetStackBase)
            vm.valueStack.resize(targetStackBase);
        if (vm.withStack.size() > targetWithBase)
            vm.withStack.resize(targetWithBase);
    }
    // REVIEW MED-3 + critic §8 #2: clear CFF_FORCE_RETRY on the
    // topmost surviving frame.  The flag is a one-shot set by OP_FORCE
    // before pushing a thunk frame, consumed by the caller's
    // OP_RETURN.  If the pushed thunk threw, the flag stayed on the
    // surviving caller; the next OP_RETURN would interpret a stale
    // value as a thunk-force result and trigger spurious retry.
    //
    // N8/R8 (audit Round 2): also clear ALL force-writeback flags +
    // forceWriteTarget on every surviving frame.  These flags are
    // one-shots set by OP_FORCE / OP_CALL_PRIMOP / OP_ATTRS_SELECT_IC
    // before entering a sub-force; if the sub-force throws, the flag
    // + target stay set on the caller.  Next opcode landing on that
    // frame triggers a spurious applyForceWriteback that pops a
    // wrong-stack value and writes it to forceWriteTarget — silently
    // corrupting a list / attrset slot.
    if (!vm.frames.empty()) {
        auto & f = vm.frames.back();
        f.flags &= ~(CFF_FORCE_RETRY
                     | CFF_FORCE_WB
                     | CFF_FORCE_WB_PTR
                     | CFF_FORCE_WB_PTR_KEEP);
        f.forceWriteTarget = nullptr;
    }
}

/// #425: get the `builtins` attrset singleton (lazily built once,
/// reused process-wide).  Exposed publicly so the v3 force/call
/// hook can materialise an upvalue Value for sub-Exprs that
/// captured `builtins` (via `LitBuiltins`) as a freeVar -- there's
/// no env-side counterpart to walk to, so we just hand back the
/// singleton.  Same Value pushed by OP_LIT_BUILTINS at runtime.
// #705 (2026-05-20): file-static vBuiltins storage (was a function-
// local-static) so the scavenger can access it via `peekBuiltinsValue`
// without triggering lazy init mid-scavenge.  The init flag is set
// to true by `getBuiltinsValue` on first call; `peekBuiltinsValue`
// returns the address only after that flip.  Single-threaded VM — no
// atomics needed.
static bool g_vBuiltinsInitialized = false;
static Value g_vBuiltins;

Value * peekBuiltinsValue() noexcept
{
    return g_vBuiltinsInitialized ? &g_vBuiltins : nullptr;
}

Value getBuiltinsValue() noexcept
{
    if (g_vBuiltinsInitialized) return g_vBuiltins;
    g_vBuiltins = []{
        const auto & reg = allRegisteredPrimOps();
        // TW's `EvalState::addPrimOp` (libexpr/eval.cc:577) STRIPS the
        // leading `__` from a primop's name before inserting it into
        // the `builtins` attrset, while keeping the original prefixed
        // name in the base env (so `__add` resolves to the same primop
        // as `builtins.add`).
        //
        // 2026-05-19 #665: pre-fix v3 SKIPPED all `__`-prefixed primops
        // from vBuiltins entirely, which meant `builtins.toFile`,
        // `builtins.readFile`, `builtins.elem`, etc. weren't visible —
        // the lutok-0.4 derivation (used by libiconv → gnugrep →
        // clang-wrapper) calls `builtins.toFile` in its native-derivation
        // path, native fell back to bridge, bridge cycled, fake-store
        // path leaked into clang-wrapper's env and cascaded across all
        // wrapped derivations.
        //
        // Match TW: include `__`-prefixed primops under their stripped
        // name (skip if a non-prefixed entry already exists — TW's
        // addPrimOp would have shadowed too).  De-dupe up-front: count
        // distinct stripped names so allocBindings gets the right size,
        // then populate with a "first-write wins" rule.
        // C-22 (CODEBASE_REVIEW_2026-06-11): v3-INTERNAL implementation primops
        // must NOT appear in the user-facing `builtins` attrset.  TW has no such
        // builtins, so exposing them makes `builtins ? derivCoerce` /
        // `attrNames builtins` diverge and breaks nixpkgs feature-detection
        // (e.g. `builtins.splitString or lib.splitString` would pick the v3
        // builtin instead of lib's, silently changing behaviour).  These stay
        // reachable internally by their REGISTERED name (the optimizer +
        // bytecode wrappers use allRegisteredPrimOps / the strictness whitelist,
        // not this attrset).  NOTE: real builtins TW conditionally hides
        // (currentSystem/currentTime/outputOf/fetchClosure/fetchFinalTree —
        // gated by impure/experimental) are deliberately KEPT.  Keyed by the
        // stripped name so both `foo` and `__foo` registrations are covered.
        static const std::unordered_set<std::string_view> kV3InternalBuiltins = {
            "derivCoerce", "foldlMap", "derivationFromPreprocessed",
            "derivationStrictRaw", "derivationRaw", "v3CompileCallFlake",
            "parseInt", "splitString",
        };
        std::unordered_map<std::string, std::reference_wrapper<const nix::v3::PrimOp>> stripped;
        // C-22: DETERMINISTIC alias pick.  allRegisteredPrimOps is an
        // unordered_map (hash order), so a single first-wins pass would choose
        // between a bare `foo` and its `__foo` alias non-deterministically
        // across processes when their flags differ (the old "since reg is
        // ordered, first wins" comment was wrong — reg is NOT ordered).  Two
        // passes make the BARE name always win (TW semantics: `__foo` is the
        // alias of the primary `foo`); the `__`-prefixed alias only fills a gap
        // where no bare registration exists.
        for (auto & [poName, po] : reg) {                       // pass 1: bare
            if (poName.size() >= 2 && poName[0] == '_' && poName[1] == '_')
                continue;
            if (kV3InternalBuiltins.count(poName)) continue;    // C-22
            stripped.emplace(poName, std::cref(po));
        }
        for (auto & [poName, po] : reg) {                       // pass 2: __alias
            if (!(poName.size() >= 2 && poName[0] == '_' && poName[1] == '_'))
                continue;
            std::string key = poName.substr(2);
            if (kV3InternalBuiltins.count(key)) continue;       // C-22
            stripped.emplace(std::move(key), std::cref(po));     // no-op if bare present
        }
        uint32_t nVisible = static_cast<uint32_t>(stripped.size());
        Bindings * b = Alloc::allocBindings(nVisible);
        uint32_t i = 0;
        for (auto & [poName, poRef] : stripped) {
            const auto & po = poRef.get();
            Value v;
            // 2026-05-18 cc-wrapper bisection layer 3: arity-0 primops
            // are CONSTANTS (currentSystem, storeDir, nixVersion, ...).
            // lower.cc:826,2861 already pre-calls them at lower time
            // for static `builtins.X` references.  For DYNAMIC paths
            // (`with builtins; storeDir`, `inherit (builtins) storeDir;`,
            // `let b = builtins; in b.storeDir`), v3 used to leave them
            // as raw Tag::PrimOp values — which then leaked into
            // downstream toString / STR_CONCAT / etc. as tag=11 errors.
            // TW evaluates them implicitly because TW's force machinery
            // calls arity-0 primops on attr-select.
            //
            // Pre-call here once at vBuiltins lazy-init time.  By the
            // time getBuiltinsValue is first called (on OP_LIT_BUILTINS),
            // setNixEvalState has been wired (run.cc:105 / v3-eval main),
            // so the primop body has access to store + paths.
            //
            // Caveat: primops that need a live VMState (rather than just
            // nixEvalState) can't be pre-called here.  For nullary
            // primops in the current registry — currentSystem,
            // currentTime, storeDir, langVersion, nixVersion, nixPath,
            // nixVersion — none touch state.vm, so the pre-call is safe.
            if (po.arity == 0 && po.fn) {
                try {
                    EvalState evs;
                    evs.vm = nullptr;
                    evs.nixEvalState = getNixEvalState();
                    po.fn(evs, nullptr, v);
                } catch (...) {
                    // Some primops may legitimately fail at init time
                    // (e.g. `nixPath` if NIX_PATH is unset).  Fall back
                    // to the raw primop value; user-level access still
                    // gets the proper exception when actually used.
                    v.mkPrimOp(&po);
                }
            } else {
                v.mkPrimOp(&po);
            }
            SymbolId sid = ir::globalInternSymbol(poName);
            bindingsSetEntry(b, i, { sid, 0, v });  // Phase D
            ++i;
        }
        // Bindings expects entries sorted by SymbolId for binary search.
        std::sort(&b->entries[0], &b->entries[b->size],
            [](const auto & a, const auto & b){ return a.name < b.name; });
        Value v;
        v.mkAttrs(b);
        return v;
    }();
    g_vBuiltinsInitialized = true;
    return g_vBuiltins;
}

/// REVIEW §3 fold: shared dispatch wrapper.  Every entry point below
/// (run / runFunction / runFunctionWithUpvalues / runLambda) ends with
/// `try { dispatchLoop; clearBlackMarksOnException; return r; } catch
/// (...) { clearBlackMarksOnException; throw; }`.  Centralise so the
/// pattern is in one place; if the cleanup ever needs more steps,
/// they're added once.
[[gnu::always_inline]] inline Value dispatchAndClear(VMState & vm)
{
    try {
        Value r = dispatchLoop(vm, /*exitDepth=*/0);
        // WC-15 defensive: even on success, residual Black marks can
        // persist on the frame stack from incomplete sub-evals (e.g.
        // transitive thunk chains where intermediate frames don't
        // reach OP_RETURN).  Reset them so subsequent forces don't
        // see a stale Black mark.
        clearBlackMarksOnException(vm, 0);
        return r;
    } catch (...) {
        clearBlackMarksOnException(vm, 0);
        throw;
    }
}

/// STG-10 (#498) single-VM evaluation: when `activeV3VM()` is non-null
/// (= we're being re-entered from inside an outer v3 dispatchLoop via
/// a TW callback into the eval/call hook), push a frame onto that
/// existing VM and re-enter dispatchLoop with `exitDepth` set to the
/// frame count BEFORE we pushed.  When the new frame returns
/// (OP_RETURN brings frames back to exitDepth), dispatchLoop exits and
/// returns the body's result.
///
/// This eliminates fresh-VMState spawning at TW→v3 boundaries.  All v3
/// evaluation runs on a single VM per thread; thunks Black-marked by
/// any nested call are visible to all frames on the same VM (so the
/// existing forceValue cycle detection works correctly), and the
/// cross-VMState fresh-VMState pattern that surfaced in nixpkgs
/// hello.name under STG_KEEP_HOOKS=1 disappears at the source.
///
/// Mirrors the OP_CALL frame-setup contract: caller-provided arg goes
/// at slot 0 (when `arg` is non-null); caller-provided upvalues go on
/// a synthetic Closure; capturedWiths are pushed AFTER the frame's
/// withStackBase is set, so OP_WITH_LOOKUP picks them up.  Any
/// exception thrown from the body is caught here, the inner frames
/// are unwound via clearBlackMarksOnException scoped to exitDepth (so
/// the outer's existing Black marks are preserved), and the exception
/// is re-thrown to the caller of run/runFunction/runLambda.
inline Value runOnExistingVm(VMState & vm,
                              const CompilationUnit & cu,
                              const LambdaDescriptor & desc,
                              const Value * upvalues,
                              uint32_t nUpvalues,
                              ListVec * capturedWiths,
                              const Value * arg)
{
    const size_t exitDepth = vm.frames.size();

    // Synthesize a Closure (carries upvalues + captured-withs through
    // the frame for OP_GET_UPVALUE / pushCapturedWiths).  Allocated on
    // the v3 heap (Boehm GC); lives as long as the frame needs it.
    Closure * fakeClo = nullptr;
    if (nUpvalues > 0 || arg != nullptr) {
        // Even arg-only frames (no upvalues) need a Closure so the
        // dispatch loop's `closure` register has a valid descriptor
        // to query (e.g. for capturedWiths or selector fast-paths).
        // EXIT_GC_SPIRAL Day 6-8: use pool.
        fakeClo = Alloc::allocFakeClo(static_cast<uint16_t>(nUpvalues));
        fakeClo->desc = &desc;
        fakeClo->cu   = &cu;
        fakeClo->capturedWiths = capturedWiths;
        fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
        for (uint32_t i = 0; i < nUpvalues; ++i)
            fakeClo->upvalues[i] = upvalues[i];
        closurePostConstructBarrier(fakeClo);  // Phase D coverage
    }

    // Frame-setup mirrors OP_CALL (vm.cc:2316+) for arg-bearing calls
    // and runFunction's outer-with carriage layering for the no-arg
    // case (#416 layering rationale).
    const size_t base = vm.valueStack.size();
    vm.valueStack.resize(base + desc.nLocals);
    if (arg != nullptr)
        vm.valueStack[base] = *arg;

    const uint32_t newWithBase =
        static_cast<uint32_t>(vm.withStack.size());

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(base),
        .withStackBase = newWithBase,
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    // Re-enter dispatchLoop on the SAME VM.  2026-05-17: exception
    // cleanup happens INSIDE dispatchLoop's body-level try/catch
    // (which calls clearBlackMarksOnException before re-throwing).
    // Plain tail call enables compiler TCO — no try/catch in this
    // wrapper means runOnExistingVm's frame can be elided in favor
    // of dispatchLoop's directly.
    Value r = dispatchLoop(vm, exitDepth);
    if (fakeClo) Alloc::recycleFakeClo(fakeClo);
    return r;
}

Value run(const CompilationUnit & rootCu)
{
    // STG-10: re-use an active VM if available (TW→v3 re-entry while a
    // v3 dispatchLoop is on the C-stack).  `run` is the top-level
    // entry, so no arg / no upvalues; the body opens at
    // `rootCu.entryOffset` which lambdas[0]'s codeOffset points at by
    // construction (see CompilationUnit::entryOffset documentation).
    if (VMState * activeVm = activeV3VM(); activeVm && !rootCu.lambdas.empty()) {
        // The top-level "lambda" is the synthetic entry function the
        // emitter generates for the whole program; its codeOffset is
        // `rootCu.entryOffset` (verified equal in the emitter).  Push
        // a frame at that offset onto the existing VM.
        return runOnExistingVm(*activeVm, rootCu, rootCu.lambdas[0],
                                /*upvalues*/nullptr, /*nUp*/0,
                                /*capturedWiths*/nullptr,
                                /*arg*/nullptr);
    }

    VMState vm;
    // Generous initial reservations: deep-recursive workloads (fib,
    // ackermann, large fold chains) churn the value/frame stacks
    // many times.  Avoiding reallocation through the hot path is a
    // measurable win.
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    vm.frames.push_back(CallFrame{
        .cu = &rootCu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = rootCu.entryOffset,
        .stackBaseOffset = 0,
        .withStackBase = 0,
        .flags = 0,
    });

    if (!rootCu.lambdas.empty())
        vm.valueStack.resize(rootCu.lambdas[0].nLocals);

    return dispatchAndClear(vm);
}

/// CO-3: run an arbitrary FuncId in `cu` as if it were a thunk body.
/// No upvalues, no args.  Used by the forceValue cutover hook for
/// per-thunk-body Functions whose `nUpvalues == 0` — i.e., closed
/// thunks the lowerer recorded in `Module::subExprFuncs`.
Value runFunction(const CompilationUnit & cu, uint32_t funcIdx,
                   ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunction: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != 0)
        throw std::runtime_error("v3 runFunction: function expects upvalues; use runFunctionWithUpvalues");

    // STG-10: re-use the active VM if we're being re-entered from
    // inside an outer v3 dispatchLoop (TW→v3 re-entry).  Avoids
    // spawning a fresh VMState, which would create cross-VMState
    // Black-mark leaks for thunks visited by both VMs.
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                /*upvalues*/nullptr, /*nUp*/0,
                                capturedWiths, /*arg*/nullptr);
    }

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // #416: outer with-stack carriage.  Set the frame's withStackBase
    // BELOW the captured chain (i.e., at the current vm.withStack.size,
    // which is 0 here), then push.  withLookup walks from top of stack
    // DOWN to withStackBase (vm.cc:381), so anything ABOVE the base is
    // visible -- exactly what we want for the captured outer withs.
    // The function's own ir::With blocks push/pop on top of these.
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = nullptr,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    vm.valueStack.resize(desc.nLocals);

    return dispatchAndClear(vm);
}

/// CO-2 phase B: run a per-thunk Function with caller-provided
/// upvalues.  The forceValue cutover walks tree-walker's Env to
/// collect upvalue values, then calls here.  We synthesize a
/// Closure on the heap (allocated via Boehm GC; lives as long as
/// the call's frame), point the frame's closure to it, and run.
Value runFunctionWithUpvalues(const CompilationUnit & cu, uint32_t funcIdx,
                               const Value * upvalues, uint32_t nUpvalues,
                               ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runFunctionWithUpvalues: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != nUpvalues)
        throw std::runtime_error("v3 runFunctionWithUpvalues: nUpvalues mismatch");

    // STG-10: re-use the active VM if available (TW→v3 re-entry).
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                upvalues, nUpvalues,
                                capturedWiths, /*arg*/nullptr);
    }

    Closure * fakeClo = Alloc::allocFakeClo(static_cast<uint16_t>(nUpvalues));
    fakeClo->desc = &desc;
    fakeClo->cu   = &cu;
    // #416: also publish the captured chain on the closure so any
    // sub-call that re-uses fakeClo (e.g. via tail calls in the body)
    // sees the outer withs through pushCapturedWiths().
    fakeClo->capturedWiths = capturedWiths;
    fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
    for (uint32_t i = 0; i < nUpvalues; ++i)
        fakeClo->upvalues[i] = upvalues[i];
    closurePostConstructBarrier(fakeClo);  // Phase D coverage

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // #416: see runFunction() for the layering rationale -- frame's
    // withStackBase below the captured chain, captured chain pushed
    // on top, so OP_WITH_LOOKUP picks it up while the function's own
    // ir::With pushes layer above.
    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = 0,
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    vm.valueStack.resize(desc.nLocals);

    Value r = dispatchAndClear(vm);
    Alloc::recycleFakeClo(fakeClo);
    return r;
}

/// #426: invoke a v3 lambda body Function with one supplied argument.
/// Mirrors runFunctionWithUpvalues but seeds slot 0 with `arg` so the
/// body's OP_GET_LOCAL 0 reads the caller-supplied value -- exactly
/// matching what OP_CALL does at vm.cc:1323-1325.
Value runLambda(const CompilationUnit & cu, uint32_t funcIdx,
                Value arg,
                const Value * upvalues, uint32_t nUpvalues,
                ListVec * capturedWiths)
{
    if (funcIdx >= cu.lambdas.size())
        throw std::runtime_error("v3 runLambda: funcIdx out of range");
    const auto & desc = cu.lambdas[funcIdx];
    if (desc.nUpvalues != nUpvalues)
        throw std::runtime_error("v3 runLambda: nUpvalues mismatch");

    // Phase 1.2 identity-lambda fast path — mirrored from OP_CALL /
    // callClosure.  Body is `x: x`; return arg directly, no force
    // (the consumer drives force-on-demand per Nix semantics).
    if (__builtin_expect(desc.identityLambda, 0)) {
        return arg;
    }

    // #424: selector-lambda fast path also fires when the call hook
    // routes here (bypassing OP_CALL).  Same shape -- force arg,
    // project, return.  Skips the entire frame setup + dispatch loop.
    if (__builtin_expect(desc.selectorSym != 0, 0)) {
        allocStats().selectorLambdaCalls++;
        // The arg may still be a Thunk/App/Slot; force first.
        //
        // STG-10 (#498): re-use the active VM's forceValue rather than
        // a throwaway VMState.  The throwaway pattern was the original
        // source of cross-VMState Black-mark leaks: if `arg` is a slot
        // pointing at a thunk currently being forced on the outer VM,
        // forcing on a fresh VM throws BlackholeError that the outer
        // VM has no way to recover from.  Sharing the outer VM means
        // the cycle detection sees the in-flight thunk on the same
        // frame stack and the existing chase logic (vm.cc:5293+)
        // handles it correctly.
        //
        // REVIEW §3: wrap forceValue in try/catch + clearBlackMarks
        // so a thrown forceValue doesn't leave Black marks on the
        // (potentially throwaway) VMState's frames.  Mirror what the
        // main dispatchLoop does below.
        Value sArg = arg;
        if (sArg.isThunk() || sArg.isAppLike()
            || sArg.tag() == Tag::Slot) {
            if (VMState * activeVm = activeV3VM()) {
                size_t exitDepth = activeVm->frames.size();
                try {
                    sArg = forceValue(*activeVm, sArg);
                } catch (...) {
                    clearBlackMarksOnException(*activeVm, exitDepth);
                    throw;
                }
                clearBlackMarksOnException(*activeVm, exitDepth);
            } else {
                VMState forceVm;
                forceVm.valueStack.reserve(64);
                forceVm.frames.reserve(64);
                forceVm.withStack.reserve(8);
                try {
                    sArg = forceValue(forceVm, sArg);
                } catch (...) {
                    clearBlackMarksOnException(forceVm, 0);
                    throw;
                }
                clearBlackMarksOnException(forceVm, 0);
            }
        }
        if (!sArg.isAttrs() || !sArg.asAttrs())
            throw std::runtime_error(
                "v3 selector lambda: arg not an attrset");
        const Value * v = sArg.asAttrs()->lookup(desc.selectorSym);
        if (!v)
            throw std::runtime_error(
                "v3 selector lambda: missing attr");
        return *v;
    }

    // STG-10: re-use the active VM if available (TW→v3 re-entry).
    // Slot identity in `arg` is preserved through runOnExistingVm's
    // OP_CALL-shaped frame setup.
    if (VMState * activeVm = activeV3VM()) {
        return runOnExistingVm(*activeVm, cu, desc,
                                upvalues, nUpvalues,
                                capturedWiths, /*arg*/&arg);
    }

    Closure * fakeClo = Alloc::allocFakeClo(static_cast<uint16_t>(nUpvalues));
    fakeClo->desc = &desc;
    fakeClo->cu   = &cu;
    fakeClo->capturedWiths = capturedWiths;
    fakeClo->nUpvalues = static_cast<uint16_t>(nUpvalues);
    for (uint32_t i = 0; i < nUpvalues; ++i)
        fakeClo->upvalues[i] = upvalues[i];
    closurePostConstructBarrier(fakeClo);  // Phase D coverage

    VMState vm;
    vm.valueStack.reserve(64 * 1024);
    vm.frames.reserve(4096);
    vm.withStack.reserve(64);

    // Mirror OP_CALL's frame setup: nLocals slots reserved, slot 0 = arg.
    size_t base = vm.valueStack.size();
    vm.valueStack.resize(base + desc.nLocals);
    vm.valueStack[base] = arg;

    vm.frames.push_back(CallFrame{
        .cu = &cu,
        .closure = fakeClo,
        .thunk = nullptr,
        .ip = desc.codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(base),
        .withStackBase = static_cast<uint32_t>(vm.withStack.size()),
        .flags = 0,
    });
    pushCapturedWiths(vm, capturedWiths);

    Value r = dispatchAndClear(vm);
    Alloc::recycleFakeClo(fakeClo);
    return r;
}

// v3ValueTypeName, v3ThunkTracePos and v3DescTracePos: hoisted above
// dispatchLoop so OP_FORCE / OP_RETURN can call them.

Value forceValue(VMState & vm, Value v)
{
    // A8 (2026-05-13): the previous Phase-A7 hard-abort at VM frame
    // depth 2000 was a stopgap to prevent OOM during C-stack overflow.
    // With the iterative-force refactor (single-arg branch opcodes +
    // OP_CALL_PRIMOP / V3_IS_OP / OP_HEAD / OP_TAIL / OP_LENGTH /
    // OP_ELEM_AT / OP_LIST_CONCAT / OP_ATTRS_UPDATE / OP_STR_CONCAT
    // now driving forces through the VM frame stack via
    // op_force_slow + writeback, rather than C-recursive forceValue
    // calls), C-stack growth is bounded by the remaining direct
    // forceValue call sites (OP_CALL's `fun` force, callClosure's
    // primop arg loop, helpers like valueEqual).  Those sites are
    // shallow per-opcode-call, so VM frame depth dominates and the
    // existing `kMaxCallDepth` (5000) clean-throw guard at
    // dispatchLoop:3160 / 4662 and forceValue:8526 is sufficient.
    //
    // Opt-in logging via NIX_V3_LOG_DEPTH=1 (peak every +100 frames).
    {
        static const bool s_logDepth =
            std::getenv("NIX_V3_LOG_DEPTH") != nullptr;
        if (__builtin_expect(s_logDepth, 0)) {
            static thread_local size_t peakDepth = 0;
            if (vm.frames.size() > peakDepth) {
                peakDepth = vm.frames.size();
                if (peakDepth > 500 && (peakDepth % 100) == 0) {
                    std::fprintf(stderr,
                        "v3 forceValue depth=%zu (peak)\n", peakDepth);
                    std::fflush(stderr);
                }
            }
            // A12b diagnostic: at NIX_V3_DBG_DEEP_FRAMES (single
            // threshold), dump every Nth distinct frame so we can see
            // what's filling vm.frames when we approach kMaxCallDepth.
            // One-shot: fires on the first frame size at or beyond the
            // threshold, then never again in this thread.
            static const char * s_dbgDeep = std::getenv("NIX_V3_DBG_DEEP_FRAMES");
            if (__builtin_expect(s_dbgDeep != nullptr, 0)) {
                static thread_local bool fired = false;
                size_t threshold = (size_t)std::atoi(s_dbgDeep);
                if (!fired && threshold > 0 && vm.frames.size() >= threshold) {
                    fired = true;
                    std::fprintf(stderr,
                        "v3 NIX_V3_DBG_DEEP_FRAMES: depth=%zu, dumping every-50th frame:\n",
                        vm.frames.size());
                    for (size_t i = 0; i < vm.frames.size(); i += 50) {
                        const auto & fr = vm.frames[i];
                        const LambdaDescriptor * d = nullptr;
                        if (fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole))
                            d = fr.thunk->suspended.desc;
                        else if (fr.closure) d = fr.closure->desc;
                        const char * nm = d && !d->name.empty()
                            ? d->name.c_str() : "<?>";
                        unsigned codeOff = d ? d->codeOffset : 0u;
                        std::fprintf(stderr,
                            "  fr[%zu]: %s codeOff=%u ip=%u thunk=%p flags=0x%x\n",
                            i, nm, codeOff, fr.ip, (void *)fr.thunk,
                            (unsigned)fr.flags);
                    }
                    // Also dump top-12 frames so we can see the immediate
                    // recursion pattern.
                    std::fprintf(stderr, "  ---- TOP 12 ----\n");
                    size_t n = vm.frames.size();
                    size_t lo = n > 12 ? n - 12 : 0;
                    for (size_t i = lo; i < n; ++i) {
                        const auto & fr = vm.frames[i];
                        const LambdaDescriptor * d = nullptr;
                        if (fr.thunk
                            && (fr.thunk->state == ThunkState::Suspended
                                || fr.thunk->state == ThunkState::Blackhole))
                            d = fr.thunk->suspended.desc;
                        else if (fr.closure) d = fr.closure->desc;
                        const char * nm = d && !d->name.empty()
                            ? d->name.c_str() : "<?>";
                        unsigned codeOff = d ? d->codeOffset : 0u;
                        std::fprintf(stderr,
                            "  TOP fr[%zu]: %s codeOff=%u ip=%u\n",
                            i, nm, codeOff, fr.ip);
                    }
                    std::fflush(stderr);
                }
            }
        }
    }
    // STG-12 (#498) diagnostic: log the call site (current top frame
    // CU + ip) when forceValue is invoked with an input that, after
    // chase, lands on a Black thunk on this VM's frames.  That tells
    // us which opcode handler is calling forceValue with the cycle
    // source.  V3_DBG_FORCE_CALLSITE=1 to enable.  Cached because
    // forceValue is called on every OP_FORCE / get-local-force-on-
    // thunk slow path; the per-call getenv was visible in profiling.
    static const bool s_dbgForceCallsite =
        std::getenv("V3_DBG_FORCE_CALLSITE") != nullptr;
    if (__builtin_expect(s_dbgForceCallsite, 0)) {
        Value chase = v;
        Thunk * blackOnFrames = nullptr;
        // Record chase trace for printing.
        constexpr int kTraceMax = 8;
        Tag traceTag[kTraceMax] = {};
        void * tracePtr[kTraceMax] = {};
        int traceCount = 0;
        auto recordHop = [&](Value val) {
            if (traceCount < kTraceMax) {
                traceTag[traceCount] = val.tag();
                tracePtr[traceCount] = val.asThunk();  // any pointer
                ++traceCount;
            }
        };
        recordHop(chase);
        for (int hops = 0; hops < 16; ++hops) {
            if (chase.tag() == Tag::Slot && chase.asSlot()) {
                chase = *chase.asSlot();
                recordHop(chase);
            } else if (chase.tag() == Tag::Thunk
                       && chase.asThunk()) {
                Thunk * th = chase.asThunk();
                if (th->state == ThunkState::Evaluated) {
                    chase = th->evaluated;
                    recordHop(chase);
                } else if (th->state == ThunkState::Blackhole) {
                    // Black on this VM's frames?
                    for (size_t i = 0; i < vm.frames.size(); ++i) {
                        if (vm.frames[i].thunk == th) {
                            blackOnFrames = th; break;
                        }
                    }
                    break;
                } else {
                    break;
                }
            } else break;
        }
        if (blackOnFrames) {
            const LambdaDescriptor * bd = blackOnFrames->suspended.desc;
            void * caller = __builtin_return_address(0);
            std::fprintf(stderr,
                "v3 forceValue → BLACK on-frames thunk=%p name=%s frames=%zu caller=%p\n",
                (void *)blackOnFrames,
                bd && !bd->name.empty() ? bd->name.c_str() : "<?>",
                vm.frames.size(), caller);
            // C-stack backtrace via execinfo so we can see who called
            // public forceValue.
            {
                void * cstack[32];
                int nFrames = ::backtrace(cstack, 32);
                char ** syms = ::backtrace_symbols(cstack, nFrames);
                std::fprintf(stderr, "  C-stack (%d frames):\n", nFrames);
                for (int k = 0; k < nFrames && k < 12; ++k)
                    std::fprintf(stderr, "    %s\n", syms[k]);
                if (syms) std::free(syms);
            }
            std::fprintf(stderr, "  chase trace (%d hops):", traceCount);
            for (int k = 0; k < traceCount; ++k) {
                std::fprintf(stderr, " [%d:tag=%d ptr=%p]",
                    k, (int)traceTag[k], tracePtr[k]);
            }
            std::fprintf(stderr, "\n");
            // Find which frame holds the BLACK thunk so we can show
            // it explicitly in the trace.
            size_t blackIdx = (size_t)-1;
            for (size_t i = 0; i < vm.frames.size(); ++i) {
                if (vm.frames[i].thunk == blackOnFrames) {
                    blackIdx = i; break;
                }
            }
            std::fprintf(stderr, "  blackIdx=%zd\n", (ssize_t)blackIdx);
            // Backtrace: show the top 12 frames so we can identify the
            // forceValue caller chain.  Always include the BLACK frame
            // even if outside the window.
            size_t n = vm.frames.size();
            size_t lo = n > 12 ? n - 12 : 0;
            if (blackIdx != (size_t)-1 && blackIdx < lo) lo = blackIdx;
            for (size_t i = n; i-- > lo;) {
                const auto & fr = vm.frames[i];
                const LambdaDescriptor * d = nullptr;
                if (fr.thunk
                    && (fr.thunk->state == ThunkState::Suspended
                        || fr.thunk->state == ThunkState::Blackhole))
                    d = fr.thunk->suspended.desc;
                else if (fr.closure) d = fr.closure->desc;
                Instruction prev = (fr.cu && fr.ip > 0
                                    && fr.ip <= fr.cu->code.size())
                    ? fr.cu->code[fr.ip - 1] : 0;
                std::fprintf(stderr,
                    "  fr[%zu]: %s ip=%u prev-op=0x%02x cu=%p flags=0x%x thunk=%p"
                    " codeOff=%u%s\n",
                    i,
                    d && !d->name.empty() ? d->name.c_str() : "<?>",
                    fr.ip, (unsigned)((prev >> 24) & 0xFF),
                    (const void *)fr.cu,
                    (unsigned)fr.flags, (void *)fr.thunk,
                    d ? d->codeOffset : 0u,
                    fr.thunk == blackOnFrames ? " ← BLACK" : "");
                if (fr.cu && fr.ip > 0
                    && fr.ip <= fr.cu->code.size()) {
                    // Find the enclosing LambdaDescriptor by scanning the
                    // cu's funcs (codeOffset closest to but not exceeding
                    // fr.ip).
                    const LambdaDescriptor * encl = nullptr;
                    for (const auto & ld : fr.cu->lambdas) {
                        if (ld.codeOffset <= fr.ip
                            && (!encl || ld.codeOffset > encl->codeOffset))
                            encl = &ld;
                    }
                    std::fprintf(stderr, "    enclosing-lambda: %s codeOff=%u nL=%u\n",
                        encl
                            ? (encl->name.empty() ? "<?>" : encl->name.c_str())
                            : "<no-funcs>",
                        encl ? encl->codeOffset : 0u,
                        encl ? encl->nLocals : 0u);
                    // For fr[16] only: dump the full body of the
                    // enclosing lambda so we can see where ip=157 sits.
                    static bool dumped_full = false;
                    if (encl && i == n - 2 && !dumped_full) {
                        dumped_full = true;
                        // Find end of this lambda (start of next lambda
                        // by codeOffset).
                        uint32_t bodyEnd = (uint32_t)fr.cu->code.size();
                        for (const auto & ld : fr.cu->lambdas) {
                            if (ld.codeOffset > encl->codeOffset
                                && ld.codeOffset < bodyEnd)
                                bodyEnd = ld.codeOffset;
                        }
                        std::fprintf(stderr,
                            "    full body codeOff=%u..%u:\n",
                            encl->codeOffset, bodyEnd);
                        for (uint32_t k = encl->codeOffset; k < bodyEnd; ++k) {
                            Instruction w = fr.cu->code[k];
                            std::fprintf(stderr,
                                "      [%u:%02x %06x]%s\n",
                                k, (unsigned)((w >> 24) & 0xFF),
                                (unsigned)(w & 0xFFFFFF),
                                k == fr.ip ? "  <-- ip" : "");
                        }
                        // Dump symbols 1176 (prev) and 138 (the one referenced
                        // in x's body — see fr[5]'s code).
                        const auto & tbl = ir::globalSymbolTable();
                        std::fprintf(stderr, "    sym 1176 = %s   sym 138 = %s\n",
                            1176u < tbl.size() ? tbl[1176].c_str() : "<?>",
                            138u < tbl.size() ? tbl[138].c_str() : "<?>");
                    }
                    int lo = (int)fr.ip - 6; if (lo < 0) lo = 0;
                    int hi = (int)fr.ip + 4;
                    if (hi > (int)fr.cu->code.size())
                        hi = (int)fr.cu->code.size();
                    std::fprintf(stderr, "    code: ");
                    for (int k = lo; k < hi; ++k) {
                        Instruction w = fr.cu->code[k];
                        std::fprintf(stderr, "[%d:%02x %06x]%s",
                            k, (unsigned)((w >> 24) & 0xFF),
                            (unsigned)(w & 0xFFFFFF),
                            k == (int)fr.ip ? "*" : " ");
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            const auto & fr = vm.frames.back();
            const LambdaDescriptor * d = nullptr;
            if (fr.thunk
                && (fr.thunk->state == ThunkState::Suspended
                    || fr.thunk->state == ThunkState::Blackhole))
                d = fr.thunk->suspended.desc;
            else if (fr.closure) d = fr.closure->desc;
            // Print the opcode at ip-1 (the op that was just running)
            // and ip (next op).
            if (fr.cu && fr.ip > 0
                && fr.ip <= fr.cu->code.size()) {
                // Opcode = top 8 bits of the 32-bit Instruction word.
                Instruction prev = fr.ip > 0 ? fr.cu->code[fr.ip - 1] : 0;
                Instruction curr = fr.ip < fr.cu->code.size()
                    ? fr.cu->code[fr.ip] : 0;
                std::fprintf(stderr,
                    "  prev op (ip-1=%u) = 0x%02x  next op (ip=%u) = 0x%02x\n",
                    fr.ip - 1, (unsigned)((prev >> 24) & 0xFF),
                    fr.ip, (unsigned)((curr >> 24) & 0xFF));
                // Also dump a small window so we can see the surrounding
                // instructions if op alone isn't enough.
                std::fprintf(stderr, "  ip window: ");
                int lo = (int)fr.ip - 3; if (lo < 0) lo = 0;
                int hi = (int)fr.ip + 3;
                if (hi > (int)fr.cu->code.size())
                    hi = (int)fr.cu->code.size();
                for (int k = lo; k < hi; ++k) {
                    Instruction w = fr.cu->code[k];
                    std::fprintf(stderr, "[%d:%02x %06x]%s",
                        k, (unsigned)((w >> 24) & 0xFF),
                        (unsigned)(w & 0xFFFFFF),
                        k == (int)fr.ip ? "*" : " ");
                }
                std::fprintf(stderr, "\n");
            }
        }
    }
    // Track the FIRST slot we passed through so we can memoize the
    // final result back into it.  Mirrors tree-walker's behavior:
    // `state.forceValue(*v2)` mutates the slot directly, so a future
    // read of the same slot sees the resolved value (no need to walk
    // the thunk chain again).  Without memoization, every withLookup
    // through a Tag::Slot would re-force the underlying thunk.
    Value * memoSlot = nullptr;
    // #558 Phase 4 follow-up: path compression for Evaluated thunk
    // chains.  When we walk through Tag::Thunk → Tag::Thunk → ... in
    // the Evaluated state, every consumer that holds a pointer to the
    // FIRST thunk pays the full chase cost on every force.  Record up
    // to kCompressMax thunks and, after resolution, write the final
    // WHNF back to each so future forces resolve in one hop.  Cheap:
    // 16 pointers on the C-stack, written only on success.  Opt-out
    // via NIX_V3_NO_PATH_COMPRESS=1.
    static const bool s_noPathCompress =
        std::getenv("NIX_V3_NO_PATH_COMPRESS") != nullptr;
    constexpr int kCompressMax = 16;
    Thunk * compressChain[kCompressMax];
    int compressCount = 0;
    // Iteration bound: detect infinite chases through Tag::Slot →
    // Tag::Thunk(Eval=Slot→...) cycles that arise from self-referential
    // let-rec patterns like `let x = x; in x` or `let x = y; y = x; in x`.
    // The Black-state check normally catches direct recursion, but
    // SECD-style indirections (Slot→Eval→Slot) can chase forever
    // without re-entering the Black thunk.  16 iterations is well
    // beyond any realistic indirection chain (≤4 in practice for
    // recref+thunkify+slot+memo) and only fires on pathological
    // cycles.
    // See kMaxIndirectionChase / kMaxCallDepth at the top of the
    // anonymous namespace for rationale.
    int chaseIters = 0;
    // V3_DBG_CHASE: optional ring-buffer of recent (tag, ptr) pairs so
    // we can dump the chain shape if the limit fires.  Cheap when
    // disabled (single static check on hot path).
    static const bool s_dbg_chase = std::getenv("V3_DBG_CHASE") != nullptr;
    constexpr int kRingSize = 32;
    Tag ringTag[kRingSize] = {};
    void * ringPtr[kRingSize] = {};
    int ringIdx = 0;
    // Loop until WHNF: a thunk's body might itself yield a thunk
    // (e.g., `let inherit outer; in outer` returns the outer thunk),
    // and we want to chase the chain until we land on a real value.
    while (true) {
        if (__builtin_expect(s_dbg_chase, 0)) {
            ringTag[ringIdx % kRingSize] = v.tag();
            void * p = nullptr;
            if (v.tag() == Tag::Thunk) p = v.asThunk();
            else if (v.tag() == Tag::Slot) p = v.asSlot();
            else if (v.isAppLike()) p = v.asPair();
            ringPtr[ringIdx % kRingSize] = p;
            ringIdx++;
        }
        if (__builtin_expect(++chaseIters > kMaxIndirectionChase, 0)) {
            if (s_dbg_chase) {
                std::fprintf(stderr,
                    "v3 chase-cycle limit %d hit; last %d steps:\n",
                    kMaxIndirectionChase, kRingSize);
                int start = ringIdx > kRingSize ? ringIdx - kRingSize : 0;
                for (int i = start; i < ringIdx; ++i) {
                    int slot = i % kRingSize;
                    std::fprintf(stderr,
                        "  step[%d]: tag=%d ptr=%p", i,
                        (int)ringTag[slot], ringPtr[slot]);
                    // For Thunks, additionally show their state +
                    // evaluated tag so we can see "Evaluated → Thunk →
                    // Evaluated → ...".
                    if (ringTag[slot] == Tag::Thunk && ringPtr[slot]) {
                        auto * t = static_cast<Thunk *>(ringPtr[slot]);
                        std::fprintf(stderr, " state=%d", (int)t->state);
                        if (t->state == ThunkState::Evaluated)
                            std::fprintf(stderr, " evaluated.tag=%d",
                                (int)t->evaluated.tag());
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            // #680 — match TW's `InfiniteRecursionError` text
            // (libexpr/eval.cc:2594).  Internal cause (chase-cycle
            // through Tag::Slot/Thunk) is debug-only; not user-facing.
            throw std::runtime_error("infinite recursion encountered");
        }
        // Same call-depth guard — `let x = x; in x` lands here in
        // a C++ recursion via dispatchLoop → forceValue → dispatchLoop
        // and never grows through the bytecode-level OP_CALL/OP_FORCE
        // guards.  Match those guards.
        if (__builtin_expect(vm.frames.size() >= kMaxCallDepth, 0))
            throw std::runtime_error("v3 forceValue: stack overflow; call depth exceeded "
                                      + std::to_string(kMaxCallDepth));
        // Tag::Slot — SECD-style indirection.  The slot pointer
        // references another stable Value that gets mutated when its
        // let-rec body publishes a result.  Dereference and continue
        // chasing.  See `with self;` semantics in Tag::Slot's docstring
        // for why this matters: sub-thunks captured under `with self;`
        // must observe the latest slot contents at force time, not a
        // snapshot from when the with-stack was pushed.
        if (v.tag() == Tag::Slot) {
            Value * p = v.asSlot();
            if (!p) throw std::runtime_error("v3 forceValue: null slot pointer");
            // Remember the OUTERMOST slot for memoization.  If we
            // pass through multiple Tag::Slot indirections (chained),
            // memoize at the first one — its slot is what consumers
            // hold.  Inner slots get memoized by their own future
            // forceValue calls.
            if (!memoSlot) memoSlot = p;
            v = *p;
            continue;
        }
        // Tag::App is a deferred application — force it by actually
        // applying.  Used by primops like mapAttrs that build lazy
        // entries: each entry is `App(fn, arg)` and we materialize on
        // demand.  `left` may itself be an App (e.g. mapAttrs builds
        // App(App(fn, name), value) for curried application).
        //
        // Mirrors OP_FORCE's iterative spine walk (vm.cc:3823): walk
        // down the left chain into a small `rights` buffer, then
        // force the leaf and apply the rights in source order.  The
        // pre-fix recursive `forceValue(vm, left)` pattern blew C-stack
        // on long mapAttrs/map chains in nixpkgs; this version bottoms
        // out the recursion at the leaf rather than at every App
        // level.  `rights.reserve(8)` keeps typical depths on the
        // stack — std::vector only grows beyond that.
        //
        // 2026-05-18: App-result memoization.  After resolving the
        // App chain to a WHNF result, store the result in the OUTERMOST
        // App's `pair->evaluated` field.  On the next force of the
        // same App pointer, the top-of-loop fast-path below returns
        // the cached result directly without re-running the lambda.
        // Closes the H3 memoization gap (extendDerivation outputsList
        // at customisation.nix:409 forced 64K times pre-fix).
        if (v.isAppLike()) {
            // eval/apply (#3): an under-applied closure-PAP is WHNF (a partial
            // application) — return it as-is rather than evaluating it as a
            // deferred call (which would enter the arity-N body short of args).
            if (isUnderappliedClosurePap(v)) return v;
            // EXIT_GC_SPIRAL Week 1 Day 9-11 (2026-05-29):
            // Tag::App3 = 3-arg App stored as ONE ValuePair (saves
            // one 32 B Pair vs the legacy 2-pair encoding for
            // mapAttrs / zipAttrsWith).  In the spine walk the App3
            // pair contributes TWO rights: arg2 (`evaluated`) and
            // arg1 (`right`).  Memoization is disabled for App3
            // (single-shot pattern by construction in mapAttrs /
            // zipAttrsWith — no re-entry); only Tag::App memoizes.
            //
            // gate: NIX_V3_NO_APP_MEMO — disables the App-result memo
            // for A/B measurement.  Retire when the memo is stable
            // (parity + non-regression demonstrated across the bench
            // corpus + cardano-node nixpkgs eval).
            static const bool s_noAppMemo =
                std::getenv("NIX_V3_NO_APP_MEMO") != nullptr;
            bool outerIsAppLike = v.isAppLike();
            ValuePair * outerPair = outerIsAppLike ? v.asPair() : nullptr;
            // Memo-hit fast path: outerPair->evaluated holds the
            // previously-resolved result.  Tag::Uninitialized (== 0)
            // is the sentinel meaning "not yet resolved".  2026-05-30:
            // Tag::App3 also memoizes — it now uses `pair->third` for
            // arg2 (separate from `evaluated`), so the memo slot is
            // available for both pair tags.
            if (__builtin_expect(!s_noAppMemo
                && outerIsAppLike
                && outerPair
                && outerPair->evaluated.tag() != Tag::Uninitialized, 1))
            {
                v = outerPair->evaluated;
                continue;
            }
            // P-4 (CODEBASE_REVIEW_2026-06-11): inline small buffer (observed
            // spine depth ≤4) avoids a per-force heap std::vector alloc on this
            // cold (non-memoized) App-force path; overflow to heap for deep
            // spines.  Stack-local → re-entrant-safe (forceValue re-enters).
            // Spine-walk: descend the left chain, pushing rights so the
            // apply-loop below (nRights-1 down to 0) applies them in source
            // order.  App3 pushes arg2 first, then arg1, so arg1 is applied
            // before arg2 (the curried semantics).
            constexpr size_t kInlineRights = 16;
            Value inlineRights[kInlineRights];
            std::vector<Value> overflowRights;
            size_t nRights = 0;
            auto pushRight = [&](const Value & val) {
                if (nRights < kInlineRights) inlineRights[nRights] = val;
                else overflowRights.push_back(val);
                ++nRights;
            };
            auto rightAt = [&](size_t i) -> const Value & {
                return i < kInlineRights ? inlineRights[i]
                                         : overflowRights[i - kInlineRights];
            };
            while (v.isAppLike()) {
                ValuePair * p = v.asPair();
                if (v.tag() == Tag::App3)
                    pushRight(p->third);   // arg2 (separate slot from `evaluated`)
                pushRight(p->right);
                v = p->left;
            }
            if (v.tag() == Tag::Slot
                || v.tag() == Tag::Thunk
                || v.isAppLike())
                v = forceValue(vm, v);
            for (size_t i = nRights; i > 0; --i)
                v = callClosure(vm, v, rightAt(i - 1));
            // Memoize: store the result in the outermost App / App3
            // pair's evaluated field so the next force short-circuits.
            // Defensive: avoid writing back a non-WHNF result.
            if (!s_noAppMemo && outerIsAppLike && outerPair) {
                Tag rt = v.tag();
                if (rt != Tag::Thunk && rt != Tag::App && rt != Tag::App3
                    && rt != Tag::Slot
                    && rt != Tag::Uninitialized && rt != Tag::Blackhole)
                    pairSetEvaluated(outerPair, v);  // Phase D barrier
            }
            continue;
        }
        if (!v.isThunk()) break;
        Thunk * t = v.asThunk();
        if (t->state == ThunkState::Evaluated) {
            // #558 Phase 4 follow-up: record this thunk for path
            // compression below.  After the chase resolves to a final
            // WHNF, we rewrite each recorded thunk's `evaluated` slot
            // so future forces hit in O(1) instead of walking the same
            // chain again.  Cap at kCompressMax to bound the chain
            // memory; longer chains are rare and the cap is well above
            // any observed depth (≤4 in practice for nested let-rec).
            if (!s_noPathCompress && compressCount < kCompressMax)
                compressChain[compressCount++] = t;
            v = t->evaluated;
            continue;
        }
        if (t->state == ThunkState::Blackhole) {
            // #458 lambda-skip leaked-Black recovery (opt-in).  When the
            // thunk's Black state was set by a previous VMState that has
            // since unwound, the current VMState's frame stack does NOT
            // contain the thunk.  Treat this as a leaked mark — reset to
            // Suspended and let the chase fall through to the Suspended
            // handler below to re-run the body idempotently.  Gated
            // behind NIX_V3_LEAKED_BLACK_RECOVER=1 because the prior
            // attempt (memory file) turned BlackHole into a chase cycle
            // when the leak interpretation was wrong on a real cycle.
            // With the slot-capture redesign + RecBuildSlot in place,
            // genuine self-reference cycles are sidestepped earlier (via
            // Tag::Slot deref instead of forcing a wrap thunk), so the
            // leak interpretation should be correct in more cases.
            static const bool s_recover =
                std::getenv("NIX_V3_LEAKED_BLACK_RECOVER") != nullptr;
            if (s_recover) {
                bool onCurrentFrames = false;
                for (size_t i = 0; i < vm.frames.size(); ++i) {
                    if (vm.frames[i].thunk == t) { onCurrentFrames = true; break; }
                }
                if (!onCurrentFrames) {
                    // Per-(VMState, Thunk) re-entry counter: if recovery
                    // recurs on the same thunk pointer N times in a row
                    // from the same VMState, the leak interpretation is
                    // wrong (it's a real cycle).  Throw the BlackholeError
                    // through the normal path instead of looping.
                    //
                    // REVIEW_2026-05-06b C1: was a thread_local static
                    // map keyed only by Thunk*.  Across multiple top-level
                    // evals on the same thread, stale counts could leak
                    // (a Thunk* freed in eval A whose address gets
                    // reused for a NEW thunk in eval B inherits A's
                    // count, mis-classifying a fresh leak as a "real
                    // cycle").  Now keyed by (VMState*, Thunk*) so each
                    // VMState has its own counter space.  Bounded blow-
                    // up: stale entries from destroyed VMStates linger
                    // until they hit the 4-attempt cap and are erased.
                    struct KeyHash {
                        size_t operator()(const std::pair<const void *, Thunk *> & p) const noexcept {
                            return std::hash<const void *>{}(p.first)
                                 ^ (std::hash<Thunk *>{}(p.second) << 1);
                        }
                    };
                    static thread_local std::unordered_map<
                        std::pair<const void *, Thunk *>, int, KeyHash>
                        reentryCount;
                    auto key = std::make_pair(
                        static_cast<const void *>(&vm), t);
                    int & cnt = reentryCount[key];
                    if (++cnt > 4) {
                        reentryCount.erase(key);
                        // Fall through to throw below.
                    } else {
                        static const bool s_dbgRec =
                            std::getenv("V3_DBG_LEAKED_BLACK") != nullptr;
                        if (s_dbgRec) std::fprintf(stderr,
                            "v3 forceValue: leaked-Black recover thunk=%p "
                            "(vm=%p frames=%zu, not on stack, attempt %d) -> Suspended\n",
                            (void*)t, (void*)&vm, vm.frames.size(), cnt);
                        t->state = ThunkState::Suspended;
                        continue;
                    }
                }
            }
            // Same diagnostic as OP_FORCE's blackhole path — V3_DBG_OPCYCLE
            // dumps the frame stack so the cycle source is visible.
            static const bool s_dbg = std::getenv("V3_DBG_OPCYCLE") != nullptr;
            if (s_dbg) {
                // suspended.desc is only valid for Suspended/Blackhole
                // thunks — reading it on Bridge/Evaluated thunks accesses
                // the wrong union variant and the resulting `desc->name`
                // segfaults silently, terminating the dump after one
                // frame.  Guard the read.
                auto frameInfo = [&](Thunk * th, const Closure * cl, uint32_t /*fip*/) -> std::string {
                    const LambdaDescriptor * desc = nullptr;
                    if (th && (th->state == ThunkState::Suspended
                            || th->state == ThunkState::Blackhole))
                        desc = th->suspended.desc;
                    else if (cl) desc = cl->desc;
                    if (!desc) return "<closure-body>";
                    char buf[512];
                    const PosSnapshot * ps =
                        desc->posHandle ? resolvePosSnapshot(desc->posHandle) : nullptr;
                    if (ps && !ps->file.empty()) {
                        std::snprintf(buf, sizeof buf,
                            "%s code=[%u..) nUp=%u nLocals=%u @ %s:%u:%u",
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            desc->codeOffset, desc->nUpvalues, desc->nLocals,
                            ps->file.c_str(), ps->line, ps->column);
                    } else {
                        std::snprintf(buf, sizeof buf,
                            "%s code=[%u..) nUp=%u nLocals=%u",
                            !desc->name.empty() ? desc->name.c_str() : "<anon>",
                            desc->codeOffset, desc->nUpvalues, desc->nLocals);
                    }
                    return buf;
                };
                const LambdaDescriptor * tdesc =
                    (t && (t->state == ThunkState::Suspended
                        || t->state == ThunkState::Blackhole))
                    ? t->suspended.desc : nullptr;
                const PosSnapshot * tps = (tdesc && tdesc->posHandle)
                    ? resolvePosSnapshot(tdesc->posHandle) : nullptr;
                std::fprintf(stderr,
                    "v3 forceValue Black thunk=%p frames=%zu desc.name=%s desc.code=%u @ %s:%u:%u\n",
                    (void*)t, vm.frames.size(),
                    (tdesc && !tdesc->name.empty()) ? tdesc->name.c_str() : "<anon>",
                    tdesc ? tdesc->codeOffset : 0,
                    (tps && !tps->file.empty()) ? tps->file.c_str() : "<no-pos>",
                    tps ? tps->line : 0u, tps ? tps->column : 0u);
                size_t lim = vm.frames.size();
                ssize_t blackIdx = -1;
                for (size_t i = lim; i > 0; --i) {
                    const auto & fr = vm.frames[i - 1];
                    bool isBlack = (fr.thunk == t);
                    if (isBlack) blackIdx = (ssize_t)(i - 1);
                    std::fprintf(stderr,
                        "  frame[%zu]:%s %s flags=%u ip=%u\n",
                        i - 1, isBlack ? " <-BLACK" : "",
                        frameInfo(fr.thunk, fr.closure, fr.ip).c_str(),
                        (unsigned)fr.flags, fr.ip);
                }
                static const bool s_dbg_disasm =
                    std::getenv("V3_DBG_OPCYCLE_DISASM") != nullptr;
                if (s_dbg_disasm && blackIdx >= 0) {
                    // Dump the BLACK frame's prologue (start of body)
                    // through current ip — captures every OP_FORCE the
                    // body ran before re-entering itself.
                    const auto & fr = vm.frames[blackIdx];
                    if (fr.cu) {
                        const LambdaDescriptor * desc = nullptr;
                        if (fr.thunk)
                            desc = fr.thunk->suspended.desc;
                        else if (fr.closure)
                            desc = fr.closure->desc;
                        if (desc) {
                            uint32_t lo = desc->codeOffset;
                            uint32_t hi = fr.ip + 8;
                            std::fprintf(stderr,
                                "  BLACK frame[%zd] disasm [%u..%u) (prologue→ip):\n",
                                blackIdx, lo, hi);
                            disassembleWindow(stderr, *fr.cu, lo, hi);
                        }
                    }
                    // Also dump the innermost frame's prologue → ip.
                    const auto & inner = vm.frames.back();
                    if (inner.cu) {
                        const LambdaDescriptor * idesc = nullptr;
                        if (inner.thunk)
                            idesc = inner.thunk->suspended.desc;
                        else if (inner.closure)
                            idesc = inner.closure->desc;
                        if (idesc) {
                            uint32_t lo = idesc->codeOffset;
                            uint32_t hi = inner.ip + 8;
                            std::fprintf(stderr,
                                "  INNER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                lim - 1, lo, hi);
                            disassembleWindow(stderr, *inner.cu, lo, hi);
                        }
                    }
                    // Also dump frame[33] — caller of innermost.  Often
                    // the App's `left` was a closure call return, which
                    // is the actual divergence source.
                    if (lim >= 2) {
                        const auto & f33 = vm.frames[lim - 2];
                        if (f33.cu) {
                            const LambdaDescriptor * d33 = nullptr;
                            if (f33.thunk)
                                d33 = f33.thunk->suspended.desc;
                            else if (f33.closure)
                                d33 = f33.closure->desc;
                            if (d33) {
                                uint32_t lo = d33->codeOffset;
                                uint32_t hi = f33.ip + 8;
                                std::fprintf(stderr,
                                    "  CALLER frame[%zu] disasm [%u..%u) (prologue→ip):\n",
                                    lim - 2, lo, hi);
                                disassembleWindow(stderr, *f33.cu, lo, hi);
                            }
                        }
                    }
                }
            }
            // #558 Phase 3.3: partial-Bindings recovery retired.

            // #466 error-as-value (GHC-style mkBlackHole).
            //
            // If the Black thunk is on THIS vm's frames, this is a
            // genuine local cycle (`let x = x; in x` shape) — throw
            // BlackholeError as before so tryEval / consumer error
            // paths see the typed exception.
            //
            // If the Black thunk is on a FOREIGN vm's frames (typical
            // under lambda-skip + bridge primop chains), the thunk is
            // genuinely mid-construction in another VMState; throwing
            // here triggers fallbackToTreeWalker retry chains that
            // re-create fresh VMStates and grow the C-stack
            // unboundedly.  Instead, return the Tag::Blackhole singleton
            // as a propagating sentinel value.  Most consumers (OP_CALL,
            // OP_ATTRS_*, OP_ADD, etc.) fail naturally on Blackhole
            // operands with regular type errors, which propagate
            // cleanly without retry cycles.  Bridge primops convert the
            // Blackhole back to TW's mkBlackHole sentinel so TW's
            // existing infinite-recursion protocol takes over.
            //
            // Mirrors GHC's blackhole-as-value protocol (rts/sm/Evac.c
            // eval_thunk_selector and friends): when forcing a thunk
            // that's already under evaluation by another stack, return
            // a marker rather than blocking or throwing immediately;
            // let the marker propagate through the operation chain
            // until something concrete tries to use it.
            //
            // Default-on; opt out via NIX_V3_NO_BLACKHOLE_AS_VALUE=1
            // for bisecting any regression.
            {
                static const bool s_blackholeAsValue =
                    std::getenv("NIX_V3_NO_BLACKHOLE_AS_VALUE") == nullptr;
                if (s_blackholeAsValue) {
                    bool onMyFrames = false;
                    for (size_t i = 0; i < vm.frames.size(); ++i) {
                        if (vm.frames[i].thunk == t) {
                            onMyFrames = true; break;
                        }
                    }
                    if (!onMyFrames) {
                        static const bool s_dbgBhv =
                            std::getenv("V3_DBG_BLACKHOLE_AS_VALUE") != nullptr;
                        if (s_dbgBhv) {
                            static thread_local uint64_t hits = 0;
                            if (++hits == 1 || (hits & (hits - 1)) == 0)
                                std::fprintf(stderr,
                                    "v3 blackhole-as-value: thunk=%p (vm=%p, "
                                    "frames=%zu) returning vBlackhole "
                                    "(hits=%llu)\n",
                                    (void *)t, (void *)&vm,
                                    vm.frames.size(),
                                    (unsigned long long)hits);
                        }
                        static const bool s_dbgVBHProd2 =
                            std::getenv("V3_DBG_VBH_PROD") != nullptr;
                        if (s_dbgVBHProd2) {
                            const auto * d = (t->state == ThunkState::Suspended
                                              || t->state == ThunkState::Blackhole)
                                ? t->suspended.desc : nullptr;
                            const PosSnapshot * ps =
                                d ? resolvePosSnapshot(d->posHandle) : nullptr;
                            std::fprintf(stderr,
                                "v3 forceValue→vBlackhole(cross-stack): thunk=%p name='%s' pos=%s:%u:%u\n",
                                (void *)t,
                                d && !d->name.empty() ? d->name.c_str() : "<?>",
                                (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                                ps ? ps->line : 0u,
                                ps ? ps->column : 0u);
                        }
                        return Value::vBlackhole;
                    }
                    // #558 (2026-05-10) STG-style "reached WHNF" recovery.
                    //
                    // Gated by NIX_V3_NO_STG_WHNF=1 (bisect kill switch).
                    //
                    // The thunk IS on our own call stack — a real cycle
                    // from forceValue's perspective.  But if the thunk
                    // has reached WHNF (its `OP_ATTRS_REC_INIT_TAIL`
                    // already fired and registered the partial Bindings
                    // shape), we can RETURN that shape as the thunk's
                    // value — the entries are progressively filled in
                    // place via OP_ATTRS_REC_SET, so consumers see the
                    // currently-known fields through the same Bindings*.
                    //
                    // STG analog: a constructor allocation reaches WHNF
                    // even when the constructor's lazy fields are still
                    // unevaluated.  Forcing the thunk again returns the
                    // already-allocated cell.  For Nix attrsets,
                    // OP_ATTRS_REC_INIT_TAIL plays the role of the
                    // constructor allocation; its trailer (sorted name
                    // list) defines the SHAPE.
                    //
                    // Without this, lib.fix's `let x = f x; in x` cycles
                    // when something deep inside f's body re-projects
                    // through x — projections would force x → throws.
                    // With this, the projection sees x's currently-known
                    // shape (the merged // result so far) and proceeds.
                    // M-8 (CODEBASE_REVIEW_2026-06-11): the #558 Phase 1.5
                    // shapeCell recovery read was REMOVED with the field.  It
                    // was gated NIX_V3_CELL_EVERYWHERE (default-off) — in
                    // production shapeCell was always null so this never fired;
                    // the local cycle already falls through to the BlackholeError
                    // throw below, exactly as in every prod eval to date.
                    // #558 Phase 3.3: STG WHNF recovery via partial-
                    // Bindings retired.  Local cycle falls through to
                    // BlackholeError throw.
                }
            }

            // #497 diagnostic: dump frame stack + identify Black thunk
            // when V3_DBG_BLACKHOLE_TRACE=1.  Used to investigate
            // post-#496 BlackholeError shape under broader thunkify.
            static const bool s_dbgBlackholeTrace =
                std::getenv("V3_DBG_BLACKHOLE_TRACE") != nullptr;
            if (__builtin_expect(s_dbgBlackholeTrace, 0)) {
                std::fprintf(stderr,
                    "v3 BLACKHOLE thunk=%p forces=%u (frames=%zu):\n",
                    (void *)t, (unsigned)t->forces, vm.frames.size());
                for (size_t fi = vm.frames.size(); fi > 0; --fi) {
                    const auto & fr = vm.frames[fi - 1];
                    const LambdaDescriptor * d = nullptr;
                    if (fr.thunk && fr.thunk->state == ThunkState::Blackhole)
                        d = fr.thunk->suspended.desc;
                    else if (fr.closure)
                        d = fr.closure->desc;
                    const PosSnapshot * ps =
                        d ? resolvePosSnapshot(d->posHandle) : nullptr;
                    std::fprintf(stderr,
                        "  [%zu] %s ip=%u flags=%u pos=%s:%u:%u%s\n",
                        fi - 1,
                        d && !d->name.empty() ? d->name.c_str() : "<?>",
                        fr.ip,
                        (unsigned)fr.flags,
                        (ps && !ps->file.empty()) ? ps->file.c_str() : "<no-pos>",
                        ps ? ps->line : 0u,
                        ps ? ps->column : 0u,
                        fr.thunk == t ? " <-- TARGET" : "");
                }
                std::fflush(stderr);
            }
            // #680 — match TW phrasing (libexpr/eval.cc:2594).
            throw BlackholeError("infinite recursion encountered");
        }
        // (forceValue Bridge-thunk handler retired with the bridge
        //  apparatus — TW_VALUE_ERADICATION F4, 2026-06-02.)

        const LambdaDescriptor * desc = t->suspended.desc;
        // #705 (2026-05-20): diagnostic — if we got here with a null
        // desc, something handed us a Thunk whose Suspended payload
        // is zeroed-out.  Most likely cause: stale nursery pointer
        // (post-scavenge memset).  Dump everything we know and abort.
        // Includes the compress-chain so we can identify whether the
        // stale pointer was the initial v or chased through an
        // Evaluated thunk's evaluated field.
        if (__builtin_expect(!desc, 0)) [[unlikely]] {
            const Nursery & nu = threadNursery();
            std::fprintf(stderr,
                "v3 forceValue: STALE THUNK suspected.\n"
                "  t=%p in nursery=%s\n"
                "  frames=%zu  valueStack=%zu  withStack=%zu\n",
                (void*)t, nu.contains(t) ? "YES" : "no",
                vm.frames.size(), vm.valueStack.size(),
                vm.withStack.size());
            // #705 (2026-05-21): search the arena for every word
            // equal to t.  That word is in a tenured Value's payload
            // field — the missed-root container.  By printing the
            // first few matches, we can identify the source of the
            // stale pointer that scavenge failed to forward.
            {
                Arena & arena = threadArena();
                auto blocks = arena.blockRanges();
                uintptr_t target = reinterpret_cast<uintptr_t>(t);
                size_t hits = 0;
                size_t cap = 8;
                uintptr_t firstHitArena = 0;
                std::fprintf(stderr, "  arena references to stale t:\n");
                for (auto & blk : blocks) {
                    uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
                    uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
                    lo = (lo + 7) & ~uintptr_t{7};
                    for (uintptr_t p = lo; p + 8 <= hi; p += 8) {
                        if (*reinterpret_cast<const uintptr_t *>(p) != target)
                            continue;
                        if (!firstHitArena) firstHitArena = p;
                        if (hits++ < cap) {
                            uint64_t tagWord = (p >= 8)
                                ? *reinterpret_cast<const uint64_t *>(p - 8) : 0;
                            std::fprintf(stderr,
                                "    @ %p (preceding word: 0x%llx, tag=%d)\n",
                                (void*)p, (unsigned long long)tagWord,
                                (int)(tagWord & 0xff));
                            // Dump 64 bytes before to inspect container header.
                            std::fprintf(stderr, "    context [-64..-8]:");
                            for (int off = -64; off < 0; off += 8) {
                                uintptr_t cp = p + off;
                                if (cp >= reinterpret_cast<uintptr_t>(blk.begin)) {
                                    uint64_t w = *reinterpret_cast<const uint64_t *>(cp);
                                    std::fprintf(stderr, " %llx", (unsigned long long)w);
                                }
                            }
                            // Decode SymbolId from the Bindings::Entry
                            // pattern: name is at (Value-tag-pos -8),
                            // i.e. p - 16.  If this looks like a
                            // Bindings entry, name is in low 4 bytes.
                            uint32_t maybeName =
                                (p >= 16) ? (uint32_t)*reinterpret_cast<const uint32_t *>(p - 16) : 0;
                            const auto & symtab = ir::globalSymbolTable();
                            const char * symName =
                                (maybeName < symtab.size())
                                    ? symtab[maybeName].c_str() : "<?>";
                            std::fprintf(stderr,
                                "    symbol-name guess: id=%u \"%s\"\n",
                                maybeName, symName);
                            std::fprintf(stderr, "    context [+8..+24]:");
                            for (int off = 8; off <= 24; off += 8) {
                                uintptr_t cp = p + off;
                                if (cp + 8 <= reinterpret_cast<uintptr_t>(blk.end)) {
                                    uint64_t w = *reinterpret_cast<const uint64_t *>(cp);
                                    std::fprintf(stderr, " %llx", (unsigned long long)w);
                                }
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
                std::fprintf(stderr, "  total arena refs to t: %zu\n", hits);

                // #705 N1-followup: search vm.valueStack / vm.withStack
                // and ALL active VMStates for any Tag::Thunk Value with
                // payload == stale t.  Pinpoints which root holds the
                // stale pointer that scavenge missed.
                {
                    auto searchVm = [&](const char * label, const VMState * vmp) {
                        if (!vmp) return;
                        size_t vhits = 0;
                        for (size_t i = 0; i < vmp->valueStack.size(); ++i) {
                            const Value & sv = vmp->valueStack[i];
                            if (sv.tag() != Tag::Thunk) continue;
                            if ((uintptr_t)sv.asThunk() != target) continue;
                            if (vhits++ < 4)
                                std::fprintf(stderr,
                                    "  %s.valueStack[%zu] = Tag::Thunk(stale)\n",
                                    label, i);
                        }
                        for (size_t i = 0; i < vmp->withStack.size(); ++i) {
                            const Value & sv = vmp->withStack[i];
                            if (sv.tag() != Tag::Thunk) continue;
                            if ((uintptr_t)sv.asThunk() != target) continue;
                            std::fprintf(stderr,
                                "  %s.withStack[%zu] = Tag::Thunk(stale)\n",
                                label, i);
                        }
                        for (size_t i = 0; i < vmp->frames.size(); ++i) {
                            const CallFrame & f = vmp->frames[i];
                            if ((uintptr_t)f.thunk == target)
                                std::fprintf(stderr,
                                    "  %s.frames[%zu].thunk = stale\n",
                                    label, i);
                        }
                        std::fprintf(stderr,
                            "  %s: total valueStack matches=%zu\n",
                            label, vhits);
                    };
                    searchVm("currentVm", &vm);
                    for (VMState * vmp : activeVMStack()) {
                        if (vmp && vmp != &vm) {
                            searchVm("otherVm", vmp);
                        }
                    }
                }

                // Second pass: the missed-root Bindings is at (p - 48)
                // per the layout decoded above.  Search the arena for
                // pointers to it — those are the places holding the
                // container that scavenge missed.  Uses the ARENA
                // address of the first stale-pointer hit (not the
                // nursery target address).
                uintptr_t containerAddr = firstHitArena ? (firstHitArena - 48) : 0;
                if (containerAddr) {
                    std::fprintf(stderr,
                        "  searching for refs to Bindings @ %p:\n",
                        (void*)containerAddr);
                    size_t chits = 0;
                    for (auto & blk : blocks) {
                        uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
                        uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
                        lo = (lo + 7) & ~uintptr_t{7};
                        for (uintptr_t p2 = lo; p2 + 8 <= hi; p2 += 8) {
                            if (*reinterpret_cast<const uintptr_t *>(p2) != containerAddr)
                                continue;
                            if (chits++ < 8) {
                                uint64_t prevWord = (p2 >= 8)
                                    ? *reinterpret_cast<const uint64_t *>(p2 - 8) : 0;
                                std::fprintf(stderr,
                                    "    arena @ %p holds container ptr "
                                    "(tag_payload word @ %p = 0x%llx tag=%d)\n",
                                    (void*)p2, (void*)(p2 - 8),
                                    (unsigned long long)prevWord,
                                    (int)(prevWord & 0xff));
                                // Dump 256 bytes before p2 to find
                                // the container header.
                                std::fprintf(stderr, "    context [-256..-16] (8B/word):\n     ");
                                int wordCount = 0;
                                for (int off = -256; off <= -16; off += 8) {
                                    uintptr_t cp = p2 + off;
                                    if (cp >= reinterpret_cast<uintptr_t>(blk.begin)) {
                                        uint64_t w = *reinterpret_cast<const uint64_t *>(cp);
                                        std::fprintf(stderr, " %llx",
                                            (unsigned long long)w);
                                        if (++wordCount % 4 == 0)
                                            std::fprintf(stderr, "\n     ");
                                    }
                                }
                                std::fprintf(stderr, "\n");
                            }
                        }
                    }
                    std::fprintf(stderr, "  total refs to Bindings: %zu\n",
                                 chits);

                    // Chain up: the holder of the Bindings ref is a
                    // Value{Attrs, ...} inside some container.  Try
                    // to find what holds it — likely a ValuePair
                    // (Tag::App), Bindings entry, Closure upvalue,
                    // etc.  We search for the LIVE-VALUE address
                    // (containerHolderAddr = arena pos of that Value).
                    if (chits == 1) {
                        // We have only one ref — find it again and
                        // search upward.
                        for (auto & blk : blocks) {
                            uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
                            uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
                            lo = (lo + 7) & ~uintptr_t{7};
                            for (uintptr_t p2 = lo; p2 + 8 <= hi; p2 += 8) {
                                if (*reinterpret_cast<const uintptr_t *>(p2) != containerAddr)
                                    continue;
                                // Our Value at (p2-8, p2).  Find
                                // who points to the WHOLE PAIR
                                // (which starts at p2-24 if this is
                                // the right-Value of a Pair).
                                uintptr_t pairAddr = p2 - 24;
                                std::fprintf(stderr,
                                    "  searching for refs to ValuePair @ %p:\n",
                                    (void*)pairAddr);
                                size_t phits = 0;
                                for (auto & blk2 : blocks) {
                                    uintptr_t lo2 = reinterpret_cast<uintptr_t>(blk2.begin);
                                    uintptr_t hi2 = reinterpret_cast<uintptr_t>(blk2.end);
                                    lo2 = (lo2 + 7) & ~uintptr_t{7};
                                    for (uintptr_t p3 = lo2; p3 + 8 <= hi2; p3 += 8) {
                                        if (*reinterpret_cast<const uintptr_t *>(p3) != pairAddr)
                                            continue;
                                        if (phits++ < 4) {
                                            uint64_t prev = (p3 >= 8)
                                                ? *reinterpret_cast<const uint64_t *>(p3 - 8) : 0;
                                            std::fprintf(stderr,
                                                "    arena @ %p holds pair ptr "
                                                "(tag_payload @ %p = 0x%llx tag=%d)\n",
                                                (void*)p3, (void*)(p3 - 8),
                                                (unsigned long long)prev,
                                                (int)(prev & 0xff));
                                        }
                                    }
                                }
                                std::fprintf(stderr,
                                    "  total refs to ValuePair: %zu\n", phits);
                                break;  // only do one chase
                            }
                            if (chits) break;
                        }
                    }
                }
            }
            std::fprintf(stderr, "  compressChain (chase predecessors), N=%d:\n",
                compressCount);
            for (int ci = 0; ci < compressCount; ++ci) {
                const Thunk * pt = compressChain[ci];
                std::fprintf(stderr,
                    "    [%d] t=%p state=%d in-nursery=%s evaluated.tag=%d\n",
                    ci, (const void*)pt, (int)pt->state,
                    nu.contains(pt) ? "YES" : "no",
                    (int)pt->evaluated.tag());
            }
            // Dump valueStack non-uninitialized entries (the active
            // ones; trailing slots may have been pre-resized).
            std::fprintf(stderr, "  valueStack non-zero entries:\n");
            size_t nz = 0;
            for (size_t i = 0; i < vm.valueStack.size() && nz < 16; ++i) {
                const Value & sv = vm.valueStack[i];
                if (sv.tag() == Tag::Uninitialized) continue;
                const void * sp =
                    sv.tag() == Tag::Thunk   ? (const void*)sv.asThunk()
                    : sv.tag() == Tag::Closure ? (const void*)sv.asClosure()
                    : sv.tag() == Tag::Attrs   ? (const void*)sv.asAttrs()
                    : sv.tag() == Tag::List    ? (const void*)sv.asList()
                    : sv.isAppLike() || sv.tag() == Tag::PrimOpApp
                                              ? (const void*)sv.asPair()
                    : sv.tag() == Tag::Slot   ? (const void*)sv.asSlot()
                    : nullptr;
                std::fprintf(stderr,
                    "    [%zu] tag=%d ptr=%p%s\n",
                    i, (int)sv.tag(), sp,
                    (sp && nu.contains(sp)) ? " <-- IN NURSERY" : "");
                ++nz;
            }
            // Dump the top few frames to see what's executing.
            std::fprintf(stderr, "  frames (top -8):\n");
            size_t flim = vm.frames.size();
            for (size_t i = flim; i > 0 && i + 8 > flim; --i) {
                const auto & fr = vm.frames[i - 1];
                const LambdaDescriptor * fd = nullptr;
                if (fr.thunk) fd = fr.thunk->suspended.desc;
                else if (fr.closure) fd = fr.closure->desc;
                std::fprintf(stderr,
                    "    [%zu] ip=%u flags=%u closure=%p thunk=%p%s%s desc=%s\n",
                    i - 1, fr.ip, (unsigned)fr.flags,
                    (const void*)fr.closure, (const void*)fr.thunk,
                    (fr.closure && nu.contains(fr.closure)) ? " CL-IN-NURSERY" : "",
                    (fr.thunk   && nu.contains(fr.thunk))   ? " TH-IN-NURSERY" : "",
                    (fd && !fd->name.empty()) ? fd->name.c_str() : "<?>");
            }
            std::fflush(stderr);
            std::abort();
        }
        // NIX_TRACE_EVAL: emit F at the about-to-push point.  The
        // matching W is emitted by OP_RETURN's CFF_THUNK_RETURN branch
        // (3925-ish) when the frame pops, so the F/W pair brackets the
        // entire thunk-body evaluation including all nested forces.
        // The depth counter (`nix::evalTrace::depthRef`) tracks active
        // CFF_THUNK_RETURN frames and aligns the trace shape between
        // OP_FORCE-driven and forceValue-driven thunk pushes.
        if (__builtin_expect(nix::evalTrace::enabled(), 0))
            nix::evalTrace::enterForce(v3ThunkTracePos(t));
        hotForceCheck(t);
        // Phase D Step 12 (2026-05-21): retired the closure-pool —
        // see the OP_FORCE Thunk dispatch site above for rationale.
        Closure * fakeClo = Alloc::allocFakeClo(t->nUpvalues);
        fakeClo->desc = desc;
        fakeClo->nUpvalues = t->nUpvalues;
        fakeClo->capturedWiths = thunkCapturedWiths(t);  // FP-2b: was suspended.capturedWiths
        fakeClo->cu = thunkCU(t);  // FP-2a: was t->suspended.cu
        // env-sharing: share the thunk's Env directly (no per-force copy); see
        // the OP_FORCE path above for the FAM-poison rationale.
        if (Env * te = thunkUpvalEnv(t)) {
            fakeClo->upvalEnv = te;
            for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i].mkUninitialized();
        } else {
            for (uint16_t i = 0; i < t->nUpvalues; ++i) fakeClo->upvalues[i] = t->tail[i];
        }
        // Phase D coverage: same as the OP_FORCE fakeClo path above.
        closurePostConstructBarrier(fakeClo);
        ListVec * thunkWiths = thunkCapturedWiths(t);  // FP-2b: was suspended.capturedWiths
        const CompilationUnit * thunkCu = thunkCU(t);  // FP-2a: was t->suspended.cu
        if (!thunkCu) thunkCu = vm.frames.back().cu;    // ...?: frame-cu fallback preserved
        t->state = ThunkState::Blackhole;

        size_t exitDepth = vm.frames.size();
        size_t newBase = vm.valueStack.size();
        vm.valueStack.resize(newBase + desc->nLocals);
        uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

        {
            static const bool s_dbg_fv =
                std::getenv("V3_DBG_STORE_PREVSTAGE") != nullptr;
            if (s_dbg_fv && t->nUpvalues == 5) {
                std::fprintf(stderr,
                    "v3 forceValue: pushing thunk %p desc=%s codeOffset=%u nUp=%u cu=%p\n",
                    (void*)t,
                    !desc->name.empty() ? desc->name.c_str() : "<anon>",
                    desc->codeOffset, (unsigned)t->nUpvalues,
                    (void*)thunkCu);
            }
        }

        vm.frames.push_back(CallFrame{
            .cu = thunkCu,
            .closure = fakeClo,
            .thunk = t,
            .ip = desc->codeOffset,
            .stackBaseOffset = static_cast<uint32_t>(newBase),
            .withStackBase = newWithBase,
            .flags = CFF_THUNK_RETURN,
        });
        pushCapturedWiths(vm, thunkWiths);

        // WC-5: if dispatchLoop throws, every thunk frame we'd unwind
        // is currently marked Blackhole.  Without cleanup, a later
        // force of the same thunk (e.g. when tree-walker takes over
        // and accesses the same lib attr) would hit the stale mark
        // and report "infinite recursion (blackhole)" — masking the
        // real error.  Tree-walker's mkFailed stores the exception
        // and re-throws on subsequent forces (eval-inline.hh:125);
        // the bare-minimum equivalent here is to revert each frame's
        // thunk back to Suspended so the next force re-runs.
        //
        // We don't store the exception (would need a Failed state
        // and re-throw machinery), so the next force simply re-runs
        // the body — slow but correct, and idempotent throws will
        // re-throw the same error consistently.
        try {
            v = dispatchLoop(vm, exitDepth);
        } catch (...) {
            // WC-37: clear blackmarks AND unwind the leftover frames
            // so they can't become "ghost frames" picked up by a later
            // OP_RETURN in an outer dispatchLoop (which would corrupt
            // the thunk pointed-to by the ghost frame).
            //
            // #557 hardening: wrap clearBlackMarksOnException in its
            // own try/catch.  If it ever throws during the outer
            // exception handling (e.g. partialBindingsRegistry hash
            // operation, or accessing a freed thunk pointer), the
            // C++ runtime would call terminate() — surfacing as the
            // brk #0x1 / EXC_BREAKPOINT trap observed when the
            // libsForQt5 cycle bypass diverges into a cascading
            // exception loop.  Swallowing here lets the original
            // exception propagate normally.
            try {
                clearBlackMarksOnException(vm, exitDepth);
            } catch (...) { /* swallow secondary throws during cleanup */ }
            // Also clear the outer Black mark we set just above.
            if (t->state == ThunkState::Blackhole)
                t->state = ThunkState::Suspended;
            throw;
        }
        // WC-14.5 success-path defensive cleanup: if the outer thunk
        // somehow remains Black after a successful dispatchLoop
        // (theoretical impossibility per the invariant, but observed
        // in cross-VMState bridge scenarios where another VMState's
        // frames interleave with this one), revert it to Suspended
        // so subsequent forces re-run idempotently rather than
        // throwing "infinite recursion (blackhole)" on a stale mark.
        if (t->state == ThunkState::Blackhole) {
            static const bool s_dbg = std::getenv("V3_DBG_BLACK") != nullptr;
            if (s_dbg) std::fprintf(stderr,
                "v3 forceValue: SUCCESS-path Black leak; reverting "
                "thunk=%p Suspended\n", (void*)t);
            t->state = ThunkState::Suspended;
        }
        // NIX_TRACE_EVAL: the matching W for the F emitted at frame-
        // push (above) is produced by OP_RETURN's CFF_THUNK_RETURN
        // branch when the thunk body's OP_RETURN runs inside the
        // dispatchLoop above.  No emit needed here.
    }
    // SECD-style slot memoization: write the resolved value back into
    // the slot we entered through.  Future Tag::Slot derefs through
    // the same slot will see the resolved value directly.  Mirrors
    // tree-walker's `state.forceValue(*v2)` which mutates the slot
    // in-place; sub-thunks observing the slot see the mutation.
    // Important: only write back if v is a concrete WHNF value (not
    // another Slot/Thunk/App that we somehow exited the loop with —
    // shouldn't happen, but be safe).
    // C-4 (CODEBASE_REVIEW_2026-06-11): also never memoize the transient
    // Tag::Blackhole sentinel — it means "value not yet known" (a self-cycle
    // in progress), not a real value, and writing it permanently into the
    // entered slot turns a "not yet computed" into a permanent Blackhole that
    // later reads fail on where TW succeeds. The path-compression block below
    // already excludes Blackhole; this matches it (the inconsistency the
    // review flagged).
    if (memoSlot && v.tag() != Tag::Slot && v.tag() != Tag::Blackhole)
        cellWrite(memoSlot, v, nullptr);  // PhD-6: barrier raw withLookup memo writeback
    // #558 Phase 4 follow-up: path compression writeback.  Only write
    // back when v is a stable WHNF — never vBlackhole (the cross-stack
    // deferred-value marker, which is transient and shouldn't be
    // memoized; the next force should re-attempt) and never another
    // Tag::Thunk/Tag::Slot/Tag::App (defensive: the chase loop
    // shouldn't exit with one of these, but guard regardless).
    if (compressCount > 0
        && v.tag() != Tag::Thunk
        && v.tag() != Tag::Slot
        && !v.isAppLike()
        && v.tag() != Tag::Blackhole)
    {
        for (int i = 0; i < compressCount; ++i)
            thunkSetEvaluated(compressChain[i], v);  // Phase D barrier
    }
    return v;
}

// T1 (LIST_ITERATION_FIX_PLAN_2026-06-08) — saturated 2-arg call.
//
// `foldl' op acc x` and friends apply a 2-arg callback per element via the
// curried `callClosure(callClosure(op, acc), x)`.  With eval/apply default-on
// (lower_v3.hh:527) `op` is an arity-2 closure, so the FIRST callClosure is an
// under-application: it allocates a throwaway App `ValuePair` PAP (`step1`,
// holding [op, acc]) just so the SECOND callClosure can immediately consume it
// to saturate.  Measured: map.foldl allocates 6M `ValuePair`s = 2M genList +
// 2M map + **2M of these curry PAPs (128MB pure waste)** on the dominant pass.
//
// callClosure2 enters the arity-2 closure body ONCE with both args already in
// slots 0..1 — no intermediate PAP, one dispatch prologue instead of two.  It
// mirrors callClosure's arity>1 saturated-entry block (the `papBase` path) for
// the total==arity==2 case.  Any other callee shape (PAP / primop / __functor /
// arity!=2 / under- or over-application) falls back to the exact curried form,
// so the result is byte-identical to `callClosure ∘ callClosure`.
Value callClosure2(VMState & vm, Value fun, Value arg1, Value arg2)
{
    // Default-ON; opt-out NIX_V3_NO_SATURATED_CALL=1 is the A/B + bisect
    // handle.  Retirement: remove the gate + the curried-fallback duplication
    // once it has soaked on the cutover-parity corpus + M5/HNE (mirrors the
    // eval/apply gate it depends on).  Result-preserving by construction (same
    // body, same args, fewer allocs) — the gate measures magnitude, not sign.
    static const bool s_saturatedCall =
        std::getenv("NIX_V3_NO_SATURATED_CALL") == nullptr;

    if (__builtin_expect(s_saturatedCall, 1)) {
        // Resolve callee to WHNF (mirror callClosure's entry fast-path).
        {
            Tag ft = fun.tag();
            if (__builtin_expect(ft == Tag::Thunk || ft == Tag::App
                                 || ft == Tag::App3 || ft == Tag::Slot, 0))
                fun = forceValue(vm, fun);
        }
        // Plain arity-2 closure: enter the body directly with both args.
        // (Intrinsics are arity-1 / handled in the curried path below; the
        // arity>1 eval/apply block in callClosure likewise skips the intrinsic
        // check, so matching `arity == 2` here is byte-identical.)
        if (fun.tag() == Tag::Closure && fun.asClosure()
            && fun.asClosure()->desc
            && fun.asClosure()->desc->arity == 2) {
            const Closure * c = fun.asClosure();
            const LambdaDescriptor * d = c->desc;
            const CompilationUnit * ccu = c->cu ? c->cu : vm.frames.back().cu;
            size_t exitDepth = vm.frames.size();
            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + d->nLocals);
            vm.valueStack[newBase + 0] = arg1;
            vm.valueStack[newBase + 1] = arg2;
            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            vm.frames.push_back(CallFrame{
                .cu = ccu,
                .closure = c,
                .thunk = nullptr,
                .ip = d->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = 0,
            });
            pushCapturedWiths(vm, c->capturedWiths);
            // Stage 2: callClosure2's only callers (primFoldl/primFoldlMap)
            // run UNDER the outer OP_CALL_PRIMOP dispatch, so `vm` is already
            // current + active — pass reuseScope to skip the redundant
            // per-element active-VM re-push.  Default-ON; opt-out
            // NIX_V3_NO_LEAFCALL_FAST=1 (A/B + bisect handle).  Retire with
            // the saturated-call gate once soaked on cutover-parity + M5/HNE.
            static const bool s_leafCallFast =
                std::getenv("NIX_V3_NO_LEAFCALL_FAST") == nullptr;
            return dispatchLoop(vm, exitDepth, /*reuseScope=*/s_leafCallFast);
        }
        // Fall through with the already-WHNF `fun` to the curried form.
    }
    // Fallback — byte-identical to the original curried call sequence.
    Value step1 = callClosure(vm, fun, arg1);
    return callClosure(vm, step1, arg2);
}

Value callClosure(VMState & vm, Value fun, Value arg)
{
    // Mirror tree-walker's `callFunction`: callable values must be in
    // WHNF before we dispatch on shape.  Most callers force first
    // (OP_CALL's preceding OP_FORCE; OP_RETURN's transitive chase),
    // but a few internal paths (the __functor recursion below; primop
    // map-style App entries forced inline) leave a Tag::Thunk or
    // Tag::App on `fun`.  Fast-path: skip forceValue call if `fun` is
    // already in WHNF.  Saves the CALL/RET + chase-loop setup for the
    // common case where the caller already forced.  CPU sample showed
    // callClosure+forceValue at >50% of CPU on nixpkgs THUNK_ALL;
    // every saved no-op call counts.
    {
        Tag ft = fun.tag();
        if (__builtin_expect(ft == Tag::Thunk
                             || ft == Tag::App || ft == Tag::App3
                             || ft == Tag::Slot, 0))
            fun = forceValue(vm, fun);
    }
    // V3_DBG_CALL_CLOSURE=1 prints every call: closure name + arg
    // shape.  Used to trace the broader-thunkify upvalue bug.
    static const bool s_dbgCallClosure =
        std::getenv("V3_DBG_CALL_CLOSURE") != nullptr;
    if (s_dbgCallClosure) {
        const char * nm = "<?>";
        if (fun.tag() == Tag::Closure && fun.asClosure()
            && fun.asClosure()->desc)
            nm = fun.asClosure()->desc->name.c_str();
        int arg_tag = (int)arg.tag();
        int arg_size = -1;
        if (arg.tag() == Tag::Attrs && arg.asAttrs())
            arg_size = arg.asAttrs()->size;
        std::fprintf(stderr,
            "v3 callClosure: fun.tag=%d name=%s arg.tag=%d size=%d\n",
            (int)fun.tag(), nm, arg_tag, arg_size);
    }
    // PrimOp / PrimOpApp: build a partial application or invoke once
    // we have all the args.  Mirrors the OP_CALL primop branch.
    if (fun.isPrimOp() || fun.tag() == Tag::PrimOpApp) {
        Value cur = fun;
        size_t depth = 0;
        while (cur.tag() == Tag::PrimOpApp) { ++depth; cur = cur.asPair()->left; }
        if (!cur.isPrimOp())
            throw std::runtime_error("v3 callClosure: PrimOpApp chain doesn't terminate in a PrimOp");
        const PrimOp * po = cur.asPrimOp();
        size_t totalArgs = depth + 1;
        if (totalArgs < po->arity) {
            ValuePair * vp = Alloc::allocPair();
            vp->left = fun;
            vp->right = arg;
            pairPostConstructBarrier(vp);  // Phase D
            Value v;
            v.mkPair(Tag::PrimOpApp, vp);
            return v;
        }
        if (totalArgs > po->arity)
            throw std::runtime_error("v3 callClosure: too many args for primop");
        Value buf[8];
        if (po->arity > 8) throw std::runtime_error("v3 callClosure: primop arity > 8");
        buf[totalArgs - 1] = arg;
        Value chain = fun;
        for (size_t i = totalArgs - 1; i > 0; --i) {
            buf[i - 1] = chain.asPair()->right;
            chain = chain.asPair()->left;
        }
        // Phase 1.2 (action plan): mirror OP_CALL's primop-arg force loop
        // (vm.cc:2708-2715) — fast-path the WHNF case inline so that
        // arg-forcing on an already-resolved Value doesn't pay the
        // forceValue function-call + chase-loop setup cost.  callClosure
        // is the entry point primops use for callback lambdas (map,
        // filter, foldl', etc.), so this fires on the dominant
        // `(p: p.name)`-style nixpkgs callbacks AND on every recursive
        // primop-from-primop call.  Without this fast path, callClosure
        // diverged from OP_CALL on the same workload.
        //
        // Audit category B1 (see ITERATIVE_FORCE_AUDIT_2026-05-18.md):
        // first scoped iterative-force conversion landed.
        for (uint32_t i = 0; i < po->arity; ++i) {
            if (po->lazyArgs & (1u << i)) continue;
            Tag at = buf[i].tag();
            if (__builtin_expect(at == Tag::Thunk
                                 || at == Tag::App || at == Tag::App3
                                 || at == Tag::Slot, 0))
                buf[i] = forceValue(vm, buf[i]);
        }
        EvalState state; state.vm = &vm; state.nixEvalState = getNixEvalState();
        Value out;
        po->fn(state, buf, out);
        return out;
    }

    // eval/apply (#3, gate-on only): arity-N closure / Tag::App PAP — the
    // closure analogue of the PrimOpApp block above, mirroring OP_CALL.  This
    // is the C++-apply path (App-spine force-apply + primops applying a user
    // fn), so it must agree with OP_CALL on the PAP convention.  Under-
    // application returns a PAP; saturation enters the callee with all N args
    // in slots 0..N-1 and runs it via dispatchLoop.  Gate-off inert:
    // papBase->desc->arity is 0/1.
    {
        const Closure * papBase = nullptr;
        size_t papDepth = 0;
        if (fun.tag() == Tag::Closure && fun.asClosure()) {
            papBase = fun.asClosure();
        } else if (fun.tag() == Tag::App && fun.asPair()) {
            const Value * cur = &fun;
            while (cur->tag() == Tag::App && cur->asPair()) {
                ++papDepth; cur = &cur->asPair()->left;
            }
            if (cur->tag() == Tag::Closure && cur->asClosure())
                papBase = cur->asClosure();
        }
        if (papBase && papBase->desc && papBase->desc->arity > 1) {
            const uint8_t A = papBase->desc->arity;
            const size_t total = papDepth + 1;
            if (total < A) {
                ValuePair * vp = Alloc::allocPair();
                vp->left = fun; vp->right = arg;
                pairPostConstructBarrier(vp);
                Value v;
                v.mkPair(Tag::App, vp);
                return v;
            }
            if (A > 16) throw std::runtime_error("v3 callClosure: arity > 16");
            Value argbuf[16];
            argbuf[total - 1] = arg;
            Value chain = fun;
            for (size_t i = total - 1; i > 0; --i) {
                argbuf[i - 1] = chain.asPair()->right;
                chain = chain.asPair()->left;
            }
            const LambdaDescriptor * d = papBase->desc;
            const CompilationUnit * ccu =
                papBase->cu ? papBase->cu : vm.frames.back().cu;
            size_t exitDepth = vm.frames.size();
            size_t newBase = vm.valueStack.size();
            vm.valueStack.resize(newBase + d->nLocals);
            for (size_t i = 0; i < A; ++i)
                vm.valueStack[newBase + i] = argbuf[i];
            uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());
            vm.frames.push_back(CallFrame{
                .cu = ccu,
                .closure = papBase,
                .thunk = nullptr,
                .ip = d->codeOffset,
                .stackBaseOffset = static_cast<uint32_t>(newBase),
                .withStackBase = newWithBase,
                .flags = 0,
            });
            pushCapturedWiths(vm, papBase->capturedWiths);
            return dispatchLoop(vm, exitDepth);
        }
    }

    // Attrset with __functor: apply functor self arg.
    if (fun.isAttrs() && fun.asAttrs()) {
        static const SymbolId functorId = ir::globalInternSymbol("__functor");
        if (auto * fn = fun.asAttrs()->lookup(functorId)) {
            Value forced = forceValue(vm, *fn);
            Value firstStep = callClosure(vm, forced, fun);
            return callClosure(vm, firstStep, arg);
        }
    }

    // (#483 part 3 callClosure Bridge-thunk callee handler retired with the
    //  bridge apparatus — TW_VALUE_ERADICATION F4, 2026-06-02.)

    if (!fun.isClosure()) {
        static const bool dbg = std::getenv("V3_DBG_CALL") != nullptr;
        if (dbg) {
            std::fprintf(stderr,
                "v3 callClosure: not callable tag=%u frames=%zu\n",
                (unsigned)fun.tag(), vm.frames.size());
            size_t lim = vm.frames.size();
            for (size_t i = lim; i > 0 && i + 8 > lim; --i) {
                const auto & fr = vm.frames[i - 1];
                const LambdaDescriptor * desc = nullptr;
                if (fr.thunk)
                    desc = fr.thunk->suspended.desc;
                else if (fr.closure)
                    desc = fr.closure->desc;
                std::fprintf(stderr,
                    "  frame[%zu]: %s code=[%u..) ip=%u flags=%u\n",
                    i - 1,
                    desc && !desc->name.empty() ? desc->name.c_str()
                        : (desc ? "<anon>" : "<closure-body>"),
                    desc ? desc->codeOffset : 0,
                    fr.ip, (unsigned)fr.flags);
            }
        }
        throw std::runtime_error("v3 callClosure: not callable");
    }

    const Closure * callee = fun.asClosure();
    const LambdaDescriptor * desc = callee->desc;

    // #495: native fix-point intrinsic -- mirrored from OP_CALL.
    // callClosure is the entry point primops + bridges use; the
    // intrinsic check must fire here too or recognised lambdas
    // dispatched via this path silently take the bytecode body.
    static const bool s_intrinsicEnable =
        std::getenv("NIX_V3_INTRINSIC_DISPATCH") != nullptr;
    if (s_intrinsicEnable && __builtin_expect(
            desc->intrinsicKind != LambdaDescriptor::Intrinsic::None, 0)) {
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::Fix) {
            // (Bridge-thunk arg guard retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
            bool argIsBridge = false;
            if (!argIsBridge) {
                allocStats().intrinsicFixCalls++;
                static const bool s_dbg =
                    std::getenv("V3_DBG_INTRINSIC") != nullptr;
                if (s_dbg) std::fprintf(stderr,
                    "v3 callClosure intrinsic Fix [#%llu]: arg.tag=%d\n",
                    (unsigned long long)allocStats().intrinsicFixCalls,
                    (int)arg.tag());
                Value * slotStorage = Alloc::allocValue();
                slotStorage->mkUninitialized();
                Value slotV;
                slotV.mkSlot(slotStorage);
                Value res = callClosure(vm, arg, slotV);
                cellWrite(slotStorage, res, nullptr);  // PhD-6: barrier raw writeback
                return res;
            }
        }

        // STG-13c (#509/#512): native dispatch for ExtendsBody --
        // mirrored from OP_CALL.  callClosure is the entry point for
        // primops + bridges, so the intrinsic check must fire here too
        // or recognised lambdas dispatched via this path silently take
        // the bytecode body.
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ExtendsBody
            && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
            && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues) {
            allocStats().intrinsicExtendsCalls++;
            Value overlay = closureUpvalue(callee, (uint16_t)desc->intrinsicVar0);
            Value f       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar1);
            Value final_  = arg;
            Value prev = callClosure(vm, f, final_);
            prev = forceValue(vm, prev);
            if (!prev.isAttrs() || !prev.asAttrs())
                throw std::runtime_error(
                    "v3 callClosure intrinsic ExtendsBody: prev not attrs");
            Value overlay_partial = callClosure(vm, overlay, final_);
            Value overlay_result  = callClosure(vm, overlay_partial, prev);
            overlay_result = forceValue(vm, overlay_result);
            if (!overlay_result.isAttrs() || !overlay_result.asAttrs())
                throw std::runtime_error(
                    "v3 callClosure intrinsic ExtendsBody: overlay-result not attrs");
            Bindings * merged = mergeBindings(prev.asAttrs(),
                                               overlay_result.asAttrs(),
                                               MergeBindingsSite::ExtendsTailPrev);
            Value res;
            res.mkAttrs(merged);
            return res;
        }

        // STG-13c (#509/#512): native dispatch for ComposeBody.
        if (desc->intrinsicKind == LambdaDescriptor::Intrinsic::ComposeBody
            && desc->intrinsicVar0 >= 0 && desc->intrinsicVar1 >= 0
            && desc->intrinsicVar2 >= 0
            && (uint16_t)desc->intrinsicVar0 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar1 < callee->nUpvalues
            && (uint16_t)desc->intrinsicVar2 < callee->nUpvalues) {
            allocStats().intrinsicComposeCalls++;
            Value f       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar0);
            Value g       = closureUpvalue(callee, (uint16_t)desc->intrinsicVar1);
            Value final_  = closureUpvalue(callee, (uint16_t)desc->intrinsicVar2);
            Value prev_   = arg;
            Value f_partial = callClosure(vm, f, final_);
            Value fApplied  = callClosure(vm, f_partial, prev_);
            fApplied = forceValue(vm, fApplied);
            if (!fApplied.isAttrs() || !fApplied.asAttrs())
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: fApplied not attrs");
            Value prevForced = forceValue(vm, prev_);
            if (!prevForced.isAttrs() || !prevForced.asAttrs())
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: prev not attrs");
            Bindings * prevPrimeB = mergeBindings(prevForced.asAttrs(),
                                                   fApplied.asAttrs(),
                                                   MergeBindingsSite::ExtendsTailPrevPrime);
            Value prevPrime;
            prevPrime.mkAttrs(prevPrimeB);
            Value g_partial = callClosure(vm, g, final_);
            Value gApplied  = callClosure(vm, g_partial, prevPrime);
            gApplied = forceValue(vm, gApplied);
            if (!gApplied.isAttrs() || !gApplied.asAttrs())
                throw std::runtime_error(
                    "v3 callClosure intrinsic ComposeBody: gApplied not attrs");
            Bindings * merged = mergeBindings(fApplied.asAttrs(),
                                               gApplied.asAttrs(),
                                               MergeBindingsSite::ComposeTailApplied);
            Value res;
            res.mkAttrs(merged);
            return res;
        }
    }

    // #424: selector-lambda fast path -- mirrored from OP_CALL.
    // callClosure is the entry point primops use for callback lambdas
    // (map, filter, foldl', etc.), so this fires on the dominant
    // `(p: p.name)`-style nixpkgs callbacks.
    if (__builtin_expect(desc->selectorSym != 0, 0)) {
        allocStats().selectorLambdaCalls++;
        Value sArg = arg;
        if (sArg.isThunk() || sArg.isAppLike()
            || sArg.tag() == Tag::Slot) {
            sArg = forceValue(vm, sArg);
        }
        if (!sArg.isAttrs() || !sArg.asAttrs())
            throw std::runtime_error(
                "v3 selector lambda: arg not an attrset");
        const Value * v = sArg.asAttrs()->lookup(desc->selectorSym);
        if (!v)
            throw std::runtime_error(
                "v3 selector lambda: missing attr");
        return *v;
    }

    // Phase 1.2 identity-lambda fast path — mirrored from OP_CALL.
    // Body is `x: x`; return arg directly with no frame push.
    if (__builtin_expect(desc->identityLambda, 0)) {
        return arg;
    }

    // Cross-CU calls (e.g., calling a closure returned from
    // builtins.import): use the closure's own CU when available.
    const CompilationUnit * cu = callee->cu ? callee->cu : vm.frames.back().cu;

    // Push a CALL frame for the callee — mirrors OP_CALL.
    size_t exitDepth = vm.frames.size();
    size_t newBase = vm.valueStack.size();
    vm.valueStack.resize(newBase + desc->nLocals);
    vm.valueStack[newBase + 0] = arg;

    // #498 frame-entry diagnostic for callClosure path.
    {
        static const char * s_filter =
            std::getenv("V3_DBG_FRAME_ENTRY");
        if (s_filter && desc && desc->name == s_filter) {
            Value chase = arg;
            int hops = 0;
            while (hops < 4) {
                if (chase.tag() == Tag::Slot && chase.asSlot())
                    chase = *chase.asSlot();
                else if (chase.tag() == Tag::Thunk && chase.asThunk()
                         && chase.asThunk()->state == ThunkState::Evaluated)
                    chase = chase.asThunk()->evaluated;
                else break;
                ++hops;
            }
            std::fprintf(stderr,
                "v3 FRAME_ENTRY callClosure %s codeOff=%u: local[0].tag=%d",
                desc->name.c_str(), (unsigned)desc->codeOffset,
                (int)arg.tag());
            if (chase.tag() == Tag::Attrs && chase.asAttrs()) {
                auto * b = chase.asAttrs();
                std::fprintf(stderr, " -> attrs size=%u {", b->size);
                const auto & tbl = ir::globalSymbolTable();
                for (uint32_t i = 0; i < b->size && i < 4; ++i) {
                    uint32_t nm = b->entries[i].name;
                    std::fprintf(stderr, "%s%s", i ? "," : "",
                        nm < tbl.size() ? tbl[nm].c_str() : "?");
                }
                if (b->size > 4) std::fprintf(stderr, ",...");
                std::fprintf(stderr, "}");
            } else {
                std::fprintf(stderr, " -> tag=%d", (int)chase.tag());
            }
            std::fprintf(stderr, " (frames=%zu)\n", vm.frames.size());
        }
    }

    uint32_t newWithBase = static_cast<uint32_t>(vm.withStack.size());

    vm.frames.push_back(CallFrame{
        .cu = cu,
        .closure = callee,
        .thunk = nullptr,
        .ip = desc->codeOffset,
        .stackBaseOffset = static_cast<uint32_t>(newBase),
        .withStackBase = newWithBase,
        .flags = 0,
    });
    pushCapturedWiths(vm, callee->capturedWiths);

    // 2026-05-17: exception cleanup happens inside dispatchLoop's
    // body-level try/catch; the plain tail call here enables compiler
    // TCO so callClosure's C-frame can be elided in favor of
    // dispatchLoop's.  Saves ~0.5 KB per call (callClosure's frame
    // size) which compounds across the App-spine / __functor /
    // intrinsic-dispatch recursive sites that re-enter callClosure
    // from inside forceValue or another callClosure body.
    return dispatchLoop(vm, exitDepth);
}

} // namespace nix::v3
