/// @file
/// IR optimisation pass: pure primop constant folding (Phase B).
///
/// Recognises specific `PrimOpCall(p, [args])` shapes whose RESULT is
/// statically computable AT IR LEVEL and rewrites them to a literal /
/// VarRef.  Complements opt_const_fold.cc (which only folds the
/// arithmetic / comparison IR nodes like Add / Sub / Eq, NOT primop
/// dispatch through OP_CALL_PRIMOP).
///
/// Patterns folded (Phase B subset — conservative; pure-data only):
///
///   1. `length [a b c]`               → LitInt(3)              (ListExpr shape)
///   2. `length (x ++ y)`              → length(x) + length(y)  if both ListExpr
///   3. `stringLength "abc"`           → LitInt(3)
///   4. `head [a b c]`                 → VarRef(a)
///   5. `tail [a b c]`                 → ListExpr([b, c])
///   6. `elemAt [a b c] LitInt(N)`     → VarRef(elements[N])
///   7. `toString LitInt(N)`           → LitString(repr(N))
///   8. `toString LitBool(true|false)` → LitString("1" | "")  (Nix semantics)
///   9. `attrNames {a=...; b=...}`     → ListExpr of LitStrings  (sorted)
///
/// EXCLUDED (intentionally — would change observable behavior):
///   - Anything that may throw (e.g. `head []`, `elemAt xs (negative)`)
///   - Primops with string-context (toString of a derivation)
///   - Primops with effect (import, fetchurl, pathExists, readFile)
///   - Floating-point conversions (NaN / Inf representation differs by
///     platform; defer until needed)
///
/// All folded shapes are observably IDENTICAL to runtime evaluation,
/// so this pass cannot introduce semantic divergence.
///
/// Gate: NIX_V3_NO_PRIMOP_FOLD=1 disables for A/B measurement.
///
/// Runs AFTER fusePrimOpApps (so we see PrimOpCall shapes for
/// `length`, `head`, etc. that the lower.cc lowering exposed) and
/// AFTER inlineTrivialBindings (so the operand VarRef chains are
/// path-compressed).  See opt_const_fold.cc::optimise pipeline.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/ir.hh"
#include "v3/primop.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nix::v3::ir {

namespace {

// ---------------------------------------------------------------------------
// Same-block VarRef chase — local to this block; returns the underlying
// defining Expr or nullptr if v is bound outside.  Same pattern as
// opt_beta_reduce.cc and opt_const_fold.cc.
// ---------------------------------------------------------------------------

const Expr * chaseInBlock(VarId v,
                          const std::unordered_map<VarId, const Expr *> & defs)
{
    size_t hops = 0;
    while (hops++ < defs.size() + 1) {
        auto it = defs.find(v);
        if (it == defs.end()) return nullptr;
        const Expr * e = it->second;
        if (const auto * vr = std::get_if<VarRef>(e)) {
            v = vr->var;
            continue;
        }
        return e;
    }
    return nullptr;
}

std::unordered_map<VarId, const Expr *> mapBlockDefs(const Block & b)
{
    std::unordered_map<VarId, const Expr *> defs;
    defs.reserve(b.bindings.size());
    for (const auto & bd : b.bindings)
        defs.emplace(bd.var, &bd.expr);
    return defs;
}

// ---------------------------------------------------------------------------
// Per-primop fold rules.  Each rule:
//   - is called when PrimOpCall(p, args) matches p->name
//   - inspects args' resolved Exprs (via chaseInBlock)
//   - returns std::nullopt to skip, or an Expr to substitute
// ---------------------------------------------------------------------------

// Helper: resolve a VarId arg to its Expr (or nullptr).
const Expr * arg(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    return chaseInBlock(v, defs);
}

// Helper: extract a ListExpr from a possibly VarRef-aliased arg.
const ListExpr * asListExpr(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<ListExpr>(e) : nullptr;
}

const LitInt * asLitInt(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<LitInt>(e) : nullptr;
}

const LitFloat * asLitFloat(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<LitFloat>(e) : nullptr;
}

const LitString * asLitString(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<LitString>(e) : nullptr;
}

const LitBool * asLitBool(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<LitBool>(e) : nullptr;
}

const ConcatLists * asConcatLists(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<ConcatLists>(e) : nullptr;
}

const AttrSet * asAttrSet(VarId v, const std::unordered_map<VarId, const Expr *> & defs)
{
    const Expr * e = arg(v, defs);
    return e ? std::get_if<AttrSet>(e) : nullptr;
}

// Try to fold a single PrimOpCall.  Returns std::optional<Expr>; the
// `replacement` Module owns any new ListExpr storage that returned
// Exprs may reference.  Returns std::nullopt if no rule applies.
//
// Rules may need to allocate new VarIds (e.g. attrNames returning a
// list of fresh LitStrings).  They receive the Module by reference.
std::optional<Expr> tryFoldPrimOpCall(
    Module & m,
    const PrimOpCall & call,
    const std::unordered_map<VarId, const Expr *> & defs,
    std::vector<Binding> & emit)
{
    if (!call.primop) return std::nullopt;
    std::string_view name = call.primop->name;

    // ----- arithmetic / comparison over constant operands -----
    // `2 * 3`, `i + 1`, `n < m`, … lower to PrimOpCall("__mul"/"__add"/
    // "__lessThan"/…) — NOT to the ir::Add/Mul/Less nodes (which the
    // lowerer never constructs; constantFold's matchers for them are dead
    // for operator syntax).  Fold the primop form here when BOTH operands
    // are literal numbers, mirroring opt_const_fold.cc's foldNumeric /
    // foldLess semantics + guards EXACTLY:
    //   - integer +/-/* that OVERFLOW are not folded (runtime wraps/errs);
    //   - integer / by 0 and INT64_MIN / -1 are not folded (runtime throws);
    //   - float / by 0 is not folded (#683 — TW throws);
    //   - NaN comparisons are not folded (TW throws on float <).
    // Non-constant operands leave the PrimOpCall intact for the runtime.
    if ((name == "__add" || name == "__sub" || name == "__mul"
         || name == "__div" || name == "__lessThan")
        && call.args.size() == 2) {
        const auto * ai = asLitInt(call.args[0], defs);
        const auto * bi = asLitInt(call.args[1], defs);
        const auto * af = asLitFloat(call.args[0], defs);
        const auto * bf = asLitFloat(call.args[1], defs);
        const bool aNum = ai || af, bNum = bi || bf;
        if (!aNum || !bNum) return std::nullopt;
        const bool bothInt = ai && bi;
        const double da = ai ? static_cast<double>(ai->value) : af->value;
        const double db = bi ? static_cast<double>(bi->value) : bf->value;
        if (name == "__lessThan") {
            if (bothInt) return LitBool{ai->value < bi->value};
            if (std::isnan(da) || std::isnan(db)) return std::nullopt;
            return LitBool{da < db};
        }
        if (name == "__add") {
            if (bothInt) { int64_t r;
                if (__builtin_add_overflow(ai->value, bi->value, &r)) return std::nullopt;
                return LitInt{r}; }
            return LitFloat{da + db};
        }
        if (name == "__sub") {
            if (bothInt) { int64_t r;
                if (__builtin_sub_overflow(ai->value, bi->value, &r)) return std::nullopt;
                return LitInt{r}; }
            return LitFloat{da - db};
        }
        if (name == "__mul") {
            if (bothInt) { int64_t r;
                if (__builtin_mul_overflow(ai->value, bi->value, &r)) return std::nullopt;
                return LitInt{r}; }
            return LitFloat{da * db};
        }
        // __div
        if (bothInt) {
            if (bi->value == 0) return std::nullopt;
            if (ai->value == std::numeric_limits<int64_t>::min() && bi->value == -1)
                return std::nullopt;
            return LitInt{ai->value / bi->value};
        }
        if (db == 0.0) return std::nullopt;
        return LitFloat{da / db};
    }

    // ----- length -----
    if (name == "length" || name == "__length") {
        if (call.args.size() != 1) return std::nullopt;
        if (const auto * lst = asListExpr(call.args[0], defs))
            return LitInt{static_cast<int64_t>(lst->elems.size())};
        // Recursive: length(a ++ b) = length(a) + length(b), each
        // foldable.  Walk a single layer.
        if (const auto * cc = asConcatLists(call.args[0], defs)) {
            const auto * la = asListExpr(cc->lhs, defs);
            const auto * lb = asListExpr(cc->rhs, defs);
            if (la && lb)
                return LitInt{static_cast<int64_t>(la->elems.size()
                                                    + lb->elems.size())};
        }
        return std::nullopt;
    }

    // ----- stringLength -----
    if (name == "stringLength" || name == "__stringLength") {
        if (call.args.size() != 1) return std::nullopt;
        if (const auto * s = asLitString(call.args[0], defs))
            return LitInt{static_cast<int64_t>(s->value.size())};
        return std::nullopt;
    }

    // ----- head -----
    if (name == "head" || name == "__head") {
        if (call.args.size() != 1) return std::nullopt;
        if (const auto * lst = asListExpr(call.args[0], defs)) {
            // Empty list would throw at runtime; PRESERVE that — skip
            // the fold so the runtime error fires.
            if (lst->elems.empty()) return std::nullopt;
            return VarRef{lst->elems[0]};
        }
        return std::nullopt;
    }

    // ----- tail -----
    if (name == "tail" || name == "__tail") {
        if (call.args.size() != 1) return std::nullopt;
        if (const auto * lst = asListExpr(call.args[0], defs)) {
            if (lst->elems.empty()) return std::nullopt;
            std::vector<VarId> rest(lst->elems.begin() + 1, lst->elems.end());
            return ListExpr{std::move(rest)};
        }
        return std::nullopt;
    }

    // ----- elemAt -----
    if (name == "elemAt" || name == "__elemAt") {
        if (call.args.size() != 2) return std::nullopt;
        const auto * lst = asListExpr(call.args[0], defs);
        const auto * idx = asLitInt(call.args[1], defs);
        if (!lst || !idx) return std::nullopt;
        if (idx->value < 0 || (size_t)idx->value >= lst->elems.size())
            return std::nullopt;  // runtime would throw; preserve
        return VarRef{lst->elems[(size_t)idx->value]};
    }

    // ----- toString (literal Int/Bool only — String/Float deferred) -----
    if (name == "toString" || name == "__toString") {
        if (call.args.size() != 1) return std::nullopt;
        if (const auto * n = asLitInt(call.args[0], defs)) {
            // Allocate the string in the Module's owned-string pool
            // so the LitString.value (a string_view) outlives the
            // pass.  Modules intern symbols but not arbitrary strings
            // — use a simple per-Module string-storage vector.  We
            // reuse the symbol table here (interning a number's
            // string repr) because symbols ARE stable string_views
            // owned by the global table.  Mild abuse but
            // semantically fine.
            std::string s = std::to_string(n->value);
            SymbolId sid = m.internSymbol(s);
            return LitString{globalSymbolTable()[sid]};
        }
        if (const auto * b = asLitBool(call.args[0], defs)) {
            // Nix's toString-on-bool: true → "1", false → "" (yes,
            // really; see TW's primToString).
            return LitString{std::string_view(b->value ? "1" : "")};
        }
        // Literal strings: toString s = s (passthrough).
        if (const auto * s = asLitString(call.args[0], defs))
            return LitString{s->value};
        return std::nullopt;
    }

    // ----- attrNames -----
    if (name == "attrNames" || name == "__attrNames") {
        if (call.args.size() != 1) return std::nullopt;
        const auto * as = asAttrSet(call.args[0], defs);
        if (!as) return std::nullopt;
        // `builtins.attrNames` returns names sorted ALPHABETICALLY
        // (per Nix spec), NOT by interned SymbolId.  AttrSet::entries
        // is sorted by SymbolId (interning order), which is NOT the
        // alphabetical order users expect.  Sort the names explicitly.
        //
        // Skip inherit-from entries — their `value` field is invalid
        // (the binding is sourced from another attrset via OP_INHERIT_
        // FROM) and we don't need the value here anyway, just the name.
        std::vector<std::string_view> sorted_names;
        sorted_names.reserve(as->entries.size());
        for (const auto & ent : as->entries)
            sorted_names.push_back(globalSymbolTable()[ent.name]);
        std::sort(sorted_names.begin(), sorted_names.end());
        // Deduplicate just in case the same symbol appears twice
        // (shouldn't happen in well-formed IR, but defensive).
        sorted_names.erase(
            std::unique(sorted_names.begin(), sorted_names.end()),
            sorted_names.end());

        std::vector<VarId> elems;
        elems.reserve(sorted_names.size());
        for (auto nm : sorted_names) {
            VarId nameVar = m.freshVar();
            emit.push_back({nameVar, LitString{nm}});
            elems.push_back(nameVar);
        }
        return ListExpr{std::move(elems)};
    }

    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry: primOpFold.
// ---------------------------------------------------------------------------

size_t primOpFold(Module & m)
{
    static const bool s_disabled =
        std::getenv("NIX_V3_NO_PRIMOP_FOLD") != nullptr;
    if (s_disabled) return 0;

    size_t folded = 0;

    for (BlockId bid = 1; bid < (BlockId)m.blocks.size(); ++bid) {
        Block & blk = m.blocks[bid];
        auto defs = mapBlockDefs(blk);

        std::vector<Binding> out;
        out.reserve(blk.bindings.size());

        for (const auto & bd : blk.bindings) {
            const auto * call = std::get_if<PrimOpCall>(&bd.expr);
            if (!call) { out.push_back(bd); continue; }

            // Emit-side bindings produced by `attrNames` (etc.) need
            // to land BEFORE the folded result binding so that the
            // VarIds in the resulting ListExpr are in scope.  Collect
            // them in `emit`, then append before our replacement.
            std::vector<Binding> emit;
            auto replacement = tryFoldPrimOpCall(m, *call, defs, emit);
            if (!replacement) { out.push_back(bd); continue; }

            for (auto & e : emit) out.push_back(std::move(e));
            out.push_back({bd.var, std::move(*replacement)});
            ++folded;
        }

        if (folded > 0)
            blk.bindings = std::move(out);
    }

    return folded;
}

} // namespace nix::v3::ir
