/// @file
/// v3 root-expression entry point implementation.
///
/// Mirrors the pipeline open-coded in `cli/v3-eval.cc:430-438`.  Lifted
/// to a shared helper so the integrated `nix` CLI can use the same
/// path under v3-direct (inversion phase 1).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/run.hh"
#include "v3/run_internal.hh"  // per-eval diagnostics extracted to run_diag.cc
#include "v3/vm.hh"
#include "v3/primop.hh"        // registerBuiltinPrimOps + applied-cache dumps
#include "v3/ir.hh"
#include "v3/alloc.hh"         // allocStats (WS-2 V2 IFD summary)
#include "v3/bytecode_primops.hh"
#include "v3/aot_cache.hh"      // WS5-B2 eager canonical-table adoption
#include "v3/live_trace.hh"     // flushPeriodicLiveTraceCsv (self-gated, default-on)
#include "v3/par_trace.hh"      // parallel-potential (work/span) trace instrument
#include "v3/forcerate_trace.hh" // per-creation-site force-rate histogram instrument
#include "v3/dedup_survey.hh"   // #772 surveyCUBytecodeDedup (outer-CU survey)
#include "v3/bytecode.hh"       // CompilationUnit + ifdProbeKindName (IFD summary)
#include "v3/cu_registry.hh"    // allRegisteredCus (COMPILE-WASTE spike walk)
#include "v3/limits.hh"

#include "v3/ffi.hh"  // ffi::symbols/positions + EvalState fwd — no direct eval.hh

// PARSER_PROJECT_PLAN §5.3: the native parse+lower+run entry, so the
// `nix` binary's CLI (src/nix/eval.cc) needn't pull parser/cli headers.
#include "v3-parse-api.hh"   // nix::v3::parser::parseString
#include "lower_v3.hh"       // canLowerV3 + lowerV3Ast
#include "v3/tw_baseenv.hh"  // twBaseEnvGlobals

// NOTE: the per-eval DIAGNOSTIC headers (import_timing / disk_cache /
// cache_probe / precise_root / disasm / serialize / value_serialize /
// nursery / barrier / gc-config + <algorithm> / <sys/resource.h> / <gc/gc.h>)
// moved to run_diag.cc with the NIX_VM_STATS / V3_TIMING dump bodies.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nix::v3 {

/// Force-link the v3 library.  Called once from `mainWrapped` so the
/// linker's `-dead_strip_dylibs` pass keeps libnixexprv3 in the
/// binary.  No side effects.
bool keepLibAlive()
{
    return true;
}

// Forward decl (defined in the anonymous namespace before
// runRootExprFromString): the top-level result cache v1 shadow stats dump,
// called from runRootExprModule's end-of-eval stats region below.

// ---------------------------------------------------------------------------
// COMPILE-WASTE spike (2026-08-07, TEMPORARY instrument) — see emit.cc's
// g_compileWaste comment + Rule-0 retirement criterion.  Sizes the prize for
// "deferred per-attribute compilation": what fraction of emitted attr/let
// value-body bytecode belongs to a thunk that is NEVER forced (so a lazy
// per-attr compiler would never have compiled it).
// ---------------------------------------------------------------------------
namespace {

// Re-entrancy depth so the report fires exactly once, at the OUTERMOST
// runRootExprModule (the bytecode-primop install path + any re-entrant compile
// each call this recursively; only the top-level eval should print).
thread_local int g_compileWasteDepth = 0;
struct CompileWasteDepthGuard {
    CompileWasteDepthGuard()  noexcept { ++g_compileWasteDepth; }
    ~CompileWasteDepthGuard() noexcept { --g_compileWasteDepth; }
    bool outermost() const noexcept { return g_compileWasteDepth == 1; }
};

// Cross-reference per-funcId emitted bytecode BYTES (rt.compileWaste, set at
// emit) with per-funcId alloc/force counts (rt.lambdaState, bumped at runtime)
// across every CU that ran this eval, and print the wasted-compile fraction.
// COLD — called once at eval-end.
void dumpCompileWasteReport()
{
    uint64_t walkedTotal = 0, attrBodyBytes = 0;
    uint64_t abForced = 0, abAllocNeverForced = 0, abNeverAlloc = 0;
    uint64_t abForcedN = 0, abAllocNeverForcedN = 0, abNeverAllocN = 0;
    uint64_t nCUs = 0, nFuncs = 0, nAttrFuncs = 0;

    for (const CompilationUnit * cu : allRegisteredCus()) {
        if (!cu) continue;
        ++nCUs;
        const auto & cw = cu->rt.compileWaste;
        const size_t nf = cu->lambdas.size();
        for (size_t fid = 0; fid < nf; ++fid) {
            if (fid >= cw.size()) continue;  // not emitted under the flag
            const uint32_t bytes = cw[fid].codeBytes;
            walkedTotal += bytes;
            ++nFuncs;
            if (!cw[fid].isAttrBody) continue;
            attrBodyBytes += bytes;
            ++nAttrFuncs;
            const auto ls = cu->lambdaStateAt(fid);
            if (ls.allocCount == 0)      { abNeverAlloc       += bytes; ++abNeverAllocN; }
            else if (ls.forceCount == 0) { abAllocNeverForced += bytes; ++abAllocNeverForcedN; }
            else                         { abForced           += bytes; ++abForcedN; }
        }
    }

    const uint64_t wasted      = abNeverAlloc + abAllocNeverForced;   // attr-body, never forced
    const uint64_t globalTotal = compileWasteTotalEmittedBytes();     // ALL compiles (cold-compile cost)
    auto pct = [](uint64_t a, uint64_t b) {
        return b ? 100.0 * double(a) / double(b) : 0.0;
    };

    std::fprintf(stderr,
        "\nv3 COMPILE-WASTE (NIX_V3_COMPILE_WASTE) — deferred-per-attr-compile prize\n"
        "  CUs walked=%llu  functions=%llu  attr-body-thunk functions=%llu\n"
        "  total emitted bytecode:    %10llu B  (all compile() calls = cold-compile cost)\n"
        "  walked emitted bytecode:   %10llu B  (functions in CUs that ran)\n"
        "  attr-body-thunk bytecode:  %10llu B  (%.1f%% of total)\n"
        "    allocated + forced:      %10llu B  (n=%llu)\n"
        "    allocated, NEVER forced: %10llu B  (n=%llu)\n"
        "    NEVER allocated:         %10llu B  (n=%llu)\n"
        "  WASTED (attr-body never forced): %llu B\n"
        "    = %.1f%% of total emitted bytecode   [FALSIFIER metric]\n"
        "    = %.1f%% of attr-body-thunk bytecode\n",
        (unsigned long long) nCUs, (unsigned long long) nFuncs,
        (unsigned long long) nAttrFuncs,
        (unsigned long long) globalTotal,
        (unsigned long long) walkedTotal,
        (unsigned long long) attrBodyBytes, pct(attrBodyBytes, globalTotal),
        (unsigned long long) abForced, (unsigned long long) abForcedN,
        (unsigned long long) abAllocNeverForced, (unsigned long long) abAllocNeverForcedN,
        (unsigned long long) abNeverAlloc, (unsigned long long) abNeverAllocN,
        (unsigned long long) wasted,
        pct(wasted, globalTotal),
        pct(wasted, attrBodyBytes));
}

} // namespace

RootResult runRootExprModule(nix::EvalState & state, ir::Module module)
{
    // COMPILE-WASTE spike: fire the eval-end report only at the outermost call.
    CompileWasteDepthGuard compileWasteGuard;

    // Idempotent: register the builtin primop table on first call.
    // Safe to call per-invocation — the underlying registry is global
    // and de-duplicates by name.  The registration cost is constant
    // (one-time map fill) so per-call overhead is negligible.
    registerBuiltinPrimOps();

    // #741 Phase 5b (2026-05-23): a per-eval `EvalResultBatchGuard`
    // wrapped this body in `disk_cache::beginEvalResultBatch` /
    // `commitEvalResultBatch` to amortise the ~1 ms / insert commit
    // cost over a single COMMIT.  REMOVED — hyperfine measurement:
    //   COLD ACTIVE+DISK (unbatched): 1429 ms ± 31 ms
    //   COLD ACTIVE+DISK (batched):   1666 ms ± 764 ms
    // Variance ballooned by 24× and the mean got WORSE, not better.
    // Hypothesised cause: long-held transaction with ~256 KB of
    // pending WAL data triggers SQLite's checkpoint behavior at
    // COMMIT in non-deterministic ways (interaction with APFS /
    // page-cache flushes).  The `disk_cache::beginEvalResultBatch` /
    // `commitEvalResultBatch` helpers ARE kept in the disk_cache
    // namespace as gated infrastructure (no callers in production)
    // for future iteration — try smaller batches (e.g. every 50
    // inserts) or explicit `PRAGMA wal_autocheckpoint=0` tuning.

    // Wire the global tlNixEvalState pointer so v3 primops that need
    // to reach back into TW (e.g. `import`, derivation strict-merge,
    // store-side path operations) can find it.  Caller is responsible
    // for keeping `state` alive for the lifetime of any returned Bridge
    // thunks; see `primops.cc treeWalkerToV3` nFunction case for the
    // address-stability contract.
    setNixEvalState(&state);

    // Phase 1.6 — initialise resource limits.  Idempotent across
    // subsequent runRootExpr calls.  Reads NIX_V3_MAX_HEAP /
    // NIX_V3_MAX_CPU_TIME / NIX_V3_MAX_WALL_TIME and installs the
    // Boehm OOM handler if heap cap is set.  Cheap (one mutex + a
    // boolean check) when re-entered.
    initLimits();

    // A12b T0: install bytecode replacements for callback primops
    // (foldl' / map / filter / etc.) once per process.  The install
    // itself runs runRootExpr recursively to compile each primop's
    // Nix source; the function has its own thread-local guard that
    // short-circuits on recursive entry so we don't loop.  Disable
    // with NIX_V3_NO_BYTECODE_PRIMOPS=1 for A/B comparison.
    static const bool s_noBytecodePrimops =
        std::getenv("NIX_V3_NO_BYTECODE_PRIMOPS") != nullptr;
    if (!s_noBytecodePrimops)
        installAllBytecodePrimops(state);

    // V3_TIMING phase split — capture lower / compile / run / bridge
    // phase durations so bench harnesses can attribute time.  A no-op
    // (zero overhead) when V3_TIMING is unset.
    PhaseTimer pt;

    // DIAG-4 (2026-05-29 evening, per DIAGNOSTIC_AUDIT §6.4 + user
    // directive on "elsewhere is ominous"): per-phase ALLOCATION
    // accounting.  Captures arena.bytesAllocated() + per-Tag byte
    // totals at each phase boundary so we can attribute v3 arena
    // growth to lower/optimise/compile/run.  The snapshot type +
    // `takePhaseSnap()` live in run_internal.hh / run_diag.cc; here we
    // just thread the five boundary snapshots into `dumpVmStats` below.
    //
    // No per-allocator modification needed — snapshots at boundaries
    // give us delta per phase.  Cost: 6 × small struct copy.
    //
    // Dumped under NIX_VM_STATS at the end of runRootExpr.
    PhaseSnaps snaps;
    snaps.start         = takePhaseSnap();
    snaps.afterLower    = snaps.start;
    snaps.afterOptimise = snaps.start;
    snaps.afterCompile  = snaps.start;
    snaps.afterRun      = snaps.start;

    // `module` is the already-lowered IR (from lowerV3Ast — the native
    // parse+lower path; there is no longer a nix::Expr lowering path).
    pt.mark(pt.lower_ms);

    // #538: run the IR optimization pipeline (constant fold, CSE,
    // strictness, alias inline, primop fuse, DCE).  Without this the
    // v3-direct path emits massive amounts of redundant SET_LOCAL /
    // GET_LOCAL through trivial bindings, plus per-LitInt force
    // overhead — the lowerer's A-normal-form-style binding-per-
    // subexpression pattern bloats the bytecode unless the optimizer
    // collapses VarRef chains and elides redundant Forces.  The
    // import-primop path (`primops.cc primImport`) already does this;
    // the runRootExpr path silently skipped it before this fix.
    static const bool s_noOptimise =
        std::getenv("NIX_V3_NO_OPTIMISE") != nullptr;
    snaps.afterLower = takePhaseSnap();  // DIAG-4
    if (!s_noOptimise) ir::optimise(module);
    pt.mark(pt.optimise_ms);
    snaps.afterOptimise = takePhaseSnap();  // DIAG-4

    // #737 Stage 4 v2: per-Function strictness inference.  Runs
    // AFTER `optimise` (so any DCE-removed dead bindings and any
    // inlined VarRef aliases are already collapsed) and BEFORE
    // `computeFreeVars`.  Result is stored in
    // `ir::Function::strictArgs`.
    //
    // #774 (2026-05-23): kept on the outer expression only.
    // Falsifier: moving these into `ir::optimise()` (so primImport
    // also exercises them) measured 1 elision in 40 961 considered
    // Apps on hello.drvPath at ~33 ms wall-clock cost.  The
    // bottleneck is `isInlinableMkThunk`'s single-use + simple-body
    // constraints, not strictness analysis coverage — so per-module
    // Stage 4 was falsified and retired (the gate is gone).
    // #742/#743 caller-side strictness: computeFunctionStrictness then
    // applyStrictnessAtCallSites to a fixpoint (compound shapes — an outer
    // MkThunk over an AttrSet whose entries are themselves thunked — need a
    // pass per nesting level).  Factored into ir::applyStrictnessPasses so the
    // `--emit-bytecode` dump runs the IDENTICAL sequence (see ir.hh).
    ir::applyStrictnessPasses(module);

    // computeFreeVars: populates each `ir::Function::freeVars` from
    // `Function::vars`.  Required before `compile` so the emitter
    // knows which upvalues each closure captures.  Must run AFTER
    // `optimise` so any newly-introduced VarRef aliases are walked.
    ir::computeFreeVars(module);

    // Compile IR to bytecode.  The CompilationUnit owns
    // `stringConstants` referenced by OP_LIT_STR / OP_LIT_PATH; the
    // resulting Value's string/path payloads point into that vector.
    //
    // #676 — heap-allocate the CU via make_unique so its address is
    // stable across any RootResult moves the caller may do (e.g.
    // `std::optional::emplace` in eval.cc --apply).  Closures emitted
    // by `run()` capture `c->cu = &cu` at OP_MAKE_CLOSURE time; if
    // the CU lived inside RootResult by value, a subsequent move
    // would leave every captured cu pointer dangling.  Pre-#676 this
    // manifested as a SIGTRAP on `nix eval --impure --apply '(x: 42)'
    // --expr '1'` — the closure's stale cu pointer made dispatchLoop
    // read garbage bytecode.
    RootResult out{std::make_unique<CompilationUnit>(compile(module)), Value{}};
    // #772 spike: survey the OUTER expression's bytecode dedup too,
    // so the survey reflects ALL compiled CUs, not just inner-import
    // ones.  No-op when NIX_V3_DEDUP_SURVEY is unset.
    surveyCUBytecodeDedup(*out.cu);
    pt.mark(pt.compile_ms);
    snaps.afterCompile = takePhaseSnap();  // DIAG-4

    // Run.  STG-10 (vm.cc:5530) automatically routes through
    // `runOnExistingVm` if we're re-entered from another v3 dispatch
    // loop — so calling `runRootExpr` from inside a primop is safe.
    //
    // #795 Phase A1+: emit stats even when run() throws (e.g.
    // WallTimeExceededError on long-running haskell.nix evals).  Lets
    // hypothesis-triage tests collect per-site bridge counts via a
    // bounded-time probe rather than requiring the eval to complete.
    // The static dump-stats gate below decides whether to emit; this
    // catch only ensures the emission HAPPENS before re-throw.
    //
    // #875 Stage 1.5 (2026-05-29): tried setting the root Expr as
    // the default fallback for bridges created during this eval.
    // No effect — bridges created downstream inside primV3CallBridge1
    // / primV3ForceAttr / primV3ForceListElem are wrapped by inner
    // `ScopedBridgeFallbackExpr` guards that overwrite tl to the
    // per-call fallback (typically nullptr today).  The runRootExpr-
    // level guard is shadowed.  Stage 1.5 proper requires either an
    // API refactor (v3ToTreeWalker takes Expr*) or per-bridge-
    // creation-site Scoped guards.  See
    // WEAK_BRIDGE_EVICTION_DESIGN_2026-05-29.md.
    try {
        out.value = run(*out.cu);
    } catch (...) {
        // NIX_VM_STATS abort dump lives in run_diag.cc (dumpVmAbortStats);
        // the gate stays here so default eval never crosses the TU boundary.
        static const bool s_dumpStatsOnThrow =
            std::getenv("NIX_VM_STATS") != nullptr;
        if (s_dumpStatsOnThrow) dumpVmAbortStats();
        throw;
    }
    pt.mark(pt.run_ms);
    snaps.afterRun = takePhaseSnap();  // DIAG-4
    // (bridge-table DIAG clear retired — TW_VALUE_ERADICATION F4, 2026-06-02.)

    // Periodic L(t) CSV flush (Step 4 of post-Phase-3.8).  Hoisted OUT of
    // the NIX_VM_STATS block (2026-06-15): the periodic live-trace is its
    // own self-contained feature gated by NIX_V3_LIVE_TRACE_PERIODIC, and
    // requiring the unrelated NIX_VM_STATS to also be set to get the CSV
    // was a footgun (samples accumulated but never flushed).  The function
    // self-gates (no-op unless periodicLiveTraceEnabled()), so this is
    // unconditional and runs exactly once.
    flushPeriodicLiveTraceCsv();

    // LEVER-1 applied-import cache PROBE: self-gated (NIX_V3_APPLIED_CACHE=
    // probe + non-zero counters), unconditional here for the same reason as
    // flushPeriodicLiveTraceCsv above — the atexit variant loses its output in
    // the `nix` binary.  Cumulative; the LAST line per process is authoritative.
    dumpAppliedCacheProbeStats();
    appliedCacheStatsDump();   // LEVER-1 real-cache counters (self-gates on activity)
    // Parallel-potential trace (NIX_V3_PAR_TRACE): work/span ceiling on
    // intra-eval parallelism — the measure-first input for the
    // parallel-eval candidate (PARALLEL_EVAL_CAPABILITIES §8/§9).  Placed
    // here (NOT inside the NIX_VM_STATS block) + self-gated internally so
    // the integrated `nix` CLI (flake/IFD workloads like M5) reaches it —
    // the atexit variant loses output in the `nix` binary, same reason as
    // appliedCacheStatsDump above.  Delete with the instrument once the
    // parallel-eval GO/NO-GO is decided (Rule 0: no lingering opt-in gate).
    nix::v3::partrace::dumpReport();
    // Per-creation-site force-rate histogram (NIX_V3_FORCERATE_TRACE): the
    // cheap-eagerness / optimistic-eval measure-first gate. Same placement
    // rationale + self-gate + retirement rule as partrace above.
    nix::v3::forcerate::dumpReport();

    // NIX_VM_STATS=1: dump alloc counters at completion.  Lets us
    // attribute alloc explosions to thunks vs closures vs Bindings
    // vs lists.
    static const bool s_dumpStats =
        std::getenv("NIX_VM_STATS") != nullptr;
    if (__builtin_expect(s_dumpStats, 0))
        dumpVmStats(snaps);

    // COMPILE-WASTE spike: at the OUTERMOST eval only, report the fraction of
    // emitted attr/let value-body bytecode whose thunk was never forced.
    if (__builtin_expect(compileWasteActive(), 0) && compileWasteGuard.outermost())
        dumpCompileWasteReport();

    return out;
}

// PARSER_PROJECT_PLAN §5.3: native parse+lower+run from raw `.nix` source
// — NO nix::Expr.  The single library entry the CLI (src/nix/eval.cc) and
// any other top-level caller use, so they needn't pull the parser/cli
// headers.  `basePath`/`homePath` resolve relative/`~` path literals (as
// TW's parseExprFromFile/String does); `origin` is the source's
// PosTable::Origin (Pos::Origin(sp) / Pos::String / Pos::Stdin) so
// positions match TW.  canLowerV3 is total for parser-produced ASTs, so
// the throw is a should-never-fire guard.
namespace {
// WS-2 V2 (2026-07-13): default-on end-of-eval IFD visibility.  Emits ONE (or
// two) stderr lines when the eval touched any IFD candidate — so CI SEES IFD
// activity without enabling any diagnostic — and is SILENT otherwise (the
// common pure-eval / `nix build` case: no context-bearing reads → nothing
// printed).  The line goes to stderr DURING eval, before the CLI writes the
// result value to stdout, so value-capturing callers (`… | tail -1`) are
// unaffected.  v3 measures candidate counts + realise-blocked wall-time
// unconditionally; the per-derivation built/substituted/ms detail still
// requires `--option profile-import-from-derivation true` (which populates
// nrIFDs/totalIFDTime — those are private EvalState members populated only
// under that setting, and NIX_SHOW_STATS already emits them, so V2 stays
// purely v3-native and points there for the per-derivation detail).
static void emitIfdEndOfEvalSummary(std::chrono::steady_clock::time_point wallStart)
{
    const auto & a = allocStats();
    uint64_t candidates = 0;
    for (int k = 1; k < (int) kIfdProbeKindCount; ++k)
        candidates += a.ifdProbeWithCtx[k];
    // Only real IFD activity triggers output: context-bearing IFD-class reads
    // or timed context-bearing realises.  Plain source-file reads (no context)
    // never count, so a pure eval / `nix build` stays silent.
    if (candidates == 0 && a.ifdRealiseCalls == 0)
        return;

    double blockedS = (double) a.ifdRealiseNanos / 1e9;
    double wallS = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - wallStart).count();
    double pct = wallS > 0.0 ? 100.0 * blockedS / wallS : 0.0;

    std::string kinds;
    for (int k = 1; k < (int) kIfdProbeKindCount; ++k)
        if (a.ifdProbeWithCtx[k] > 0) {
            kinds += " ";
            kinds += ifdProbeKindName(static_cast<uint8_t>(k));
            kinds += "=";
            kinds += std::to_string((unsigned long long) a.ifdProbeWithCtx[k]);
        }

    std::fprintf(stderr,
        "v3: IFD — %llu context-bearing IFD-class read(s):%s; "
        "blocked %.3fs in realise across %llu call(s) (%.1f%% of %.3fs eval wall). "
        "Per-derivation build/substitute/ms detail: "
        "--option profile-import-from-derivation true\n",
        (unsigned long long) candidates,
        kinds.empty() ? " (none)" : kinds.c_str(),
        blockedS, (unsigned long long) a.ifdRealiseCalls, pct, wallS);
}
} // namespace

RootResult runRootExprFromString(nix::EvalState & state, const std::string & source,
                                 const std::string & basePath, const std::string & homePath,
                                 const nix::SourcePath * originPath)
{
    registerBuiltinPrimOps();  // before lowering (lower-time findPrimOp)
    // WS5-B2 — adopt the AOT canonical symbol/pos id assignment NOW, before the
    // root expr is lowered below (the first symbol-interning event).  init() is
    // idempotent (does its work once, on the first call) and a near-no-op when
    // NIX_V3_AOT_CACHE_FILE is unset, so calling it at every (incl. nested)
    // entry is free.  Placing it here — ahead of lowerV3Ast — is what lets the
    // reader's own interns land on the writer's canonical ids, so borrowed CU
    // code stays un-rewritten (Shared_Clean).  See aot_cache::init.
    aot_cache::init();
    // Re-entry depth: runRootExprModule installs bytecode primops via NESTED
    // runRootExprFromString calls; the top-level cache acts ONLY on the
    // outermost (the user's actual expr), never the installer sub-evals.
    static thread_local int s_rrDepth = 0;
    struct DepthGuard { int & d; ~DepthGuard() { --d; } } _dg{s_rrDepth};
    ++s_rrDepth;
    // WS-2 V2: outermost eval wall-clock start, for the default-on IFD summary
    // ("blocked X.Xs = Y% of eval wall").  Captured per-invocation; only the
    // depth-1 frame's span covers the whole eval (incl. nested imports).
    auto _v2WallStart = std::chrono::steady_clock::now();
    nix::v3::ast::ParserState st;
    st.basePath = basePath;
    st.homePath = homePath;
    nix::v3::parser::parseString(st, source);
    if (!canLowerV3(st.result))
        throw nix::Error("v3: native lowering cannot handle this expression");
    // Build the position origin: a file (Pos::Origin(*originPath)) when
    // given, else an in-memory string.  Done here so run.hh's signature
    // carries no `nix/...` position type.
    auto origin = originPath
        ? ffi::positions(state).addOrigin(nix::Pos::Origin(*originPath), source.size())
        : ffi::positions(state).addOrigin(
              nix::Pos::String{.source = nix::make_ref<std::string>(source)}, source.size());
    auto module = lowerV3Ast(ffi::symbols(state), st.result, &ffi::positions(state), origin,
                             &twBaseEnvGlobals(state));
    auto rr = runRootExprModule(state, std::move(module));
    // Re-entry depth: runRootExprModule installs bytecode primops via NESTED
    // runRootExprFromString calls; only the OUTERMOST frame's wall-span covers
    // the whole eval, so the default-on IFD summary runs on depth 1 only.
    if (s_rrDepth == 1) {
        emitIfdEndOfEvalSummary(_v2WallStart);  // WS-2 V2 (default-on, silent at 0 IFDs)
    }
    return rr;
}

// Synthetic-source overload (no path literals): builds a Pos::String
// origin (originPath = null) + empty base/home so callers (the
// bytecode-primop installer) needn't touch eval.hh / parser / position
// headers.
RootResult runRootExprFromString(nix::EvalState & state, const std::string & source)
{
    return runRootExprFromString(state, source, /*basePath*/ "", /*homePath*/ "",
                                 /*originPath*/ nullptr);
}

} // namespace nix::v3
