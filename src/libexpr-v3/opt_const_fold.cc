/// @file
/// IR optimisation pass: constant folding.
///
/// Walks every Block in the Module and rewrites bindings whose RHS is a
/// pure operation (Add/Sub/Mul/Div/Eq/NEq/Less/Not/Negate-via-Sub) over
/// operands that all resolve, via VarRef chains within the same Block,
/// to literal LitInt/LitFloat/LitBool nodes.
///
/// We are deliberately conservative:
///   - Only fold when the operation cannot throw at runtime.  Div-by-zero,
///     INT64_MIN / -1, and integer overflow on Add/Sub/Mul are *not*
///     folded; the original IR is preserved so the runtime emits the
///     same exception tree-walker would.
///   - Only chase VarRef references inside the same Block.  Cross-block
///     dataflow is the job of later passes (DCE/inlining/CSE).
///   - We do *not* fold ConcatStrings (string contexts need careful
///     handling — separate pass) and we do *not* fold attrset/list ops.
///
/// The pass is idempotent: a single linear walk per block.  Convergence
/// in the same block is achieved by walking forward and looking up
/// already-folded bindings as we encounter VarRefs (the binding we
/// rewrote earlier is now a literal).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/ir_scratch.hh"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Helpers — chase a VarId to its defining Expr inside one block.
// ---------------------------------------------------------------------------

/// Resolve `v` through any chain of `VarRef` bindings *within the same
/// block*.  Returns the underlying Expr*, or nullptr if the var is
/// defined in an outer scope (we only fold when both operands resolve
/// inside this block).  Bounded by the map size to avoid pathological
/// cycles -- v3 IR is acyclic by construction, but defence in depth.
///
/// #766: uses FlatBlockMap (sorted-vector scratch) instead of a
/// per-call std::unordered_map to skip per-node malloc/free that
/// profile data (#765) showed dominant in the IR pipeline.
static const Expr * resolve(VarId v, const FlatBlockMap & m)
{
    size_t hops = 0;
    const auto cap = m.size() + 1;
    while (hops++ < cap) {
        const Expr * e = m.find(v);
        if (!e) return nullptr;
        if (auto * vr = std::get_if<VarRef>(e)) {
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

// --- Literal extraction ----------------------------------------------------

struct Lit {
    enum Kind { K_Int, K_Float, K_Bool } kind;
    int64_t i = 0;
    double  f = 0.0;
    bool    b = false;
};

static bool extractLit(const Expr * e, Lit & out)
{
    if (!e) return false;
    if (auto * x = std::get_if<LitInt>(e))   { out.kind = Lit::K_Int;   out.i = x->value; return true; }
    if (auto * x = std::get_if<LitFloat>(e)) { out.kind = Lit::K_Float; out.f = x->value; return true; }
    if (auto * x = std::get_if<LitBool>(e))  { out.kind = Lit::K_Bool;  out.b = x->value; return true; }
    return false;
}

// --- Arithmetic folding (preserve VM throw semantics) ----------------------

/// Fold `a OP b` where OP is one of +, -, *.  Returns true iff the
/// fold succeeded and `out` was populated.  Returns false (no
/// rewrite) when the runtime would throw (integer overflow) -- the
/// original IR keeps the throw in place.
template <typename IntOp, typename FloatOp>
static bool foldNumeric(const Lit & a, const Lit & b, Expr & out, IntOp intOp, FloatOp floatOp)
{
    if (a.kind == Lit::K_Bool || b.kind == Lit::K_Bool) return false;
    if (a.kind == Lit::K_Int && b.kind == Lit::K_Int) {
        int64_t r;
        if (!intOp(a.i, b.i, r)) return false; // overflow -> let runtime throw
        out = LitInt{r};
        return true;
    }
    double da = (a.kind == Lit::K_Float) ? a.f : (double)a.i;
    double db = (b.kind == Lit::K_Float) ? b.f : (double)b.i;
    double r = floatOp(da, db);
    if (std::isnan(r) && !std::isnan(da) && !std::isnan(db)) {
        // Operation produced fresh NaN (e.g. 0.0/0.0); skip so the runtime
        // observes the same NaN propagation tree-walker produces.  Float
        // ops in v3 don't throw, but we still avoid synthesising NaNs to
        // keep behaviour bit-identical.
        return false;
    }
    out = LitFloat{r};
    return true;
}

// --- Comparison folding ----------------------------------------------------

static bool foldEq(const Lit & a, const Lit & b, bool wantEq, Expr & out)
{
    bool eq = false;
    // Bool == Bool: only equal types compare equal in tree-walker for
    // primitives (int vs bool always !=).  Mixed int/float compare
    // numerically.
    if (a.kind == Lit::K_Bool && b.kind == Lit::K_Bool) {
        eq = (a.b == b.b);
    } else if (a.kind == Lit::K_Bool || b.kind == Lit::K_Bool) {
        eq = false;
    } else if (a.kind == Lit::K_Int && b.kind == Lit::K_Int) {
        eq = (a.i == b.i);
    } else {
        double da = (a.kind == Lit::K_Float) ? a.f : (double)a.i;
        double db = (b.kind == Lit::K_Float) ? b.f : (double)b.i;
        // NaN != NaN per IEEE-754 -- tree-walker matches this.  Don't
        // fold a constant NaN compare to "true" by accident.
        if (std::isnan(da) || std::isnan(db)) eq = false;
        else eq = (da == db);
    }
    out = LitBool{wantEq ? eq : !eq};
    return true;
}

static bool foldLess(const Lit & a, const Lit & b, Expr & out)
{
    if (a.kind == Lit::K_Bool || b.kind == Lit::K_Bool) return false;
    bool lt;
    if (a.kind == Lit::K_Int && b.kind == Lit::K_Int) {
        lt = a.i < b.i;
    } else {
        double da = (a.kind == Lit::K_Float) ? a.f : (double)a.i;
        double db = (b.kind == Lit::K_Float) ? b.f : (double)b.i;
        // CODEBASE_REVIEW_2026-06-11 §10: refuse to const-fold a NaN compare
        // (defer to runtime).  The prior comment claimed "TW throws" — it does
        // NOT: TW's CompareValues does an IEEE `<` which yields false for any
        // NaN operand, and v3's runtime `<` matches.  Refusing to fold is the
        // conservative choice (the runtime produces the same false either way).
        if (std::isnan(da) || std::isnan(db)) return false;
        lt = da < db;
    }
    out = LitBool{lt};
    return true;
}

// --- Per-binding fold ------------------------------------------------------

static bool tryFold(const Expr & in, const FlatBlockMap & m, Expr & out)
{
    auto getLit = [&](VarId v, Lit & lit) {
        return extractLit(resolve(v, m), lit);
    };

    if (auto * op = std::get_if<Add>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldNumeric(a, b, out,
            [](int64_t x, int64_t y, int64_t & r) { return !__builtin_add_overflow(x, y, &r); },
            [](double x, double y) { return x + y; });
    }
    if (auto * op = std::get_if<Sub>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldNumeric(a, b, out,
            [](int64_t x, int64_t y, int64_t & r) { return !__builtin_sub_overflow(x, y, &r); },
            [](double x, double y) { return x - y; });
    }
    if (auto * op = std::get_if<Mul>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldNumeric(a, b, out,
            [](int64_t x, int64_t y, int64_t & r) { return !__builtin_mul_overflow(x, y, &r); },
            [](double x, double y) { return x * y; });
    }
    if (auto * op = std::get_if<Div>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        if (a.kind == Lit::K_Bool || b.kind == Lit::K_Bool) return false;
        // Int / Int: must NOT fold div-by-zero or INT64_MIN/-1 -- both
        // throw at runtime; folding would silently change the program's
        // behaviour from "raises" to "yields a constant".
        if (a.kind == Lit::K_Int && b.kind == Lit::K_Int) {
            if (b.i == 0) return false;
            if (a.i == std::numeric_limits<int64_t>::min() && b.i == -1) return false;
            out = LitInt{a.i / b.i};
            return true;
        }
        // #683 — TW also throws on float div-by-zero
        // (libexpr/primops.cc:4703 `if (f2 == 0) division by zero`,
        // unconditional on int/float).  Pre-fix this comment claimed
        // otherwise and folded to ±inf — silently bypassing the
        // runtime check.  Refuse to fold when denominator is 0 in
        // either form so the runtime path raises matching TW.
        double da = (a.kind == Lit::K_Float) ? a.f : (double)a.i;
        double db = (b.kind == Lit::K_Float) ? b.f : (double)b.i;
        if (db == 0.0) return false;
        out = LitFloat{da / db};
        return true;
    }
    if (auto * op = std::get_if<Eq>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldEq(a, b, /*wantEq=*/true, out);
    }
    if (auto * op = std::get_if<NEq>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldEq(a, b, /*wantEq=*/false, out);
    }
    if (auto * op = std::get_if<Less>(&in)) {
        Lit a, b;
        if (!getLit(op->lhs, a) || !getLit(op->rhs, b)) return false;
        return foldLess(a, b, out);
    }
    if (auto * op = std::get_if<Not>(&in)) {
        Lit a;
        if (!getLit(op->operand, a)) return false;
        if (a.kind != Lit::K_Bool) return false;
        out = LitBool{!a.b};
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: constantFold
// ---------------------------------------------------------------------------

size_t constantFold(Module & m)
{
    size_t folded = 0;
    auto & map = FlatBlockMap::scratch();
    // blocks[0] is the kInvalidBlock sentinel and has no bindings.
    for (BlockId i = 1; i < (BlockId)m.blocks.size(); ++i) {
        Block & blk = m.blocks[i];
        if (blk.bindings.empty()) continue;

        // Build the lookup map once per block.  Each `bind.expr =`
        // mutation below updates the Expr in place (same address as
        // the map already records) — no map rebuild required for
        // downstream bindings in the same block to see the new RHS.
        map.rebuild(blk);

        for (auto & bind : blk.bindings) {
            Expr replacement;
            if (tryFold(bind.expr, map, replacement)) {
                bind.expr = std::move(replacement);
                ++folded;
            }
        }
    }
    return folded;
}

// ---------------------------------------------------------------------------
// Dead-function elimination (2026-06-05).
//
// After constant-folding / spine-inlining rewrites a PrimOpCall/App to a
// literal, the MkThunk/Lambda binding that fed it becomes dead and is swept
// by deadBindingElim — but the FUNCTION it pointed at lingers in m.functions
// with its now-orphaned body (e.g. `length [1 2 3]` folds to LitInt 3, yet
// the list thunk's ListExpr body survives in an unreferenced function).
//
// This pass clears the entry block of every function unreachable from the
// program entry (f0).  It deliberately does NOT renumber funcIdx — that would
// have to track Lambda + MkThunk + LetRec Entry/HiddenEntry thunkBodies AND
// the VarOrigin/RecVarOrigin/SubExprEntry side-tables, and a single miss
// corrupts the IR.  Instead every funcIdx stays valid, pointing at a now-
// trivial `return null` body; emit produces a tiny dead stub.  Reachability
// scans the in-block funcIdx carriers (Lambda, MkThunk, LetRec entry/hidden
// thunkBodies), following If-branch blocks (Terminal is always TermReturn).
//
// Safety: if reachability ever UNDER-approximates (marks a live function
// dead) the clear would break evaluation — caught immediately by the --core
// byte-identity suite.  The carrier set above is exhaustive for in-block
// references, so this is sound.
size_t deadFunctionElim(Module & m)
{
    const size_t nf = m.functions.size();
    if (nf == 0) return 0;
    std::vector<bool> reachable(nf, false);
    std::vector<bool> seenBlock(m.blocks.size(), false);
    std::vector<FuncId> work;
    auto markFunc = [&](FuncId f) {
        if (f < nf && !reachable[f]) { reachable[f] = true; work.push_back(f); }
    };
    markFunc(0);  // the entry function is always live
    while (!work.empty()) {
        FuncId f = work.back(); work.pop_back();
        std::vector<BlockId> bw;
        if (m.functions[f].entryBlock != kInvalidBlock)
            bw.push_back(m.functions[f].entryBlock);
        while (!bw.empty()) {
            BlockId b = bw.back(); bw.pop_back();
            if (b == kInvalidBlock || b >= (BlockId)m.blocks.size() || seenBlock[b])
                continue;
            seenBlock[b] = true;
            for (const auto & bd : m.blocks[b].bindings) {
                std::visit([&](const auto & e) {
                    using T = std::decay_t<decltype(e)>;
                    // funcIdx carriers — mark the referenced function live.
                    if constexpr (std::is_same_v<T, Lambda>)
                        markFunc(e.funcIdx);
                    else if constexpr (std::is_same_v<T, MkThunk>)
                        markFunc(e.funcIdx);
                    else if constexpr (std::is_same_v<T, LetRec>) {
                        for (const auto & en : e.entries)       markFunc(en.thunkBody);
                        for (const auto & he : e.hiddenEntries) markFunc(he.thunkBody);
                    }
                    // BlockId carriers — follow the sub-block (it may itself
                    // contain funcIdx carriers).  Terminal is always
                    // TermReturn, so these Exprs are the ONLY intra-function
                    // block edges: If, With, Assert, And/Or/Impl.
                    else if constexpr (std::is_same_v<T, If>) {
                        bw.push_back(e.thenBlock);
                        bw.push_back(e.elseBlock);
                    }
                    else if constexpr (std::is_same_v<T, With>)
                        bw.push_back(e.bodyBlock);
                    else if constexpr (std::is_same_v<T, Assert>)
                        bw.push_back(e.bodyBlock);
                    else if constexpr (std::is_same_v<T, And>)
                        bw.push_back(e.rhsBlock);
                    else if constexpr (std::is_same_v<T, Or>)
                        bw.push_back(e.rhsBlock);
                    else if constexpr (std::is_same_v<T, Impl>)
                        bw.push_back(e.rhsBlock);
                }, bd.expr);
            }
        }
    }
    size_t cleared = 0;
    for (FuncId f = 0; f < (FuncId)nf; ++f) {
        if (reachable[f]) continue;
        BlockId eb = m.functions[f].entryBlock;
        if (eb == kInvalidBlock || eb >= (BlockId)m.blocks.size()) continue;
        Block & blk = m.blocks[eb];
        if (blk.bindings.empty()) continue;  // already trivial
        VarId nullVar = m.freshVar();
        blk.bindings.clear();
        blk.bindings.push_back(Binding{ nullVar, LitNull{} });
        blk.terminal = TermReturn{ nullVar };
        ++cleared;
    }
    return cleared;
}

// ---------------------------------------------------------------------------
// Public entry: optimise (pipeline driver)
// ---------------------------------------------------------------------------

void optimise(Module & m)
{
    // Escape hatch: NIX_V3_NO_OPT=1 disables every optimisation pass so
    // a regression can be bisected to "v3 IR opt pipeline" vs "v3 core
    // pipeline" without rebuilding.
    static const bool disabled = std::getenv("NIX_V3_NO_OPT") != nullptr;
    if (disabled) return;

    // #815 RCA hook: NIX_V3_OPT_PHASE_LIMIT=N runs only the first N
    // passes in this function (0 = no passes, 1 = constantFold only,
    // 2 = +betaReduce, etc.).  Used to bisect non-deterministic opt
    // output cross-process.
    static const int s_phaseLimit = [](){
        const char * e = std::getenv("NIX_V3_OPT_PHASE_LIMIT");
        return e ? std::atoi(e) : -1;
    }();
    int phase = 0;
    auto checkPhase = [&]() -> bool {
        if (s_phaseLimit < 0) return true;
        return phase++ < s_phaseLimit;
    };
    #define OPT_RUN(call) do { if (!checkPhase()) return; call; } while (0)

    OPT_RUN(constantFold(m));
    // 2026-05-18 IR Phase A: beta-reduce App(VarRef→Lambda, arg)
    // patterns before constantFold runs again — inlining frequently
    // surfaces new literal-arithmetic shapes that constantFold can
    // collapse.  Runs BEFORE CSE so inlined bindings get the CSE
    // pass too (the inlined body may duplicate existing bindings
    // in the enclosing block).
    OPT_RUN(betaReduce(m));
    // Re-run constantFold to pick up the literal-arithmetic exposed
    // by beta-reduction (e.g. `(x: x + 1) 5` → `5 + 1` → `6`).
    OPT_RUN(constantFold(m));
    OPT_RUN(commonSubexprElim(m));
    // #423 runs after CSE (so duplicate Force operands collapse to a
    // single resolved root via the alias map) and before
    // inlineTrivialBindings (so the freshly-introduced VarRef aliases
    // get path-compressed in the same pipeline).
    OPT_RUN(elimRedundantForce(m));
    OPT_RUN(inlineTrivialBindings(m));
    // #429 runs after alias collapse (so VarRef chains are flattened
    // and LitPrimOp -> App pairings are observable in one block) and
    // before DCE (so the partial-App orphans get swept).
    OPT_RUN(fusePrimOpApps(m));

    // Sweep fusePrimOpApps's dead partial-App residue BEFORE de-thunk so the
    // de-thunk use-once check sees accurate counts (the residue `App(__mul,
    // t)` is a second, dead reference to the arg-thunk `t`).
    OPT_RUN(deadBindingElim(m));
    // De-thunk use-once MkThunk args to strict arithmetic primops.  Runs
    // AFTER fusePrimOpApps (so `__mul`/… are canonical PrimOpCalls whose
    // thunked args are visible) and BEFORE primOpFold / appSpineFold (so the
    // exposed inner arithmetic — `x*y*z`'s inner `x*y` thunk — folds and the
    // arity-N body becomes thunk-free for spine folding).
    OPT_RUN(deThunkForcedStrictArgs(m));

    // 2026-05-18 IR Phase B: pure-primop constant folding.  Runs
    // AFTER fusePrimOpApps so we see the canonical PrimOpCall shape
    // (rather than the App-chain that lower.cc emits for indirect
    // primop calls like `let map = builtins.map; in map f xs`).  Then
    // we re-run constantFold + inlineTrivialBindings to propagate the
    // folded literals + collapse the VarRef aliases primOpFold
    // introduces (e.g. `head [a b c]` → VarRef(a)).
    OPT_RUN(primOpFold(m));
    OPT_RUN(constantFold(m));
    OPT_RUN(inlineTrivialBindings(m));

    // 2026-08-04: the IR Phase-C stream-fusion pass (opt_stream_fusion.cc,
    // `foldl'(op,init,map(f,xs))` → `__foldlMap`) was RETIRED.  It was
    // FALSIFIED as a perf lever (measured 2026-06-05: NEUTRAL-to-regressing —
    // the intermediate-eliminating rewrite costs more than the cheap C-built
    // genList spine) and had been default-OFF (opt-in `NIX_V3_STREAM_FUSION`)
    // ever since, so it was a dead 802-line no-op in every real eval.  The
    // falsified-candidate registry it documented lives in git history.  The
    // `__foldlMap` primop it targeted has now been removed too (it was an
    // unproduced orphan — no pass emitted it after the fusion pass went).

    // RETAINED DIAGNOSTIC (detection-only, no rewrite): count the
    // `foldl' (acc: x: acc ++ G) [] xs` O(n²) accumulation idiom.  Gated
    // entirely by V3_DBG_FOLDL_APPEND — zero cost (not even called) when
    // unset; the getenv read is cached static (lint-no-inline-getenv only
    // scans vm.cc/primops.cc, but we follow the pattern anyway).
    //
    // MEASURED 2026-06-07: 0 occurrences on hello/firefox/git/python3.drvPath
    // AND lib.unique.  An optimiser rewrite to `concatLists (map (x: G) xs)`
    // can only match a *fully-saturated inline* `builtins.foldl' (λ) [] xs`;
    // real nixpkgs always goes through `lib.foldl'` / partial application
    // (e.g. `unique = foldl' step []`), so the step lambda is opaque at every
    // call site and the rewrite would fire ~never — not worth the IR-surgery
    // risk.  The bytecode primops that DID use this O(n²) shape
    // (filter/concatMap/partition/sort) were fixed directly instead.
    //
    // KEPT so the frequency can be re-measured on future / user workloads
    // (point V3_DBG_FOLDL_APPEND=1 at a suspected target).  RETIREMENT: drop
    // this probe + detectFoldlAppendIdiom if the general in-place-append
    // ("Lever B": O(1)-amortised `++` on a uniquely-owned growable ListVec)
    // ever lands — that subsumes the idiom regardless of call shape, making
    // the detector moot.
    {
        static const bool s_probe = std::getenv("V3_DBG_FOLDL_APPEND") != nullptr;
        if (s_probe) detectFoldlAppendIdiom(m);
    }

    // 2026-05-18 IR Phase G: pure if-then-else folding.  Recognises
    // `If(LitBool, then, else)` patterns and inlines the chosen
    // branch.  Runs AFTER Phase B's constantFold loop so any
    // condition exposed by primOpFold (e.g. `if builtins.length [] == 0
    // then a else b`) has resolved to a literal.  Then re-run
    // constantFold + inlineTrivialBindings to propagate the inlined
    // VarRef chain through the surrounding bindings.
    if (!checkPhase()) return;
    if (ifThenFold(m)) {
        OPT_RUN(constantFold(m));
        OPT_RUN(inlineTrivialBindings(m));
    }

    // 2026-05-18 IR Phase H: static genList unrolling.  Recognises
    // `PrimOpCall("genList", [f, n])` where n is a known small
    // literal (≤ 8) and expands to an N-element ListExpr of
    // per-element MkThunks.  Runs after Phase B so that
    // `builtins.length [a b c d]`-style n-args fold to LitInt before
    // we examine them.  No follow-up cleanup pass — the unrolled
    // form is itself in canonical IR shape.
    OPT_RUN(genListUnroll(m));

    // 2026-05-18 IR Phase F: static App-spine folding.  Collapses
    // curried multi-arg chains (`(x: y: z: x+y+z) 1 2 3` → `1+2+3`)
    // in a single rewrite, eliminating intermediate PartialApp
    // allocations.  Runs AFTER Phase A's single-step beta-reduce so
    // any 1-arg inlinings that Phase A already handled don't bloat
    // the spines this pass walks.  If folds fired, re-run the
    // cleanup chain (elimRedundantForce removes the lowerer's
    // Force-on-param-access wrappers that the cloned body inherits;
    // then constantFold can see arithmetic on literals; then
    // inlineTrivialBindings collapses the VarRef aliases).
    if (!checkPhase()) return;
    if (appSpineFold(m)) {
        OPT_RUN(elimRedundantForce(m));
        // primOpFold folds the literal arithmetic the inlined body exposes
        // (`(x: y: x*y) 6 7` → `__mul [6,7]` → `LitInt 42`).  The historical
        // cleanup relied on constantFold here, but operator syntax lowers to
        // PrimOpCall("__mul"/…) — which constantFold's ir::Mul matcher never
        // sees — so the fold lives in primOpFold (opt_primop_fold.cc).
        OPT_RUN(primOpFold(m));
        OPT_RUN(constantFold(m));
        OPT_RUN(inlineTrivialBindings(m));
    }

    // OPT_OCCUR Phase B: opt-in occurrence-info-driven DCE.  When both
    // gates are set, run side-by-side and assert identical removal
    // sets (the migration-validation harness).  When only NIX_V3_OCCUR_DCE
    // is set, use the new variant.  Default: old deadBindingElim.
    //
    // gate: NIX_V3_OCCUR_DCE — opt-in to deadBindingElimViaOccur.
    // Retire after one release of NIX_V3_OCCUR_DCE_VALIDATE clean
    // runs across the functional test suite (Phase B exit criterion).
    //
    // gate: NIX_V3_OCCUR_DCE_VALIDATE — side-by-side migration
    // harness.  Retire alongside NIX_V3_OCCUR_DCE.
    static const bool occurDce =
        std::getenv("NIX_V3_OCCUR_DCE") != nullptr;
    static const bool occurDceValidate =
        std::getenv("NIX_V3_OCCUR_DCE_VALIDATE") != nullptr;

    if (!checkPhase()) return;
    if (__builtin_expect(occurDceValidate, 0)) {
        // Snapshot pre-DCE binding set: a vector of (block, var) pairs
        // gives us an O(1) per-binding "did this survive" check.
        std::unordered_set<uint64_t> beforeKey;
        auto key = [](BlockId b, VarId v) -> uint64_t {
            return (uint64_t)b << 32 | (uint64_t)v;
        };
        for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid)
            for (const auto & bd : m.blocks[bid].bindings)
                beforeKey.insert(key(bid, bd.var));

        // Path A: old DCE on a clone-by-copy of the bindings only
        // (cheap — no need to clone the whole Module since DCE only
        // touches Block::bindings).  Capture the survivors.
        std::vector<std::vector<Binding>> savedBindings;
        savedBindings.reserve(m.blocks.size());
        for (const auto & b : m.blocks) savedBindings.push_back(b.bindings);
        size_t removedOld = deadBindingElim(m);
        std::unordered_set<uint64_t> oldSurvivors;
        for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid)
            for (const auto & bd : m.blocks[bid].bindings)
                oldSurvivors.insert(key(bid, bd.var));

        // Restore, then run new DCE.
        for (BlockId bid = 0; bid < (BlockId)m.blocks.size(); ++bid)
            m.blocks[bid].bindings = std::move(savedBindings[bid]);
        size_t removedNew = deadBindingElimViaOccur(m);

        // Compare survivors.
        std::unordered_set<uint64_t> newSurvivors;
        for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid)
            for (const auto & bd : m.blocks[bid].bindings)
                newSurvivors.insert(key(bid, bd.var));

        if (oldSurvivors != newSurvivors || removedOld != removedNew) {
            std::fprintf(stderr,
                "v3 OPT_OCCUR_DCE_VALIDATE MISMATCH: "
                "old removed=%zu new removed=%zu old.survivors=%zu "
                "new.survivors=%zu\n",
                removedOld, removedNew,
                oldSurvivors.size(), newSurvivors.size());
        }
        // If we got here, the new DCE result is the module's current
        // state — that's what we keep going.
    } else if (occurDce) {
        deadBindingElimViaOccur(m);
    } else {
        deadBindingElim(m);
    }

    // Sweep the bodies of functions left unreachable by the folds/inlines
    // above (e.g. `length [1 2 3]`'s now-orphaned list thunk).  Runs AFTER
    // deadBindingElim so the dead MkThunk/Lambda bindings are already gone,
    // leaving their functions unreferenced.
    if (!checkPhase()) return;
    deadFunctionElim(m);

    // #774 (2026-05-23): Stage 4 strictness analysis + call-site
    // elision pass.  Gated by NIX_V3_STAGE4_ALL_MODULES=1.
    //
    // Default OFF after falsification (see commit body):
    //   * 781 656 Apps analyzed across 281 modules on hello.drvPath
    //   * 11 870 resolved (1.5 % resolveCalleeLambda hit rate)
    //   * 2 338 strict-hits (0.3 %)
    //   * **1 elision** out of 40 961 considered Apps
    //   * Wall-clock cost: ~33 ms slower on hello.drvPath (1.407 s
    //     vs 1.374 s, n=10 hyperfine)
    //
    // The bottleneck isn't strictness analysis depth — it's
    // `isInlinableMkThunk`'s constraints (single-use binding +
    // simple cloneable body).  Even with 100 % pipeline coverage
    // (this commit's change) the elision rate stays at 0.0024 %.
    // The pass remains opt-in for future investigation; future
    // v4.4 work needs a different elision shape (relaxed
    // single-use, larger body templates) before this is worth
    // running on every module.
    //
    // The OUTER expression's Stage 4 still runs from run.cc as
    // before (the wall-clock cost there is ~zero — outer Apps
    // count is small).
    if (!checkPhase()) return;
    static const bool stage4AllModules =
        std::getenv("NIX_V3_STAGE4_ALL_MODULES") != nullptr;
    if (__builtin_expect(stage4AllModules, 0)) {
        computeFunctionStrictness(m);
        for (int it = 0; it < 8; ++it) {
            if (applyStrictnessAtCallSites(m) == 0) break;
        }
    }
    #undef OPT_RUN
}

} // namespace nix::v3::ir
