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

#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Helpers — chase a VarId to its defining Expr inside one block.
// ---------------------------------------------------------------------------

/// Map VarId -> Expr* for every binding in `block`.  We store pointers
/// into the live block's vector; the pass never resizes that vector,
/// so the pointers stay valid for the duration of the visit.
using BlockMap = std::unordered_map<VarId, const Expr *>;

static BlockMap mapBlock(const Block & block)
{
    BlockMap m;
    m.reserve(block.bindings.size());
    for (const auto & b : block.bindings)
        m.emplace(b.var, &b.expr);
    return m;
}

/// Resolve `v` through any chain of `VarRef` bindings *within the same
/// block*.  Returns the underlying Expr*, or nullptr if the var is
/// defined in an outer scope (we only fold when both operands resolve
/// inside this block).  Bounded by the map size to avoid pathological
/// cycles -- v3 IR is acyclic by construction, but defence in depth.
static const Expr * resolve(VarId v, const BlockMap & m)
{
    size_t hops = 0;
    const auto cap = m.size() + 1;
    while (hops++ < cap) {
        auto it = m.find(v);
        if (it == m.end()) return nullptr;
        const Expr * e = it->second;
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
        if (std::isnan(da) || std::isnan(db)) return false; // tree-walker behaviour: throws
        lt = da < db;
    }
    out = LitBool{lt};
    return true;
}

// --- Per-binding fold ------------------------------------------------------

static bool tryFold(const Expr & in, const BlockMap & m, Expr & out)
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
        // Float division does not throw; produce the IEEE-754 result.
        double da = (a.kind == Lit::K_Float) ? a.f : (double)a.i;
        double db = (b.kind == Lit::K_Float) ? b.f : (double)b.i;
        // Mirror tree-walker: float / 0.0 yields ±inf (no throw) per IEEE.
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
    // blocks[0] is the kInvalidBlock sentinel and has no bindings.
    for (BlockId i = 1; i < (BlockId)m.blocks.size(); ++i) {
        Block & blk = m.blocks[i];
        if (blk.bindings.empty()) continue;

        // Build the lookup map after each successful fold so resolve()
        // can chase through freshly-created literal bindings.  Cheap:
        // O(N) per block, and folds typically cluster.
        BlockMap map = mapBlock(blk);

        for (auto & bind : blk.bindings) {
            Expr replacement;
            if (tryFold(bind.expr, map, replacement)) {
                bind.expr = std::move(replacement);
                // Update the map entry to point at the new RHS so
                // downstream bindings in the same block see the literal.
                map[bind.var] = &bind.expr;
                ++folded;
            }
        }
    }
    return folded;
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

    constantFold(m);
}

} // namespace nix::v3::ir
