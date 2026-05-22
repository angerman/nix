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
#include "v3/limits.hh"
#include "v3/nursery.hh"
#include "v3/barrier.hh"

#include "nix/expr/eval.hh"

#include "nix/expr/config.hh"

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
                "run=%.3f diskLookup=%.3f diskInsert=%.3f\n",
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
                it.diskLookupNs / 1e6,
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
    pt.mark(pt.compile_ms);

    // Run.  STG-10 (vm.cc:5530) automatically routes through
    // `runOnExistingVm` if we're re-entered from another v3 dispatch
    // loop — so calling `runRootExpr` from inside a primop is safe.
    out.value = run(*out.cu);
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
        size_t boehmHeap = GC_get_heap_size();
        size_t boehmFree = GC_get_free_bytes();
#else
        size_t boehmHeap = 0;
        size_t boehmFree = 0;
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
            "boehm_free=%.1fMB v3_arena=%.1fMB elsewhere=%.1fMB\n",
            rssBytes   / 1e6,
            boehmHeap  / 1e6,
            boehmFree  / 1e6,
            arenaPin   / 1e6,
            elsewhere  / 1e6);
        // #660 verification: dump bridge-primop call counts.  v3-eval
        // already does this via its own NIX_VM_STATS path; mirror here
        // so the integrated `nix` CLI (and any future v3 driver that
        // goes through `runRootExpr`) reports the same data without
        // depending on the CLI specifically.
        dumpPrimOpStats(stderr);
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
        }
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
            // Nursery: young + (when Phase E active) two survivor
            // buffers of equal size.  When Phase E is off the
            // single nursery is just `sizeBytes`.
            uint64_t nurseryBytes = nstats.sizeBytes;
            if (threadNursery().isPhaseEActive())
                nurseryBytes += 2 * nstats.sizeBytes; // approx, S=Y default

            const size_t sumEst = sctEst + ppsEst + ppsStringBytes
                                + botEst + cotEst + gstEst + dirtyEst
                                + standaloneEst + nurseryBytes;
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
                sumEst / 1e6);
        }
    }
    return out;
}

} // namespace nix::v3
