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
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3 {

// Forward declarations of the bridge helper in primops.cc.  The
// `Public` wrapper takes a nix::EvalState directly so we don't need
// to construct a v3 EvalState here.
nix::Value * v3ToTreeWalkerPublic(nix::EvalState & nixState, Value v);
Value treeWalkerToV3Public(nix::EvalState & nixState, nix::Value & nv);

// #455: forward decl of the eager-bridge knob in primops.cc.
// Forces v3ToTreeWalker to use eager mode (no PrimOpApp deferrals)
// while in scope.  Used by the call hook for on-demand-root-
// populated lambda results to avoid the lazy-bridge cycle.
bool pushForceEagerBridge();
void popForceEagerBridge(bool prev);
struct ScopedEagerBridge {
    bool prev;
    ScopedEagerBridge() : prev(pushForceEagerBridge()) {}
    ~ScopedEagerBridge() { popForceEagerBridge(prev); }
};

// #452 / Phase C: forward decl of the shallow-TW-attrs-bridge knob.
// Held by the call hook for the duration of runLambda when the
// lambda has formals.  Switches treeWalkerToV3's nAttrs case from
// deep conversion to per-entry Bridge-thunk wrap, matching TW's
// per-formal lazy semantics so mid-construction attrset entries
// don't blackhole on lambda entry.
bool pushShallowTWAttrsBridge();
void popShallowTWAttrsBridge(bool prev);
struct ScopedShallowTWAttrsBridge {
    bool prev;
    ScopedShallowTWAttrsBridge() : prev(pushShallowTWAttrsBridge()) {}
    ~ScopedShallowTWAttrsBridge() { popShallowTWAttrsBridge(prev); }
};

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

/// #455 diag: tracks which sub-Expr cache entries were populated by
/// on-demand-root (vs the eval-hook path).  Lets the call hook
/// selectively block only on-demand-root-populated entries from
/// running, isolating which path's populate is producing buggy
/// upvalueSources / IR.
static std::unordered_set<const nix::Expr *> & v3OnDemandRootPopulated()
{
    static std::unordered_set<const nix::Expr *> s;
    return s;
}

/// #451 / Phase B: parse-time map ExprLambda* -> the root file's Expr*
/// that contains it.  Built by `collectLambdasIntoMap` walking the
/// freshly-parsed AST in v3RegisterExprEntry.
///
/// On call-hook miss, we look up `lambda` here, find the enclosing
/// root file, and call `lowerCompileAndPopulate(root, ...)` -- which
/// lowers the *whole* file with full enclosing-scope context, so
/// `populateSubExprCacheLocal` produces correct upvalueSources for
/// every lambda in the file (the prior failed on-demand attempt
/// lowered the lambda in isolation, dropping the enclosing scope's
/// varOrigins -> silent wrong-output regression).
///
/// Memory cost: one (ptr, ptr) entry per parsed lambda.  On nixpkgs
/// this is a few hundred KB; cheap relative to the AST itself.
static std::unordered_map<
    const nix::ExprLambda *, nix::Expr *> & v3LambdaRoot()
{
    static std::unordered_map<const nix::ExprLambda *, nix::Expr *> tbl;
    return tbl;
}

/// AST walker that finds every ExprLambda reachable from `e` and
/// records (lambda -> root) in `out`.  Recurses through every Expr
/// kind (including thunkified attr values, list elements, call args)
/// so the map covers lambdas behind those layers, not just direct
/// children.  Lambda bodies ARE descended into (nested lambdas).
///
/// Called once per parseExprFromFile via v3RegisterExprEntry.  The
/// root pointer stays stable for the lifetime of the EvalState's
/// AST allocator.
static void collectLambdasIntoMap(
    const nix::Expr * e,
    nix::Expr * root,
    std::unordered_map<const nix::ExprLambda *, nix::Expr *> & out)
{
    if (!e) return;
    using K = nix::Expr::Kind;
    switch (e->exprKind) {
    case K::Unknown:
    case K::Int:
    case K::Float:
    case K::String:
    case K::Path:
    case K::Var:
    case K::InheritFrom:
    case K::Pos:
    case K::BlackHole:
        return;
    case K::Lambda: {
        auto * lam = static_cast<const nix::ExprLambda *>(e);
        // Record this lambda -> root.  First-write-wins is fine: the
        // same ExprLambda* can't appear in two different roots (each
        // parseExprFromFile produces a fresh AST).
        out.emplace(lam, root);
        // Descend into the body to catch nested lambdas + thunkified
        // attr values inside (e.g. `a: { x = (b: a + b); }`).
        collectLambdasIntoMap(lam->body, root, out);
        // Also descend into formal default expressions; defaults are
        // thunked in v3 but contain real ASTs that may include
        // lambdas.
        if (auto formals = lam->getFormals()) {
            for (auto & fm : formals->formals)
                if (fm.def) collectLambdasIntoMap(fm.def, root, out);
        }
        return;
    }
    case K::Let:
    case K::Attrs: {
        // ExprLet's `attrs` field has the AttrDefs we need to walk;
        // ExprAttrs has them directly.  ExprLet additionally has body.
        const nix::ExprAttrs * a;
        const nix::Expr * body = nullptr;
        if (e->exprKind == K::Let) {
            auto * l = static_cast<const nix::ExprLet *>(e);
            a = l->attrs;
            body = l->body;
        } else {
            a = static_cast<const nix::ExprAttrs *>(e);
        }
        if (a) {
            // attrs (the AttrDefs map): ordered by symbol; iterate.
            if (a->attrs.has_value()) {
                for (auto & kv : *a->attrs)
                    collectLambdasIntoMap(kv.second.e, root, out);
            }
            if (a->dynamicAttrs) {
                for (auto & da : *a->dynamicAttrs) {
                    collectLambdasIntoMap(da.nameExpr, root, out);
                    collectLambdasIntoMap(da.valueExpr, root, out);
                }
            }
            if (a->inheritFromExprs) {
                for (auto * fe : *a->inheritFromExprs)
                    collectLambdasIntoMap(fe, root, out);
            }
        }
        if (body) collectLambdasIntoMap(body, root, out);
        return;
    }
    case K::Call: {
        auto * c = static_cast<const nix::ExprCall *>(e);
        collectLambdasIntoMap(c->fun, root, out);
        if (c->args)
            for (auto * arg : *c->args)
                collectLambdasIntoMap(arg, root, out);
        return;
    }
    case K::List: {
        auto * l = static_cast<const nix::ExprList *>(e);
        for (auto * x : l->elems)
            collectLambdasIntoMap(x, root, out);
        return;
    }
    case K::With: {
        auto * w = static_cast<const nix::ExprWith *>(e);
        collectLambdasIntoMap(w->attrs, root, out);
        collectLambdasIntoMap(w->body, root, out);
        return;
    }
    case K::If: {
        auto * i = static_cast<const nix::ExprIf *>(e);
        collectLambdasIntoMap(i->cond, root, out);
        collectLambdasIntoMap(i->then, root, out);
        collectLambdasIntoMap(i->else_, root, out);
        return;
    }
    case K::Assert: {
        auto * a = static_cast<const nix::ExprAssert *>(e);
        collectLambdasIntoMap(a->cond, root, out);
        collectLambdasIntoMap(a->body, root, out);
        return;
    }
    case K::OpNot:
        collectLambdasIntoMap(static_cast<const nix::ExprOpNot *>(e)->e, root, out);
        return;
    case K::OpUpdate:
    case K::OpConcatLists:
    case K::OpEq:
    case K::OpNEq:
    case K::OpAnd:
    case K::OpOr:
    case K::OpImpl: {
        // All the binop variants share the e1/e2 layout.
        auto * b = static_cast<const nix::ExprOpEq *>(e);
        collectLambdasIntoMap(b->e1, root, out);
        collectLambdasIntoMap(b->e2, root, out);
        return;
    }
    case K::Select: {
        auto * s = static_cast<const nix::ExprSelect *>(e);
        collectLambdasIntoMap(s->e, root, out);
        if (s->def) collectLambdasIntoMap(s->def, root, out);
        // attrPath components: dynamic-name expressions can host lambdas.
        for (auto & ap : s->getAttrPath())
            if (ap.expr) collectLambdasIntoMap(ap.expr, root, out);
        return;
    }
    case K::OpHasAttr: {
        auto * h = static_cast<const nix::ExprOpHasAttr *>(e);
        collectLambdasIntoMap(h->e, root, out);
        for (auto & ap : h->attrPath)
            if (ap.expr) collectLambdasIntoMap(ap.expr, root, out);
        return;
    }
    case K::ConcatStrings: {
        auto * cs = static_cast<const nix::ExprConcatStrings *>(e);
        for (auto & p : cs->es)
            collectLambdasIntoMap(p.second, root, out);
        return;
    }
    }
}

// Forward decl: defined later in this file (after CachedUnit, etc).
struct V3HookStats;
V3HookStats & v3HookStats();
static bool lowerCompileAndPopulate(
    nix::Expr * e, nix::EvalState & state, V3HookStats & st,
    bool bypassHookGate = false);

static void v3RegisterExprEntry(nix::EvalState & state,
                                 const nix::Expr * e,
                                 const nix::SourcePath & p)
{
    if (!e) return;
    v3ExprPaths().emplace(e, p);
    // #451 / Phase B: walk the parsed root and record (lambda -> root)
    // for every reachable ExprLambda.  Cheap O(N) AST walk; cost is a
    // few hundred KB of map entries on a nixpkgs eval.  Keyed by
    // ExprLambda*, which is address-stable for the AST's lifetime.
    //
    // Consumed by v3CallFunctionEntry's cache-miss path: when a
    // lambda isn't in subCache, look up its root here, lower+compile
    // the *whole root file* (preserving enclosing-scope varOrigins),
    // and re-probe.  Avoids the silent-wrong-output regression of
    // the prior on-demand attempt that lowered lambdas in isolation
    // (commit 2b9295437).
    //
    // Gated on NIX_USE_V3=1 so non-v3 invocations don't pay the walk.
    static const bool useV3 = []{
        const char * a = std::getenv("NIX_USE_V3");
        return a && std::string_view(a) == "1";
    }();
    static const bool lambdaRootMapEnabled = []{
        if (const char * v = std::getenv("NIX_V3_NO_LAMBDA_ROOT_MAP"))
            return std::string_view(v) != "1";
        return true;  // default ON when v3 is on
    }();
    if (useV3 && lambdaRootMapEnabled) {
        try {
            collectLambdasIntoMap(
                e, const_cast<nix::Expr *>(e), v3LambdaRoot());
        } catch (...) { /* opportunistic; ignore */ }
    }
    // #430 / #445: opt-in parse-time precompile.  Off by default so
    // existing workloads aren't taxed; flip with
    // NIX_V3_PARSE_PRECOMPILE=1 to populate v3SubExprCache for every
    // parsed file.  This expands v3 call-hook coverage from "the 19
    // thunk-bodies the eval hook sees" to "every lambda in every
    // parsed file".
    //
    // Gated additionally on NIX_USE_V3=1 -- no point compiling if
    // v3 won't ever run.  `lowerCompileAndPopulate` itself has an
    // internal cap (NIX_V3_PRECOMPILE_MAX_FNS, default 200 fns)
    // that drops oversized modules, so we don't blow up on
    // nixpkgs's all-packages.nix.
    //
    // #445: pass `bypassHookGate=true` so the hook-gate check inside
    // lowerCompileAndPopulate (which short-circuits on
    // `v3ForceHook == nullptr`) doesn't no-op the populate when
    // only the call hook is wired.  Without bypass, parse-precompile
    // populated nothing in default mode -- callHookHits stayed at 0
    // because the cache was never written.
    static const bool parsePrecompile =
        std::getenv("NIX_V3_PARSE_PRECOMPILE") != nullptr;
    if (useV3 && parsePrecompile) {
        try {
            (void)lowerCompileAndPopulate(
                const_cast<nix::Expr *>(e), state, v3HookStats(),
                /*bypassHookGate=*/true);
        } catch (...) { /* opportunistic; ignore failures */ }
    }
}

/// CO-3: sub-Expr cache.  Each entry maps a tree-walker AST Expr*
/// (one of the per-thunk-body Exprs the lowerer recorded in
/// `Module::subExprFuncs`) to (CompilationUnit, FuncId).  The
/// forceValue hook consults this on every force; when tree-walker
/// is forcing a thunk whose underlying Expr* matches one v3 has
/// already lowered + compiled, we run that FuncId in v3 instead of
/// dispatching to expr->eval.
/// CO-2 phase B + WC-2-followup: for each upvalue (in freeVars
/// order) describe how to materialise its value from tree-walker's
/// env at force time.
///
///   - kind == Direct: walk env up `level` times, read values[displ].
///   - kind == RecBuild: walk env up `level` times, then synthesise
///     a v3 Bindings* whose entries are (names[i],
///     env.values[i]) for i in 0..names.size()-1.  This bridges
///     v3's "rec attrset is one VarId" representation to
///     tree-walker's "each rec binding is its own env cell".
///   - kind == LitBuiltins: hand back the v3 vBuiltins singleton
///     (#425).  No env-side counterpart -- the lowerer bound the
///     freeVar to ir::LitBuiltins, which has no tree-walker shape;
///     the singleton is process-wide constant.
struct UpvalueSource {
    enum class Kind : uint8_t { Direct, RecBuild, LitBuiltins };
    Kind                  kind  = Kind::Direct;
    uint32_t              level = 0;
    uint32_t              displ = 0;        // valid when kind=Direct
    /// Shared with the originating ir::RecVarOrigin; O(1) copy.
    /// Valid when kind == RecBuild (otherwise null).
    std::shared_ptr<const std::vector<SymbolId>> names;
};

struct SubExprCacheEntry {
    const CompilationUnit * cu;
    ir::FuncId              funcIdx;
    uint16_t                nUpvalues;
    /// For each upvalue (in freeVars order) — see UpvalueSource doc.
    /// Empty when nUpvalues == 0 OR when one or more freeVars
    /// can't be expressed in either supported shape.
    std::vector<UpvalueSource> upvalueSources;
    /// Phase B failure tracking.  REVIEW_2026-05-04 F2 / B-5: previously
    /// `bool phaseBFailed` permanently disabled v3 for an Expr* on the
    /// FIRST thrown exception.  Symptoms: a user `throw` inside one
    /// upvalue context permanently disabled v3 in every other context;
    /// transient errors stuck.  Now a 3-strike counter -- need 3
    /// failures to stick.  Successful runs DON'T decrement (would race
    /// with concurrent retries) but the threshold gives transient
    /// throws (e.g. tryEval probes) up to 3 chances before sticking.
    uint8_t                 phaseBFailureCount = 0;
    /// Convenience: returns true when the entry is over the failure
    /// limit and should be skipped.  Threshold tunable via
    /// NIX_V3_PHASEB_FAIL_LIMIT (default 3).
    bool isPhaseBSkipped() const noexcept;

    // -----------------------------------------------------------------
    // #416: outer-with carriage.
    // -----------------------------------------------------------------
    //
    // Static analysis of the sub-Expr's AST tree at populate time tells
    // us which env-frames above this sub-Expr's call site are with-
    // frames (`values[0]` of those env frames hold the with-attrset).
    // At force-hook entry we walk the runtime env according to these
    // offsets, snapshot each `values[0]` into a Bridge thunk, and pass
    // the resulting list to runFunction[WithUpvalues] as capturedWiths
    // -- so OP_WITH_LOOKUP inside the body can find names defined in
    // outer with-scopes that the function body itself does not push.
    //
    // Innermost first; offset i is the number of `up` walks from the
    // hook-entry env required to reach with-frame i.  Empty means
    // either: (a) no outer-with dependency at all (the function never
    // pushes WithLookup), or (b) all WithLookups inside the body are
    // satisfied by the function's own ir::With pushes.  Either way
    // capturedWiths is null at runtime.
    std::vector<uint32_t>   outerWithLevels;
    /// True if the analyzer found outer-with usage but couldn't fully
    /// resolve the chain (e.g. an ExprVar inside the sub-Expr depends
    /// on an outer with-frame whose env-level we cannot statically
    /// determine).  At force time we refuse the v3 path with a fresh
    /// skip reason instead of throwing through the body.
    bool                    outerWithRefused = false;

    /// #426: true when this entry's astExpr is an ExprLambda, NOT a
    /// thunk body.  Lambda bodies have a paramVar that the body's
    /// OP_GET_LOCAL 0 reads -- forcing the function via runFunction /
    /// runFunctionWithUpvalues would leave slot 0 uninitialised.  The
    /// callFunction hook (v3CallFunctionEntry) handles these via
    /// runLambda; the force hook (v3ForceEntry) refuses them early.
    bool                    isLambda = false;

    /// #450 / Phase A: true when this entry is a Lambda whose body
    /// statically resolves to a closure (literal `body : Lambda`,
    /// or let/with/assert wrapping a Lambda, or if both branches are
    /// lambdas).  v3 cannot bridge a Tag::Closure result back to
    /// tree-walker without falling back, so running the v3 body and
    /// then refusing the result is pure wasted work.  The call hook
    /// gates on this flag at the head of v3CallFunctionEntry.
    /// Computed once at populateSubExprCacheLocal time via the same
    /// `willReturnClosure` walk the eval hook uses (v3_hook.cc:840+).
    bool                    callReturnsClosure = false;
};

inline bool SubExprCacheEntry::isPhaseBSkipped() const noexcept
{
    // #449: default lowered from 3 to 1.  The 3-strike threshold
    // (#430-quickwin-5 / 4b7acff76) was meant to give transient
    // throws (e.g. tryEval probes) a chance to recover, but cardano-
    // node-class workloads have many Exprs whose first force throws
    // a fix-point blackhole / cycle deterministically — retrying
    // these 2 more times costs ~1 s of v3-fhook wall (verified by
    // bisect: f456dd76c v3-fhook 4.41 s vs 4b7acff76 5.49 s, both
    // warm; setting NIX_V3_PHASEB_FAIL_LIMIT=1 at HEAD recovers
    // 0.6 s).  Users who need the 3-strike resurrection (e.g.
    // workloads dominated by tryEval probes) can set the env var.
    static const uint8_t kFailLimit = []{
        if (const char * v = std::getenv("NIX_V3_PHASEB_FAIL_LIMIT"))
            return (uint8_t)std::min(255, std::max(1, std::atoi(v)));
        return uint8_t{1};
    }();
    return phaseBFailureCount >= kFailLimit;
}

// Forward decl: defined below at line ~903; used by populateSubExprCacheLocal
// (#450 Phase A) and v3EvalEntry (the original consumer).
static bool willReturnClosure(const nix::Expr * e);

static std::unordered_map<const nix::Expr *, SubExprCacheEntry> & v3SubExprCache()
{
    static std::unordered_map<const nix::Expr *, SubExprCacheEntry> tbl;
    return tbl;
}

/// REVIEW MED-17: per-(env, names) memoization of RecBuild Bindings.
/// Every force with a RecBuild upvalue source builds a fresh
/// Bindings* + N Bridge thunks; tree-walker's Env identity is stable
/// across forces, so memoizing by (env-pointer, shared-names-pointer)
/// turns the second-and-later forces into a single hash lookup.
/// Key: pair of (nix::Env *, const std::vector<SymbolId> *) -- the
/// names pointer is the shared_ptr's underlying pointer (MED-9), which
/// is identical across UpvalueSources for the same recVar.
struct RecBuildCacheKey {
    const nix::Env *  env;
    const std::vector<SymbolId> * names;
    bool operator==(const RecBuildCacheKey & o) const noexcept
    { return env == o.env && names == o.names; }
};
struct RecBuildCacheKeyHash {
    size_t operator()(const RecBuildCacheKey & k) const noexcept
    {
        return std::hash<const void *>{}(k.env)
             ^ (std::hash<const void *>{}(k.names) << 1);
    }
};
/// REVIEW §1.2: per-cache-entry stamp to detect Env-pointer ABA reuse
/// across GC.  We snapshot the first slot pointer (`env->values[0]`)
/// when the Bindings is built; on lookup, re-read the same slot and
/// compare.  Boehm's collector can recycle an Env's address after it
/// becomes unreachable, and the recycled Env will have a different
/// (or null) values[0] -- the stamp catches that, treats the cache
/// hit as stale, and rebuilds.  Cheap (one pointer compare on hit).
struct RecBuildCacheValue {
    Bindings * b;
    const void * slot0Stamp;
};
static std::unordered_map<RecBuildCacheKey, RecBuildCacheValue,
                           RecBuildCacheKeyHash>
    & recBuildCache()
{
    thread_local std::unordered_map<RecBuildCacheKey, RecBuildCacheValue,
                                     RecBuildCacheKeyHash> tbl;
    return tbl;
}

/// Populate `v3SubExprCache` from a freshly lowered + compiled module.
/// Used by the eval hook (after lower+compile via the cutover) and by
/// primImport (WC-4) so that imported files contribute their per-thunk
/// functions too.  Idempotent: if an Expr* is already cached, the
/// existing entry wins (`emplace` semantics).
// ---------------------------------------------------------------------------
// #416: outer-with chain analysis
// ---------------------------------------------------------------------------
//
// For a sub-Expr `e` (which becomes a thunk-body Function), walk e's
// AST tree to determine the chain of OUTER `with` frames statically
// enclosing it.  An outer with is one that's lexically above `e`'s
// position; an inner with is one that lives inside `e`'s subtree (the
// IR's own With block handles those at runtime).
//
// Algorithm:
//   1. Walk `e`'s subtree once, collecting every `nix::ExprWith *`
//      lexically inside.  These are the "interior" withs.
//   2. Walk `e`'s subtree again with intra-e env-depth tracking; find
//      the FIRST reachable `ExprVar` whose `fromWith` is non-null
//      AND whose `fromWith` is not in the interior set.  That var's
//      (level - intra_depth) gives us the env offset (relative to E0,
//      the env at hook entry) of the innermost OUTER with-frame.
//   3. Chain through `fromWith->parentWith` accumulating prevWith to
//      compute offsets for subsequent outer with-frames.
//
// Sub-Expr boundaries we DO NOT descend through (each is its own
// thunkified Expr with its own SubExprCacheEntry):
//   - ExprLambda::body            (lambda body runs in fresh env at call)
//   - ExprAttrs attr values        (each becomes its own thunk)
//   - ExprAttrs inheritFromExprs   (each becomes a separate inheritEnv slot)
//   - ExprAttrs dynamic attr values
//   - ExprList elements
//   - ExprCall::args
//
// AST nodes that add an env-frame (depth +1 inside their body):
//   - ExprLet           (let frame for the bindings)
//   - ExprAttrs (rec)   (rec frame for entries)
//   - ExprWith          (with frame for the attrs)
//
// All other constructs keep depth unchanged.

namespace {

void collectInteriorWiths(const nix::Expr * e, std::unordered_set<const nix::ExprWith *> & set);

// Forward decls so the recursive helpers below can refer to one another.
void collectInteriorWiths_dispatch(const nix::Expr * e, std::unordered_set<const nix::ExprWith *> & set);

void collectInteriorWiths(const nix::Expr * e, std::unordered_set<const nix::ExprWith *> & set)
{
    if (!e) return;
    collectInteriorWiths_dispatch(e, set);
}

void collectInteriorWiths_dispatch(const nix::Expr * e, std::unordered_set<const nix::ExprWith *> & set)
{
    using K = nix::Expr::Kind;
    switch (e->exprKind) {
    case K::Unknown:
    case K::Int:
    case K::Float:
    case K::String:
    case K::Path:
    case K::Var:
    case K::InheritFrom:
    case K::Pos:
    case K::BlackHole:
        return; // leaves -- nothing to recurse into
    case K::With: {
        auto * w = static_cast<const nix::ExprWith *>(e);
        set.insert(w);
        // Don't descend into attrs (evaluated in OUTER env -- but its
        // own ExprVars share the same outer-with chain as `e`, so we
        // could descend; safer to skip since the attrs become a
        // bridge-thunk at force time anyway).  DO descend into body.
        collectInteriorWiths(w->body, set);
        break;
    }
    case K::Let: {
        auto * l = static_cast<const nix::ExprLet *>(e);
        // attrs values are thunkified separately; don't descend into them.
        // body lives in the let frame.
        collectInteriorWiths(l->body, set);
        break;
    }
    case K::Attrs: {
        // attr values are thunkified.  We DO descend into the
        // top-level attrset structure but not into the value Exprs --
        // those are separate sub-Exprs.  In practice ExprAttrs holds
        // no ExprWith of its own, so this is effectively a no-op leaf.
        break;
    }
    case K::Lambda:
        // lambda body is a separate sub-Expr; don't descend.
        break;
    case K::Call: {
        auto * c = static_cast<const nix::ExprCall *>(e);
        collectInteriorWiths(c->fun, set);
        // args are thunkified per-call; don't descend.
        break;
    }
    case K::List:
        // elements are thunkified; don't descend.
        break;
    case K::If: {
        auto * i = static_cast<const nix::ExprIf *>(e);
        collectInteriorWiths(i->cond, set);
        collectInteriorWiths(i->then, set);
        collectInteriorWiths(i->else_, set);
        break;
    }
    case K::Assert: {
        auto * a = static_cast<const nix::ExprAssert *>(e);
        collectInteriorWiths(a->cond, set);
        collectInteriorWiths(a->body, set);
        break;
    }
    case K::OpNot:
        collectInteriorWiths(static_cast<const nix::ExprOpNot *>(e)->e, set);
        break;
    case K::OpUpdate:
    case K::OpConcatLists:
    case K::OpEq:
    case K::OpNEq:
    case K::OpAnd:
    case K::OpOr:
    case K::OpImpl: {
        // All MakeBinOp variants share e1/e2 layout (see nixexpr.hh:802).
        auto * b = static_cast<const nix::ExprOpEq *>(e); // any binop layout works
        collectInteriorWiths(b->e1, set);
        collectInteriorWiths(b->e2, set);
        break;
    }
    case K::Select:
        collectInteriorWiths(static_cast<const nix::ExprSelect *>(e)->e, set);
        // attr-path components are static (Symbol or computed via nameExpr,
        // which would be thunkified).
        break;
    case K::OpHasAttr:
        collectInteriorWiths(static_cast<const nix::ExprOpHasAttr *>(e)->e, set);
        break;
    case K::ConcatStrings: {
        auto * cs = static_cast<const nix::ExprConcatStrings *>(e);
        for (auto & p : cs->es)
            collectInteriorWiths(p.second, set);
        break;
    }
    }
}

/// AST walk to find the first reachable ExprVar with non-null fromWith
/// whose fromWith is NOT in the interior set, tracking intra-`e` env
/// depth.  If found, returns true and writes (anchorVar, depthAtAnchor)
/// to outparams.  If not found, returns false (caller treats as "no
/// outer-with dependency" -- safe: empty chain).
bool findOuterAnchor(
    const nix::Expr * e,
    const std::unordered_set<const nix::ExprWith *> & interior,
    uint32_t depth,
    const nix::ExprVar *& anchorOut,
    uint32_t & anchorDepthOut)
{
    if (!e) return false;
    using K = nix::Expr::Kind;
    switch (e->exprKind) {
    case K::Unknown:
    case K::Int:
    case K::Float:
    case K::String:
    case K::Path:
    case K::InheritFrom:
    case K::Pos:
    case K::BlackHole:
        return false;
    case K::Var: {
        auto * v = static_cast<const nix::ExprVar *>(e);
        if (v->fromWith && interior.count(v->fromWith) == 0) {
            anchorOut = v;
            anchorDepthOut = depth;
            return true;
        }
        return false;
    }
    case K::With: {
        auto * w = static_cast<const nix::ExprWith *>(e);
        // attrs evaluates in OUTER scope at depth `depth`.
        if (findOuterAnchor(w->attrs, interior, depth, anchorOut, anchorDepthOut))
            return true;
        return findOuterAnchor(w->body, interior, depth + 1, anchorOut, anchorDepthOut);
    }
    case K::Let: {
        auto * l = static_cast<const nix::ExprLet *>(e);
        // Body runs at depth+1; attr values are thunkified (skip).
        return findOuterAnchor(l->body, interior, depth + 1, anchorOut, anchorDepthOut);
    }
    case K::Attrs:
        // attr values are thunkified; nothing to recurse into for
        // anchor purposes here.
        return false;
    case K::Lambda:
        // separate sub-Expr; don't descend.
        return false;
    case K::Call: {
        auto * c = static_cast<const nix::ExprCall *>(e);
        if (findOuterAnchor(c->fun, interior, depth, anchorOut, anchorDepthOut))
            return true;
        // args thunkified.
        return false;
    }
    case K::List:
        return false; // elements thunkified.
    case K::If: {
        auto * i = static_cast<const nix::ExprIf *>(e);
        if (findOuterAnchor(i->cond, interior, depth, anchorOut, anchorDepthOut)) return true;
        if (findOuterAnchor(i->then, interior, depth, anchorOut, anchorDepthOut)) return true;
        return findOuterAnchor(i->else_, interior, depth, anchorOut, anchorDepthOut);
    }
    case K::Assert: {
        auto * a = static_cast<const nix::ExprAssert *>(e);
        if (findOuterAnchor(a->cond, interior, depth, anchorOut, anchorDepthOut)) return true;
        return findOuterAnchor(a->body, interior, depth, anchorOut, anchorDepthOut);
    }
    case K::OpNot:
        return findOuterAnchor(static_cast<const nix::ExprOpNot *>(e)->e,
            interior, depth, anchorOut, anchorDepthOut);
    case K::OpUpdate:
    case K::OpConcatLists:
    case K::OpEq:
    case K::OpNEq:
    case K::OpAnd:
    case K::OpOr:
    case K::OpImpl: {
        auto * b = static_cast<const nix::ExprOpEq *>(e);
        if (findOuterAnchor(b->e1, interior, depth, anchorOut, anchorDepthOut)) return true;
        return findOuterAnchor(b->e2, interior, depth, anchorOut, anchorDepthOut);
    }
    case K::Select:
        return findOuterAnchor(static_cast<const nix::ExprSelect *>(e)->e,
            interior, depth, anchorOut, anchorDepthOut);
    case K::OpHasAttr:
        return findOuterAnchor(static_cast<const nix::ExprOpHasAttr *>(e)->e,
            interior, depth, anchorOut, anchorDepthOut);
    case K::ConcatStrings: {
        auto * cs = static_cast<const nix::ExprConcatStrings *>(e);
        for (auto & p : cs->es)
            if (findOuterAnchor(p.second, interior, depth, anchorOut, anchorDepthOut))
                return true;
        return false;
    }
    }
    return false;
}

/// Compute outer-with offsets for a sub-Expr `e`.  Writes to `out`
/// (innermost outer with first); returns true if the analysis completed
/// cleanly (which includes the trivial "no outer-with dependency"
/// case -- empty `out`), false if we found a dependency we can't
/// resolve (caller should mark outerWithRefused).
bool analyzeOuterWiths(const nix::Expr * e, std::vector<uint32_t> & out)
{
    out.clear();
    if (!e) return true;

    std::unordered_set<const nix::ExprWith *> interior;
    collectInteriorWiths(e, interior);

    const nix::ExprVar * anchor = nullptr;
    uint32_t anchorDepth = 0;
    if (!findOuterAnchor(e, interior, 0, anchor, anchorDepth))
        return true; // no outer-with dependency

    // anchor->level is relative to the env at the anchor's AST
    // position.  Subtracting anchorDepth (the count of intra-e env
    // frames between e's root and the anchor) gives the offset
    // relative to E0 = the env at hook entry.
    if (anchor->level < anchorDepth) {
        // Should not happen if our depth tracking matches the parser
        // -- it would mean the anchor's static binder thinks the var
        // is bound deeper than our walk does.  Refuse rather than
        // produce a wrong offset.
        return false;
    }
    uint32_t off = anchor->level - anchorDepth;
    out.push_back(off);

    // Chain through fromWith.  prevWith on the CURRENT fromWith
    // advances to its parent.
    const nix::ExprWith * w = anchor->fromWith;
    while (w && w->parentWith) {
        // Defensive: prevWith is a uint32_t; check for overflow.
        uint64_t next = (uint64_t)off + (uint64_t)w->prevWith;
        if (next > UINT32_MAX) return false;
        off = (uint32_t)next;
        out.push_back(off);
        w = w->parentWith;
    }
    return true;
}

} // namespace

static void populateSubExprCacheLocal(
    const ir::Module & module, const CompilationUnit * cu)
{
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> originLookup;
    for (auto & vo : module.varOrigins) {
        uint64_t key = (static_cast<uint64_t>(vo.func) << 32) | vo.var;
        originLookup.emplace(key, std::make_pair(vo.level, vo.displ));
    }
    // WC-2-followup: per-(func, recVar) shape lookup.  First entry
    // wins (duplicates are identical info).
    std::unordered_map<uint64_t, const ir::RecVarOrigin *> recOriginLookup;
    for (auto & rvo : module.recVarOrigins) {
        uint64_t key = (static_cast<uint64_t>(rvo.func) << 32) | rvo.recVar;
        recOriginLookup.emplace(key, &rvo);
    }
    std::unordered_set<ir::VarId> recVarSet(
        module.recVarIds.begin(), module.recVarIds.end());
    // #425: VarIds bound to ir::LitBuiltins -- inner functions
    // capturing one as freeVar get a LitBuiltins UpvalueSource.
    std::unordered_set<ir::VarId> litBuiltinsSet(
        module.litBuiltinsVarIds.begin(), module.litBuiltinsVarIds.end());
    auto & subCache = v3SubExprCache();
    static const bool diagOrigins = std::getenv("V3_DEBUG_ORIGINS") != nullptr;
    for (auto & sef : module.subExprFuncs) {
        if (sef.funcIdx >= cu->lambdas.size()) continue;
        if (sef.funcIdx >= module.functions.size()) continue;
        SubExprCacheEntry entry{cu, sef.funcIdx,
            cu->lambdas[sef.funcIdx].nUpvalues, {}};
        if (entry.nUpvalues > 0) {
            auto & fvs = module.functions[sef.funcIdx].freeVars;
            bool ok = true;
            const char * failClass = nullptr; // for #425 diagnostics
            ir::VarId failedVar = ir::kInvalid;
            entry.upvalueSources.reserve(fvs.size());
            for (auto fv : fvs) {
                uint64_t key = (static_cast<uint64_t>(sef.funcIdx) << 32) | fv;
                if (recVarSet.count(fv)) {
                    auto rit = recOriginLookup.find(key);
                    if (rit == recOriginLookup.end()) {
                        if (diagOrigins) std::fprintf(stderr,
                            "v3 origins: func=%u skip — fv=%u in recVarSet "
                            "but no recVarOrigins entry\n",
                            sef.funcIdx, fv);
                        failClass = "rec-no-origin";
                        failedVar = fv;
                        ok = false; break;
                    }
                    UpvalueSource src;
                    src.kind  = UpvalueSource::Kind::RecBuild;
                    src.level = rit->second->level;
                    src.names = rit->second->names;  // shared_ptr<vector<SymbolId>>; O(1) copy.
                    entry.upvalueSources.push_back(std::move(src));
                    continue;
                }
                // #425: LitBuiltins fv -- no env walk needed;
                // hand back the singleton at hook time.
                if (litBuiltinsSet.count(fv)) {
                    UpvalueSource src;
                    src.kind = UpvalueSource::Kind::LitBuiltins;
                    entry.upvalueSources.push_back(std::move(src));
                    continue;
                }
                auto oit = originLookup.find(key);
                if (oit == originLookup.end()) {
                    if (diagOrigins) std::fprintf(stderr,
                        "v3 origins: func=%u skip — fv=%u has no varOrigins entry\n",
                        sef.funcIdx, fv);
                    failClass = "synthetic-no-origin";
                    failedVar = fv;
                    ok = false; break;
                }
                UpvalueSource src;
                src.kind  = UpvalueSource::Kind::Direct;
                src.level = oit->second.first;
                src.displ = oit->second.second;
                entry.upvalueSources.push_back(std::move(src));
            }
            if (!ok) {
                entry.upvalueSources.clear();
                // #425 diagnostics: print which class of synthetic
                // VarId is killing the upvalue translation, plus the
                // function's own metadata so we can see the AST shape
                // at fault.  Also locate WHICH binding defines the
                // failed VarId so we can identify the lowerer helper
                // responsible for the synth.
                static const bool diagNoUpv =
                    std::getenv("V3_DEBUG_NOUPV") != nullptr;
                if (diagNoUpv) {
                    const auto * astE =
                        static_cast<const nix::Expr *>(sef.astExpr);
                    // Find the binding that defines failedVar.  VarIds
                    // are unique across the Module so a single linear
                    // scan suffices (slow, debug-only).
                    int binderKind = -1;
                    nix::v3::ir::BlockId binderBlock = 0;
                    for (nix::v3::ir::BlockId bid = 1;
                         bid < (nix::v3::ir::BlockId)module.blocks.size(); ++bid)
                    {
                        for (const auto & bd : module.blocks[bid].bindings) {
                            if (bd.var == failedVar) {
                                binderKind = (int)bd.expr.index();
                                binderBlock = bid;
                                goto found;
                            }
                        }
                    }
                    found:
                    std::fprintf(stderr,
                        "v3 noUpvSrc: func=%u fid_kind=%d astKind=%d "
                        "fv=%u (%s) binder=variant#%d block=%u freeVars=[",
                        sef.funcIdx,
                        (int)module.functions[sef.funcIdx].entryBlock,
                        (int)astE->exprKind,
                        failedVar,
                        failClass ? failClass : "?",
                        binderKind, (unsigned)binderBlock);
                    for (size_t i = 0; i < fvs.size(); ++i)
                        std::fprintf(stderr, "%s%u",
                            i ? "," : "", fvs[i]);
                    std::fprintf(stderr, "]\n");
                }
            }
        }

        // #416: outer-with chain analysis.  Costs one AST walk per
        // sub-Expr at populate time; result is reused for every force.
        // The static analysis is conservative -- a refused entry just
        // routes future forces to tree-walker (existing skipReturn).
        {
            const auto * astE =
                static_cast<const nix::Expr *>(sef.astExpr);
            // #426: tag lambda registrations so the force hook can
            // refuse them early (they need a callFunction-style entry
            // with arg, not a thunk-body force).
            entry.isLambda =
                astE->exprKind == nix::Expr::Kind::Lambda;
            // #450 / Phase A: static closure-result predicate for the
            // call hook.  A Lambda whose body returns a closure (e.g.
            // `f = a: b: a + b` -- inner `b: a + b` is a closure)
            // would, if called via v3, run the body and produce a
            // Tag::Closure that the bridge cannot return to TW.  Mark
            // such entries so v3CallFunctionEntry can gate before
            // running.  Walks the AST body via the same predicate
            // the eval hook uses (willReturnClosure).
            if (entry.isLambda) {
                const auto * lam =
                    static_cast<const nix::ExprLambda *>(astE);
                entry.callReturnsClosure = willReturnClosure(lam->body);
            }
            if (!analyzeOuterWiths(astE, entry.outerWithLevels)) {
                entry.outerWithRefused = true;
                entry.outerWithLevels.clear();
            }
            static const bool diagOuterWith =
                std::getenv("V3_DEBUG_OUTER_WITH") != nullptr;
            if (diagOuterWith) {
                if (entry.outerWithRefused) {
                    std::fprintf(stderr,
                        "v3 outerWith: REFUSED expr=%p kind=%d\n",
                        (void *)astE, (int)astE->exprKind);
                } else if (!entry.outerWithLevels.empty()) {
                    std::fprintf(stderr,
                        "v3 outerWith: chain=%zu offsets=[",
                        entry.outerWithLevels.size());
                    for (size_t i = 0; i < entry.outerWithLevels.size(); ++i)
                        std::fprintf(stderr, "%s%u",
                            i ? "," : "", entry.outerWithLevels[i]);
                    std::fprintf(stderr,
                        "] kind=%d\n", (int)astE->exprKind);
                }
            }
        }

        // REVIEW MED-12: this remaining const_cast is monotonic-true:
        // once an Expr is registered as a v3 cache candidate, it
        // stays one for all states sharing the AST.  The mutation is
        // benign across states because it only enables the
        // eval-inline.hh fast-path branch (which then calls the v3
        // hook, which has its own per-state skip set for structural
        // failures).  The skipPermanently false-mutation that caused
        // cross-state pollution was removed above.
        //
        // #455 diag: NIX_V3_NO_CACHE_CANDIDATE_FLAG=1 skips the AST
        // mutation entirely, isolating that side effect for bisecting
        // the on-demand-root infinite-recursion bug.
        static const bool noCandidateFlag =
            std::getenv("NIX_V3_NO_CACHE_CANDIDATE_FLAG") != nullptr;
        if (!noCandidateFlag) {
            const_cast<nix::Expr *>(static_cast<const nix::Expr *>(sef.astExpr))
                ->isV3CacheCandidate = true;
        }
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

    // WC-12: per-upvalue-source-kind counters for force-hook entries
    // that proceed past upvalue materialisation.  Lets us decide
    // whether RecBuild is the primary driver of wins (or losses) and
    // whether selectively gating it avoids cross-VM cycles.
    uint64_t forceHookDirectUpvalues   = 0;
    uint64_t forceHookRecBuildUpvalues = 0;
    uint64_t forceHookHitsDirectOnly   = 0;
    uint64_t forceHookHitsWithRecBuild = 0;

    /// #416 instrumentation: how often the outer-with carriage path
    /// fires.  `Built` counts subCache hits whose entry has a
    /// non-empty outerWithLevels (a thunk that statically depends on
    /// outer with-frames -- the new feature actually does work here).
    /// `Refused` counts entries we marked outerWithRefused at populate
    /// time.  `Empty` counts entries with an empty chain (the trivial
    /// case -- no outer-with dependency, no extra cost).
    uint64_t forceHookOuterWithBuilt   = 0;
    uint64_t forceHookOuterWithRefused = 0;
    uint64_t forceHookOuterWithEmpty   = 0;

    /// #430 measurement: how often the call hook fires + outcome.
    /// Pre-#430 the hook had no entry counter, hiding what fraction
    /// of tree-walker callFunction invocations actually reach v3.
    uint64_t callHookEntries          = 0;  // every call to v3CallFunctionEntry
    uint64_t callHookGated            = 0;  // returned false from gate (NIX_USE_V3 off, lambda has formals, etc.)
    uint64_t callHookCacheMiss        = 0;  // ent missing from subCache
    uint64_t callHookHits             = 0;  // ran v3 closure body successfully
    uint64_t callHookBodyThrew        = 0;  // v3 closure threw during run
    uint64_t callHookResultBridgeFailed = 0; // result bridge declined
    uint64_t callHookUniqueMisses     = 0;  // #unique ExprLambda*s missing
    uint64_t callHookHottestMiss      = 0;  // max count for any single lambda
    /// #436: call-hook closure-shape result refusals.  Tracks how
    /// often v3CallFunctionEntry declined to bridge a Tag::Closure /
    /// PrimOp / PrimOpApp / Thunk / App / Blackhole result back to
    /// tree-walker.  Tree-walker re-runs the call natively in those
    /// cases.  Non-zero is normal (curried lambdas, function-returning
    /// helpers); spikes correlate with bodies that v3 owns but whose
    /// consumers expect a forced primitive.
    uint64_t callHookClosureResultRefused = 0;

    /// #425: per-gate call-hook funnel.  Each counter increments at
    /// exactly one specific gate; the sum equals callHookGated minus
    /// the rare paths not yet broken down.  Lets us size which gate
    /// is the bottleneck before adding compensating logic.
    uint64_t callHookGateUseV3              = 0;  // NIX_USE_V3=0 (full disable)
    uint64_t callHookGateNotLambda          = 0;  // !fun.isLambda()
    uint64_t callHookGateNullLambda         = 0;  // fun.lambda().fun==nullptr
    uint64_t callHookGateFormals            = 0;  // lambda has formal-attrset
    uint64_t callHookGateReentrant          = 0;  // s_callDepth > 0
    uint64_t callHookGateNotIsLambdaEnt     = 0;  // ent.isLambda==false
    /// #450 / Phase A: lambda body provably returns a closure.
    /// Skips the wasted v3 run that would just refuse the result.
    uint64_t callHookGateReturnsClosure     = 0;
    uint64_t callHookGatePhaseBSkipped      = 0;  // 3-strike fail counter tripped
    uint64_t callHookGateOuterWith          = 0;  // outerWithRefused
    /// Past every gate but failed at prepHookUpvaluesAndWiths or
    /// `fun.lambda().env == nullptr`.  Closes the funnel.
    uint64_t callHookPrepFail               = 0;

    /// #451 / Phase B: on-demand-with-root cache-miss outcomes.
    /// Each lambda call-hook miss either resolves via root compile
    /// (CacheMissResolved -> proceeds to run v3) or fails one of
    /// the sub-paths.  Sums to callHookCacheMiss when on-demand-root
    /// is enabled.
    uint64_t callHookCacheMissNoRoot         = 0;  // lambda not in v3LambdaRoot
    uint64_t callHookCacheMissCompileFailed  = 0;  // root lowerCompileAndPopulate threw / returned false
    uint64_t callHookCacheMissPostCompile    = 0;  // root compiled but lambda still not in subCache
    uint64_t callHookCacheMissResolved       = 0;  // root compiled + lambda in subCache; proceeds
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

/// WC-11: tracks Expr*s for which we've already speculatively
/// lowered+compiled+populated the sub-Expr cache on a fallback path.
/// Without this, every re-entry into the eval hook for the same
/// short-circuited Expr would re-pay the lower+compile cost.
static std::unordered_set<const nix::Expr *> & v3FallbackPopulated()
{
    static std::unordered_set<const nix::Expr *> s;
    return s;
}

/// WC-11: lower + compile + populateSubExprCacheLocal for `e` and
/// stash the CU in v3HookCache so future entries find it cached.
/// Returns true if populate succeeded; false if lower or compile
/// threw (in which case we just fall back without caching).
///
/// Why this exists: pre-WC-11, fallback paths (willReturnClosure /
/// sizeHeuristicSkip / IR-level closure prediction) called
/// `e->eval(...)` directly without populating v3SubExprCache.  On
/// real-world workloads the file-toplevel always falls back through
/// willReturnClosureStatic (nixpkgs files are `let ... in lambda`),
/// so v3SubExprCache stays empty and the v3 force hook never fires
/// — `forceEntries=0` across every nixpkgs trace.  By populating
/// the sub-Expr cache here, every per-thunk function in the file
/// becomes a v3 force candidate, even though the file's toplevel
/// result is delegated to tree-walker.
///
/// Trade-off: speculatively pays the lower+compile cost (~3-15 ms
/// per file in nixpkgs).  Worth it if the resulting force traffic
/// benefits more than the precompile cost.  Gated on
/// NIX_V3_NO_PRECOMPILE to A/B-test; default is ON.
static bool lowerCompileAndPopulate(
    nix::Expr * e, nix::EvalState & state, V3HookStats & st,
    bool bypassHookGate)
{
    static const bool disabled = std::getenv("NIX_V3_NO_PRECOMPILE") != nullptr;
    if (disabled) return false;
    if (!e) return false;
    // WC-11 follow-up: if the force hook is OFF, the populated cache
    // is rarely consulted enough to amortise the compile cost.
    // Skip precompile to keep default-mode perf at parity with
    // tree-walker.  When the force hook is enabled (NIX_USE_V3_FORCE),
    // precompile is what makes the 57-76% wins possible.
    //
    // #430: tested relaxing this gate to also fire on
    // `v3CallFunctionHook != nullptr` so the call hook (always wired
    // post-#426) consumes the populated cache.  Result on cardano-
    // node: ~7,301 callHookHits (up from 0) BUT a 2x wall-clock
    // regression (3.2s -> 7.0s) because the compile cost for
    // 50-200-function modules dominated the 7k-hit benefit.  Reverted.
    // The proper coverage upgrade needs lazy demand-driven precompile
    // (lambda -> root map at parse time + compile-on-first-miss in
    // the call hook) so we only pay compile cost for files whose
    // lambdas are actually called -- documented in the v3_hook.cc
    // call-hook on-miss comment block.
    //
    // #445: parse-precompile (NIX_V3_PARSE_PRECOMPILE=1) explicitly
    // opts in to compiling every parsed root.  When called from that
    // path, `bypassHookGate=true` skips this short-circuit so the
    // call hook actually receives populated entries.  Disk-cache
    // amortises the compile cost across runs (NIX_V3_DISK_CACHE).
    if (!bypassHookGate && nix::EvalState::v3ForceHook == nullptr) return false;
    auto & populatedSet = v3FallbackPopulated();
    if (populatedSet.count(e)) return true;
    auto & cache = v3HookCache();
    if (cache.find(e) != cache.end()) {
        populatedSet.insert(e);
        return true;
    }
    static const bool timingEnabled = std::getenv("V3_TIMING") != nullptr;
    using clock = std::chrono::steady_clock;
    try {
        auto t0 = timingEnabled ? clock::now() : clock::time_point{};
        // Always lower: we need the freshly-built Module to know
        // which Expr* maps to which FuncId for populateSubExprCacheLocal,
        // and Expr* identities aren't stable across runs (so a disk-
        // cache hit can't restore the AST -> FuncId mapping on its own).
        auto module = lowerNixExpr(e, state.symbols, state.positions);
        ir::optimise(module);
        ir::computeFreeVars(module);
        auto t1 = timingEnabled ? clock::now() : clock::time_point{};
        // Skip precompile of huge modules (e.g. nixpkgs/lib's 504-lambda
        // makeExtensible chain): the populated entries throw at force
        // time, so the lower+compile cost is wasted.  Note: we run
        // `compile` only on cache miss; a disk-cache hit bypasses
        // this cap because the bytecode has already proven itself
        // serializable on a prior run (where it was below the cap).
        static const size_t kMaxFunctions = []{
            if (const char * v = std::getenv("NIX_V3_PRECOMPILE_MAX_FNS"))
                return (size_t)std::atoi(v);
            return (size_t)200;
        }();
        // #447: try the disk cache before paying the compile cost.
        // Source content keying lives in the parse-time side table
        // (v3ExprPaths, populated by EvalState::v3RegisterExprHook).
        // On a hit we deserialize the CU and skip compile entirely,
        // saving ~28% of LCAP wall (compile is ~21 ms / lower 53 ms
        // in V3_TIMING runs) per cached file.
        static const bool diskCacheEnabled =
            std::getenv("NIX_V3_DISK_CACHE") != nullptr;
        std::unique_ptr<CompilationUnit> compiled;
        disk_cache::CacheKey diskKey{};
        if (diskCacheEnabled && e) {
            try {
                auto & paths = v3ExprPaths();
                auto pit = paths.find(e);
                if (pit != paths.end()) {
                    // Match parseExprFromFile's symlink behaviour
                    // (eval.cc:3803 calls .resolveSymlinks() when
                    // it reads the file content for parsing).
                    std::string srcContent =
                        pit->second.resolveSymlinks().readFile();
                    diskKey = disk_cache::computeKeyForString(srcContent);
                }
            } catch (...) { /* read failure -> empty key -> no cache */ }
        }
        if (!diskKey.empty()) {
            if (auto blob = disk_cache::lookup(diskKey)) {
                try {
                    compiled = std::make_unique<CompilationUnit>(
                        serialize::deserializeCU(*blob));
                } catch (...) { compiled.reset(); /* fall through */ }
            }
        }
        if (!compiled) {
            if (module.functions.size() > kMaxFunctions) {
                populatedSet.insert(e);
                return false;
            }
            compiled = std::make_unique<CompilationUnit>(compile(module));
            // Insert into disk cache for the next run.  Best-effort.
            if (!diskKey.empty() && serialize::isCacheable(*compiled)) {
                try {
                    disk_cache::insert(diskKey,
                        serialize::serializeCU(*compiled));
                } catch (...) { /* advisory; failures are silent */ }
            }
        }
        auto t2 = timingEnabled ? clock::now() : clock::time_point{};
        if (timingEnabled) {
            st.lowerNs   += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            st.compileNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        }
        const CompilationUnit * cu = compiled.get();
        populateSubExprCacheLocal(module, cu);
        cache.emplace(e, CachedUnit{std::move(compiled)});
        populatedSet.insert(e);
        return true;
    } catch (...) {
        populatedSet.insert(e);
        return false;
    }
}

/// The hook entry point.  Called from libnixexpr's EvalState::eval
/// when NIX_USE_V3=1 and this hook is non-null.
static void v3EvalEntry(nix::EvalState & state, nix::Expr * e, nix::Value & v)
{
    auto & st = v3HookStats();
    st.evalEntries++;
    // Register a stats-dump atexit handler on first entry.  As of
    // #453 Phase D the primOpCounter mutex is heap-allocated and
    // leaked (see primops.cc), so dumpPrimOpStats is also safe to
    // call from atexit -- gated on NIX_V3_PRIMOP_DUMP=1 because the
    // dump itself is noisy.
    static bool atexitDone = []{
        if (std::getenv("NIX_VM_STATS") || std::getenv("V3_TIMING")
            || std::getenv("NIX_V3_PRIMOP_DUMP")) {
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
                    "outerWithRefused",       // #416 static-analysis refused
                    "outerWithEnvWalkOff",    // #416 env walk fell off the end
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
                // #447: disk-cache visibility.  Print whenever the
                // process touched the SQLite cache.
                {
                    auto & ds = disk_cache::stats();
                    if (ds.lookups || ds.inserts) {
                        std::fprintf(stderr,
                            "v3 disk-cache: lookups=%llu hits=%llu misses=%llu "
                            "inserts=%llu insertFailures=%llu\n",
                            (unsigned long long)ds.lookups,
                            (unsigned long long)ds.hits,
                            (unsigned long long)ds.misses,
                            (unsigned long long)ds.inserts,
                            (unsigned long long)ds.insertFailures);
                    }
                }
                if (s.forceHookDirectUpvalues || s.forceHookRecBuildUpvalues)
                    std::fprintf(stderr,
                        "v3 force upvalues: direct=%llu recBuild=%llu hitsDirect=%llu hitsWithRec=%llu\n",
                        (unsigned long long)s.forceHookDirectUpvalues,
                        (unsigned long long)s.forceHookRecBuildUpvalues,
                        (unsigned long long)s.forceHookHitsDirectOnly,
                        (unsigned long long)s.forceHookHitsWithRecBuild);
                if (s.forceHookOuterWithBuilt
                    || s.forceHookOuterWithRefused
                    || s.forceHookOuterWithEmpty)
                    std::fprintf(stderr,
                        "v3 force outer-with: built=%llu refused=%llu trivial(empty)=%llu\n",
                        (unsigned long long)s.forceHookOuterWithBuilt,
                        (unsigned long long)s.forceHookOuterWithRefused,
                        (unsigned long long)s.forceHookOuterWithEmpty);
                // #424: selector-lambda fast-path firings.
                if (uint64_t selectorCalls =
                        allocStats().selectorLambdaCalls;
                    selectorCalls > 0)
                    std::fprintf(stderr,
                        "v3 selector-lambda: fast-path calls=%llu\n",
                        (unsigned long long)selectorCalls);
                // #436: call-hook closure-result refusals.
                if (s.callHookClosureResultRefused > 0)
                    std::fprintf(stderr,
                        "v3 call-hook: closure-result refused=%llu\n",
                        (unsigned long long)s.callHookClosureResultRefused);
                // #430: call-hook coverage funnel.  How often does
                // v3CallFunctionEntry fire and what fraction takes
                // the v3 fast-path?  Critical for sizing the
                // bytecode-stdlib opportunity.
                if (s.callHookEntries > 0) {
                    std::fprintf(stderr,
                        "v3 call-hook: entries=%llu hits=%llu gated=%llu cacheMiss=%llu bodyThrew=%llu resultBridgeFailed=%llu prepFail=%llu\n",
                        (unsigned long long)s.callHookEntries,
                        (unsigned long long)s.callHookHits,
                        (unsigned long long)s.callHookGated,
                        (unsigned long long)s.callHookCacheMiss,
                        (unsigned long long)s.callHookBodyThrew,
                        (unsigned long long)s.callHookResultBridgeFailed,
                        (unsigned long long)s.callHookPrepFail);
                    // #425: per-gate funnel.  Only print non-zero buckets
                    // to keep the output tight.
                    auto pg = [](const char * nm, uint64_t v) {
                        if (v) std::fprintf(stderr,
                            "v3 call-hook gate %s=%llu\n",
                            nm, (unsigned long long)v);
                    };
                    pg("useV3",          s.callHookGateUseV3);
                    pg("notLambda",      s.callHookGateNotLambda);
                    pg("nullLambda",     s.callHookGateNullLambda);
                    pg("formals",        s.callHookGateFormals);
                    pg("reentrant",      s.callHookGateReentrant);
                    pg("notIsLambdaEnt", s.callHookGateNotIsLambdaEnt);
                    pg("returnsClosure", s.callHookGateReturnsClosure);
                    pg("phaseBSkipped",  s.callHookGatePhaseBSkipped);
                    pg("outerWith",      s.callHookGateOuterWith);
                    // #451: on-demand-root cache-miss outcomes.
                    if (s.callHookCacheMissResolved
                        || s.callHookCacheMissNoRoot
                        || s.callHookCacheMissCompileFailed
                        || s.callHookCacheMissPostCompile) {
                        std::fprintf(stderr,
                            "v3 call-hook on-demand-root: resolved=%llu noRoot=%llu compileFailed=%llu postCompileMiss=%llu\n",
                            (unsigned long long)s.callHookCacheMissResolved,
                            (unsigned long long)s.callHookCacheMissNoRoot,
                            (unsigned long long)s.callHookCacheMissCompileFailed,
                            (unsigned long long)s.callHookCacheMissPostCompile);
                    }
                    if (s.callHookUniqueMisses > 0)
                        std::fprintf(stderr,
                            "v3 call-hook: uniqueLambdaMisses=%llu hottestMissCount=%llu (avg=%.1f calls per lambda)\n",
                            (unsigned long long)s.callHookUniqueMisses,
                            (unsigned long long)s.callHookHottestMiss,
                            s.callHookUniqueMisses
                                ? (double)s.callHookCacheMiss / s.callHookUniqueMisses
                                : 0.0);
                }
                // #453 Phase D: dump per-primop call counts so we can
                // tell which primops dominate cost / are candidates
                // for native conversion.
                if (std::getenv("NIX_V3_PRIMOP_DUMP"))
                    dumpPrimOpStats(stderr);
                // #458 step B: bridge telemetry.  Always dump (the
                // function returns early if no bridges fired).  Used
                // to localize "where does v3 still go through TW"
                // independent of call-hook traffic.  OUTSIDE the
                // callHookEntries > 0 gate above so workloads that
                // don't fire the call hook (small evals, fib-style)
                // still see bridge counts.
                dumpBridgeTelemetry(stderr);
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

    // WC-30a: short-circuits stay default-on (CO-6 / WC-7 perf opt
    // documented at +21 tests).  Removing them showed measurable
    // 2-3% regression on real workloads despite test sweeps passing.
    // Opt out via NIX_V3_NO_SHORTCIRCUIT=1 for inversion-path
    // testing.  Real inversion (WC-30b+) needs a structural change
    // that REPLACES the short-circuit work with a v3-internal fast
    // path, not just removes it.
    static const bool noShortcircuit =
        std::getenv("NIX_V3_NO_SHORTCIRCUIT") != nullptr;

    // #454 Phase E: invert eval entry.  Opt-in via NIX_V3_INVERT_EVAL=1.
    // Lets v3 own closure-producing Exprs (skip the willReturnClosure
    // short-circuit) and bridges Tag::Closure results back via
    // __v3_call_bridge_1 — implies NIX_V3_BRIDGE_CLOSURE behaviour.
    // The trivial-kind short-circuit (Int/Float/String/Path/Var/Pos)
    // and the Attrs/List short-circuit stay on; those measured net-
    // negative when run through v3's lower+compile+run path.  Phase E
    // is purposefully narrow: only the closure-result wedge moves.
    // Mirror Phase C: opt-in until we have on-real-workload perf data.
    // #454 Phase E defaults flip (2026-05-05): the safe lifts (bare-
    // Lambda short-circuit, top-level Attrs/List short-circuit, AST-
    // level willReturnClosure short-circuit) are now ON by default;
    // master gate is NIX_V3_NO_INVERT_EVAL=1 to opt out.
    //
    // The IR-level willProduceClosure lift (post-lower predicate that
    // catches Exprs whose terminal is an ir::Lambda binding) STAYS
    // opt-in via NIX_V3_LIFT_IRWPC=1 because lifting it together with
    // willReturnClosure-AST hits an infinite-recursion bug on cardano-
    // node's nixpkgs-lib `eachSystem` shape (each lift alone is fine;
    // the combo triggers a cycle).  Bisect knob: NIX_V3_NO_LIFT_<lift>
    // to disable a specific lift independently.
    static const bool invertEval =
        std::getenv("NIX_V3_NO_INVERT_EVAL") == nullptr;
    static const bool liftLambda =
        invertEval && std::getenv("NIX_V3_NO_LIFT_LAMBDA") == nullptr;
    static const bool liftAttrsList =
        invertEval && std::getenv("NIX_V3_NO_LIFT_ATTRSLIST") == nullptr;
    static const bool liftWillReturnClosure =
        invertEval && std::getenv("NIX_V3_NO_LIFT_WRC") == nullptr;
    // IRWPC opt-in only (cardano-node bug above).
    static const bool liftIRWillProduceClosure =
        invertEval
        && std::getenv("NIX_V3_LIFT_IRWPC") != nullptr
        && std::getenv("NIX_V3_NO_LIFT_IRWPC") == nullptr;

    if (e && !noShortcircuit) {
        auto k = e->exprKind;
        // #454 Phase E: under invert mode, lift the bare-Lambda
        // short-circuit too -- the closure bridge handles Tag::Closure
        // results, so a top-level `x: ...` flows through v3 like any
        // other closure-producing Expr.  The trivial constants
        // (Int/Float/String/Path/Var/Pos) stay short-circuited:
        // lower+compile+run is pure cost over TW's `mkInt(n)` /
        // `mkString(s)`.  Top-level Attrs/List also stay short-
        // circuited because v3's lower forces every entry eagerly
        // and the upvalue translation has known runtime gaps
        // ("OP_GET_UPVALUE: no closure context") for some shapes.
        bool lambdaShortCircuit = (k == nix::Expr::Kind::Lambda) && !liftLambda;
        // Top-level Attrs / List: tree-walker constructs these with
        // lazy thunks and was materially faster than v3's lower+
        // compile+run+bridge cycle, which forces every attribute
        // eagerly to produce a v3 attrset then converts back via the
        // recursive bridge.  Historically observed ~3ms of bridge
        // work per Attrs miss on hello.name -- net negative vs TW.
        // Under #454 Phase E the closure bridge changed semantics, so
        // try lifting too -- gated separately on NIX_V3_INVERT_EVAL=1.
        bool attrsListShortCircuit =
            (k == nix::Expr::Kind::Attrs || k == nix::Expr::Kind::List)
            && !liftAttrsList;
        if (lambdaShortCircuit ||
            attrsListShortCircuit ||
            k == nix::Expr::Kind::Int    ||
            k == nix::Expr::Kind::Float  ||
            k == nix::Expr::Kind::String ||
            k == nix::Expr::Kind::Path   ||
            k == nix::Expr::Kind::Var    ||
            k == nix::Expr::Kind::Pos) {
            if (diag) std::fprintf(stderr, "v3 hook: short-circuit kind=%d\n", (int)k);
            st.evalFallbackReason[4]++;
            e->eval(state, state.baseEnv, v);
            return;
        }
        // Static closure-result predicate.  v3 can't currently
        // bridge a Closure result back to tree-walker without falling
        // back anyway (see Tag::Closure case below); detecting this
        // up front saves the wasted lower+compile+run cycle.
        //
        // #454 Phase E: when NIX_V3_INVERT_EVAL=1 the closure bridge
        // takes care of Tag::Closure results, so the short-circuit
        // becomes wasted opportunity — let v3 own these and skip
        // straight through to lower+compile+run.
        if (!liftWillReturnClosure && willReturnClosure(e)) {
            if (diag) std::fprintf(stderr,
                "v3 hook: static closure result predicted, kind=%d\n", (int)k);
            st.evalFallbackReason[5]++;
            // WC-11: even though we can't run-and-bridge this Expr,
            // we can populate v3SubExprCache so per-thunk-body
            // functions inside become v3 force candidates.  Without
            // this, real-world workloads see forceEntries=0 because
            // every nixpkgs file is `let ... in lambda` and short-
            // circuits here before lower runs.
            (void)lowerCompileAndPopulate(e, state, st);
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
            ir::optimise(module);
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
            // 2026-05-06 #457/#458 T4: was empirically 50 since
            // hello.name's 74/504-function modules tended to throw
            // blackhole or return Tag::Closure -- the run+throw cycle
            // wasted lower-compile work.  But the user-directive
            // "stay in v3 VM as much as possible" inverts the
            // priority: declining at 50 is a hard exit-to-TW for
            // every nixpkgs file.  Re-measured 2026-05-06: lifting
            // the threshold costs ~3-5% on cardano-node (1.36 ->
            // 1.42s user) but no correctness regression.  Default
            // raised to effective infinity (SIZE_MAX); cap via
            // NIX_V3_SKIP_THRESHOLD if a runaway-compile workload
            // surfaces.  Use NIX_V3_SKIP_THRESHOLD=50 to restore
            // the legacy default for A/B testing.
            static const size_t kSkipThresholdFunctions = []{
                if (const char * v = std::getenv("NIX_V3_SKIP_THRESHOLD"))
                    return (size_t)std::atoi(v);
                return (size_t)SIZE_MAX;
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
            // #454 Phase E: when invert mode is active, the closure
            // bridge handles Tag::Closure results — don't skip here.
            // Otherwise (default), skip the run+bridge cycle since the
            // result must fall back to TW anyway.
            if (!liftIRWillProduceClosure && willProduceClosure()) {
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
    case Tag::PrimOp:
    case Tag::PrimOpApp: {
        // WC-14: re-enable WC-6's closure bridge.  Previously rolled
        // back due to Black-state cascades, but with WC-13 (GC roots
        // for v3 arena) and WC-14.6 (bounded depth yield) the
        // mechanics are safer.  v3ToTreeWalker wraps the closure as
        // a tree-walker PrimOpApp(__v3_call_bridge_1, handle); when
        // tree-walker calls it, primV3CallBridge1 dispatches back
        // into v3.  Gate via NIX_V3_BRIDGE_CLOSURE=1 to opt-in for
        // benchmarking; default OFF until the regression is verified
        // gone.
        // #454 Phase E (now default): the closure bridge is required
        // when v3 owns closure-producing Exprs; without it a Tag::Closure
        // result re-enters the hook on a non-closure shape and double-
        // evaluates.  Default ON; opt out via NIX_V3_NO_INVERT_EVAL=1
        // (which also disables the structural lifts above so the bridge
        // becomes unnecessary).  Legacy NIX_V3_BRIDGE_CLOSURE=1 still
        // forces it explicitly even with NO_INVERT_EVAL.
        static const bool bridgeEnabled =
            std::getenv("NIX_V3_NO_INVERT_EVAL") == nullptr
            || std::getenv("NIX_V3_BRIDGE_CLOSURE") != nullptr;
        if (!bridgeEnabled) {
            if (diag) std::fprintf(stderr,
                "v3 hook: closure-shape result tag=%d, falling back\n",
                (int)r.tag());
            st.evalFallbackReason[1]++;
            e->eval(state, state.baseEnv, v);
            return;
        }
        // WC-21: if the v3 closure expects formals (e.g. `{a, b ? def}: ...`),
        // bridging it as `PrimOpApp(__v3_call_bridge_1, handle)` strips the
        // formals — tree-walker's autoCallFunction won't fire for a primop,
        // so the call arrives with the raw arg and v3's body throws
        // OP_ATTRS_SELECT for the missing formals.  Fall back to tree-walker
        // for these; only bridge plain `x: ...` lambdas.
        if (r.tag() == Tag::Closure
            && r.payload.closure
            && r.payload.closure->desc
            && r.payload.closure->desc->hasFormals)
        {
            if (diag) std::fprintf(stderr,
                "v3 hook: closure has formals (%zu) — falling back to "
                "tree-walker so auto-args work\n",
                r.payload.closure->desc->formals.size());
            st.evalFallbackReason[1]++;
            e->eval(state, state.baseEnv, v);
            return;
        }
        // WC-20 / REVIEW §2.1: capture outer Expr so primV3CallBridge1's
        // lazy safety net can fall back to tree-walker on a deferred
        // v3-only blackhole inside the closure body.  RAII guard
        // ensures the prior outer Expr's fallback is restored on every
        // exit including throws (manual save/restore had a leak path).
        ScopedBridgeFallbackExpr fallbackGuard{const_cast<nix::Expr *>(e)};
        // #458 architectural note: this is the v3->TW closure bridge
        // -- v3ToTreeWalkerPublic wraps the v3 closure as
        // mkPrimOpApp(__v3_call_bridge_1, handle).  TW invokes via the
        // primop chain; bridge1 dispatches back into v3.  Every such
        // bridged closure that ends up called by TW is a candidate
        // for the v3-primary inversion: if we can detect "the
        // consumer is also v3" at this hook-exit point, we'd skip the
        // bridge and keep the closure as a v3 Tag::Closure.  Today the
        // consumer is opaque from here -- TW's eval of `e` returned to
        // a TW caller; we don't see whether THAT caller is itself
        // inside a v3 frame.  Cardano-node has 18+ overlay layers
        // crossing the bridge; eliminating those would close most of
        // the #455 cycle.  Tracked under #458; needs call-graph
        // awareness (thread-local v3-frame depth or similar).
        try {
            nix::Value * tmp = v3ToTreeWalkerPublic(state, r);
            if (tmp) {
                v = *tmp;
                if (diag) std::fprintf(stderr,
                    "v3 hook: bridged closure result tag=%d\n",
                    (int)r.tag());
                return;
            }
        } catch (const std::exception &) {
            // bridge fail — fall through to tree-walker
        }
        if (diag) std::fprintf(stderr,
            "v3 hook: closure bridge failed tag=%d, falling back\n",
            (int)r.tag());
        st.evalFallbackReason[1]++;
        e->eval(state, state.baseEnv, v);
        return;
    }
    case Tag::Thunk:
    case Tag::App:
    case Tag::Blackhole:
    case Tag::Uninitialized:
    case Tag::External:
    case Tag::Slot: {
        // Thunks/apps shouldn't escape v3 in normal flow (we force
        // the result at run() exit) — these are pathology cases.
        // Fall back; not worth bridging.
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
        //
        // WC-19 / REVIEW §2.1: RAII fallback-Expr guard so the lazy
        // bridge can re-run the outer Expr through tree-walker if a
        // deferred force later trips a v3-only blackhole.  Restore on
        // throw paths via destructor.
        ScopedBridgeFallbackExpr fallbackGuard{const_cast<nix::Expr *>(e)};
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
// ---------------------------------------------------------------------------
// Shared hook-entry helper: assemble v3 upvalues + capturedWiths from
// a tree-walker `env` and a populated SubExprCacheEntry.  Used by
// both v3ForceEntry (force a thunk body) and v3CallFunctionEntry
// (call a lambda body); the assembly logic is identical so we route
// through one place to keep the two hooks bug-compatible.
//
// Returns Ok on success.  On any non-Ok return, `upvalues` and
// `capturedWiths` are valid-but-undefined; the caller treats this as
// fall-back-to-tree-walker.  Stats counters are bumped at every
// success site (Direct / RecBuild / outer-with built / outer-with
// trivial-empty); refusal stats are the caller's responsibility
// since they map to caller-specific skipReturn slots.
// ---------------------------------------------------------------------------

enum class HookPrepResult : uint8_t
{
    Ok,
    NoUpvalueSources,        // entry.nUpvalues > 0 but upvalueSources empty
    EnvWalkLevelTooDeep,     // walking env.up off the chain
    EnvValueNull,            // cur->values[i] was null where required
    TwToV3ConversionThrew,   // upvalue assembly threw
    OuterWithEnvWalkOff,     // outer-with chain walked past env top
    LevelBelowEnvBase,       // #438: src.level < envBaseLevel (call-hook only)
};

/// `envBaseLevel` describes which tree-walker level the caller's `env`
/// argument represents.  Force-hook starts at the thunk's captured env
/// which IS the body's formal env (= tree-walker level 0), so
/// envBaseLevel=0.  Call-hook starts at `fun.lambda().env` which is the
/// PARENT of the formal env tree-walker would build at this call site
/// (= tree-walker level 1), so envBaseLevel=1.  This field exists to
/// fix the #438 cardano-node crash where the call hook walked one level
/// too far up the env chain and read unrelated (often uninitialized)
/// slots from the wrong env.
static HookPrepResult prepHookUpvaluesAndWiths(
    nix::Env & env,
    const SubExprCacheEntry & ent,
    std::vector<Value> & upvalues,
    ListVec *& capturedWiths,
    bool & sawRecBuild,
    bool outerWithEnabled,
    uint32_t envBaseLevel = 0)
{
    auto & st = v3HookStats();
    upvalues.clear();
    capturedWiths = nullptr;
    sawRecBuild = false;

    if (ent.nUpvalues != 0) {
        if (ent.upvalueSources.empty())
            return HookPrepResult::NoUpvalueSources;
        try {
            upvalues.reserve(ent.nUpvalues);
            for (auto & src : ent.upvalueSources) {
                // #425: LitBuiltins -- no env walk, just push the
                // singleton.  Skip the env-walk preamble entirely.
                if (src.kind == UpvalueSource::Kind::LitBuiltins) {
                    upvalues.push_back(getBuiltinsValue());
                    continue;
                }
                // #438: walk `src.level - envBaseLevel` levels.  In the
                // force hook envBaseLevel=0 so the walk is `src.level`.
                // In the call hook envBaseLevel=1 (because lambda.env is
                // already the formal env's parent), so the walk is one
                // less.  src.level < envBaseLevel means the upvalue lives
                // in an env that doesn't exist at hook entry (the formal
                // env tree-walker would build) -- refuse.
                if (src.level < envBaseLevel)
                    return HookPrepResult::LevelBelowEnvBase;
                uint32_t walkSteps = src.level - envBaseLevel;
                nix::Env * cur = &env;
                for (uint32_t i = 0; i < walkSteps; ++i) {
                    if (!cur || !cur->up)
                        return HookPrepResult::EnvWalkLevelTooDeep;
                    cur = cur->up;
                }
                if (!cur)
                    return HookPrepResult::EnvWalkLevelTooDeep;
                if (src.kind == UpvalueSource::Kind::Direct) {
                    st.forceHookDirectUpvalues++;
                    nix::Value * srcV = cur->values[src.displ];
                    if (!srcV)
                        return HookPrepResult::EnvValueNull;
                    // #438 diagnostic: refuse to wrap an uninitialized
                    // tree-walker Value -- forcing such a bridge later
                    // walks UB-territory inside `Value::type()`.
                    {
                        const uint64_t * raw =
                            reinterpret_cast<const uint64_t *>(srcV);
                        if (__builtin_expect((raw[0] & 0x7) == 0, 0))
                            [[unlikely]] {
                            static const bool s_dbg_alloc =
                                std::getenv("V3_DEBUG_ALLOC_BRIDGE") != nullptr;
                            if (s_dbg_alloc) {
                                std::fprintf(stderr,
                                    "v3 prepHookUpvalues Direct: UNINIT "
                                    "srcV=%p displ=%u level=%u\n",
                                    (void *)srcV, src.displ, src.level);
                                std::fflush(stderr);
                                std::abort();
                            }
                        }
                    }
                    // WC-25: defer the force.  Bridge thunk wraps the
                    // tree-walker Value*; OP_FORCE on the slot
                    // resolves on demand via forceBridgeThunk.
                    Thunk * bridge = Alloc::allocBridgeThunk(
                        static_cast<void *>(srcV));
                    allocStats().thunksAllocated++;
                    Value entry;
                    entry.tag_payload =
                        static_cast<uint64_t>(Tag::Thunk);
                    entry.payload.thunk = bridge;
                    upvalues.push_back(entry);
                } else {
                    st.forceHookRecBuildUpvalues++;
                    sawRecBuild = true;
                    // WC-10 (Option 1): build a v3 Bindings* whose
                    // entries are Bridge thunks (lazy bridge).  MED-17
                    // memoises by (env, names) so repeat forces of
                    // the same per-thunk function reuse the Bindings*.
                    if (!src.names || src.names->empty())
                        return HookPrepResult::NoUpvalueSources;
                    RecBuildCacheKey k{cur, src.names.get()};
                    auto & cache = recBuildCache();
                    Bindings * b;
                    // REVIEW §1.2 ABA stamp: re-read cur->values[0] on
                    // lookup, compare to the stamp captured at insert
                    // time.  If different, the Env at this address has
                    // been recycled by the GC -- treat as cache miss.
                    const void * slot0 = (cur && cur->values[0])
                        ? (const void *)cur->values[0] : nullptr;
                    auto cit = cache.find(k);
                    if (cit != cache.end() && cit->second.slot0Stamp == slot0) {
                        b = cit->second.b;
                    } else {
                        if (cit != cache.end()) cache.erase(cit);
                        const auto & names = *src.names;
                        std::vector<std::pair<SymbolId, Value>> pairs;
                        pairs.reserve(names.size());
                        for (uint32_t i = 0; i < names.size(); ++i) {
                            nix::Value * srcV = cur->values[i];
                            if (!srcV)
                                return HookPrepResult::EnvValueNull;
                            {
                                const uint64_t * raw =
                                    reinterpret_cast<const uint64_t *>(srcV);
                                if (__builtin_expect((raw[0] & 0x7) == 0, 0))
                                    [[unlikely]] {
                                    static const bool s_dbg_alloc =
                                        std::getenv("V3_DEBUG_ALLOC_BRIDGE") != nullptr;
                                    if (s_dbg_alloc) {
                                        const auto & symTab =
                                            ir::globalSymbolTable();
                                        std::fprintf(stderr,
                                            "v3 prepHookUpvalues RecBuild: "
                                            "UNINIT srcV=%p i=%u nNames=%zu cur=%p src.level=%u envStart=%p\n",
                                            (void *)srcV, i, names.size(),
                                            (void *)cur, src.level, (void *)&env);
                                        for (size_t j = 0; j < names.size(); ++j) {
                                            auto sid = names[j];
                                            std::string_view nm = sid < symTab.size()
                                                ? std::string_view(symTab[sid])
                                                : std::string_view("?");
                                            nix::Value * sv = cur->values[j];
                                            uint64_t p0 = sv ? ((const uint64_t*)sv)[0] : 0;
                                            uint64_t p1 = sv ? ((const uint64_t*)sv)[1] : 0;
                                            std::fprintf(stderr,
                                                "  names[%zu]='%.*s' slotPtr=%p p0=%016llx p1=%016llx pd=%u\n",
                                                j,
                                                (int)nm.size(), nm.data(),
                                                (void *)sv,
                                                (unsigned long long)p0,
                                                (unsigned long long)p1,
                                                (unsigned)(p0 & 0x7));
                                        }
                                        std::fflush(stderr);
                                        std::abort();
                                    }
                                }
                            }
                            Thunk * bridge = Alloc::allocBridgeThunk(
                                static_cast<void *>(srcV));
                            allocStats().thunksAllocated++;
                            Value entry;
                            entry.tag_payload =
                                static_cast<uint64_t>(Tag::Thunk);
                            entry.payload.thunk = bridge;
                            pairs.emplace_back(names[i], entry);
                        }
                        std::sort(pairs.begin(), pairs.end(),
                            [](auto & a, auto & b) {
                                return a.first < b.first;
                            });
                        b = Alloc::allocBindings(
                            static_cast<uint32_t>(pairs.size()));
                        allocStats().attrsetsAllocated++;
                        for (size_t i = 0; i < pairs.size(); ++i) {
                            b->entries[i].name  = pairs[i].first;
                            b->entries[i].value = pairs[i].second;
                        }
                        cache.emplace(k, RecBuildCacheValue{b, slot0});
                    }
                    Value v;
                    v.tag_payload = static_cast<uint64_t>(Tag::Attrs);
                    v.payload.bindings = b;
                    upvalues.push_back(v);
                }
            }
        } catch (const std::exception &) {
            return HookPrepResult::TwToV3ConversionThrew;
        }
    }

    // #416: capturedWiths from outerWithLevels.
    if (outerWithEnabled && ent.outerWithLevels.empty()) {
        st.forceHookOuterWithEmpty++;
    }
    if (outerWithEnabled && !ent.outerWithLevels.empty()) {
        st.forceHookOuterWithBuilt++;
        const auto & lv = ent.outerWithLevels;
        ListVec * out = Alloc::allocList(static_cast<uint32_t>(lv.size()));
        allocStats().listsAllocated++;
        for (size_t i = 0; i < lv.size(); ++i) {
            // #438: same envBaseLevel correction as the upvalue walks.
            if (lv[i] < envBaseLevel)
                return HookPrepResult::LevelBelowEnvBase;
            uint32_t levels = lv[i] - envBaseLevel;
            nix::Env * cur = &env;
            for (uint32_t k = 0; k < levels; ++k) {
                if (!cur || !cur->up)
                    return HookPrepResult::OuterWithEnvWalkOff;
                cur = cur->up;
            }
            if (!cur)
                return HookPrepResult::OuterWithEnvWalkOff;
            nix::Value * srcV = cur->values[0];
            if (!srcV)
                return HookPrepResult::OuterWithEnvWalkOff;
            Thunk * bridge = Alloc::allocBridgeThunk(
                static_cast<void *>(srcV));
            allocStats().thunksAllocated++;
            Value entry;
            entry.tag_payload =
                static_cast<uint64_t>(Tag::Thunk);
            entry.payload.thunk = bridge;
            out->elems[i] = entry;
        }
        capturedWiths = out;
    }

    return HookPrepResult::Ok;
}

static bool v3ForceEntry(nix::EvalState & state, nix::Expr * e,
                          nix::Env & env, nix::Value & v)
{
    // WC-25 + WC-26: force hook is now CORRECT (WC-23 cycle resolved
    // by lazy upvalue Bridge thunks; WC-26's isV3CacheCandidate
    // permanent-skip flag dropped forceEntries 421→140 on attr-hask).
    // Best-of-7 bench still shows +3..+6% regression on real
    // workloads — Bridge-thunk allocations + per-force overhead
    // exceeds v3's actual ownership benefit.  Stays opt-in via
    // NIX_USE_V3_FORCE=1 until WC-27 (native auto-args) and friends
    // increase v3's ownership share enough to amortise the cost.
    static const bool useV3Force = []{
        const char * a = std::getenv("NIX_USE_V3");
        const char * b = std::getenv("NIX_USE_V3_FORCE");
        return a && std::string_view(a) == "1"
            && b && std::string_view(b) == "1";
    }();
    if (!useV3Force) return false;

    // WC-2-followup re-entrancy guard.  The RecBuild materialisation
    // path bridges tree-walker values via treeWalkerToV3Public, which
    // calls forceValue, which can re-invoke the hook on a nested
    // thunk that ALSO needs RecBuild.  Without a re-entrancy guard
    // this ladders down the C stack until it overflows (saw a
    // SIGSEGV in nix-instantiate when a 3-drv probe's mkDerivation
    // chain hit nested rec-attrset materialisations).  Set a thread-
    // local "in hook" depth counter; bail out (return false) when
    // we're already inside the hook on this thread.  The outer hook
    // call can finish synchronously without help from inner ones.
    static thread_local int s_hookDepth = 0;
    if (s_hookDepth > 0) return false;
    struct DepthGuard {
        int & d;
        DepthGuard(int & d_) : d(d_) {
            ++d;
            // WC-14.6: also bump the cross-VM active-depth counter so
            // forceValue can detect "we're inside a v3 hook" and yield
            // before C-stack growth becomes catastrophic.
            ++nix::EvalState::v3HookActiveDepth;
        }
        ~DepthGuard() {
            --d;
            --nix::EvalState::v3HookActiveDepth;
        }
    } _guard{s_hookDepth};

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
    // REVIEW MED-12: per-thread skip set for Exprs whose subCache
    // entry has structural failure (phaseBFailed or empty
    // upvalueSources).  Pre-fix used const_cast on the AST flag,
    // which mutated state shared across EvalStates -- a different
    // EvalState with different hook config could legitimately handle
    // the Expr but saw the cleared flag and skipped.
    // Per-thread set: each thread's evaluator builds its own view;
    // mutation safe; no cross-thread or cross-state leak.  The
    // eval-inline.hh fast-path branch on isV3CacheCandidate stays
    // (still set true once at populate time), so the hot path is
    // still one branch + one hashset lookup on managed Exprs.
    static thread_local std::unordered_set<const nix::Expr *> skipSet;
    if (e && skipSet.count(e)) return skipReturn(0);
    auto skipPermanently = [&](size_t reasonIdx) {
        if (e) skipSet.insert(e);
        return skipReturn(reasonIdx);
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
    bool sawRecBuild = false;
    // #416: capturedWiths assembled from the sub-Expr's outer-with
    // chain (computed at populate time, walked at hook entry).  Null
    // when the sub-Expr has no outer-with dependency, or when the
    // outer-with feature is disabled.
    ListVec * capturedWiths = nullptr;
    // #416: outer-with carriage is ON by default after passing the
    // full v3 test sweep + targeted synthetic regressions in both
    // modes.  NIX_V3_NO_OUTER_WITH=1 acts as a kill-switch for
    // bisection.
    static const bool outerWithEnabled = []{
        return std::getenv("NIX_V3_NO_OUTER_WITH") == nullptr;
    }();
    auto & subCache = v3SubExprCache();
    auto sit = subCache.find(e);
    if (sit != subCache.end()) {
        auto & ent = sit->second;
        if (ent.isPhaseBSkipped()) {
            // WC-26: structural failure — clear the candidate flag so
            // future forces of this Expr skip the hook at the eval-
            // inline.hh:119 short-circuit check.
            return skipPermanently(0);
        }
        // #426: lambda registrations are for the call hook only.
        // Forcing an ExprLambda Value yields the closure, NOT the
        // body's result -- running the body via runFunction here
        // would read uninitialised slot 0 (the paramVar).  Decline.
        if (ent.isLambda) return false;
        if (outerWithEnabled && ent.outerWithRefused) {
            // Static analysis flagged this Expr as having an outer-with
            // dependency we cannot resolve.  Skip permanently; tree-
            // walker handles the chain natively.
            st.forceHookOuterWithRefused++;
            return skipPermanently(6);
        }
        // #426 refactor: assembly factored into prepHookUpvaluesAndWiths
        // so the call hook (v3CallFunctionEntry) shares the exact same
        // logic.  WC-26 noUpvalueSources still permanently skips here;
        // other failure modes are per-call (env walk fell off, value
        // null, conversion threw).
        switch (prepHookUpvaluesAndWiths(env, ent, upvalues, capturedWiths,
                                          sawRecBuild, outerWithEnabled)) {
        case HookPrepResult::Ok: break;
        case HookPrepResult::NoUpvalueSources:
            return skipPermanently(1);
        case HookPrepResult::EnvWalkLevelTooDeep:
            return skipReturn(2);
        case HookPrepResult::EnvValueNull:
            return skipReturn(3);
        case HookPrepResult::TwToV3ConversionThrew:
            return skipReturn(4);
        case HookPrepResult::OuterWithEnvWalkOff:
            return skipReturn(7);
        case HookPrepResult::LevelBelowEnvBase:
            // #438: only the call hook passes envBaseLevel=1; force
            // hook is envBaseLevel=0, so this can't fire here.  Treat
            // as a permanent skip just in case.
            return skipPermanently(8);
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
                upvalues.data(), static_cast<uint32_t>(upvalues.size()),
                capturedWiths);
        } else if (funcIdx == 0) {
            // Top-level entry takes the same `run` path as standalone
            // top-level eval -- it runs from offset 0 with no upvalues
            // and no captured-with carriage (capturedWiths null by
            // construction here).  If capturedWiths IS non-null we
            // must still propagate it; route through runFunction(0).
            if (capturedWiths)
                r = runFunction(*cu, funcIdx, capturedWiths);
            else
                r = run(*cu);
        } else {
            r = runFunction(*cu, funcIdx, capturedWiths);
        }
    } catch (const std::exception & ex) {
        if (diag) std::fprintf(stderr, "v3 force hook: run threw: %s\n", ex.what());
        // WC-14.6: blacklist on ALL throws (not just upvalue-bearing
        // entries).  Cycles, V3DepthYield, infinite-recursion etc.
        // are deterministic per-Expr at this env shape — retrying
        // just throws again.
        auto sit2 = v3SubExprCache().find(e);
        if (sit2 != v3SubExprCache().end()) {
            // §3 saturating: don't wrap past 255 (uint8_t).
            if (sit2->second.phaseBFailureCount < 0xFF)
                sit2->second.phaseBFailureCount++;
        }
        return false;  // Fall back: tree-walker handles the rest.
    } catch (...) {
        if (diag) std::fprintf(stderr, "v3 force hook: run threw NON-std-exception (likely BaseError-only)\n");
        auto sit2 = v3SubExprCache().find(e);
        if (sit2 != v3SubExprCache().end()) {
            // §3 saturating: don't wrap past 255 (uint8_t).
            if (sit2->second.phaseBFailureCount < 0xFF)
                sit2->second.phaseBFailureCount++;
        }
        return false;
    }
    if (diag) std::fprintf(stderr, "v3 force hook: ran ok, tag=%d\n", (int)r.tag());

    // Bridge result back to tree-walker Value.  Mirror the eval hook's
    // logic — same bridge, same fallback rules.
    switch (r.tag()) {
    case Tag::Bool:   v.mkBool(r.payload.i == 1); st.forceHits++; st.forceHitsByKind[kindIdx]++; if (sawRecBuild) st.forceHookHitsWithRecBuild++; else st.forceHookHitsDirectOnly++; return true;
    case Tag::Int:    v.mkInt(r.payload.i);       st.forceHits++; st.forceHitsByKind[kindIdx]++; if (sawRecBuild) st.forceHookHitsWithRecBuild++; else st.forceHookHitsDirectOnly++; return true;
    case Tag::Float:  v.mkFloat(r.payload.f);     st.forceHits++; st.forceHitsByKind[kindIdx]++; if (sawRecBuild) st.forceHookHitsWithRecBuild++; else st.forceHookHitsDirectOnly++; return true;
    case Tag::Null:   v.mkNull();                 st.forceHits++; st.forceHitsByKind[kindIdx]++; if (sawRecBuild) st.forceHookHitsWithRecBuild++; else st.forceHookHitsDirectOnly++; return true;
    case Tag::String:
        v.mkString(r.payload.str ? r.payload.str : "", state.mem);
        st.forceHits++; st.forceHitsByKind[kindIdx]++;
        if (sawRecBuild) st.forceHookHitsWithRecBuild++;
        else st.forceHookHitsDirectOnly++;
        return true;
    case Tag::Path:
    case Tag::Attrs:
    case Tag::List: {
        try {
            nix::Value * tmp = v3ToTreeWalkerPublic(state, r);
            if (tmp) { v = *tmp; st.forceHits++; st.forceHitsByKind[kindIdx]++; if (sawRecBuild) st.forceHookHitsWithRecBuild++; else st.forceHookHitsDirectOnly++; return true; }
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
    case Tag::Slot:
    default:
        return false;
    }
}

/// #426 / MED-21: v3 callFunction cutover hook.
///
/// Tree-walker reaches `EvalState::callFunction` with `fun` (a tree-
/// walker lambda) and `arg`.  When `fun.lambda().fun` (the ExprLambda*)
/// has been pre-lowered to v3 IR and registered in v3SubExprCache,
/// hand the call to v3:
///
///   1. Build the upvalues array from `fun.lambda().env` using the
///      same upvalueSources mechanism as v3ForceHook.
///   2. Build capturedWiths from outerWithLevels (#416 carriage).
///   3. Bridge `arg` to a v3 Value.
///   4. Call `runLambda(*cu, funcIdx, v3Arg, upvalues, capturedWiths)`.
///   5. Bridge the result back to tree-walker.
///
/// On any failure path -- cache miss, upvalue translation failure,
/// outer-with refusal, throw inside the body -- return false WITHOUT
/// mutating vRes.  The tree-walker dispatch then proceeds normally.
///
/// MVP scope: handles `fun.isLambda()` with simple-arg lambdas (no
/// formals).  Lambdas with formals (`{a, b ? def}: ...`) require the
/// callee to handle attrset destructuring inside the body, which the
/// lowerer DOES set up -- but the runLambda entry currently passes
/// the arg as the first slot raw, matching OP_CALL.  So formals
/// should "just work" since the body's prologue handles the formals
/// attrset the same way as via OP_CALL.  Verify in tests.
///
/// Opt-in via NIX_USE_V3_CALL=1 (matches the v3ForceHook discipline).
static bool v3CallFunctionEntry(nix::EvalState & state,
                                 nix::Value & fun,
                                 nix::Value * arg,
                                 nix::Value & vRes,
                                 const nix::PosIdx pos)
{
    // Gate: NIX_USE_V3 must be on (otherwise v3 isn't owning anything),
    // and the call hook is now ON by default within that.  Kill-switch
    // is NIX_V3_NO_CALL=1 (matches the #416 outer-with convention).
    // NIX_USE_V3_CALL=1 stays as a no-op alias for back-compat.
    static const bool useV3Call = []{
        const char * a = std::getenv("NIX_USE_V3");
        if (!a || std::string_view(a) != "1") return false;
        return std::getenv("NIX_V3_NO_CALL") == nullptr;
    }();
    auto & st = v3HookStats();
    st.callHookEntries++;
    if (!useV3Call) { st.callHookGated++; st.callHookGateUseV3++; return false; }
    // #457 perf: empty-subCache short-circuit FIRST (before isLambda
    // / formals checks).  When OD is off and parse-precompile is off,
    // the cache is permanently empty -- no point paying the gate cost
    // (~10ns) on every callFunction entry.  Phase D's empty-bypass
    // moved here from after the formals gate; saves ~1-2 ns per entry
    // and on hello.name (394k entries) shaves an additional ~0.4 ms
    // off the cutover overhead.
    static const bool onDemandRootEnabled =
        std::getenv("NIX_V3_ON_DEMAND_ROOT") != nullptr;
    // #458 step 2: bridge1 short-circuit.  When fun is the TW wrapper
    // of a v3 closure (`mkPrimOpApp(__v3_call_bridge_1, handle)`),
    // dispatch DIRECTLY via v3's callClosure -- bypassing TW's primop
    // layer + bridge1's eager-arg-force (the cardano-node #455 cycle
    // source).  Gate via NIX_V3_NO_BRIDGE1_SHORTCIRCUIT=1 for A/B.
    //
    // OD / PARSE_PRECOMPILE OPT-OUT: under those modes the v3 closure
    // bodies that produce bridge1 PrimOpApps recursively reference
    // mid-construction fix-point participants (cardano-node).  The
    // existing primV3CallBridge1 path has handlers (depth limit +
    // fallbackExpr re-eval through TW) that surface the underlying
    // InfiniteRecursionError; the direct shortcut evades them, turning
    // the clean error into a hang.  Keep the shortcut for default v3
    // mode (where bridge1 is rarely exercised but the shortcut is
    // safe) and bypass it under OD/PP, where the legacy primop path
    // is the better-tested fallback.
    static const bool bridge1Shortcut = []{
        if (std::getenv("NIX_V3_NO_BRIDGE1_SHORTCIRCUIT") != nullptr)
            return false;
        if (std::getenv("NIX_V3_ON_DEMAND_ROOT") != nullptr)
            return false;
        if (std::getenv("NIX_V3_PARSE_PRECOMPILE") != nullptr)
            return false;
        return true;
    }();
    if (bridge1Shortcut && fun.isPrimOpApp()) {
        if (tryDispatchBridge1Direct(state, fun, arg, vRes, pos)) {
            st.callHookHits++;
            return true;
        }
    }
    if (__builtin_expect(v3SubExprCache().empty() && !onDemandRootEnabled, 1)) {
        st.callHookCacheMiss++;
        return false;
    }
    if (!fun.isLambda()) { st.callHookGated++; st.callHookGateNotLambda++; return false; }
    nix::ExprLambda * lambda = fun.lambda().fun;
    if (!lambda) { st.callHookGated++; st.callHookGateNullLambda++; return false; }
    // #437 -> #452 Phase C -> #458 step "kill declining gates":
    // formals gate was OPT-IN until 2026-05-06 because Phase C made
    // it correct but the bridge overhead made cardano-node ~30%
    // slower.  Re-measured on 2026-05-06 after Phase D (native primop
    // coverage) and Phase E (invert eval entry) landed: cardano-node
    // is now AT PARITY with TW under formals=1 (1.38-1.42s user vs
    // TW 1.35-1.42s), and the v3 default-without-formals path is
    // ~3% slower than formals=1 because the formals gate's `gated`
    // counter dominates over `cacheMiss`.
    //
    // FLIPPED to default-on (NIX_V3_NO_CALL_FORMALS=1 to opt out).
    // Net effect: 133k formals lambdas on cardano-node move from
    // "gated, never enter v3" to "considered by the cache lookup
    // (still cacheMiss in default, but ready to fire when PP /
    // disk-cache populates the cache).
    //
    // The user-directive #457/#458 goal: stay in the v3 VM as much
    // as possible.  Refusing every formals lambda was a pure-TW
    // exit; lifting it lets v3 own the formals-lambda call site
    // even when the body falls back today.
    static const bool refuseCallFormals =
        std::getenv("NIX_V3_NO_CALL_FORMALS") != nullptr;
    if (refuseCallFormals && lambda->getFormals()) {
        st.callHookGated++; st.callHookGateFormals++; return false;
    }
    bool hasFormals = lambda->getFormals().has_value();

    // Re-entrancy guard: cap nested call-hook entries.
    //
    // 2026-05-06 #457/#458 T2: was default 0 (any nested entry
    // declines).  That was a hard "exit to TW on any nested call"
    // policy -- exactly what we're trying to eliminate.  Re-measured
    // on cardano-node default after T1 (formals gate flip): depth=8
    // is parity with depth=0 (1.36-1.48s user vs 1.36-1.43s, in
    // noise), full regression suite green.  Flipped default to 8.
    //
    // The closure-result-refused work-waste argued in the original
    // comment still applies, but T1 + T3 (closure-result refusal
    // re-investigation) is the right place to address that.  Keeping
    // depth=0 just to avoid waste-on-closure-results is a worse cure
    // than the disease (declines all nested calls vs filters which
    // ones produce closures).  Override via NIX_V3_CALL_DEPTH_LIMIT.
    static const int kCallDepthLimit = []{
        if (const char * v = std::getenv("NIX_V3_CALL_DEPTH_LIMIT"))
            return std::max(0, std::atoi(v));
        return 8;
    }();
    static thread_local int s_callDepth = 0;
    if (s_callDepth > kCallDepthLimit) { st.callHookGated++; st.callHookGateReentrant++; return false; }
    struct DepthGuard {
        int & d;
        DepthGuard(int & d_) : d(d_) { ++d; }
        ~DepthGuard() { --d; }
    } guard(s_callDepth);

    // Probe the sub-Expr cache by ExprLambda*.  We register lambdas
    // alongside thunks in v3SubExprCache (lowerLambda + this hook
    // share the same map) -- the call hook differentiates by AST kind.
    //
    // #453 Phase D fast-bypass: on real-world workloads (hello.name
    // baseline) the cache stays empty when on-demand-root is off --
    // every probe misses.  A 5.6 ms cutover regression on hello.name
    // (vs TW) collapses to ~0 when this hook returns false without
    // probing.  Skip the probe outright when the map is empty.
    // The empty() check is one inline load; the find() it replaces
    // is a hash + bucket lookup + key compare.  At 394k entries on
    // hello.name, the difference is the entire cutover overhead.
    auto & subCache = v3SubExprCache();
    // (Empty-subCache + OD-enabled gates moved to top of function in
    // #457 -- by the time we reach here the cache is non-empty OR OD
    // is on, so the second empty check would be redundant.)
    // #455 mitigation: per-lambda blacklist for lambdas where OD gave
    // up (root compiled but lambda still ineligible -- has upvalues
    // under SAFE mode, or never made it into subCache).  Without this
    // we re-pay the populatedSet probe + post-compile re-probe + upvalue
    // gate on every call, which on hello.name with OD enabled produces
    // ~250k pointless cycles costing ~38 s.  The blacklist makes those
    // cycles a single hash probe -> early return.
    auto & blacklisted = []{
        static std::unordered_set<const nix::ExprLambda *> s;
        return std::ref(s);
    }().get();
    if (__builtin_expect(blacklisted.count(lambda) != 0, 0)) {
        st.callHookCacheMiss++;
        return false;
    }
    auto sit = subCache.find(lambda);
    if (sit == subCache.end()) {
        st.callHookCacheMiss++;
        // #430: track UNIQUE lambdas missing from subCache.  If the
        // distribution is heavy-tailed (a few lambdas account for most
        // misses), on-demand precompile pays off.  If there's a long
        // tail of one-shot lambdas, lower+compile cost dominates.
        // Keyed by ExprLambda*; that's address-stable for the run.
        // Gated to keep the hot path branch-predictable when stats
        // are off.
        static const bool s_dbg =
            std::getenv("NIX_VM_STATS") != nullptr
            || std::getenv("V3_DBG_CALL_MISS") != nullptr;
        if (__builtin_expect(s_dbg, 0)) [[unlikely]] {
            static thread_local std::unordered_map<
                nix::ExprLambda *, uint64_t> missCounts;
            missCounts[lambda]++;
            // Also record into the V3HookStats for atexit dump.
            // Lazy: track the size + the top-bucket count.
            static thread_local uint64_t maxBucket = 0;
            if (missCounts[lambda] > maxBucket)
                maxBucket = missCounts[lambda];
            st.callHookUniqueMisses = missCounts.size();
            st.callHookHottestMiss  = maxBucket;
        }
        // #430 (deferred): on-demand precompile attempted here -- when
        // the call hook hits a lambda not in subCache, lower+compile
        // it directly and re-probe.  Implementation in this commit's
        // history (reverted).
        //
        // Result: fib35 went 4.13s tw / 3.20s v3 -> 0.05s with
        // on-demand precompile (50x speedup, the v3 IR runs the
        // entire fib35 in v3 with no tree-walker callbacks).  But
        // cardano-node SILENTLY produced an empty result string
        // instead of "cardano-node-exe-cardano-node-10.6.1" -- a
        // correctness regression.
        //
        // Root cause: `lowerNixExpr(lambda, ...)` in isolation drops
        // the enclosing scope's varOrigins.  v3's lowerer walks the
        // ExprLambda's body and computes freeVars against the lambda's
        // OWN scope, but the freeVar -> (level, displ) origin map
        // populated by `resolveVar` requires the enclosing
        // scope-stack the eval hook had when it lowered the
        // top-level Expr.  Without that, populateSubExprCacheLocal
        // creates SubExprCacheEntries with empty upvalueSources for
        // any freeVar coming from outside the lambda -- and the
        // call-hook then runs the v3 body with garbage upvalues,
        // producing wrong results.
        //
        // #451 / Phase B: on-demand-with-root precompile.  Implements
        // option (a) from the comment block above: at parse time we
        // recorded every (ExprLambda* -> root Expr*) in v3LambdaRoot.
        // Here, on call-hook miss, we look up the root and lower the
        // *whole file* via lowerCompileAndPopulate -- which produces
        // correct upvalueSources for every lambda in the file (the
        // eval-hook's existing pipeline).
        //
        // This is the keystone of #451: without it, real workloads
        // (cardano-node) see 10.7M cacheMiss / 0 hits in this hook
        // because the eval-hook only compiles 19 sub-Exprs per run
        // and those lambdas rarely match what's actually called.
        //
        // Root compile is one-time per root + amortised across runs
        // by the SQLite disk cache (#446 / #447).  After successful
        // root compile, we re-probe subCache and proceed normally.
        //
        // Currently OPT-IN via NIX_V3_ON_DEMAND_ROOT=1 because it
        // exposes a silent-wrong-output regression on cardano-node:
        // the root compiles successfully and the lambda's upvalue
        // sources populate, but at run time something in the
        // upvalue-walk produces an empty result string.  Likely
        // related to env shape mismatches when the lambda's runtime
        // env differs from the level-0 v3 expected at populate time
        // (e.g. import boundaries collapse levels in tree-walker
        // but not in the AST resolveVar saw).  Documented in #451;
        // the keystone speedup (3.46 s -> 0.96 s on cardano-node)
        // is real once the env-shape issue is fixed.
        static const bool onDemandRoot =
            std::getenv("NIX_V3_ON_DEMAND_ROOT") != nullptr;
        if (!onDemandRoot) return false;
        auto & roots = v3LambdaRoot();
        auto rit = roots.find(lambda);
        if (rit == roots.end()) {
            // Lambda was parsed before v3LambdaRoot was active, or
            // came from a string-eval path that doesn't go through
            // v3RegisterExprHook.  Fall back.
            st.callHookCacheMissNoRoot++;
            return false;
        }
        nix::Expr * root = rit->second;
        // Run lowerCompileAndPopulate on the root.  The bypassHookGate
        // flag matches what parse-precompile uses: forces the
        // call-hook-friendly populate even when v3ForceHook is null.
        // Re-uses the same idempotent populatedSet so a root that was
        // already compiled (e.g. via parse-precompile) short-circuits
        // in O(1).
        bool ok = false;
        try {
            // Track which subCache entries this populate writes by
            // diffing the subCache key set before/after.  The diff
            // is on-demand-root's contribution; we add it to
            // v3OnDemandRootPopulated() so the gate below can refuse
            // to run those entries when NIX_V3_NEVER_RUN_OD=1.
            auto & subCache = v3SubExprCache();
            std::unordered_set<const nix::Expr *> before;
            before.reserve(subCache.size());
            for (auto & kv : subCache) before.insert(kv.first);
            ok = lowerCompileAndPopulate(
                root, state, st, /*bypassHookGate=*/true);
            for (auto & kv : subCache) {
                if (!before.count(kv.first))
                    v3OnDemandRootPopulated().insert(kv.first);
            }
        } catch (...) { ok = false; }
        if (!ok) {
            st.callHookCacheMissCompileFailed++;
            return false;
        }
        // Re-probe.  After a successful root compile, the lambda
        // *should* be in subCache (it's a child of root).  If not,
        // the populate dropped it (e.g. kMaxFunctions cap hit, or
        // the lambda's freeVars couldn't be expressed); fall back.
        sit = subCache.find(lambda);
        if (sit == subCache.end()) {
            st.callHookCacheMissPostCompile++;
            // #455: this lambda will never be in subCache for this
            // root.  Blacklist so subsequent calls bail at the
            // empty-cache fast-path's blacklist check (~10 ns instead
            // of running OD again every time).
            blacklisted.insert(lambda);
            return false;
        }
        // #455 mitigation: env-shape mismatches between TW and v3
        // surface as `with self;` infinite-recursion on workloads
        // with rec-attrset captures (cardano-node).  Until properly
        // fixed, restrict on-demand-root to lambdas with ZERO
        // upvalues -- those have no env-walking risk because their
        // body's freeVars are empty (pure functions of their args).
        // Disable the safety net via NIX_V3_ON_DEMAND_ROOT_UNSAFE=1
        // for full coverage while debugging #455.
        static const bool onDemandRootSafe =
            std::getenv("NIX_V3_ON_DEMAND_ROOT_UNSAFE") == nullptr;
        if (onDemandRootSafe && sit->second.nUpvalues > 0) {
            st.callHookCacheMissPostCompile++;
            // #455: blacklist -- this lambda's nUpvalues won't change
            // across calls, so the SAFE-mode gate will refuse every
            // subsequent attempt.  Save the work.
            blacklisted.insert(lambda);
            return false;
        }
        // #455 diag: NIX_V3_ON_DEMAND_ROOT_POPULATE_ONLY=1 lets us
        // isolate "populate side-effects" from "lambda execution" --
        // we still run lowerCompileAndPopulate(root) but always
        // refuse to resolve.  If the cardano-node failure persists
        // with this on, the bug is in the populate's side effects
        // (isV3CacheCandidate flag, force-hook indirect routing,
        // import-triggered re-entrancy, ...) not in running v3
        // lambda bodies.
        static const bool populateOnly =
            std::getenv("NIX_V3_ON_DEMAND_ROOT_POPULATE_ONLY") != nullptr;
        if (populateOnly) {
            st.callHookCacheMissPostCompile++;
            return false;
        }
        // #455 diag: NIX_V3_ON_DEMAND_ROOT_LIMIT=N lets us bisect
        // which specific lambda (by resolution order) introduces the
        // wrong-output bug on cardano-node.  Resolves the first N
        // lambdas via on-demand-root, blocks all subsequent ones.
        // Setting N to 0 disables resolution entirely (matches
        // POPULATE_ONLY semantics for the on-demand-root path; the
        // first cache-miss path through here is what's blocked).
        static const int kResolveLimit = []{
            if (const char * v = std::getenv("NIX_V3_ON_DEMAND_ROOT_LIMIT"))
                return std::atoi(v);
            return -1;  // no limit
        }();
        if (kResolveLimit >= 0
            && st.callHookCacheMissResolved >= (uint64_t)kResolveLimit) {
            st.callHookCacheMissPostCompile++;
            return false;
        }
        st.callHookCacheMissResolved++;
        // #455 diag: dump per-resolution info to track which lambdas
        // are being unlocked + how many upvalueSources of each kind.
        // Enables post-mortem analysis when the workload fails (e.g.
        // cardano-node).  Disabled by default; opt in via env var.
        static const bool diagOnDemand =
            std::getenv("V3_DBG_ON_DEMAND_ROOT") != nullptr;
        if (diagOnDemand) {
            const auto & e2 = sit->second;
            uint32_t direct = 0, recBuild = 0, litBuiltins = 0;
            for (auto & u : e2.upvalueSources) {
                switch (u.kind) {
                case UpvalueSource::Kind::Direct:      ++direct; break;
                case UpvalueSource::Kind::RecBuild:    ++recBuild; break;
                case UpvalueSource::Kind::LitBuiltins: ++litBuiltins; break;
                }
            }
            std::fprintf(stderr,
                "v3 on-demand-root: lambda=%p funcIdx=%u nUpvalues=%u "
                "direct=%u recBuild=%u litBuiltins=%u "
                "outerWithLevels=%zu callReturnsClosure=%d\n",
                (const void *)lambda,
                (unsigned)e2.funcIdx, (unsigned)e2.nUpvalues,
                direct, recBuild, litBuiltins,
                e2.outerWithLevels.size(),
                (int)e2.callReturnsClosure);
        }
        // Fall through to the normal post-cache-hit path below.
    }
    auto & ent = sit->second;
    if (!ent.isLambda) { st.callHookGated++; st.callHookGateNotIsLambdaEnt++; return false; }
    // #455 diag: NIX_V3_ON_DEMAND_ROOT_NEVER_RUN=1 makes the call
    // hook refuse to run *any* lambda that's in v3LambdaRoot --
    // tracking whether the bug is in v3 lambda execution at all,
    // or purely in the populate's AST mutation / subCache writes.
    static const bool neverRun =
        std::getenv("NIX_V3_ON_DEMAND_ROOT_NEVER_RUN") != nullptr;
    if (neverRun && v3LambdaRoot().count(lambda)) {
        st.callHookGated++;
        return false;
    }
    // #455 diag: NIX_V3_NEVER_RUN_OD=1 only refuses lambdas that
    // were specifically populated by on-demand-root (the diff between
    // before/after subCache).  This isolates buggy populate from
    // correct eval-hook populate.  If cardano-node passes with this
    // on but fails without, the bug is exclusively in on-demand-root's
    // populated entries.
    static const bool neverRunOD =
        std::getenv("NIX_V3_NEVER_RUN_OD") != nullptr;
    if (neverRunOD && v3OnDemandRootPopulated().count(lambda)) {
        st.callHookGated++;
        return false;
    }
    // #450 / Phase A: static closure-result gate.  When the lambda's
    // body provably returns a closure, running v3 just to refuse the
    // result is wasted work.  Bail before paying the upvalue prep +
    // runLambda + bridge cost.  Closes the closure-result-refused
    // amplification when NIX_V3_CALL_DEPTH_LIMIT is raised above 0.
    if (ent.callReturnsClosure) {
        st.callHookGated++;
        st.callHookGateReturnsClosure++;
        return false;
    }
    if (ent.isPhaseBSkipped()) { st.callHookGated++; st.callHookGatePhaseBSkipped++; return false; }
    if (ent.outerWithRefused) { st.callHookGated++; st.callHookGateOuterWith++; return false; }

    // env at call entry == fun.lambda().env (the lambda's captured env).
    // upvalueSources offsets are relative to this env, NOT to the env
    // at the call site (which would be the caller's env).
    if (!fun.lambda().env) { st.callHookPrepFail++; return false; }
    nix::Env & env = *fun.lambda().env;

    // Assembly factored into prepHookUpvaluesAndWiths so the call hook
    // shares the exact same Direct / RecBuild / outer-with logic as
    // v3ForceEntry.  Any non-Ok result -> fall back to tree-walker.
    static const bool outerWithEnabled = []{
        return std::getenv("NIX_V3_NO_OUTER_WITH") == nullptr;
    }();
    std::vector<Value> upvalues;
    ListVec * capturedWiths = nullptr;
    bool sawRecBuild = false;
    // #438: call hook starts at lambda.env (= the formal env's parent
    // tree-walker would build = level 1), so pass envBaseLevel=1 to
    // compensate the env walk.
    if (prepHookUpvaluesAndWiths(env, ent, upvalues, capturedWiths,
                                  sawRecBuild, outerWithEnabled,
                                  /*envBaseLevel=*/1)
        != HookPrepResult::Ok) {
        st.callHookPrepFail++;
        return false;
    }

    // Bridge arg.  #437: previously this called
    // `treeWalkerToV3Public(state, *arg)` which DEEP-FORCES every
    // sub-attr of an attrset arg.  For NixOS-module-shaped args
    // (`{config, options, lib, ...}`) where `config` is mid-
    // construction in an outer tree-walker frame, the deep force
    // trips ExprBlackHole AND -- critically -- caches the failure
    // as `nFailed` on `config`.  Even returning false from the hook
    // doesn't undo the cache; tree-walker's subsequent access sees
    // the cached error and rethrows.
    //
    // Bridge the arg as a v3 Bridge thunk instead -- shallow / lazy.
    // The closure body forces individual sub-values only when it
    // accesses them, matching tree-walker's `callFunction`'s lazy
    // semantics.  If the body references an attr that's still being
    // computed, OP_ATTRS_SELECT's force will see the same blackhole
    // and the v3 closure body falls back via its existing catch.
    if (!arg) return false;
    Value v3Arg;

    // #458 step B (canonicalization, "reduce TW reliance"): scalar
    // fast-path for already-forced TW arg values.  Every TW arg used
    // to be wrapped as a Bridge thunk; v3 forces the thunk on first
    // access via forceBridgeThunk -> treeWalkerToV3Public, which
    // allocates a VMState and calls treeWalkerToV3.  For ints/floats/
    // bools/null the result is just a direct field copy -- the heavy
    // path is pure overhead.
    //
    // Detect "already-forced scalar" by inspecting the TW Value's
    // discriminator (no force call: arg->type() returns the cached
    // nInt/nFloat/etc tag).  If scalar, inline-bridge directly to a
    // v3::Value -- ZERO heap alloc, ZERO future TW callback.
    //
    // Composite types (attrs/list/function/external) still take the
    // Bridge thunk path -- bridging them eagerly would change
    // laziness semantics.  And tThunk-discriminated values fall through
    // because we can't peek inside without a force.
    // Use the shared scalar-fast-path helper (declared in primop.hh).
    // Composite types (string/path/attrs/list/function) keep the
    // Bridge thunk path to preserve laziness; thunks fall through
    // because we can't peek inside without forcing.
    bool fastBridged = tryFastBridgeScalarTwToV3(*arg, v3Arg);

    if (!fastBridged) {
        Thunk * argBridge = Alloc::allocBridgeThunk(static_cast<void *>(arg));
        allocStats().thunksAllocated++;
        v3Arg.tag_payload = static_cast<uint64_t>(Tag::Thunk);
        v3Arg.payload.thunk = argBridge;
    }

    // Run the body.  #455: hold ScopedEagerBridge for the WHOLE
    // duration of runLambda + result bridge, but ONLY for lambdas
    // populated by on-demand-root (which are the ones at risk of
    // capturing a mid-evaluation TW slot via the Bridge thunk arg).
    // Eval-hook-populated lambdas don't have this risk and shouldn't
    // pay the eager-bridge perf cost (which on hello.name + PP is
    // ~2x because every entry of every returned attrset gets
    // recursively bridged eagerly).
    //
    // The bridge guard, when held, bypasses the size-based eager/lazy
    // threshold in v3ToTreeWalker; every nested attrset / list bridge
    // during the call-hook scope is eager, so no PrimOpApp re-entry
    // can form.  Disable via NIX_V3_NO_CALL_HOOK_EAGER=1 for A/B.
    bool isOnDemandRoot = v3OnDemandRootPopulated().count(lambda) > 0;
    static const bool callHookEager =
        std::getenv("NIX_V3_NO_CALL_HOOK_EAGER") == nullptr;
    setNixEvalState(&state);
    Value r;
    // Optional RAII guard; held for runLambda + result bridge.
    std::optional<ScopedEagerBridge> eagerGuard;
    if (callHookEager && isOnDemandRoot) eagerGuard.emplace();
    // #452 / Phase C: shallow TW-attrs bridge for formals lambdas.
    // The body's first force of the param attrset converts each
    // entry to a Bridge thunk, not a deep v3 value; only entries
    // the body references get force-converted on demand.
    //
    // REVIEW §3: scope the shallow guard to JUST runLambda.  The
    // shallow flag affects treeWalkerToV3 (TW->v3 direction) which
    // only fires while the v3 body forces its arg.  The result-bridge
    // phase below (v3->TW) is the opposite direction and isn't
    // affected by the flag, so holding it through the result phase
    // was lifetime-overbroad.  Tighter scope reduces the chance of an
    // accidental TW->v3 conversion outside the body inheriting
    // shallow-bridge semantics.
    try {
        std::optional<ScopedShallowTWAttrsBridge> shallowGuard;
        if (hasFormals) shallowGuard.emplace();
        r = runLambda(*ent.cu, ent.funcIdx, v3Arg,
            upvalues.data(), static_cast<uint32_t>(upvalues.size()),
            capturedWiths);
        // shallowGuard's destructor pops the flag here, before any
        // result-bridge work below.
    } catch (const std::exception &) {
        // Any throw -> blacklist this lambda for the rest of the
        // process, fall back.  Mirrors v3ForceEntry's WC-14.6 policy.
        // §3 saturating: don't wrap past 255 (uint8_t).
        if (ent.phaseBFailureCount < 0xFF) ent.phaseBFailureCount++;
        st.callHookBodyThrew++;
        return false;
    } catch (...) {
        if (ent.phaseBFailureCount < 0xFF) ent.phaseBFailureCount++;
        st.callHookBodyThrew++;
        return false;
    }

    // #436: refuse closure-shape results to avoid the
    // `__v3_call_bridge_1` partial-application leak.  When the v3
    // body returns a Tag::Closure / Tag::PrimOp / Tag::PrimOpApp /
    // Tag::Thunk / Tag::App / Tag::Blackhole, v3ToTreeWalkerPublic
    // would bridge it as a tree-walker mkPrimOpApp(__v3_call_bridge_1,
    // handle) -- a partially-applied 2-arity primop pretending to be
    // a function value.  That works when the consumer is callFunction
    // again, but explodes when the consumer is `evalBool` or
    // similar coerce-to-X (cardano-node hits this on
    // `assert enableGold -> withGold stdenv.targetPlatform`).
    //
    // 2026-05-06 attempt: tried removing the refusal to reduce
    // v3->tw->v3 bridging, since step 2's shortcut was meant to keep
    // closure dispatches v3-side.  Result: #455 minimal-repro
    // POSITIVE regression test failed under
    // ON_DEMAND_ROOT+SKIP_THRESHOLD=0 -- the eager-bridge guard was
    // load-bearing for that case.  Restored the refusal.  Use
    // NIX_V3_NO_REFUSE_CLOSURE_RESULT=1 to opt out for experimentation.
    {
        static const bool noRefuse =
            std::getenv("NIX_V3_NO_REFUSE_CLOSURE_RESULT") != nullptr;
        if (!noRefuse) {
            Tag rt = r.tag();
            if (rt == Tag::Closure || rt == Tag::PrimOp || rt == Tag::PrimOpApp
                || rt == Tag::Thunk || rt == Tag::App || rt == Tag::Blackhole)
            {
                v3HookStats().callHookClosureResultRefused++;
                static const bool diagCall =
                    std::getenv("V3_DEBUG_CALL_RESULT") != nullptr;
                if (diagCall) {
                    std::fprintf(stderr,
                        "v3 call-hook closure-shape result tag=%d lambda=%p\n",
                        (int)rt, (void *)lambda);
                }
                return false;
            }
        }
    }

    // Bridge result.  #455: when this lambda was populated by
    // on-demand-root, force eager bridging.  The lazy-bridge path
    // (PrimOpApp(__v3_force_attr, ...)) creates cycles when v3
    // thunks captured the call-hook arg's Bridge thunk pointing
    // back at the let-rec slot the result is being assigned to.
    // Eager bridging converts each entry to a real TW value during
    // the bridge so no PrimOpApp re-entry occurs.  Lazy bridging is
    // preserved for non-on-demand-root paths (e.g. import primop
    // results, eval-hook nested attrsets) where the cycle isn't
    // possible.
    // The eagerGuard (set above for runLambda) is still in scope
    // here, so v3ToTreeWalkerPublic also runs eager.
    nix::Value * tmp = nullptr;
    try {
        tmp = v3ToTreeWalkerPublic(state, r);
    } catch (const std::exception &) {
        st.callHookResultBridgeFailed++;
        return false;
    }
    if (!tmp) { st.callHookResultBridgeFailed++; return false; }
    vRes = *tmp;
    st.callHookHits++;
    (void)pos; // currently unused; could decorate trace messages later.
    return true;
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
        // WC-11: forceValue hook stays opt-in via NIX_USE_V3_FORCE=1
        // for now.  Per-thunk overhead is one branch on
        // isV3CacheCandidate (false for ~99.9% of forced thunks),
        // and on simple workloads (hello-name, attr-pkgs) it yields
        // a ~57% wall-clock win.  But it currently SIGSEGVs on
        // derivationStrict-heavy workloads (drv3 — pkgs.{hello,git,vim}.drvPath)
        // because the force hook + WC-10 Bridge thunks interact in a
        // way that overflows the stack across the v3<->tree-walker
        // boundary.  Until that is root-caused, default OFF.
        // WC-25 + WC-26: force hook now correct but stays opt-in via
        // NIX_USE_V3_FORCE=1 — flipping default-on regresses real
        // workloads 3-6% (Bridge-thunk allocation overhead exceeds
        // v3's ownership benefit until WC-27+ widen ownership).
        if (const char * v = std::getenv("NIX_USE_V3_FORCE");
            v && std::string_view(v) == "1") {
            nix::EvalState::v3ForceHook = &v3ForceEntry;
        }
        // #426 / MED-21: install the call-function hook unconditionally
        // (its internal NIX_USE_V3 / NIX_V3_NO_CALL gate decides whether
        // it actually fires).  Always-installed pointer keeps the
        // tree-walker fast-path shape unchanged when v3 is off.
        nix::EvalState::v3CallFunctionHook = &v3CallFunctionEntry;
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
    // WC-25 + WC-26: force hook now correct but stays opt-in via
    // NIX_USE_V3_FORCE=1 (3-6% regression on real workloads when
    // default-on; future WC-27+ should amortise).
    if (const char * v = std::getenv("NIX_USE_V3_FORCE");
        v && std::string_view(v) == "1") {
        nix::EvalState::v3ForceHook = &v3ForceEntry;
    }
    // #426: install callFunction hook (gated internally by
    // NIX_USE_V3 + NIX_V3_NO_CALL).
    nix::EvalState::v3CallFunctionHook = &v3CallFunctionEntry;
}
} // namespace nix::v3
