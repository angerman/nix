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
#include "v3/lower.hh"
#include "v3/vm.hh"
#include "v3/primop.hh"
#include "v3/ir.hh"
#include "v3/alloc.hh"
#include "v3/bytecode_primops.hh"
#include "v3/import_timing.hh"  // #769 per-import phase totals
#include "v3/disk_cache.hh"     // #770 cache-hit/miss stats dump
#include "v3/cache_probe.hh"    // #827 / A3 per-call-site cache-hook dump
#include "v3/precise_root.hh"   // 2026-05-27 Stage 3: dumpAllV3Roots diagnostic
#include "v3/live_trace.hh"     // 2026-05-27 Stage 6 SPIKE: live-fraction trace
#include "v3/dedup_survey.hh"   // #772 Stage 9 L0 spike
#include "v3/disasm.hh"         // #778 opcount dumper — opName()
#include "v3/bytecode.hh"
#include "v3/serialize.hh"      // #777b deserialize per-section timing
#include "v3/value_serialize.hh" // #741 Phase 1 round-trip stats dump
#include "v3/limits.hh"
#include "v3/nursery.hh"
#include "v3/barrier.hh"

#include "nix/expr/eval.hh"

#include "nix/expr/config.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#if NIX_USE_BOEHMGC
#include <gc/gc.h>
#endif

namespace nix::v3 {

namespace {

/// Phase-timing helper: present iff `V3_TIMING` env var is set.  We
/// cache the env-var lookup once per process via a static-const-bool;
/// mirrors the pattern used throughout vm.cc / primops.cc for
/// hot-path env-var checks (see #538 follow-ups).
struct PhaseTimer {
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;
    bool active;
    TP start;
    double lower_ms = 0, compile_ms = 0, optimise_ms = 0, run_ms = 0;
    explicit PhaseTimer() : active(s_active())
    {
        if (active) start = Clock::now();
    }
    void mark(double & accum)
    {
        if (!active) return;
        TP now = Clock::now();
        accum += std::chrono::duration<double, std::milli>(now - start).count();
        start = now;
    }
    ~PhaseTimer()
    {
        if (!active) return;
        // Sum bridge time (TW-side) from the per-kind telemetry so
        // we can report the v3-VM / TW split.  Returns 0 unless
        // NIX_V3_BRIDGE_TIMING=1 was set; in that case the bridge
        // callbacks accumulate wall-time at each call site.
        double bridge_ms = bridgeTotalNs() / 1e6;
        // VM-time is run_ms minus bridge_ms (the time spent inside
        // TW callbacks reached from v3, otherwise accounted under
        // run).  Clamp negative (clock granularity) to 0.
        double vm_ms = run_ms - bridge_ms;
        if (vm_ms < 0) vm_ms = 0;
        std::fprintf(stderr,
            "v3-direct timing (ms): lower=%.3f optimise=%.3f compile=%.3f "
            "run=%.3f vm=%.3f bridge=%.3f\n",
            lower_ms, optimise_ms, compile_ms, run_ms, vm_ms, bridge_ms);
        // If bridge timing is enabled, also dump the per-kind
        // breakdown so we can see WHERE the TW time goes.
        if (bridgeTimingEnabled())
            dumpBridgeTelemetry(stderr);
        // #769: per-import phase breakdown — splits the outer `run`
        // bucket into work done inside primImport recursions.  Cheap
        // (a handful of uint64_t accumulators bumped under the V3_TIMING
        // gate).
        const auto & it = importTimingTotals();
        if (it.calls + it.resultCacheHits + it.contentCacheHits + it.diskCacheHits > 0) {
            std::fprintf(stderr,
                "v3-direct import timing (ms): calls=%llu (compile %.3f, miss path) | "
                "cacheHits result=%llu content=%llu disk=%llu | "
                "parse=%.3f lower=%.3f optimise=%.3f compile=%.3f "
                "run=%.3f keyCompute=%.3f diskLookup=%.3f deserialize=%.3f diskInsert=%.3f\n",
                (unsigned long long)it.calls,
                (it.parseNs + it.lowerNs + it.optimiseNs + it.compileNs) / 1e6,
                (unsigned long long)it.resultCacheHits,
                (unsigned long long)it.contentCacheHits,
                (unsigned long long)it.diskCacheHits,
                it.parseNs    / 1e6,
                it.lowerNs    / 1e6,
                it.optimiseNs / 1e6,
                it.compileNs  / 1e6,
                it.runNs      / 1e6,
                it.keyComputeNs / 1e6,
                it.diskLookupNs / 1e6,
                it.deserializeNs / 1e6,
                it.diskInsertNs / 1e6);
        }
    }
private:
    static bool s_active()
    {
        static const bool v = std::getenv("V3_TIMING") != nullptr;
        return v;
    }
};

} // anonymous namespace

/// Force-link the v3 library.  Called once from `mainWrapped` so the
/// linker's `-dead_strip_dylibs` pass keeps libnixexprv3 in the
/// binary.  No side effects.
bool keepLibAlive()
{
    return true;
}

RootResult runRootExpr(nix::EvalState & state, nix::Expr * e)
{
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

    // Lower the AST → IR → bytecode.  `lowerNixExpr` requires `e` to
    // have had `bindVars` applied; the caller's contract.
    auto module = lowerNixExpr(e, state.symbols, state.positions);
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
    if (!s_noOptimise) ir::optimise(module);
    pt.mark(pt.optimise_ms);

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
    // constraints, not strictness analysis coverage.  Opt-in to
    // all-modules via `NIX_V3_STAGE4_ALL_MODULES=1`.
    ir::computeFunctionStrictness(module);

    // #742 Stage 4 v4 / #743 v4.1: caller-side use of strictness
    // signature.  Each pass elides single-use MkThunk wraps at
    // strict positions where the callee is statically known.  We
    // iterate up to 8× to handle compound shapes: e.g. `f { a =
    // [1 2 3]; }` has both an outer MkThunk wrapping the AttrSet
    // AND inner MkThunk wraps on the entries' values.  Pass 1
    // elides the outer wrap (exposing the AttrSet directly); pass
    // 2 sees the AttrSet and elides the strict-formal entries'
    // wraps.  Iteration terminates when a pass returns 0 elisions.
    for (int it = 0; it < 8; ++it) {
        if (ir::applyStrictnessAtCallSites(module) == 0) break;
    }

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
    try {
        out.value = run(*out.cu);
    } catch (...) {
        static const bool s_dumpStatsOnThrow =
            std::getenv("NIX_VM_STATS") != nullptr;
        if (s_dumpStatsOnThrow) {
            const auto & a = allocStats();
            std::fprintf(stderr,
                "v3-direct ABORT alloc: thunksForced=%llu insns=%llu\n",
                (unsigned long long)a.thunksForced,
                (unsigned long long)a.bytecodeInstructions);
            uint64_t totalV3Tw = 0;
            for (uint8_t i = 0; i < 16; ++i) totalV3Tw += a.v3ToTwBySite[i];
            if (totalV3Tw > 0) {
                static const char * kSiteNames[16] = {
                    "primReadFile_string_ctx", "primReadDir_attrset",
                    "primImport_string_ctx",   "primImport_attrset",
                    "primReadDir_string_ctx",  "primPathExists_ctx",
                    "primDerivationStrict_TWfb","primV3CallBridge1",
                    "FFI_leaves(fetch/path)",  "v3ToTW_eager_struct",
                    "primTrace",               "primV3ForceAttr_inner",
                    "primV3ForceListElem_inner","site_13",
                    "site_14",                 "unattributed_other",
                };
                std::fprintf(stderr,
                    "v3-direct ABORT v3ToTreeWalker (total=%llu):",
                    (unsigned long long)totalV3Tw);
                for (uint8_t i = 0; i < 16; ++i) {
                    if (a.v3ToTwBySite[i] == 0) continue;
                    std::fprintf(stderr, " %s=%llu",
                        kSiteNames[i],
                        (unsigned long long)a.v3ToTwBySite[i]);
                }
                std::fprintf(stderr, "\n");
            }
            uint64_t totalProbes = 0;
            for (uint8_t i = 0; i < 16; ++i) totalProbes += a.ifdProbeWithCtx[i];
            if (totalProbes > 0) {
                std::fprintf(stderr,
                    "v3-direct ABORT ifd probes (with-ctx): total=%llu",
                    (unsigned long long)totalProbes);
                for (uint8_t k = 1; k < 16; ++k)
                    if (a.ifdProbeWithCtx[k] > 0)
                        std::fprintf(stderr, " %s=%llu",
                            ifdProbeKindName(static_cast<uint8_t>(k)),
                            (unsigned long long)a.ifdProbeWithCtx[k]);
                std::fprintf(stderr, "\n");
            }
            std::fflush(stderr);
        }
        throw;
    }
    pt.mark(pt.run_ms);

    // NIX_VM_STATS=1: dump alloc counters at completion.  Lets us
    // attribute alloc explosions to thunks vs closures vs Bindings
    // vs lists.
    static const bool s_dumpStats =
        std::getenv("NIX_VM_STATS") != nullptr;
    if (__builtin_expect(s_dumpStats, 0)) {
        const auto & a = allocStats();
        std::fprintf(stderr,
            "v3-direct alloc: values=%llu closures=%llu thunks=%llu "
            "lists=%llu attrsets=%llu pairs=%llu thunksForced=%llu bridge=%llu insns=%llu\n",
            (unsigned long long)a.valuesAllocated,
            (unsigned long long)a.closuresAllocated,
            (unsigned long long)a.thunksAllocated,
            (unsigned long long)a.listsAllocated,
            (unsigned long long)a.attrsetsAllocated,
            (unsigned long long)a.pairsAllocated,
            (unsigned long long)a.thunksForced,
            (unsigned long long)a.bridgeThunksForced,
            (unsigned long long)a.bytecodeInstructions);
        // #702: BYTES per allocation category.  The count counters
        // above are partly bumped at primop call sites and miss
        // Alloc::* invocations from vm.cc dispatch; the byte
        // counters are bumped inside Alloc::* itself so they are
        // authoritative.  Use them to attribute non-Boehm RSS
        // growth (per `hello-drvpath-analysis.md`: the 4 GB on
        // hello.drvPath lives outside Boehm — these byte counters
        // tell us which v3 subsystem owns the growth).
        const uint64_t totalAllocBytes =
              a.bytesValues + a.bytesClosures + a.bytesThunks + a.bytesEnvs
            + a.bytesLists  + a.bytesBindings + a.bytesPairs   + a.bytesChars;
        std::fprintf(stderr,
            "v3-direct bytes (in arena/nursery): values=%.1fMB closures=%.1fMB "
            "thunks=%.1fMB envs=%.1fMB lists=%.1fMB bindings=%.1fMB pairs=%.1fMB "
            "chars=%.1fMB total_alloc=%.1fMB arena_pinned=%.1fMB\n",
            a.bytesValues   / 1e6,
            a.bytesClosures / 1e6,
            a.bytesThunks   / 1e6,
            a.bytesEnvs     / 1e6,
            a.bytesLists    / 1e6,
            a.bytesBindings / 1e6,
            a.bytesPairs    / 1e6,
            a.bytesChars    / 1e6,
            totalAllocBytes / 1e6,
            threadArena().bytesAllocated() / 1e6);
        // #703: per-Bindings-size histogram.  Tells us how much of
        // the 2.97 M Bindings would benefit from Empty/Single/Small
        // sentinel shapes vs. how many really need the full Sorted
        // form.  Buckets are: 0, 1, 2, 3-4, 5-8, 9-16, 17-32, 33-64,
        // 65-128, 129+.
        const auto & bk = a.attrsetSizeBuckets;
        std::fprintf(stderr,
            "v3-direct bindings size hist: 0=%llu 1=%llu 2=%llu "
            "3-4=%llu 5-8=%llu 9-16=%llu 17-32=%llu 33-64=%llu "
            "65-128=%llu 129+=%llu\n",
            (unsigned long long)bk[0], (unsigned long long)bk[1],
            (unsigned long long)bk[2], (unsigned long long)bk[3],
            (unsigned long long)bk[4], (unsigned long long)bk[5],
            (unsigned long long)bk[6], (unsigned long long)bk[7],
            (unsigned long long)bk[8], (unsigned long long)bk[9]);
        // #821 (2026-05-26): per-caller mergeBindings attribution.
        // On HNE .hello.drvPath, mergeBindings owns ~584 MB of 705 MB
        // Bindings allocation (82.9 %).  This per-site breakdown
        // identifies WHICH of the 9 callers dominates so the per-site
        // optimisation (ChainBindings overlay / lazy merge / etc.)
        // targets the right path instead of re-architecting all
        // 9 sites.  Suppressed when no merges occurred.
        {
            const char * site_name[AllocStats::kMergeBindingsSiteSlots] = {
                "OP_ATTRS_UPDATE             (//)",
                "OP_ATTRS_UPDATE_TAIL        (//)",
                "OP_CALL ExtendsBody  prev//ov",
                "OP_CALL ExtendsBody  prev//f",
                "OP_CALL ComposeBody  fApp//gApp",
                "OP_TAIL_CALL Extends prev//ov",
                "OP_TAIL_CALL Extends prev//f",
                "OP_TAIL_CALL Compose fApp//gApp",
                "(spare 8)",  "(spare 9)",  "(spare 10)", "(spare 11)",
                "(spare 12)", "(spare 13)", "(spare 14)", "(spare 15)",
            };
            uint64_t totalCalls = 0, totalBytes = 0;
            for (uint8_t s = 0; s < AllocStats::kMergeBindingsSiteSlots; ++s) {
                totalCalls += a.mergeBindingsCallsBySite[s];
                totalBytes += a.mergeBindingsBytesBySite[s];
            }
            if (totalCalls > 0) {
                std::fprintf(stderr,
                    "v3-direct mergeBindings by site "
                    "(total %llu calls, %.1f MB):\n",
                    (unsigned long long)totalCalls,
                    double(totalBytes) / (1024.0 * 1024.0));
                for (uint8_t s = 0; s < AllocStats::kMergeBindingsSiteSlots; ++s) {
                    uint64_t calls = a.mergeBindingsCallsBySite[s];
                    uint64_t bytes = a.mergeBindingsBytesBySite[s];
                    if (calls == 0 && bytes == 0) continue;
                    std::fprintf(stderr,
                        "  [%d] %-32s  calls=%-10llu bytes=%6.1f MB"
                        " (%5.1f %% of total)\n",
                        (int)s, site_name[s],
                        (unsigned long long)calls,
                        double(bytes) / (1024.0 * 1024.0),
                        totalBytes > 0
                            ? 100.0 * double(bytes) / double(totalBytes)
                            : 0.0);
                }
            }
            // #821 — (na, nb) histograms for site 1 (UPDATE_TAIL).
            // If the overlay (nb) histogram is heavily skewed toward
            // small buckets while parent (na) is large, ChainBindings
            // is the right architectural lever.
            uint64_t naTotal = 0, nbTotal = 0;
            for (int i = 0; i < 10; ++i) {
                naTotal += a.mergeBindingsNaHist[i];
                nbTotal += a.mergeBindingsNbHist[i];
            }
            if (naTotal > 0 || nbTotal > 0) {
                const char * labels[10] = {
                    "0", "1", "2-4", "5-8", "9-16",
                    "17-32", "33-64", "65-128", "129-256", "257+"};
                std::fprintf(stderr,
                    "  (UPDATE_TAIL na histogram, total=%llu):\n",
                    (unsigned long long)naTotal);
                for (int i = 0; i < 10; ++i)
                    std::fprintf(stderr, "    na %-8s = %llu\n",
                        labels[i], (unsigned long long)a.mergeBindingsNaHist[i]);
                std::fprintf(stderr,
                    "  (UPDATE_TAIL nb histogram, total=%llu):\n",
                    (unsigned long long)nbTotal);
                for (int i = 0; i < 10; ++i)
                    std::fprintf(stderr, "    nb %-8s = %llu\n",
                        labels[i], (unsigned long long)a.mergeBindingsNbHist[i]);
            }
        }
        // EXIT_GC_SPIRAL Day 13-15 (2026-05-29): singleton-capturedWiths
        // intern-cache hit rate.  Hit rate near 100 % means the cache
        // is doing its job (most 1-element capturedWiths reuse a
        // shared ListVec).  Low hit rate + high evicts means either
        // many unique with-targets (workload-specific) or hash
        // collisions thrashing — bump kCapWithsCacheBuckets if so.
        {
            uint64_t h = getCapWithsHits();
            uint64_t m = getCapWithsMisses();
            uint64_t e = getCapWithsEvicts();
            if (h + m > 0) {
                double hitRate = 100.0 * (double)h / (double)(h + m);
                std::fprintf(stderr,
                    "v3-direct capWiths-intern: hits=%llu misses=%llu "
                    "evicts=%llu hitRate=%.1f%% (estimated savings ~%.1f MB "
                    "@ 32 B/hit)\n",
                    (unsigned long long)h, (unsigned long long)m,
                    (unsigned long long)e,
                    hitRate,
                    h * 32.0 / 1e6);
            }
        }
        // #719 (#702 falsifier chain, 2026-05-21): three-way RSS
        // decomposition.  v3's RSS minus (Boehm-heap + v3-arena) is
        // the "elsewhere" remainder — scratch buffers, libc malloc
        // for std::vector/unordered_map growth, mmap'd nursery,
        // process bookkeeping.  Lets the user attribute the cppnix-
        // vs-v3 RSS gap by category instead of treating it as a
        // single number.
        //
        // Why this matters: hello.drvPath under v3 measures ~2.14 GB
        // peak RSS vs 145 MB for TW.  Existing byte counters already
        // attribute ~940 MB to the v3 arena; the remaining ~1.2 GB
        // must be split between Boehm (TW interop) and "other"
        // (libc malloc, mmap).  Stage 3 Phase D shape (a/b/c) depends
        // on which one dominates.
#if NIX_USE_BOEHMGC
        // 2026-05-27 §6.2 spike: NIX_V3_BOEHM_FORCE_UNMAP=1 fires
        // `GC_gcollect_and_unmap()` once before reading heap stats,
        // letting us observe whether Boehm's munmap is functional on
        // this build/platform (some builds skip USE_MUNMAP).  If the
        // mechanism works, the reported boehm_heap drops and the
        // boehm_unmapped grows by the same delta.  Sequel: fire from
        // checkLimits() periodically to reduce peak_rss mid-eval.
        // Retirement: when periodic-unmap is wired into checkLimits
        // OR when Boehm is downscoped to FFI-only, drop the gate.
        static const bool s_forceUnmap =
            std::getenv("NIX_V3_BOEHM_FORCE_UNMAP") != nullptr;
        if (s_forceUnmap) {
            // Boehm's `force_unmap_on_gcollect` flag is the actual
            // switch — `GC_gcollect_and_unmap()` is documented to
            // unmap unconditionally, but in practice on macOS the
            // unmap depends on this flag being set.  Enable it
            // alongside the explicit collect call to be sure.
            GC_set_force_unmap_on_gcollect(1);
            GC_gcollect_and_unmap();
        }
        size_t boehmHeap = GC_get_heap_size();
        size_t boehmFree = GC_get_free_bytes();
        size_t boehmUnmapped = GC_get_unmapped_bytes();
        // 2026-05-27: Boehm GC time/count instrumentation — falsifier
        // gate for "ditch Boehm" perf claims.  GC_get_gc_no() counts
        // collections; GC_get_full_gc_total_time() returns total time
        // spent in full collections (milliseconds, accumulates across
        // process lifetime).  Both APIs are zero-cost reads (atomic
        // loads of stat counters maintained by the collector).
        //
        // If Boehm GC time is sub-1 % of wall, ditching Boehm cannot
        // deliver wall improvement; the memory-RSS case (~400 MB
        // peak) becomes the sole motivation, which is bounded by
        // precise-root infrastructure work (~1-2 weeks) per
        // IDEAL_GC_DESIGN_2026-05-26.md "no-regret foundations".
        GC_word boehmGcNo = GC_get_gc_no();
        unsigned long boehmGcMs = GC_get_full_gc_total_time();
#else
        size_t boehmHeap = 0;
        size_t boehmFree = 0;
        size_t boehmUnmapped = 0;
        GC_word boehmGcNo = 0;
        unsigned long boehmGcMs = 0;
#endif
        size_t rssBytes = 0;
        {
            struct rusage ru;
            if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
                // macOS reports ru_maxrss in bytes.
                rssBytes = static_cast<size_t>(ru.ru_maxrss);
#else
                // Linux reports ru_maxrss in KB.
                rssBytes = static_cast<size_t>(ru.ru_maxrss) * 1024;
#endif
            }
        }
        const size_t arenaPin = threadArena().bytesAllocated();
        // "Elsewhere" = RSS − Boehm-heap − v3-arena (clamped at 0).
        const size_t elsewhere = (rssBytes > boehmHeap + arenaPin)
            ? rssBytes - boehmHeap - arenaPin : 0;
        std::fprintf(stderr,
            "v3-direct memory: peak_rss=%.1fMB boehm_heap=%.1fMB "
            "boehm_free=%.1fMB boehm_unmapped=%.1fMB v3_arena=%.1fMB "
            "elsewhere=%.1fMB\n",
            rssBytes      / 1e6,
            boehmHeap     / 1e6,
            boehmFree     / 1e6,
            boehmUnmapped / 1e6,
            arenaPin      / 1e6,
            elsewhere     / 1e6);
        // 2026-05-27: Boehm GC time/count line — input to the
        // "ditch Boehm" decision per IDEAL_GC_DESIGN_2026-05-26.md.
        // If boehm_gc_ms is sub-1 % of overall wall, the wall case
        // for replacement is weak; the memory-peak case (~400 MB
        // reserved heap) becomes the sole driver.
        std::fprintf(stderr,
            "v3-direct boehm: gc_count=%llu gc_total_ms=%lu "
            "(time spent in full collections during process lifetime)\n",
            (unsigned long long)boehmGcNo,
            (unsigned long)boehmGcMs);
        // 2026-05-27 Stage 3 precise-root foundation: opt-in dump
        // (V3_DBG_ROOT_DUMP=1) of every reachable v3-heap root
        // pointer.  No-op when env-var unset; near-zero cost when
        // set (one walk of the root sources).
        // See lode/GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md.
        dumpAllV3Roots();
        // 2026-05-27 Stage 6 SPIKE: live-fraction tracer.  Walks
        // transitively from precise roots; counts unique reachable
        // objects per type; reports LIVE-vs-ALLOCATED ratio per type
        // + aggregate freeable-bytes verdict.  Gated NIX_V3_LIVE_TRACE=1
        // (zero cost when unset).
        //
        // Retirement criterion: when Stage 6 lands the real precise GC
        // of v3 arena, fold into NIX_VM_STATS and remove the gate.
        dumpV3LiveFraction();
        // Day 5 2026-05-28: per-block fill probe.  Decision data for
        // Stage 6 generational tenured collector (GHC-RTS style).
        // Gated NIX_V3_BLOCK_PROBE=1; zero cost otherwise.
        dumpV3LiveBlockProbe();
        // Step 4 of post-Phase-3.8 plan (2026-05-29): periodic L(t)
        // trace flush.  If NIX_V3_LIVE_TRACE_PERIODIC=<K> was set,
        // writes the per-sample CSV at NIX_V3_LIVE_TRACE_PERIODIC_OUT
        // (or default /tmp/v3-live-periodic-<pid>.csv) + emits a
        // summary banner.  Per L_MEASUREMENT_GAP_2026-05-28 §5.
        flushPeriodicLiveTraceCsv();
        // #660 verification: dump bridge-primop call counts.  v3-eval
        // already does this via its own NIX_VM_STATS path; mirror here
        // so the integrated `nix` CLI (and any future v3 driver that
        // goes through `runRootExpr`) reports the same data without
        // depending on the CLI specifically.
        dumpPrimOpStats(stderr);
        // #777b (2026-05-23) deserialize per-section breakdown.
        // Only printed when V3_DBG_DESERIALIZE=1 (gated to avoid
        // ~50 ns / clock_gettime overhead on every section in
        // steady state).  Falsifier mechanism for "where inside
        // the 334 ms deserialize budget does the time actually
        // go?".
        if (serialize::deserializeBreakdownEnabled()) {
            const auto b = serialize::deserializeBreakdown();
            if (b.calls > 0) {
                std::fprintf(stderr,
                    "v3-direct deserialize breakdown (ms, calls=%llu): "
                    "header=%.3f code=%.3f intConsts=%.3f floatConsts=%.3f "
                    "stringConsts=%.3f symbolTable=%.3f lambdas=%.3f "
                    "lambdaCodeOffsets=%.3f primops=%.3f misc=%.3f remap=%.3f\n",
                    (unsigned long long)b.calls,
                    b.headerNs            / 1e6,
                    b.codeNs              / 1e6,
                    b.intConstantsNs      / 1e6,
                    b.floatConstantsNs    / 1e6,
                    b.stringConstantsNs   / 1e6,
                    b.symbolTableNs       / 1e6,
                    b.lambdasNs           / 1e6,
                    b.lambdaCodeOffsetsNs / 1e6,
                    b.primopsNs           / 1e6,
                    b.miscNs              / 1e6,
                    b.remapNs             / 1e6);
            }
        }
        // #827 / A3: per-call-site cache-hook dump.  Gated by
        // NIX_VM_CACHE_SITES=1 inside `dumpCacheHookSites`; empty
        // dump suppressed automatically (no probe activations).
        // Provides a per-call-site breakdown of every instrumented
        // cache check so investigations (Phase 4b cache scope, etc.)
        // can localise which site fires with which hit profile.
        dumpCacheHookSites(stderr);

        // #770 / #777 promotion (2026-05-22 / 2026-05-23): disk
        // cache effectiveness.  Now default-on; prints whenever
        // primImport ran.  hits/misses/inserts/failures lets the
        // user see whether the cache is firing.  Opt-out via
        // NIX_V3_NO_DISK_CACHE=1 leaves all counters at zero.
        {
            const auto & dc = disk_cache::stats();
            if (dc.lookups + dc.inserts > 0) {
                std::fprintf(stderr,
                    "v3-direct disk_cache: lookups=%llu hits=%llu misses=%llu "
                    "inserts=%llu insertFailures=%llu hit_rate=%.1f%%\n",
                    (unsigned long long)dc.lookups,
                    (unsigned long long)dc.hits,
                    (unsigned long long)dc.misses,
                    (unsigned long long)dc.inserts,
                    (unsigned long long)dc.insertFailures,
                    dc.lookups > 0
                        ? 100.0 * (double)dc.hits / (double)dc.lookups
                        : 0.0);
            }
        }
        // #772 Stage 9 Phase L0 spike: bytecode-level dedup survey.
        // Only printed when NIX_V3_DEDUP_SURVEY=1.  totalFunctions /
        // uniqueHashes is the LOWER BOUND dedup ratio (real IR-level
        // alpha-equivalent dedup can only be higher).  ≥5× justifies
        // Stage 9 investment; <2× kills it.
        {
            const auto & sur = dedupSurvey();
            if (sur.totalFunctions > 0) {
                double fnRatio = sur.uniqueHashes > 0
                    ? (double)sur.totalFunctions / (double)sur.uniqueHashes
                    : 0.0;
                double byteRatio = sur.uniqueBytes > 0
                    ? (double)sur.totalBytes / (double)sur.uniqueBytes
                    : 0.0;
                std::fprintf(stderr,
                    "v3-direct dedup_survey: totalFunctions=%llu "
                    "uniqueHashes=%llu fn_dedup_lb=%.2fx "
                    "totalBytes=%.1fKB uniqueBytes=%.1fKB byte_dedup_lb=%.2fx\n",
                    (unsigned long long)sur.totalFunctions,
                    (unsigned long long)sur.uniqueHashes,
                    fnRatio,
                    sur.totalBytes / 1024.0,
                    sur.uniqueBytes / 1024.0,
                    byteRatio);
            }
        }
        // #738 Phase E v0.1 (2026-05-21) survival-rate banner.
        // Emit when ANY scavenge ran during this eval.  The
        // headline number is the young-gen mortality rate:
        //     died / (died + survived).
        // High mortality (>50%) means most allocs are short-lived
        // — Phase E's survivor-pool design would recover those
        // bytes.  Low mortality (<10%) means most allocs survive
        // forever — Phase E wouldn't help; objects would just sit
        // in survivor pool instead of tenured.  This is the Rule 0
        // input that drives the v0.2 architectural decision.
        {
            const auto & nur = threadNursery();
            const auto & ns  = nur.stats();
            if (ns.scavengeCount > 0
                && (ns.survivedBytes > 0 || ns.diedBytes > 0))
            {
                const uint64_t total = ns.survivedBytes + ns.diedBytes;
                const double mortality = total > 0
                    ? (double(ns.diedBytes) * 100.0 / double(total))
                    : 0.0;
                std::fprintf(stderr,
                    "v3-direct phase-e survival: scavenges=%llu "
                    "survived=%.1fMB died=%.1fMB mortality=%.1f%% "
                    "(Phase E v0.2 design driver: kill rate)\n",
                    (unsigned long long)ns.scavengeCount,
                    ns.survivedBytes / 1e6,
                    ns.diedBytes     / 1e6,
                    mortality);
                // Path B (2026-05-27, per PHASE_E_V02_DAY2_FALSIFIED):
                // Bypass / overflow diagnostic — the Day-2 measurement
                // showed a 10× gap between expected mortality savings
                // and observed (334 MB ideal vs 33.6 MB actual on
                // hello).  Likely cause: most allocations bypass the
                // nursery via overflow → tenured-arena fallback.  This
                // ratio tells the next session whether the bypass
                // policy is the bottleneck.
                //
                // nursery_hits  = ns.allocCount (allocations that
                //                  landed in the nursery)
                // nursery_misses = ns.overflowCount (fell through to
                //                  arena because nursery was full)
                //
                // If misses >> hits → nursery is too small for the
                // workload's allocation rate → larger nursery OR
                // more aggressive scavenge trigger.
                // If hits >> misses → bypass isn't the issue; the
                // low mortality is intrinsic to the workload.
                {
                    const uint64_t hits   = ns.allocCount;
                    const uint64_t misses = ns.overflowCount;
                    const uint64_t total  = hits + misses;
                    const double hitPct = total > 0
                        ? (double(hits) * 100.0 / double(total))
                        : 0.0;
                    std::fprintf(stderr,
                        "v3-direct nursery routing: hits=%llu misses=%llu "
                        "hit_rate=%.1f%% allocBytes=%.1fMB "
                        "(Path B audit: bypass = arena fallback on full)\n",
                        (unsigned long long)hits,
                        (unsigned long long)misses,
                        hitPct,
                        ns.allocBytes / 1e6);
                }
                // #738 Phase E v0.2: when active, show per-region
                // promotion breakdown.  yToS is age-1 survivors
                // (kept in survivor pool, not tenured); sToT is
                // age-2 (truly tenured); yToTOvf is direct
                // promotion when the survivor pool overflowed (or
                // the legacy Phase D path where there's no S at
                // all).  reclaimedFromS = bytesYToS - bytesSToT
                // tracks the marginal Phase E reclamation: bytes
                // that survived Y but died in S before tenuring.
                if (nur.isPhaseEActive()) {
                    const uint64_t yToS    = nur.getBytesYToS();
                    const uint64_t sToT    = nur.getBytesSToT();
                    const uint64_t yToTOvf = nur.getBytesYToTOvf();
                    const int64_t reclaimedFromS =
                        (int64_t)yToS - (int64_t)sToT;
                    std::fprintf(stderr,
                        "v3-direct phase-e regions: yToS=%.1fMB "
                        "sToT=%.1fMB yToTOvf=%.1fMB "
                        "reclaimedFromS=%.1fMB (Phase E v0.2 marginal "
                        "win over Phase D)\n",
                        yToS    / 1e6,
                        sToT    / 1e6,
                        yToTOvf / 1e6,
                        reclaimedFromS / 1e6);
                }
            }
        }
        // #778 (2026-05-23) opcount Top-N rollup — Stage 5 (shapes/
        // PICs) decision input.  Emitted under NIX_VM_OPCOUNTS=1 only
        // (the per-op increment is the same gate from vm.cc:2572).
        // Shows the top 12 opcodes by count plus the AttrSelect /
        // AttrSelectDyn / AttrsHas share — Stage 5 only makes sense
        // if those sites are ≥10 % of dispatch.  Below that, the
        // PIC's amortisation can't move wall clock.
        {
            const auto & a = allocStats();
            uint64_t totalDispatch = 0;
            for (size_t i = 0; i < 256; ++i) totalDispatch += a.opcodeCounts[i];
            if (totalDispatch > 0) {
                // Sort opcodes by count descending.
                struct OpRow { uint8_t code; uint64_t count; };
                OpRow rows[256];
                size_t nz = 0;
                for (size_t i = 0; i < 256; ++i) {
                    if (a.opcodeCounts[i] > 0) {
                        rows[nz++] = { (uint8_t)i, a.opcodeCounts[i] };
                    }
                }
                std::sort(rows, rows + nz,
                    [](const OpRow & x, const OpRow & y) {
                        return x.count > y.count;
                    });
                std::fprintf(stderr,
                    "v3-direct opcounts: total=%llu (top-12 + AttrSelect family):\n",
                    (unsigned long long)totalDispatch);
                const size_t topN = std::min<size_t>(12, nz);
                for (size_t i = 0; i < topN; ++i) {
                    std::fprintf(stderr,
                        "  %-26s %12llu  %5.2f%%\n",
                        opName(static_cast<Op>(rows[i].code)),
                        (unsigned long long)rows[i].count,
                        100.0 * rows[i].count / totalDispatch);
                }
                // AttrSelect-family share (Stage 5 input).
                uint64_t selFam =
                      a.opcodeCounts[OP_ATTRS_SELECT]
                    + a.opcodeCounts[OP_ATTRS_SELECT_DYN]
                    + a.opcodeCounts[OP_ATTRS_HAS]
                    + a.opcodeCounts[OP_ATTRS_HAS_DYN];
                std::fprintf(stderr,
                    "  --AttrSelect family-- %12llu  %5.2f%% "
                    "(Stage 5 PIC kill criterion: <10%%)\n",
                    (unsigned long long)selFam,
                    100.0 * selFam / totalDispatch);

                // #786 OPCYCLES — observed per-op ns (avg) on this
                // run.  Only present when NIX_VM_OPCYCLES=1 was set.
                // Each row: opcode + total ns + count + ns/op.
                // Note: measurement overhead per dispatch is ~10-20 ns
                // (one steady_clock + add); subtract that from the
                // reported ns/op to get the "real" per-op cost.
                // Relative comparisons across opcodes are unaffected.
                uint64_t totalCyc = 0;
                for (size_t i = 0; i < 256; ++i) totalCyc += a.opcycleNs[i];
                if (totalCyc > 0) {
                    std::fprintf(stderr,
                        "v3-direct opcycles (observed per-op ns; "
                        "~10-20 ns measurement overhead per dispatch):\n");
                    // Sort by total ns descending.
                    struct CycRow { uint8_t code; uint64_t ns; uint64_t cnt; };
                    CycRow crows[256];
                    size_t nz = 0;
                    for (size_t i = 0; i < 256; ++i) {
                        if (a.opcycleNs[i] > 0 && a.opcodeCounts[i] > 0) {
                            crows[nz++] = { (uint8_t)i,
                                            a.opcycleNs[i],
                                            a.opcodeCounts[i] };
                        }
                    }
                    std::sort(crows, crows + nz,
                        [](const CycRow & x, const CycRow & y) {
                            return x.ns > y.ns;
                        });
                    const size_t topN = std::min<size_t>(12, nz);
                    for (size_t i = 0; i < topN; ++i) {
                        std::fprintf(stderr,
                            "  %-26s total=%llu ns  count=%llu  "
                            "avg=%6.1f ns/op\n",
                            opName(static_cast<Op>(crows[i].code)),
                            (unsigned long long)crows[i].ns,
                            (unsigned long long)crows[i].cnt,
                            (double)crows[i].ns / (double)crows[i].cnt);
                    }
                    std::fprintf(stderr,
                        "  -- total measured: %llu ns over %llu ops "
                        "(avg %.1f ns/op including measurement overhead)\n",
                        (unsigned long long)totalCyc,
                        (unsigned long long)totalDispatch,
                        (double)totalCyc / (double)totalDispatch);
                }

                // #787 OP_RETURN per-phase breakdown — only present
                // when NIX_V3_DBG_RETURN_BREAKDOWN=1.  Three phases:
                // prePop (frame capture + resize + pop), thunkEval
                // (CFF_THUNK_RETURN logic), postEval (push retVal +
                // tail-call cleanup + break).  Per-phase counter for
                // overhead estimate (~10-20 ns × 3 markers = ~50 ns
                // total per return under the gate).
                if (a.opReturnThunkCalls + a.opReturnCallCalls > 0) {
                    uint64_t nT = a.opReturnThunkCalls;
                    uint64_t nC = a.opReturnCallCalls;
                    uint64_t nTot = nT + nC;
                    std::fprintf(stderr,
                        "v3-direct OP_RETURN breakdown (%llu thunk + %llu call = %llu returns):\n",
                        (unsigned long long)nT,
                        (unsigned long long)nC,
                        (unsigned long long)nTot);
                    std::fprintf(stderr,
                        "  prePopNs     total=%llu  avg=%.1f ns/return\n",
                        (unsigned long long)a.opReturnPrePopNs,
                        nTot > 0 ? (double)a.opReturnPrePopNs / nTot : 0.0);
                    std::fprintf(stderr,
                        "  thunkEvalNs  total=%llu  avg/thunk-return=%.1f ns\n",
                        (unsigned long long)a.opReturnThunkEvalNs,
                        nT > 0 ? (double)a.opReturnThunkEvalNs / nT : 0.0);
                    std::fprintf(stderr,
                        "  postEvalNs   total=%llu  avg=%.1f ns/return\n",
                        (unsigned long long)a.opReturnPostEvalNs,
                        nTot > 0 ? (double)a.opReturnPostEvalNs / nTot : 0.0);
                    uint64_t bdTotal = a.opReturnPrePopNs
                                     + a.opReturnThunkEvalNs
                                     + a.opReturnPostEvalNs;
                    std::fprintf(stderr,
                        "  -- breakdown total: %llu ns "
                        "(subtract ~60 ns/return measurement overhead "
                        "= ~%lld ns/return real)\n",
                        (unsigned long long)bdTotal,
                        nTot > 0
                            ? (long long)((bdTotal / nTot) - 60)
                            : 0LL);
                }

                // #782 bigram top-20 (only when NIX_VM_BIGRAMS=1
                // was set during eval — non-zero entries reveal
                // common (prev, current) op-pairs.  Used as the
                // measurement spike for #780: if >5 % of dispatch
                // collapses to a handful of bigrams, super-
                // instructions can capture that without a
                // full register-VM rewrite.
                uint64_t totalBigrams = 0;
                for (size_t i = 0; i < 256; ++i)
                    for (size_t j = 0; j < 256; ++j)
                        totalBigrams += a.bigramCounts[i][j];
                if (totalBigrams > 0) {
                    struct BigramRow {
                        uint8_t prev, curr;
                        uint64_t count;
                    };
                    BigramRow brows[256];
                    size_t bnz = 0;
                    // Top-20 by count (single pass with insertion).
                    // 256² = 65 K iterations; cheap.
                    for (size_t i = 0; i < 256; ++i) {
                        for (size_t j = 0; j < 256; ++j) {
                            uint64_t c = a.bigramCounts[i][j];
                            if (c == 0) continue;
                            // Insert into sorted brows (keep top 20).
                            if (bnz < 20) {
                                brows[bnz++] = { (uint8_t)i, (uint8_t)j, c };
                            } else {
                                // Find min and replace if larger.
                                size_t minIdx = 0;
                                for (size_t k = 1; k < bnz; ++k)
                                    if (brows[k].count < brows[minIdx].count)
                                        minIdx = k;
                                if (c > brows[minIdx].count)
                                    brows[minIdx] = { (uint8_t)i, (uint8_t)j, c };
                            }
                        }
                    }
                    std::sort(brows, brows + bnz,
                        [](const BigramRow & x, const BigramRow & y) {
                            return x.count > y.count;
                        });
                    std::fprintf(stderr,
                        "v3-direct bigrams: total=%llu "
                        "(top-20; #780 super-instruction candidates):\n",
                        (unsigned long long)totalBigrams);
                    uint64_t topSum = 0;
                    for (size_t i = 0; i < bnz; ++i) {
                        std::fprintf(stderr,
                            "  %-24s -> %-24s %12llu  %5.2f%%\n",
                            opName(static_cast<Op>(brows[i].prev)),
                            opName(static_cast<Op>(brows[i].curr)),
                            (unsigned long long)brows[i].count,
                            100.0 * brows[i].count / totalBigrams);
                        topSum += brows[i].count;
                    }
                    std::fprintf(stderr,
                        "  -- top-20 sum: %5.2f%% of all bigrams "
                        "(#780 register-VM kill criterion: top-20 < 30%% "
                        "→ stack motion is spread, not pair-fusible)\n",
                        100.0 * topSum / totalBigrams);
                    // #783-measure: same-slot SET_LOCAL -> GET_LOCAL
                    // (fusion candidate for OP_SET_LOCAL_KEEP).
                    if (a.bigramSetGetSameSlot > 0) {
                        std::fprintf(stderr,
                            "  -- SET_LOCAL -> GET_LOCAL same-slot: %llu "
                            "(%5.2f%% of all dispatch, "
                            "%5.2f%% of bigram top-1; "
                            "#783 OP_SET_LOCAL_KEEP fusion candidate; "
                            "kill criterion: < 2%% of dispatch)\n",
                            (unsigned long long)a.bigramSetGetSameSlot,
                            100.0 * a.bigramSetGetSameSlot / totalDispatch,
                            // top-1 bigram count = bigramCounts[OP_SET_LOCAL][OP_GET_LOCAL]
                            (a.bigramCounts[OP_SET_LOCAL][OP_GET_LOCAL] > 0
                                ? 100.0 * a.bigramSetGetSameSlot
                                  / a.bigramCounts[OP_SET_LOCAL][OP_GET_LOCAL]
                                : 0.0));
                    }
                }
            }
        }
        // #736 (2026-05-21) IFD-probe summary.  Per IFD_DEEP_DIVE
        // §8 step 1 falsifier: "dispatcher counts the probes;
        // counter > 0 on a haskell.nix run".  When ANY probe fired,
        // emit a one-line breakdown by kind so users can attribute
        // IFD activity to specific primops without enabling per-call
        // tracing.  Zero probes = no output (silent on the default
        // hello.drvPath case where no IFD fires).
        {
            uint64_t totalProbes = 0;
            for (int k = 1; k < (int)kIfdProbeKindCount; ++k)
                totalProbes += a.ifdProbeCount[k];
            if (totalProbes > 0) {
                std::fprintf(stderr, "v3-direct ifd probes: total=%llu",
                    (unsigned long long)totalProbes);
                for (int k = 1; k < (int)kIfdProbeKindCount; ++k) {
                    if (a.ifdProbeCount[k] > 0)
                        std::fprintf(stderr, " %s=%llu",
                            ifdProbeKindName(static_cast<uint8_t>(k)),
                            (unsigned long long)a.ifdProbeCount[k]);
                }
                std::fprintf(stderr, "\n");
            }
            // #741 Phase 4 measurement spike: per-kind count of primop
            // calls whose path argument had non-empty string context.
            // Strict upper bound on potential IFD events (Phase 4
            // cache candidates).  Zero on workloads that import only
            // literal nixpkgs paths; non-zero on workloads that touch
            // derivation outputs (haskell.nix's callCabalProjectToNix
            // and similar).
            uint64_t totalWithCtx = 0;
            for (int k = 1; k < (int)kIfdProbeKindCount; ++k)
                totalWithCtx += a.ifdProbeWithCtx[k];
            if (totalWithCtx > 0) {
                std::fprintf(stderr,
                    "v3-direct ifd probes (with-ctx, potential IFD): total=%llu",
                    (unsigned long long)totalWithCtx);
                for (int k = 1; k < (int)kIfdProbeKindCount; ++k) {
                    if (a.ifdProbeWithCtx[k] > 0)
                        std::fprintf(stderr, " %s=%llu",
                            ifdProbeKindName(static_cast<uint8_t>(k)),
                            (unsigned long long)a.ifdProbeWithCtx[k]);
                }
                std::fprintf(stderr, "\n");
            } else if (totalProbes > 0) {
                std::fprintf(stderr,
                    "v3-direct ifd probes (with-ctx, potential IFD): 0 — "
                    "all probes were literal-path calls; no IFD candidates\n");
            }
            // #795 (2026-05-24): per-call-site v3ToTreeWalker counter.
            // Dumps which call sites cross to TW most often.  Read by the
            // V3 true-native investigation (V3_TRUE_NATIVE_PLAN_2026-05-24.md).
            uint64_t totalV3Tw = 0;
            for (uint8_t i = 0; i < 16; ++i) totalV3Tw += a.v3ToTwBySite[i];
            if (totalV3Tw > 0) {
                static const char * kSiteNames[16] = {
                    "primReadFile_string_ctx",  // 0
                    "primReadDir_attrset",      // 1
                    "primImport_string_ctx",    // 2
                    "primImport_attrset",       // 3
                    "primReadDir_string_ctx",   // 4
                    "primPathExists_ctx",       // 5
                    "primDerivationStrict_TWfb",// 6
                    "primV3CallBridge1",        // 7
                    "FFI_leaves(fetch/path)",   // 8
                    "v3ToTW_eager_struct",      // 9
                    "primTrace",                // 10
                    "primV3ForceAttr_inner",    // 11
                    "primV3ForceListElem_inner",// 12
                    "site_13",                  // 13
                    "site_14",                  // 14
                    "unattributed_other",       // 15
                };
                std::fprintf(stderr,
                    "v3-direct v3ToTreeWalker calls (total=%llu):",
                    (unsigned long long)totalV3Tw);
                for (uint8_t i = 0; i < 16; ++i) {
                    if (a.v3ToTwBySite[i] == 0) continue;
                    std::fprintf(stderr, " %s=%llu",
                        kSiteNames[i],
                        (unsigned long long)a.v3ToTwBySite[i]);
                }
                std::fprintf(stderr, "\n");
            }
        }
        // #741 Phase 1 spike: derivation-result round-trip diagnostics.
        // Only emits when NIX_V3_TEST_DRV_RESULT_SERIALIZE=1; no output
        // on the default path.  Validates the value-serialiser
        // architecture for the multi-week IFD eval-result cache.
        value_serialize::dumpStats(stderr);
        // #741 Phase 3a SHADOW eval-result cache diagnostics.
        // Only emits when NIX_V3_EVAL_RESULT_CACHE=1.
        value_serialize::dumpEvalResultCacheStats(stderr);
        // #741 Phase 3e SHADOW drv-hash cache diagnostics.
        // Only emits when NIX_V3_DRV_HASH_CACHE=1.
        value_serialize::dumpDrvHashCacheStats(stderr);
        // #746 (2026-05-21) Bindings-attribution rollup.  Phase 1 of
        // the post-Stage-4-v4.2 plan: the dominant v3-arena consumer
        // on hello.drvPath is Bindings (84% / 956 MB).  Until we know
        // WHERE those Bindings come from we cannot pick the next
        // lever (persistent-map overlay, construction-site inlining,
        // or shape polymorphism).
        //
        // dumpBindingsAttribution() is a no-op when
        // NIX_V3_BINDINGS_ATTR is unset; when set, the recording
        // gate also auto-enables via bindingsOriginEnabled().
        dumpBindingsAttribution(stderr);
        // T1.3 (2026-05-27) per-Thunk attribution.  No-op when
        // NIX_V3_THUNKS_ATTR is unset.  Templated from #746 BINDINGS_ATTR
        // pattern to identify Thunk allocation hot sites — Thunks are
        // the second-largest v3_arena bucket on HNE (320 MB / 320 MB-of-
        // 1594 MB total, per HNE_BUCKET_DECOMP_2026-05-27.md §"v3_arena
        // decomposition").  Without per-site data Thunks remain the
        // largest un-attributed bucket after Bindings.
        dumpThunksAttribution(stderr);
        // T1.3 (2026-05-27) per-Closure attribution.  No-op when
        // NIX_V3_CLOSURES_ATTR is unset.  Unlike Thunks (single dominant
        // site at OP_MAKE_THUNK), Closures are dispersed across ~7 vm.cc
        // sites — per-site rollup distinguishes "user lambda creation"
        // from "VM-internal fakeClo wrapping" (the latter is overhead
        // with potential elision targets).
        dumpClosuresAttribution(stderr);
        // T1.3 (2026-05-27) per-Pair + per-List attribution.  Both
        // have many distinct primops.cc sites; per-site rollup may
        // surface concrete levers analogous to fakeClo for Closures.
        // No-op when NIX_V3_PAIRS_ATTR / NIX_V3_LISTS_ATTR unset.
        dumpPairsAttribution(stderr);
        dumpListsAttribution(stderr);
        // T1.3 (2026-05-27) unified cross-type allocation attribution.
        // Master gate: NIX_V3_ALLOC_ATTR=1.  Aggregates the top sites
        // across Closures + Thunks + Pairs + Lists into a single
        // sorted-by-bytes table with a Type column.  Useful overview
        // of "where the memory is going" without scanning four
        // separate dumps.
        dumpAllocAttribution(stderr);
        // #751 (2026-05-21) "elsewhere" attribution.  After #750 the
        // v3_arena dropped 386 MB but peak_rss dropped only 248 MB;
        // the "elsewhere" share (RSS - boehm_heap - v3_arena) grew
        // from 790 → 928 MB.  Before committing to #748's multi-week
        // Bindings-overlay work we want to know what's IN that
        // 928 MB — it might host a bigger lever than the remaining
        // v3_arena.  This probe dumps the size + estimated byte
        // footprint of the major C++ containers v3 maintains
        // outside the threadArena().  Always on under NIX_VM_STATS
        // (no separate gate — these are cheap reads on shared
        // counters).
        {
            auto estUMap = [](size_t entries, size_t buckets,
                              size_t keyBytes, size_t valBytes) -> size_t {
                // Standard unordered_map memory model: bucket array
                // of pointer-per-bucket + node-per-entry (key + val
                // + next-pointer + cached-hash).
                return buckets * sizeof(void *)
                     + entries * (keyBytes + valBytes
                                  + 2 * sizeof(void *));
            };
            const auto & sct = stringContextSideTable();
            const auto & pps = posSnapshotPool();
            const auto & bot = bindingsOriginTable();
            const auto & cot = cellOwnerTable();
            const auto & gst = ir::globalSymbolTable();
            const auto & dirty = dirtyContainers();
            const auto & standalone = standaloneCellRoots();
            const auto & nstats = threadNursery().stats();
            // String-context entry approximates each vector<string>
            // by entry-count × avg-string-overhead (40 B for a
            // small std::string node).  Per-entry: pointer-key +
            // sizeof(vector<string>) header (~24 B).
            uint64_t sctEntryStrings = 0;
            uint64_t sctEntryBytes   = 0;
            for (const auto & kv : sct) {
                sctEntryStrings += kv.second.size();
                for (const auto & s : kv.second) sctEntryBytes += s.size();
            }
            const size_t sctEst = estUMap(sct.size(), sct.bucket_count(),
                                          sizeof(const char *),
                                          sizeof(std::vector<std::string>))
                                + sctEntryStrings * 40   // string node
                                + sctEntryBytes;          // string bodies
            const size_t ppsEst = pps.capacity() * sizeof(PosSnapshot);
            // PosSnapshot has a std::string; approximate string body
            // by 1.5× avg-path-length (40 B typical for /nix/store/...).
            const uint64_t ppsStringBytes = pps.size() * 40;
            const size_t botEst = estUMap(bot.size(), bot.bucket_count(),
                                          sizeof(const Bindings *),
                                          sizeof(BindingsOrigin));
            const size_t cotEst = estUMap(cot.size(), cot.bucket_count(),
                                          sizeof(const Value *),
                                          sizeof(const Thunk *));
            // Global symbol table: vector<string> + index map.
            uint64_t gstStringBytes = 0;
            for (const auto & s : gst) gstStringBytes += s.size();
            const size_t gstEst = gst.capacity() * sizeof(std::string)
                                + gstStringBytes
                                + gst.size() * (24 + 4 + 2 * sizeof(void*));
            const size_t dirtyEst      = dirty.capacity() * sizeof(void *) * 2;
            const size_t standaloneEst = standalone.capacity() * sizeof(void *);
            // Phase 3 attribution (2026-05-28): account for the
            // mark-sweep infrastructure that lives outside arena/Boehm.
            const auto & singletonReg = singletonClosureRegistry();
            const size_t singletonRegEst =
                singletonReg.capacity() * sizeof(Closure **);
            // Arena-side cell-start bitmap.  Per-block bitmap, 128 KB
            // each.  Lives as long as the arena.
            size_t cellStartsEst = 0;
            for (const auto & v : threadArena().cellStartBitmaps())
                cellStartsEst += v.capacity() * sizeof(uint64_t);
            // Free list.  Per-bin vector<void*>; many bins for distinct
            // cell sizes.  Approximate via outer + per-bin capacities.
            // We don't have public accessors; use a best-effort fixed
            // estimate based on freeListEntryCount() and assume avg
            // 8 bytes per entry plus map overhead.
            const size_t freeListCount = threadArena().freeListEntryCount();
            const size_t freeListEst =
                freeListCount * sizeof(void *) * 2;  // entries + map overhead
            // Nursery: young + (when Phase E active) two survivor
            // buffers of equal size.  When Phase E is off the
            // single nursery is just `sizeBytes`.
            uint64_t nurseryBytes = nstats.sizeBytes;
            if (threadNursery().isPhaseEActive())
                nurseryBytes += 2 * nstats.sizeBytes; // approx, S=Y default

            const size_t sumEst = sctEst + ppsEst + ppsStringBytes
                                + botEst + cotEst + gstEst + dirtyEst
                                + standaloneEst + nurseryBytes
                                + singletonRegEst + cellStartsEst + freeListEst;
            std::fprintf(stderr,
                "v3-direct elsewhere-probe (entries / est_MB):\n"
                "  stringContextSide   %12zu  ~%6.1f MB  (buckets=%zu, "
                "strings=%llu, body=%llu B)\n"
                "  posSnapshotPool     %12zu  ~%6.1f MB  (capacity=%zu)\n"
                "  bindingsOriginTable %12zu  ~%6.1f MB  (buckets=%zu)\n"
                "  cellOwnerTable      %12zu  ~%6.1f MB  (buckets=%zu)\n"
                "  globalSymbolTable   %12zu  ~%6.1f MB  (capacity=%zu, "
                "stringBytes=%llu)\n"
                "  dirtyContainers     %12zu  ~%6.1f MB  (capacity=%zu)\n"
                "  standaloneCellRoots %12zu  ~%6.1f MB  (capacity=%zu)\n"
                "  nursery (Y+S buffs) %12s  ~%6.1f MB\n"
                "  singletonClosureReg %12zu  ~%6.1f MB  (capacity=%zu)\n"
                "  arena.cellStarts    %12s  ~%6.1f MB  (per-block 128 KB)\n"
                "  arena.freeList      %12zu  ~%6.1f MB  (live entries)\n"
                "  ----- elsewhere-probe sum: ~%.1f MB -----\n",
                sct.size(),         sctEst        / 1e6, sct.bucket_count(),
                (unsigned long long)sctEntryStrings,
                (unsigned long long)sctEntryBytes,
                pps.size(),         (ppsEst + ppsStringBytes) / 1e6,
                pps.capacity(),
                bot.size(),         botEst        / 1e6, bot.bucket_count(),
                cot.size(),         cotEst        / 1e6, cot.bucket_count(),
                gst.size(),         gstEst        / 1e6, gst.capacity(),
                (unsigned long long)gstStringBytes,
                dirty.size(),       dirtyEst      / 1e6, dirty.capacity(),
                standalone.size(),  standaloneEst / 1e6, standalone.capacity(),
                "<mmap>",           nurseryBytes  / 1e6,
                singletonReg.size(), singletonRegEst / 1e6, singletonReg.capacity(),
                "<phase3>",         cellStartsEst / 1e6,
                freeListCount,      freeListEst   / 1e6,
                sumEst / 1e6);
        }
        // Step 6 of post-Phase-3.8 plan: free-list hit-rate summary.
        // Only emitted under NIX_V3_FREE_LIST_STATS=1; otherwise the
        // counters stayed zero (gate at allocation site).
        if (std::getenv("NIX_V3_FREE_LIST_STATS") != nullptr) {
            const auto & fl = freeListStats();
            const double hitPct = fl.allocCount > 0
                ? 100.0 * double(fl.hitCount) / double(fl.allocCount)
                : 0.0;
            std::fprintf(stderr,
                "v3-direct free-list stats: "
                "allocs=%llu hits=%llu hit_rate=%.2f%%\n"
                "  bin   range_bytes        requests           hits    hit%%\n",
                (unsigned long long)fl.allocCount,
                (unsigned long long)fl.hitCount, hitPct);
            for (size_t b = 0; b < FreeListStats::kNumBins; ++b) {
                const size_t lo = size_t(16) << b;
                const size_t hi = size_t(16) << (b + 1);
                const uint64_t req = fl.requestsByBin[b];
                const uint64_t hit = fl.hitsByBin[b];
                if (req == 0 && hit == 0) continue;
                const double binPct = req > 0
                    ? 100.0 * double(hit) / double(req) : 0.0;
                if (b + 1 < FreeListStats::kNumBins) {
                    std::fprintf(stderr,
                        "  %2zu   [%6zu,%7zu) %12llu %14llu  %6.2f%%\n",
                        b, lo, hi,
                        (unsigned long long)req,
                        (unsigned long long)hit, binPct);
                } else {
                    std::fprintf(stderr,
                        "  %2zu   [%6zu,    inf) %12llu %14llu  %6.2f%%\n",
                        b, lo,
                        (unsigned long long)req,
                        (unsigned long long)hit, binPct);
                }
            }
            // Pre-committed verdict per task #842 / Step 6 thresholds.
            const char * verdict;
            if (hitPct >= 50.0) {
                verdict = "PER-EXACT-SIZE BINS OK (>=50% — Step 11 NOT justified)";
            } else if (hitPct < 20.0) {
                verdict = "PER-EXACT-SIZE BOTTLENECK (<20% — Step 11 fires)";
            } else {
                verdict = "JUDGMENT CALL (20-50% — see Step 9 synthesis)";
            }
            std::fprintf(stderr,
                "  ----- free-list verdict: %s\n", verdict);
        }
        // Step 12′ of post-Phase-3.8 plan (2026-05-29): Immix
        // allocator hit-rate stats.  Only emitted under
        // V3_DBG_IMMIX_ALLOC=1.  Pre-committed acceptance per
        // task #848: hit rate ≥70% on HNE.
        if (std::getenv("V3_DBG_IMMIX_ALLOC") != nullptr) {
            const auto & is = immixAllocStats();
            const uint64_t served = is.spanHits + is.spanAdvances;
            const double allocPct = is.allocs > 0
                ? 100.0 * double(served) / double(is.allocs)
                : 0.0;
            const uint64_t totalBytes = is.bytesFromSpans + is.bytesFromBump;
            const double bytePct = totalBytes > 0
                ? 100.0 * double(is.bytesFromSpans) / double(totalBytes)
                : 0.0;
            std::fprintf(stderr,
                "v3-direct immix-alloc: "
                "allocs=%llu spanHits=%llu spanAdvances=%llu "
                "bumpFresh=%llu\n"
                "  served-from-spans: %llu (%.2f%% of allocs)\n"
                "  bytes-from-spans:  %.2f MB (%.2f%% of %.2f MB total)\n",
                (unsigned long long)is.allocs,
                (unsigned long long)is.spanHits,
                (unsigned long long)is.spanAdvances,
                (unsigned long long)is.bumpFresh,
                (unsigned long long)served, allocPct,
                double(is.bytesFromSpans) / (1ULL << 20), bytePct,
                double(totalBytes) / (1ULL << 20));
            // Pre-committed verdict per task #848.
            const char * verdict;
            if (allocPct >= 70.0) {
                verdict = "PASS (hit rate ≥70% — Step 12′ acceptance MET)";
            } else if (allocPct < 30.0) {
                verdict = "FAIL (hit rate <30% — Step 12′ acceptance MISSED)";
            } else {
                verdict = "MARGINAL (30-70% — judgment call)";
            }
            std::fprintf(stderr,
                "  ----- immix-alloc verdict: %s\n", verdict);
        }

        // Step 18 of post-Phase-3.8 plan (2026-05-29): per-site
        // allocChars attribution dump.  Only emitted under
        // NIX_V3_STRINGS_ATTR=1.
        if (std::getenv("NIX_V3_STRINGS_ATTR") != nullptr) {
            auto & sites = allocCharsSites();
            // Sort by bytes descending.
            std::sort(sites.begin(), sites.end(),
                [](const AllocCharsSite & a, const AllocCharsSite & b) {
                    return a.bytes > b.bytes;
                });
            uint64_t totalCalls = 0, totalBytes = 0;
            for (const auto & s : sites) {
                totalCalls += s.count;
                totalBytes += s.bytes;
            }
            std::fprintf(stderr,
                "v3-direct allocChars site attribution (Step 18, "
                "NIX_V3_STRINGS_ATTR=1):\n"
                "  total: %llu calls / %.2f MB across %zu unique sites\n"
                "  rank  file:line                                          "
                "calls       MB    %%cum\n",
                (unsigned long long)totalCalls,
                double(totalBytes) / (1ULL << 20),
                sites.size());
            uint64_t cumBytes = 0;
            for (size_t i = 0; i < sites.size() && i < 20; ++i) {
                const auto & s = sites[i];
                cumBytes += s.bytes;
                const double cumPct = totalBytes > 0
                    ? 100.0 * double(cumBytes) / double(totalBytes) : 0.0;
                // Truncate file to last 48 chars for readability.
                const char * f = s.file ? s.file : "?";
                const size_t flen = std::strlen(f);
                const char * fshort = flen > 48 ? (f + flen - 48) : f;
                std::fprintf(stderr,
                    "  %3zu   %-48s:%-5u %10llu  %7.2f  %5.1f%%\n",
                    i + 1, fshort, s.line,
                    (unsigned long long)s.count,
                    double(s.bytes) / (1ULL << 20),
                    cumPct);
            }
        }
    }
    return out;
}

} // namespace nix::v3
