/// @file
/// v3 cutover hook — fills in `nix::EvalState::v3EvalHook` so that
/// `EvalState::eval` can route to the v3 bytecode VM at runtime when
/// `NIX_USE_V3=1` is set.
///
/// The hook:
///   1. Lowers the AST through v3's IR + bytecode pipeline.
///   2. Runs the resulting CompilationUnit in v3's VM.
///   3. Converts the v3 Value back into a tree-walker `nix::Value`
///      written into the caller-provided slot.
///
/// Compiled CompilationUnits are cached per Expr* so that
/// re-evaluation of the same AST node (which is common across the
/// import cache) doesn't pay the lower/compile cost twice.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/vm.hh"
#include "v3/lower.hh"
#include "v3/ir.hh"
#include "v3/value.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"
#include "v3/serialize.hh"
#include "v3/disk_cache.hh"

#include "nix/expr/eval.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/util/source-path.hh"

#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3 {

// Forward declarations of the bridge helper in primops.cc.  The
// `Public` wrapper takes a nix::EvalState directly so we don't need
// to construct a v3 EvalState here.
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);
Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

// We don't expose treeWalkerToV3 here — the AST already carries
// nix::Expr nodes, not nix::Value, so we lower the Expr directly.

// Per-process cache keyed by Expr pointer (stable across the
// EvalState's lifetime).  Each entry owns a CompilationUnit; values
// stay alive for the lifetime of the EvalState since v3 doesn't yet
// have a tear-down hook.
struct CachedUnit {
    std::unique_ptr<CompilationUnit> cu;
};

static std::unordered_map<const nix::Expr *, CachedUnit> & v3HookCache()
{
    static std::unordered_map<const nix::Expr *, CachedUnit> tbl;
    return tbl;
}

/// VM-4 ext: parse-time side table populated by `EvalState::v3RegisterExprHook`.
/// Maps a top-level parsed Expr* to the SourcePath the parser was given.
/// Used as a fallback when the Expr's getPos() returns noPos (very common
/// for ExprLet / ExprAttrs which don't override getPos).
static std::unordered_map<const nix::Expr *, nix::SourcePath> & v3ExprPaths()
{
    static std::unordered_map<const nix::Expr *, nix::SourcePath> tbl;
    return tbl;
}

static void v3RegisterExprEntry(const nix::Expr * e, const nix::SourcePath & p)
{
    if (!e) return;
    v3ExprPaths().emplace(e, p);
}

/// CO-3: sub-Expr cache.  Each entry maps a tree-walker AST Expr*
/// (one of the per-thunk-body Exprs the lowerer recorded in
/// `Module::subExprFuncs`) to (CompilationUnit, FuncId).  The
/// forceValue hook consults this on every force; when tree-walker
/// is forcing a thunk whose underlying Expr* matches one v3 has
/// already lowered + compiled, we run that FuncId in v3 instead of
/// dispatching to expr->eval.
struct SubExprCacheEntry {
    const CompilationUnit * cu;
    ir::FuncId              funcIdx;
    /// Cached number of upvalues the function expects.  Phase A only
    /// handles 0; Phase B (task #278) translates env -> upvalues.
    uint16_t                nUpvalues;
    /// CO-2 phase B: for each upvalue (in `freeVars` order), the
    /// (level, displ) into the tree-walker `Env` that supplies its
    /// value.  Empty when nUpvalues == 0 (Phase A path) OR when one
    /// or more freeVars couldn't be traced back to a direct env
    /// reference (synthesized rec-attrset access, etc.).
    std::vector<std::pair<uint32_t, uint32_t>> upvalueSources;
    /// Phase B blacklist: set to true once a Phase B run for this
    /// entry threw at runtime.  Subsequent forces skip the entry
    /// instead of paying the lower+upvalue+run cost just to throw
    /// again — same Expr* + same env shape gives the same result.
    /// Reset only on cache rebuild (which doesn't happen mid-run).
    bool                    phaseBFailed = false;
};

static std::unordered_map<const nix::Expr *, SubExprCacheEntry> & v3SubExprCache()
{
    static std::unordered_map<const nix::Expr *, SubExprCacheEntry> tbl;
    return tbl;
}

/// Populate `v3SubExprCache` from a freshly lowered + compiled module.
/// Used by the eval hook (after lower+compile via the cutover) and by
/// primImport (WC-4) so that imported files contribute their per-thunk
/// functions too.  Idempotent: if an Expr* is already cached, the
/// existing entry wins (`emplace` semantics).
static void populateSubExprCacheLocal(
    const ir::Module & module, const CompilationUnit * cu)
{
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> originLookup;
    for (auto & vo : module.varOrigins) {
        uint64_t key = (static_cast<uint64_t>(vo.func) << 32) | vo.var;
        originLookup.emplace(key, std::make_pair(vo.level, vo.displ));
    }
    std::unordered_set<ir::VarId> recVarSet(
        module.recVarIds.begin(), module.recVarIds.end());
    auto & subCache = v3SubExprCache();
    for (auto & sef : module.subExprFuncs) {
        if (sef.funcIdx >= cu->lambdas.size()) continue;
        if (sef.funcIdx >= module.functions.size()) continue;
        SubExprCacheEntry entry{cu, sef.funcIdx,
            cu->lambdas[sef.funcIdx].nUpvalues, {}};
        if (entry.nUpvalues > 0) {
            auto & fvs = module.functions[sef.funcIdx].freeVars;
            bool ok = true;
            entry.upvalueSources.reserve(fvs.size());
            for (auto fv : fvs) {
                if (recVarSet.count(fv)) { ok = false; break; }
                uint64_t key = (static_cast<uint64_t>(sef.funcIdx) << 32) | fv;
                auto oit = originLookup.find(key);
                if (oit == originLookup.end()) { ok = false; break; }
                entry.upvalueSources.push_back(oit->second);
            }
            if (!ok) entry.upvalueSources.clear();
        }
        const_cast<nix::Expr *>(static_cast<const nix::Expr *>(sef.astExpr))
            ->isV3CacheCandidate = true;
        subCache.emplace(static_cast<const nix::Expr *>(sef.astExpr),
            std::move(entry));
    }
}

/// Process-wide counters that prove the cutover is firing.  Bumped on
/// every call to the v3 hook entry point and exposed via NIX_VM_STATS.
struct V3HookStats {
    uint64_t evalEntries  = 0;
    uint64_t cacheHits    = 0;
    uint64_t cacheMisses  = 0;

    // CO-2 sub-Expr cutover at forceValue.  Tracks how often the
    // forceValue hook fires + hits the cache.  Without sub-Expr cache
    // pre-population (CO-3), hits will mostly be 0 — but the Force
    // counter still reveals how much of the eval path goes through
    // sub-Expr forces vs. top-level evals.
    uint64_t forceEntries = 0;
    uint64_t forceHits    = 0;
    uint64_t forceMisses  = 0;
    /// `force` skipped because the cache entry needed upvalues we
    /// don't yet know how to translate from tree-walker's env.
    /// Phase B (env translation) will reduce this to zero.
    uint64_t forceSkippedNeedsUpvalues = 0;

    // WC-0: per-Expr::Kind force breakdown.  Indexed by static_cast
    // of the Expr's `exprKind` enum.  Lets us see WHICH kinds drive
    // forceMisses (no v3 cache entry) and forceSkippedNeedsUpvalues
    // (cache entry exists but we can't materialise upvalues).
    // Capped at 32 — Expr::Kind currently tops out around 28.
    static constexpr size_t kKindCap = 32;
    uint64_t forceEntriesByKind[kKindCap]   = {};
    uint64_t forceMissesByKind[kKindCap]    = {};
    uint64_t forceSkippedByKind[kKindCap]   = {};
    uint64_t forceHitsByKind[kKindCap]      = {};

    // WC-0: per-skip-reason breakdown for forceSkippedNeedsUpvalues.
    //   [0] phaseBFailed (previous force on same Expr* threw)
    //   [1] noUpvalueSources (synthesised rec/with/inheritFrom)
    //   [2] envWalkLevelTooDeep (env chain shorter than (level,displ))
    //   [3] envValueNull (env slot was null)
    //   [4] tw->v3 conversion threw
    //   [5] top-level cache CU needs upvalues we don't have
    static constexpr size_t kSkipReasonCap = 8;
    uint64_t forceSkipReason[kSkipReasonCap] = {};

    // WC-1: per-eval-fallback reason for the top-level eval hook.
    //   [0] runThrew              (v3 lower/compile/run threw)
    //   [1] resultClosureLikeTag  (Closure/Thunk/PrimOp/etc.)
    //   [2] bridgeReturnedNull    (v3ToTreeWalker → null)
    //   [3] bridgeThrew           (v3ToTreeWalker threw)
    //   [4] shortCircuitKindEarly (Lambda/Int/Float/.../Attrs/List)
    //   [5] willReturnClosure     (static-detector short-circuit)
    //   [6] sizeHeuristicSkip     (modules.functions > kSkipThreshold)
    static constexpr size_t kEvalFallbackCap = 8;
    uint64_t evalFallbackReason[kEvalFallbackCap] = {};

    // Per-phase total time (nanoseconds) — only populated when
    // V3_TIMING=1.  Lets us see whether lower, compile, run, or
    // bridge dominates the cutover overhead per file-toplevel Expr.
    uint64_t lowerNs   = 0;
    uint64_t compileNs = 0;
    uint64_t runNs     = 0;
    uint64_t bridgeNs  = 0;
};

V3HookStats & v3HookStats()
{
    static V3HookStats stats;
    return stats;
}

/// Statically detect whether evaluating `e` will produce a closure
/// (Lambda result) — without actually running it.  Used to skip v3
/// for shapes where v3's lower+compile+run would be wasted work
/// because the result must fall back to tree-walker anyway (closures
/// can't currently be cleanly bridged out of v3 — see Tag::Closure
/// case in the result switch below).
///
/// Recurses through trivially structural wrappers (let/with/assert/
/// if-with-both-branches-closure).  Conservative: returns false on
/// shapes whose result depends on runtime values we can't see at
/// compile time.  False positives are wrong (would skip v3 for an
/// expression that v3 could handle); false negatives only cost
/// efficiency.  We err strictly on the side of false negatives.
static bool willReturnClosure(const nix::Expr * e)
{
    if (!e) return false;
    auto k = e->exprKind;
    if (k == nix::Expr::Kind::Lambda)
        return true;
    if (k == nix::Expr::Kind::Let)
        return willReturnClosure(static_cast<const nix::ExprLet *>(e)->body);
    if (k == nix::Expr::Kind::With)
        return willReturnClosure(static_cast<const nix::ExprWith *>(e)->body);
    if (k == nix::Expr::Kind::Assert)
        return willReturnClosure(static_cast<const nix::ExprAssert *>(e)->body);
    if (k == nix::Expr::Kind::If) {
        // Both branches must produce a closure.  Otherwise we don't
        // know at compile time which arm is taken.
        auto * ei = static_cast<const nix::ExprIf *>(e);
        return willReturnClosure(ei->then) && willReturnClosure(ei->else_);
    }
    // Var (could resolve to anything), Call (depends on body of
    // callee), Select (depends on attrset shape), Op* (numeric/
    // string), literals (not closures), Attrs (an attrset, not a
    // closure), List (a list).  None of these are statically known
    // to be closures.
    return false;
}

/// The hook entry point.  Called from libnixexpr's EvalState::eval
/// when NIX_USE_V3=1 and this hook is non-null.
static void v3EvalEntry(nix::EvalState & state, nix::Expr * e, nix::Value & v)
{
    auto & st = v3HookStats();
    st.evalEntries++;
    // Register a stats-dump atexit handler on first entry.  We
    // intentionally do NOT call dumpPrimOpStats() here (its
    // static-mutex hits a destruction-order crash on libc++ exit
    // path); the simple POD counters in V3HookStats are safe to
    // read since they don't have non-trivial destructors.  Use
    // v3-eval directly for primop-level profiling.
    static bool atexitDone = []{
        if (std::getenv("NIX_VM_STATS") || std::getenv("V3_TIMING")) {
            std::atexit([]{
                auto & s = v3HookStats();
                std::fprintf(stderr,
                    "v3 hook stats: evalEntries=%llu cacheHits=%llu cacheMisses=%llu\n",
                    (unsigned long long)s.evalEntries,
                    (unsigned long long)s.cacheHits,
                    (unsigned long long)s.cacheMisses);
                std::fprintf(stderr,
                    "v3 force stats: forceEntries=%llu forceHits=%llu forceMisses=%llu skippedNeedsUpvalues=%llu\n",
                    (unsigned long long)s.forceEntries,
                    (unsigned long long)s.forceHits,
                    (unsigned long long)s.forceMisses,
                    (unsigned long long)s.forceSkippedNeedsUpvalues);
                // WC-0: per-Expr::Kind breakdown for force calls.
                // Only print kinds with non-zero counts (most are 0).
                // Names mirror Expr::Kind ordering in nixexpr.hh:108.
                static const char * kindNames[] = {
                    "Unknown","Int","Float","String","Path","Var",
                    "InheritFrom","Select","OpHasAttr","Attrs","List",
                    "Lambda","Call","Let","With","If","Assert",
                    "OpNot","OpUpdate","ConcatStrings","Pos","BlackHole",
                    "OpEq","OpNEq","OpAnd","OpOr","OpImpl","OpConcatLists"
                };
                bool anyKind = false;
                for (size_t i = 0; i < V3HookStats::kKindCap; ++i) {
                    if (s.forceEntriesByKind[i]
                        || s.forceHitsByKind[i]
                        || s.forceMissesByKind[i]
                        || s.forceSkippedByKind[i]) {
                        if (!anyKind) {
                            std::fprintf(stderr,
                                "v3 force by Expr::Kind:\n");
                            anyKind = true;
                        }
                        const char * nm = (i < std::size(kindNames))
                            ? kindNames[i] : "?";
                        std::fprintf(stderr,
                            "  %-14s entries=%llu hits=%llu miss=%llu skipNeedsUp=%llu\n",
                            nm,
                            (unsigned long long)s.forceEntriesByKind[i],
                            (unsigned long long)s.forceHitsByKind[i],
                            (unsigned long long)s.forceMissesByKind[i],
                            (unsigned long long)s.forceSkippedByKind[i]);
                    }
                }
                // WC-0: skip-reason breakdown.  Lets WC-2 see WHICH
                // upvalue-translation failure modes dominate.
                static const char * reasonNames[V3HookStats::kSkipReasonCap] = {
                    "phaseBFailedPreviously",
                    "noUpvalueSources",
                    "envWalkLevelTooDeep",
                    "envValueNull",
                    "twToV3ConversionThrew",
                    "topLevelCUNeedsUpvalues",
                    "(unused)",
                    "(unused)",
                };
                bool anyReason = false;
                for (size_t i = 0; i < V3HookStats::kSkipReasonCap; ++i) {
                    if (s.forceSkipReason[i] == 0) continue;
                    if (!anyReason) {
                        std::fprintf(stderr, "v3 force skip reasons:\n");
                        anyReason = true;
                    }
                    std::fprintf(stderr, "  %-26s %llu\n",
                        reasonNames[i],
                        (unsigned long long)s.forceSkipReason[i]);
                }
                // WC-1: top-level eval fallback reasons.
                static const char * evalReasonNames[V3HookStats::kEvalFallbackCap] = {
                    "runThrew",
                    "resultClosureLikeTag",
                    "bridgeReturnedNull",
                    "bridgeThrew",
                    "shortCircuitKindEarly",
                    "willReturnClosureStatic",
                    "sizeHeuristicSkip",
                    "(unused)",
                };
                bool anyEvalReason = false;
                for (size_t i = 0; i < V3HookStats::kEvalFallbackCap; ++i) {
                    if (s.evalFallbackReason[i] == 0) continue;
                    if (!anyEvalReason) {
                        std::fprintf(stderr, "v3 eval fallback reasons:\n");
                        anyEvalReason = true;
                    }
                    std::fprintf(stderr, "  %-26s %llu\n",
                        evalReasonNames[i],
                        (unsigned long long)s.evalFallbackReason[i]);
                }
                if (std::getenv("V3_TIMING"))
                    std::fprintf(stderr,
                        "v3 hook timing (ms): lower=%.3f compile=%.3f run=%.3f bridge=%.3f\n",
                        s.lowerNs   / 1e6,
                        s.compileNs / 1e6,
                        s.runNs     / 1e6,
                        s.bridgeNs  / 1e6);
            });
        }
        return true;
    }();
    (void)atexitDone;
    // Cache the V3_DEBUG_HOOK env var lookup at first call: getenv()
    // is not free on all libc implementations (involves a string
    // compare against the env table per call).  At ~70 us per call
    // on macOS, the per-call cost would be ~18 ms across the 255
    // hook entries hello.name triggers — same magnitude as the
    // residual cutover regression.  Convert to a static.
    static const bool diag = std::getenv("V3_DEBUG_HOOK") != nullptr;
    if (diag) std::fprintf(stderr, "v3 hook[%llu]: enter e=%p\n",
                           (unsigned long long)st.evalEntries, (void*)e);

    // Fast paths: shape-based short-circuits.  v3's lower+compile+run
    // cycle pays a ~1ms+ overhead per Expr, which dominates the
    // benefit on tiny / trivial Exprs.  For these, tree-walker's
    // direct evaluation is materially cheaper.  Profiling shows that
    // a `(import <nixpkgs> {}).hello.name` evaluation calls EvalState::eval
    // 254 times — short-circuiting the cheap ones halves overhead.
    if (e) {
        auto k = e->exprKind;
        if (k == nix::Expr::Kind::Lambda ||
            k == nix::Expr::Kind::Int    ||
            k == nix::Expr::Kind::Float  ||
            k == nix::Expr::Kind::String ||
            k == nix::Expr::Kind::Path   ||
            k == nix::Expr::Kind::Var    ||
            k == nix::Expr::Kind::Pos    ||
            // Top-level Attrs / List: tree-walker constructs these
            // with lazy thunks and is materially faster than v3's
            // lower+compile+run+bridge cycle, which forces every
            // attribute eagerly to produce a v3 attrset that is
            // then converted back via the recursive bridge.  We
            // observed 6 Attrs misses contributing ~3ms of bridge
            // work each on hello.name — net negative versus
            // tree-walker.  v3 lower also still serializes its
            // upvalue references in some shapes that throw at
            // runtime ("OP_GET_UPVALUE: no closure context"), which
            // are pure waste.  Skip these.
            k == nix::Expr::Kind::Attrs  ||
            k == nix::Expr::Kind::List) {
            if (diag) std::fprintf(stderr, "v3 hook: short-circuit kind=%d\n", (int)k);
            st.evalFallbackReason[4]++;
            e->eval(state, state.baseEnv, v);
            return;
        }
        // Static closure-result predicate.  v3 can't currently
        // bridge a Closure result back to tree-walker without falling
        // back anyway (see Tag::Closure case below); detecting this
        // up front saves the wasted lower+compile+run cycle.
        if (willReturnClosure(e)) {
            if (diag) std::fprintf(stderr,
                "v3 hook: static closure result predicted, kind=%d\n", (int)k);
            st.evalFallbackReason[5]++;
            e->eval(state, state.baseEnv, v);
            return;
        }
    }

    static bool registered = (registerBuiltinPrimOps(), true);
    (void)registered;
    setNixEvalState(&state);

    static const bool timingEnabled = std::getenv("V3_TIMING") != nullptr;
    using clock = std::chrono::steady_clock;

    auto & cache = v3HookCache();
    auto it = cache.find(e);
    const CompilationUnit * cu = nullptr;
    if (it == cache.end()) {
        st.cacheMisses++;
        if (diag) std::fprintf(stderr, "v3 hook: cache miss kind=%d\n",
                               e ? (int)e->exprKind : -1);

        // VM-4: try the disk cache before lower+compile.  Cache key
        // is SHA-256 of the source file content.  Gated on
        // NIX_V3_DISK_CACHE=1 — disabled by default.
        //
        // Source-path resolution order:
        //   1. v3ExprPaths side table — populated by the parse-time
        //      hook (V3RegisterExprHook in eval.cc).  Authoritative
        //      for top-level Expr*s parsed from a file via
        //      parseExprFromFile.  Hits even when the Expr's
        //      getPos() returns noPos (the common case for ExprLet
        //      / ExprAttrs).
        //   2. e->getPos() with a SourcePath origin — fallback for
        //      Exprs not registered (e.g. parseExprFromString).
        static const bool diskCacheEnabled =
            std::getenv("NIX_V3_DISK_CACHE") != nullptr;
        std::string srcContent;
        disk_cache::CacheKey diskKey{};
        if (diskCacheEnabled && e) {
            try {
                auto & paths = v3ExprPaths();
                auto pit = paths.find(e);
                if (pit != paths.end()) {
                    // Match parseExprFromFile's symlink behaviour
                    // (eval.cc:3706 uses .resolveSymlinks()) — without
                    // this, paths that pass through e.g. /tmp on macOS
                    // (which is a symlink to /private/tmp) throw.
                    srcContent = pit->second.resolveSymlinks().readFile();
                    diskKey = disk_cache::computeKeyForString(srcContent);
                    if (diag) std::fprintf(stderr,
                        "v3 hook: disk-cache key from side-table path=%s\n",
                        pit->second.path.abs().c_str());
                } else {
                    auto pos = state.positions[e->getPos()];
                    if (auto * sp = std::get_if<nix::SourcePath>(&pos.origin)) {
                        srcContent = sp->resolveSymlinks().readFile();
                        diskKey = disk_cache::computeKeyForString(srcContent);
                        if (diag) std::fprintf(stderr,
                            "v3 hook: disk-cache key from getPos path=%s\n",
                            sp->path.abs().c_str());
                    } else if (diag) {
                        std::fprintf(stderr,
                            "v3 hook: no SourcePath for e=%p (kind=%d)\n",
                            (const void*)e, e ? (int)e->exprKind : -1);
                    }
                }
            } catch (const std::exception & ex) {
                if (diag) std::fprintf(stderr,
                    "v3 hook: disk-cache key calc threw: %s\n", ex.what());
                // Best-effort — any read failure means no disk lookup.
            } catch (...) {
                if (diag) std::fprintf(stderr,
                    "v3 hook: disk-cache key calc threw (unknown)\n");
            }
        }
        if (!diskKey.empty()) {
            if (auto blob = disk_cache::lookup(diskKey)) {
                try {
                    auto compiled = std::make_unique<CompilationUnit>(
                        serialize::deserializeCU(*blob));
                    cu = compiled.get();
                    cache.emplace(e, CachedUnit{std::move(compiled)});
                    if (diag) std::fprintf(stderr,
                        "v3 hook: disk-cache HIT key=%s\n",
                        diskKey.hex().substr(0, 16).c_str());
                    goto cu_ready;
                } catch (const std::exception & ex) {
                    if (diag) std::fprintf(stderr,
                        "v3 hook: disk-cache deserialize threw: %s\n",
                        ex.what());
                    // Fall through to fresh lower+compile.
                }
            }
        }

        try {
            auto t0 = timingEnabled ? clock::now() : clock::time_point{};
            auto module = lowerNixExpr(e, state.symbols, state.positions);
            ir::computeFreeVars(module);
            auto t1 = timingEnabled ? clock::now() : clock::time_point{};
            // Heuristic: very large modules (typically nixpkgs/lib's
            // 504-lambda makeExtensible chain) tend to either throw
            // OP_FORCE: blackhole at runtime or return Tag::Closure —
            // both cause fall-back to tree-walker which then re-
            // evaluates the same file.  Skip the run+throw cycle by
            // bailing out before compile.  Threshold tuned to leave
            // hello.name's smaller successful cases (≤74 lambdas)
            // working while skipping the massive lib top-level.
            if (diag) std::fprintf(stderr,
                "v3 hook: lowered module has %zu functions, %zu blocks\n",
                module.functions.size(), module.blocks.size());
            // Heuristic: empirically, lowered modules with many
            // functions (= many lambdas / per-thunk units) tend to
            // either throw OP_FORCE blackhole at run time or return
            // Tag::Closure — both cause fall-back to tree-walker
            // which then re-evaluates the same file.  Skip the run+
            // throw cycle for these.  Threshold of 50 chosen
            // empirically: hello.name's successful 11/19-function
            // cases keep working; the 74/504-function blackhole/
            // closure cases skip directly to tree-walker.  Tunable
            // via NIX_V3_SKIP_THRESHOLD env var for experimentation.
            static const size_t kSkipThresholdFunctions = []{
                if (const char * v = std::getenv("NIX_V3_SKIP_THRESHOLD"))
                    return (size_t)std::atoi(v);
                return (size_t)50;
            }();
            if (module.functions.size() > kSkipThresholdFunctions) {
                if (diag) std::fprintf(stderr,
                    "v3 hook: skip lower-result (%zu functions > %zu) — "
                    "likely fall-back at run time\n",
                    module.functions.size(), kSkipThresholdFunctions);
                st.evalFallbackReason[6]++;
                e->eval(state, state.baseEnv, v);
                return;
            }
            // IR-level closure-result predicate: inspect functions[0]'s
            // entry block.  If the terminal's value is defined by an
            // ir::Lambda binding, the run result will be Tag::Closure
            // that the bridge can't currently hand back.  Skip the
            // run+bridge cycle directly.  Catches the small (e.g.
            // 4-function) `Let { x = ...; in lambda }` patterns the
            // size-based threshold above misses.
            auto willProduceClosure = [&]() -> bool {
                if (module.functions.empty()) return false;
                auto & f0 = module.functions[0];
                if (f0.entryBlock == ir::kInvalidBlock) return false;
                auto & blk = module.blocks[f0.entryBlock];
                auto * ret = std::get_if<ir::TermReturn>(&blk.terminal);
                if (!ret || ret->value == ir::kInvalid) return false;
                for (auto & bd : blk.bindings) {
                    if (bd.var != ret->value) continue;
                    return std::holds_alternative<ir::Lambda>(bd.expr);
                }
                return false;
            };
            if (willProduceClosure()) {
                if (diag) std::fprintf(stderr,
                    "v3 hook: skip — IR predicts closure result\n");
                st.evalFallbackReason[5]++;
                e->eval(state, state.baseEnv, v);
                return;
            }
            auto compiled = std::make_unique<CompilationUnit>(compile(module));
            auto t2 = timingEnabled ? clock::now() : clock::time_point{};
            if (timingEnabled) {
                st.lowerNs   += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                st.compileNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
            }
            cu = compiled.get();
            // CO-3: populate the sub-Expr cache from the lower's
            // recorded (Expr* -> FuncId) pairs.  Each thunk-body
            // function becomes a force-time entry point; nUpvalues
            // is captured here so Phase A's upvalue-free fast path
            // can decide cheaply whether to take the cutover.
            //
            // CO-2 phase B: build per-(funcId, varId) origin lookup
            // from the lower's recorded varOrigins; this lets us
            // populate per-function upvalueSources arrays so the
            // force hook can walk tree-walker's env at force time.
            populateSubExprCacheLocal(module, cu);
            cache.emplace(e, CachedUnit{std::move(compiled)});

            // VM-4: write the freshly-compiled CU to disk cache for
            // reuse on subsequent invocations of this same source.
            // Best-effort — failures (full disk, etc.) silently
            // increment insertFailures.
            if (!diskKey.empty() && serialize::isCacheable(*cu)) {
                try {
                    std::string blob = serialize::serializeCU(*cu);
                    disk_cache::insert(diskKey, blob);
                    if (diag) std::fprintf(stderr,
                        "v3 hook: disk-cache INSERT key=%s blob=%zu bytes\n",
                        diskKey.hex().substr(0, 16).c_str(), blob.size());
                } catch (const std::exception & ex) {
                    if (diag) std::fprintf(stderr,
                        "v3 hook: disk-cache serialize threw: %s\n", ex.what());
                }
            }
        } catch (const std::exception & ex) {
            if (diag) std::fprintf(stderr, "v3 hook: lower/compile threw: %s\n", ex.what());
            // Fall back to tree-walker by calling e->eval directly.
            // This lets us bail out cleanly when v3 hits something it
            // can't lower (e.g. unsupported AST shape) without crashing.
            // Tree-walker handles the rest of the evaluation.
            e->eval(state, state.baseEnv, v);
            return;
        }
        cu_ready:;
    } else {
        st.cacheHits++;
        cu = it->second.cu.get();
    }

    if (diag) std::fprintf(stderr, "v3 hook: about to run cu (%zu insts, %zu lambdas)\n",
                           cu->code.size(), cu->lambdas.size());
    Value r;
    try {
        auto t0 = timingEnabled ? clock::now() : clock::time_point{};
        r = run(*cu);
        auto t1 = timingEnabled ? clock::now() : clock::time_point{};
        if (timingEnabled)
            st.runNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    } catch (const std::exception & ex) {
        if (diag) std::fprintf(stderr, "v3 hook: run threw: %s\n", ex.what());
        st.evalFallbackReason[0]++;
        // WC-1: log the throwing Expr's source position once per
        // unique Expr*, so the user can identify which file-toplevel
        // expression v3 chokes on.  Only when V3_DEBUG_HOOK is set.
        if (diag && e) {
            static std::unordered_set<const nix::Expr *> reported;
            if (reported.insert(e).second) {
                auto pos = state.positions[e->getPos()];
                std::fprintf(stderr,
                    "v3 hook: WC-1 fall-back (run threw) — kind=%d "
                    "what=\"%s\" pos=%s\n",
                    (int)e->exprKind, ex.what(),
                    std::visit(nix::overloaded{
                        [&](const nix::SourcePath & sp) -> std::string {
                            return sp.path.abs() + ":" +
                                   std::to_string(pos.line);
                        },
                        [](const auto &) -> std::string {
                            return "<no-source>";
                        },
                    }, pos.origin).c_str());
            }
        }
        // v3 evaluation failure — fall back to tree-walker.
        e->eval(state, state.baseEnv, v);
        return;
    }
    if (diag) std::fprintf(stderr, "v3 hook: ran, tag=%d\n", (int)r.tag());

    // Convert the v3 result back to a tree-walker nix::Value in
    // the caller-provided slot.  v3ToTreeWalker GC-allocates a
    // nix::Value for nested structures; the top-level result we
    // just copy into the out-Value via mkBlah().
    switch (r.tag()) {
    case Tag::Bool:   v.mkBool(r.payload.i == 1); return;
    case Tag::Int:    v.mkInt(r.payload.i);       return;
    case Tag::Float:  v.mkFloat(r.payload.f);     return;
    case Tag::Null:   v.mkNull();                 return;
    case Tag::String: v.mkString(r.payload.str ? r.payload.str : "", state.mem); return;
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::Uninitialized:
    case Tag::External: {
        // Functions / thunks can't be cleanly handed back to the
        // tree-walker via the current bridge: v3ToTreeWalker for
        // closures uses a hardcoded-arity primop that doesn't match
        // the calling convention `autoCallFunction` expects.  Until
        // CO-3 fixes the bridge, fall back to tree-walker for these.
        if (diag) std::fprintf(stderr,
            "v3 hook: result tag=%d, falling back to tree-walker\n",
            (int)r.tag());
        st.evalFallbackReason[1]++;
        // WC-1: same logging as the throw path — identify each
        // unique Expr that returns a closure-like result.
        if (diag && e) {
            static std::unordered_set<const nix::Expr *> reported;
            if (reported.insert(e).second) {
                auto pos = state.positions[e->getPos()];
                std::fprintf(stderr,
                    "v3 hook: WC-1 fall-back (closure-like result tag=%d) "
                    "kind=%d pos=%s\n",
                    (int)r.tag(), (int)e->exprKind,
                    std::visit(nix::overloaded{
                        [&](const nix::SourcePath & sp) -> std::string {
                            return sp.path.abs() + ":" +
                                   std::to_string(pos.line);
                        },
                        [](const auto &) -> std::string {
                            return "<no-source>";
                        },
                    }, pos.origin).c_str());
            }
        }
        e->eval(state, state.baseEnv, v);
        return;
    }
    case Tag::Path:
    case Tag::Attrs:
    case Tag::List: {
        // Paths / attrsets / lists recursively convert via
        // v3ToTreeWalker.  Failures are tree-walker fallback.  Note
        // the bridge is known to crash on closures embedded inside
        // these structures — treat any throw as a signal to fall
        // back, and pre-emptively fall back if the bridge returns null.
        try {
            auto t0 = timingEnabled ? clock::now() : clock::time_point{};
            nix::Value * tmp = v3ToTreeWalkerPublic(state, r);
            if (timingEnabled) {
                auto t1 = clock::now();
                st.bridgeNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            }
            if (tmp) { v = *tmp; return; }
            st.evalFallbackReason[2]++;
        } catch (const std::exception & ex) {
            if (diag) std::fprintf(stderr, "v3 hook: bridge threw: %s\n", ex.what());
            st.evalFallbackReason[3]++;
        }
        e->eval(state, state.baseEnv, v);
        return;
    }
    }
}

/// CO-2 phase A — forceValue cutover.  Hooked from
/// EvalState::forceValue (eval-inline.hh) before the expr->eval
/// dispatch.  Returns true if v3 handled the eval; false to fall
/// through.
///
/// Phase A scope: only handle cache-hit Expr*s whose entry function
/// (functions[0]) has no freeVars — i.e., self-contained
/// CompilationUnits whose top-level needs no env from tree-walker.
/// In practice, this is the same set the top-level evalHook caches:
/// each imported file is its own CU, and its functions[0] should be
/// closed (every binding is local or comes from primops).
///
/// Phase B (CO-2 phase B / task #278) extends this to env-requiring
/// entries by walking tree-walker's `env` to materialise the v3
/// upvalues array at hook time.  Until that lands, freeVars-bearing
/// entries are signalled via forceSkippedNeedsUpvalues.
static bool v3ForceEntry(nix::EvalState & state, nix::Expr * e,
                          nix::Env & env, nix::Value & v)
{
    // Gating: NIX_USE_V3 enables the eval hook (file-toplevel
    // cutover); the forceValue hook is additionally gated on
    // NIX_USE_V3_FORCE=1.  Without sub-Expr cache pre-population
    // (CO-3 / task #279) every force call here misses and adds
    // ~75 ns of overhead — for hello.name that's 218k calls = ~16 ms.
    // Until CO-3 lands the hook is opt-in; after CO-3 we'll flip
    // the default and remove the separate env var.
    static const bool useV3Force = []{
        const char * a = std::getenv("NIX_USE_V3");
        const char * b = std::getenv("NIX_USE_V3_FORCE");
        return a && std::string_view(a) == "1"
            && b && std::string_view(b) == "1";
    }();
    if (!useV3Force) return false;

    auto & st = v3HookStats();
    st.forceEntries++;
    // WC-0: per-Expr::Kind breakdown.  Cheap (one bounded-array
    // increment) and lets WC-1 / WC-2 see WHICH Expr kinds dominate
    // the cache misses + needs-upvalues skips.
    auto kindIdx = [&]() -> size_t {
        if (!e) return 0;
        size_t k = static_cast<size_t>(e->exprKind);
        return k < V3HookStats::kKindCap ? k : 0;
    }();
    st.forceEntriesByKind[kindIdx]++;
    auto skipReturn = [&](size_t reasonIdx) {
        st.forceSkippedNeedsUpvalues++;
        st.forceSkippedByKind[kindIdx]++;
        if (reasonIdx < V3HookStats::kSkipReasonCap)
            st.forceSkipReason[reasonIdx]++;
        return false;
    };

    // CO-3 sub-Expr cache (per-thunk-body Functions recorded by the
    // lowerer).  Hits on the bulk of force traffic — every let
    // binding, every lazy attrset value, every list element wrapped
    // in a thunk goes through here.  Top-level CU lookups (the
    // original cache) are far rarer at force time and live in the
    // separate v3HookCache.
    const CompilationUnit * cu      = nullptr;
    ir::FuncId              funcIdx = 0;
    std::vector<Value>      upvalues;
    auto & subCache = v3SubExprCache();
    auto sit = subCache.find(e);
    if (sit != subCache.end()) {
        auto & ent = sit->second;
        if (ent.phaseBFailed) {
            // We tried Phase B for this entry before and it threw.
            // Same Expr* / same env shape => same outcome.  Skip.
            return skipReturn(0);
        }
        if (ent.nUpvalues != 0) {
            if (ent.upvalueSources.empty()) {
                // Phase B can't handle this entry (synthesized rec/
                // with/inheritFrom upvalues).  Skip.
                return skipReturn(1);
            }
            // CO-2 phase B: walk tree-walker's env per upvalueSource
            // to materialise the v3 upvalues array.
            try {
                upvalues.reserve(ent.nUpvalues);
                for (auto [level, displ] : ent.upvalueSources) {
                    nix::Env * cur = &env;
                    for (uint32_t i = 0; i < level; ++i) {
                        if (!cur || !cur->up) {
                            return skipReturn(2);
                        }
                        cur = cur->up;
                    }
                    if (!cur) {
                        return skipReturn(2);
                    }
                    nix::Value * srcV = cur->values[displ];
                    if (!srcV) {
                        return skipReturn(3);
                    }
                    upvalues.push_back(treeWalkerToV3Public(state, *srcV));
                }
            } catch (const std::exception &) {
                return skipReturn(4);
            }
        }
        cu      = ent.cu;
        funcIdx = ent.funcIdx;
    } else {
        auto & cache = v3HookCache();
        auto it = cache.find(e);
        if (it == cache.end()) {
            st.forceMisses++;
            st.forceMissesByKind[kindIdx]++;
            return false;  // No cached CU — fall through to expr->eval.
        }
        cu = it->second.cu.get();
        if (!cu->lambdas.empty() && cu->lambdas[0].nUpvalues != 0) {
            return skipReturn(5);
        }
        funcIdx = 0;
    }

    setNixEvalState(&state);
    // Cache the V3_DEBUG_HOOK env lookup — getenv() in a hot loop is
    // expensive on some libcs.
    static const bool diag = std::getenv("V3_DEBUG_HOOK") != nullptr;
    if (diag) std::fprintf(stderr,
        "v3 force hook: CU hit fid=%u nUp=%zu, running %zu insts / %zu lambdas\n",
        funcIdx, upvalues.size(), cu->code.size(), cu->lambdas.size());

    Value r;
    try {
        if (!upvalues.empty()) {
            r = runFunctionWithUpvalues(*cu, funcIdx,
                upvalues.data(), static_cast<uint32_t>(upvalues.size()));
        } else if (funcIdx == 0) {
            r = run(*cu);
        } else {
            r = runFunction(*cu, funcIdx);
        }
    } catch (const std::exception & ex) {
        if (diag) std::fprintf(stderr, "v3 force hook: run threw: %s\n", ex.what());
        // Phase B blacklist: same Expr* will arrive with the same env
        // shape on subsequent forces; retrying would just throw again.
        // Mark the entry as "Phase B failed" so we skip the whole
        // chain (cache lookup + env walk + run) next time.
        if (!upvalues.empty()) {
            auto sit2 = v3SubExprCache().find(e);
            if (sit2 != v3SubExprCache().end()) sit2->second.phaseBFailed = true;
        }
        return false;  // Fall back: tree-walker handles the rest.
    }

    // Bridge result back to tree-walker Value.  Mirror the eval hook's
    // logic — same bridge, same fallback rules.
    switch (r.tag()) {
    case Tag::Bool:   v.mkBool(r.payload.i == 1); st.forceHits++; st.forceHitsByKind[kindIdx]++; return true;
    case Tag::Int:    v.mkInt(r.payload.i);       st.forceHits++; st.forceHitsByKind[kindIdx]++; return true;
    case Tag::Float:  v.mkFloat(r.payload.f);     st.forceHits++; st.forceHitsByKind[kindIdx]++; return true;
    case Tag::Null:   v.mkNull();                 st.forceHits++; st.forceHitsByKind[kindIdx]++; return true;
    case Tag::String:
        v.mkString(r.payload.str ? r.payload.str : "", state.mem);
        st.forceHits++; st.forceHitsByKind[kindIdx]++;
        return true;
    case Tag::Path:
    case Tag::Attrs:
    case Tag::List: {
        try {
            nix::Value * tmp = v3ToTreeWalkerPublic(state, r);
            if (tmp) { v = *tmp; st.forceHits++; st.forceHitsByKind[kindIdx]++; return true; }
        } catch (const std::exception &) {
            // bridge fail — fall through
        }
        return false;
    }
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::PrimOp:
    case Tag::PrimOpApp:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::Uninitialized:
    case Tag::External:
    default:
        return false;
    }
}

namespace {

/// Static initializer — runs at library load time.  Once
/// libnixexprv3.dylib is linked into a binary that also pulls in
/// libnixexpr, this fills in the function pointer so `NIX_USE_V3=1`
/// can route through v3.  Also installs an atexit() handler that
/// dumps the hook stats when NIX_VM_STATS=1 is set, so the user
/// can verify the cutover is actually firing.
struct V3HookRegistrar {
    V3HookRegistrar() {
        nix::EvalState::v3EvalHook = &v3EvalEntry;
        // The parse-time hook is always installed: cheap (one map
        // insert per parsed file) and only matters when the disk
        // cache is enabled.
        nix::EvalState::v3RegisterExprHook = &v3RegisterExprEntry;
        // The forceValue hook is opt-in via NIX_USE_V3_FORCE=1; left
        // null by default so eval-inline.hh's branch-predictor folds
        // the v3 check away entirely on workloads that don't need it.
        // Until CO-3 (sub-Expr cache pre-population) lands the hook
        // costs more than it saves on real-world workloads.
        if (const char * v = std::getenv("NIX_USE_V3_FORCE");
            v && std::string_view(v) == "1") {
            nix::EvalState::v3ForceHook = &v3ForceEntry;
        }
    }
};

[[maybe_unused]] V3HookRegistrar _v3_hook_registrar_instance;

} // anonymous namespace

} // namespace nix::v3

// Public installer function — call this from the main `nix` binary so the
// linker can't strip libnixexprv3.  Without an explicit symbol reference,
// macOS's `-dead_strip_dylibs` removes the lib entirely (its only user
// is the static initializer that registers `EvalState::v3EvalHook`, and
// the linker doesn't see the static-init as "used").
namespace nix::v3 {

/// WC-4: public wrapper so primImport (in primops.cc) can pre-
/// populate the sub-Expr cache after its own lower+compile.
/// Without this, imported files are evaluated via v3 but their
/// per-thunk functions never make it into v3SubExprCache, so any
/// subsequent forceValue from tree-walker side falls through.
void populateSubExprCachePublic(
    const ir::Module & module, const CompilationUnit * cu)
{
    populateSubExprCacheLocal(module, cu);
}

void installEvalHook()
{
    nix::EvalState::v3EvalHook = &v3EvalEntry;
    nix::EvalState::v3RegisterExprHook = &v3RegisterExprEntry;
    if (const char * v = std::getenv("NIX_USE_V3_FORCE");
        v && std::string_view(v) == "1") {
        nix::EvalState::v3ForceHook = &v3ForceEntry;
    }
}
} // namespace nix::v3
