#pragma once
/// @file
/// Native v3-AST → IR lowering (Stage 2, PARSER_PROJECT_PLAN §2 /
/// NATIVE_LOWERING_PLAN_2026-06-01.md).  Lowers the v3 AST directly to
/// `ir::Module`, eliminating the `nix::Expr` bridge.
///
/// Built incrementally with a whole-program `canLowerV3` gate + bridge
/// fallback: only fully-supported trees take this path; everything else
/// uses the proven bridge.  Every phase is therefore COMPLETE (all
/// programs eval) and eval-validated (143/143 lang + drvPath on the
/// supported subset).  Validation gate is EVAL-PARITY (not IR-byte
/// equality — VarId numbering legitimately differs from lower.cc).
///
/// PHASE 1 (this file): the thunkify-free, scope-free subset — literals,
/// base-env var refs (primop / true / false / null / builtins), primop
/// App-chains with trivial args, `if`, the strict binops (==,!=,//,++),
/// `!`, string concat/interp, `assert`.  Deferred to later phases:
/// lambdas/let/closures (2), attrsets/select/with (3), inheritFrom +
/// #495 intrinsics (4).  `canLowerV3` rejects anything not yet handled.
///
/// Mirrors lower.cc's emission patterns (run/addBinding/forceVal/the
/// base-env var path) reading the v3 AST instead of nix::Expr.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"
#include "v3/ir.hh"
#include "v3/primop.hh"
#include "v3/alloc.hh"            // recordPosSnapshot
#include "nix/util/pos-table.hh"  // PosTable + Pos::Origin (+ SourcePath via position.hh)

// nix::SymbolTable is used only by reference (the SymbolTable& member +
// the lowerV3Ast param), so a forward declaration suffices — keeps this
// header self-sufficient regardless of include order (it formerly relied
// on the now-deleted v3/lower.hh for this forward-decl).
namespace nix { class SymbolTable; }

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// The Kind-dispatch switches below intentionally use `default:` for the
// internal/never-in-source kinds (Unknown/InheritFrom/BlackHole) and the
// not-yet-native kinds.  Silence -Wswitch-enum (we keep -Wswitch) so this
// header is self-sufficient when included outside the parser TU (e.g. the
// native import path in primops.cc, which doesn't pull in the bison
// header that previously suppressed this project-wide).  Balanced pop at
// end of file.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"

namespace nix::v3 {

inline bool canLowerV3(const nix::v3::ast::Node * n);

/// Attrset / let bindings are lowerable iff every value lowers.
/// Shared by the Let + Attrs gates.  Plain + Inherited + InheritedFrom
/// (4a/4b) + dynamic keys (4c).
inline bool canLowerAttrDefs(const nix::v3::ast::Attrs * at)
{
    namespace a = nix::v3::ast;
    for (auto * e : at->inheritFromExprs)
        if (!canLowerV3(e)) return false;
    for (auto & d : at->attrs)
        if (d.kind == a::Attrs::AttrKind::Plain && !canLowerV3(d.value)) return false;
    for (auto & da : at->dynamicAttrs)            // 4c: `${e} = v;`
        if (!canLowerV3(da.nameExpr) || !canLowerV3(da.valueExpr)) return false;
    return true;
}

/// Whole-program gate: true iff `n`'s entire subtree uses only
/// natively-supported constructs (so the native lowerer handles it; else
/// the caller falls back to the bridge).  Conservative by construction.
inline bool canLowerV3(const nix::v3::ast::Node * n)
{
    namespace a = nix::v3::ast;
    if (!n) return false;
    switch (n->kind) {
    case a::Kind::Int:
    case a::Kind::Float:
    case a::Kind::String:
    case a::Kind::Path:   // parser pre-resolved the path string
    case a::Kind::Pos:    // __curPos (resolved from the node's offset)
    case a::Kind::Var:
        return true;
    case a::Kind::Lambda: {
        // Phase 2a single-arg + Phase 2c formals (defaults may reference
        // sibling formals; all default exprs + the body must lower).
        auto * lam = static_cast<const a::Lambda *>(n);
        if (lam->hasFormals)
            for (auto & f : lam->formals)
                if (f.def && !canLowerV3(f.def)) return false;
        return canLowerV3(lam->body);
    }
    case a::Kind::Let: {
        // Phase 2b: plain bindings only (no inherit / inherit-from /
        // dynamic keys → bridge those).
        auto * let = static_cast<const a::Let *>(n);
        return canLowerAttrDefs(let->attrs) && canLowerV3(let->body);
    }
    case a::Kind::Attrs:
        // Phase 3a: rec + non-rec, plain bindings only.
        return canLowerAttrDefs(static_cast<const a::Attrs *>(n));
    case a::Kind::Select: {
        auto * s = static_cast<const a::Select *>(n);
        if (!canLowerV3(s->e)) return false;
        if (s->def && !canLowerV3(s->def)) return false;
        for (auto & p : s->path)
            if (p.expr && !canLowerV3(p.expr)) return false;  // dynamic ${} key
        return true;
    }
    case a::Kind::OpHasAttr: {
        auto * h = static_cast<const a::OpHasAttr *>(n);
        if (!canLowerV3(h->e)) return false;
        for (auto & p : h->path)
            if (p.expr && !canLowerV3(p.expr)) return false;
        return true;
    }
    case a::Kind::With: {
        auto * w = static_cast<const a::With *>(n);
        return canLowerV3(w->attrs) && canLowerV3(w->body);
    }
    case a::Kind::Call: {
        // Args are thunked for laziness (lowerCall::thunkifyForAttr), so
        // any lowerable arg is fine — no triviality restriction.
        auto * c = static_cast<const a::Call *>(n);
        if (!canLowerV3(c->fun)) return false;
        for (auto * arg : c->args) if (!canLowerV3(arg)) return false;
        return true;
    }
    case a::Kind::If: {
        auto * i = static_cast<const a::If *>(n);
        return canLowerV3(i->cond) && canLowerV3(i->then_) && canLowerV3(i->else_);
    }
    case a::Kind::OpEq: case a::Kind::OpNEq:
    case a::Kind::OpUpdate: case a::Kind::OpConcatLists:
    case a::Kind::OpAnd: case a::Kind::OpOr: case a::Kind::OpImpl: {
        auto * b = static_cast<const a::BinOp *>(n);
        return canLowerV3(b->lhs) && canLowerV3(b->rhs);
    }
    case a::Kind::List: {
        auto * l = static_cast<const a::List *>(n);
        for (auto * el : l->elems) if (!canLowerV3(el)) return false;
        return true;
    }
    case a::Kind::OpNot:
        return canLowerV3(static_cast<const a::OpNot *>(n)->e);
    case a::Kind::ConcatStrings: {
        auto * cs = static_cast<const a::ConcatStrings *>(n);
        for (auto * e : cs->es) if (!canLowerV3(e)) return false;
        return true;
    }
    case a::Kind::Assert: {
        auto * as = static_cast<const a::Assert *>(n);
        return canLowerV3(as->cond) && canLowerV3(as->body);
    }
    default:
        return false;  // InheritFrom/BlackHole + dynamic keys (4c) + #495 (4d) -> bridge
    }
}

struct LowererV3 {
    ir::Module m = ir::makeModule();
    const nix::SymbolTable & symbols;  // unused (names are inline in the v3 AST)
    nix::PosTable * positions = nullptr;            // for attr/formal positions
    std::optional<nix::PosTable::Origin> posOrigin; // the source's registered origin
    /// TW's bare base-env global names (twBaseEnvGlobals).  A free name
    /// resolves to a primop ONLY if it is in this set — else it falls
    /// through to the `with`-chain, matching TW's bindVars (so nixpkgs'
    /// bare `fetchurl` becomes the with-bound FOD, NOT builtins.fetchurl).
    /// nullptr → unfiltered (every registered primop counts; used only by
    /// callers that don't have a TW EvalState — kept for back-compat).
    const std::set<std::string> * baseEnvNames = nullptr;
    std::vector<ir::BlockId> blockStack;

    /// Is `name` a TW base-env primop (resolvable as a bare global)?
    /// Mirrors lower.cc's reliance on bindVars: a name is a base-env
    /// primop iff it's a registered primop AND (when the TW base-env set
    /// is known) one of TW's actual bare globals.
    bool isBaseEnvPrimop(const std::string & name) const
    {
        if (!findPrimOp(name)) return false;
        return !baseEnvNames || baseEnvNames->count(name) != 0;
    }

    /// Stable backing store for runtime-derived strings (paths, __curPos
    /// `file`).  Mirrors lower.cc's stringPool / interpStr — a deque never
    /// invalidates the string_views the IR holds.
    std::string_view internStr(const std::string & s)
    {
        static std::deque<std::string> pool;
        pool.emplace_back(s);
        return pool.back();
    }

    /// Resolve a v3 byte-offset → {file,line,column} against the source's
    /// registered PosTable::Origin (mirrors lower.cc::posIdxToHandle /
    /// lowerPosAttrs' resolution).  nullopt when no position is known.
    struct ResolvedPos { std::string file; uint32_t line; uint32_t column; };
    std::optional<ResolvedPos> resolvePos(nix::v3::ast::Pos offset)
    {
        if (!positions || !posOrigin || offset == nix::v3::ast::noPos) return std::nullopt;
        nix::PosIdx pi = positions->add(*posOrigin, offset);
        if (!pi) return std::nullopt;
        nix::Pos pos = (*positions)[pi];
        std::string file;
        if (auto * s = std::get_if<nix::SourcePath>(&pos.origin)) file = s->path.abs();
        else if (std::holds_alternative<nix::Pos::Stdin>(pos.origin)) file = "<stdin>";
        else if (std::holds_alternative<nix::Pos::String>(pos.origin)) file = "<string>";
        else file = "<unknown>";
        return ResolvedPos{std::move(file), static_cast<uint32_t>(pos.line),
                           static_cast<uint32_t>(pos.column)};
    }

    /// v3 byte-offset → IR pos-handle (snapshot interned in the global
    /// pool).  Drives builtins.unsafeGetAttrPos / functionArgs.  0 (no
    /// position) → handle 0.
    uint32_t posHandle(nix::v3::ast::Pos offset)
    {
        auto rp = resolvePos(offset);
        if (!rp) return 0;
        return nix::v3::recordPosSnapshot({std::move(rp->file), rp->line, rp->column});
    }

    /// `__curPos` → `{ file; line; column; }` (mirrors lower.cc::lowerPosAttrs).
    /// `null` when no position is known.
    ir::VarId lowerPosAttrs(nix::v3::ast::Pos offset)
    {
        auto rp = resolvePos(offset);
        if (!rp) return addBinding(ir::LitNull{});
        ir::VarId fileV   = addBinding(ir::LitString{internStr(rp->file)});
        ir::VarId lineV   = addBinding(ir::LitInt{static_cast<int64_t>(rp->line)});
        ir::VarId columnV = addBinding(ir::LitInt{static_cast<int64_t>(rp->column)});
        std::vector<ir::AttrSet::Entry> entries;
        entries.push_back({m.internSymbol("file"),   fileV});
        entries.push_back({m.internSymbol("line"),   lineV});
        entries.push_back({m.internSymbol("column"), columnV});
        return addBinding(ir::AttrSet{std::move(entries)});
    }

    /// Lexical scope stack (innermost at the back), name → VarId.  A Var
    /// resolves against this (innermost-first); cross-function refs become
    /// upvalues — emit's computeFreeVars derives those from the IR, so the
    /// lowerer only needs correct VarRefs (freeVars passed empty).
    /// A scope is either regular (byName → a real param/binding VarId)
    /// or rec (`recVar` set → its names resolve to RecBindingSlotRef on
    /// the rec attrset; byName values are placeholders).
    struct Scope {
        std::map<std::string, ir::VarId> byName;
        ir::VarId recVar = ir::kInvalid;
        ir::VarId withTargetVar = ir::kInvalid;  // set → a `with` scope
    };
    std::vector<Scope> scopes;

    /// Outermost-first VarIds of every enclosing `with` (the #530
    /// lexical with-chain), attached to thunks/lambdas/letrec entries so
    /// OP_WITH_LOOKUP inside them can scan the right targets.
    std::vector<ir::VarId> collectLexicalWiths() const
    {
        std::vector<ir::VarId> out;
        for (const Scope & s : scopes)
            if (s.withTargetVar != ir::kInvalid) out.push_back(s.withTargetVar);
        return out;
    }

    explicit LowererV3(const nix::SymbolTable & symbols) : symbols(symbols) {}

    ir::VarId addBinding(ir::Expr e)
    {
        auto v = m.freshVar();
        m.blocks[blockStack.back()].bindings.push_back({v, std::move(e)});
        return v;
    }
    void setReturn(ir::VarId v) { m.blocks[blockStack.back()].terminal = ir::TermReturn{v}; }
    ir::VarId forceVal(ir::VarId v) { return addBinding(ir::Force{v}); }

    /// If `v` is the most-recent binding in the current block and that
    /// binding is a `LitPrimOp`, return the primop; else nullptr.  Used to
    /// recognise a saturated primop call (e.g. `seq a b`) at its call site —
    /// `builtins.seq` / `__seq` / bare `seq` all lower the callee to exactly
    /// one `LitPrimOp` binding, which `lowerExpr(c->fun)` just appended.
    const v3::PrimOp * primopOfRecentBinding(ir::VarId v) const
    {
        const auto & bs = m.blocks[blockStack.back()].bindings;
        if (!bs.empty() && bs.back().var == v)
            if (auto * lp = std::get_if<ir::LitPrimOp>(&bs.back().expr))
                return lp->primop;
        return nullptr;
    }

    /// Resolve a Var: lexical scope (innermost-first) → VarRef; else the
    /// base env (literal const / primop / builtins); else unbound error
    /// (mirrors lower.cc::lowerVar).
    ir::VarId lowerVar(const nix::v3::ast::Var * v) { return lowerVarByName(v->name); }

    ir::VarId lowerVarByName(const std::string & name)
    {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->byName.find(name);
            if (f == it->byName.end()) continue;
            // A real VarId (lambda param / @-arg) → VarRef; a kInvalid
            // entry is a rec slot → heap-stable RecBindingSlotRef on the
            // scope's rec attrset (lower.cc's verified-correct default;
            // strict contexts force it).
            if (f->second != ir::kInvalid)
                return addBinding(ir::VarRef{f->second});
            return addBinding(ir::RecBindingSlotRef{it->recVar, m.internSymbol(name)});
        }
        if (name == "true")  return addBinding(ir::LitBool{true});
        if (name == "false") return addBinding(ir::LitBool{false});
        if (name == "null")  return addBinding(ir::LitNull{});
        if (name == "builtins") {
            ir::VarId bv = addBinding(ir::LitBuiltins{});
            m.litBuiltinsVarIds.push_back(bv);
            return bv;
        }
        // Base-env primop — ONLY if `name` is a genuine TW bare global
        // (isBaseEnvPrimop).  A registered primop that is NOT a TW global
        // (e.g. bare `fetchurl`, `head`, `foldl`) must fall through to the
        // with-chain so it resolves to the with-bound binding TW would
        // pick — matching bindVars exactly.
        if (isBaseEnvPrimop(name)) {
            const PrimOp * po = findPrimOp(name);
            if (po->arity == 0) return addBinding(ir::PrimOpCall{po, {}});
            return addBinding(ir::LitPrimOp{po});
        }
        // Not lexical, not base-env: if under any enclosing `with`, the
        // name resolves dynamically (WithLookup scans the with-chain at
        // runtime).  Nix order: lexical → base-env → with → error.
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->withTargetVar != ir::kInvalid)
                return addBinding(ir::WithLookup{m.internSymbol(name)});
        throw std::runtime_error("v3 native lower: unbound variable '" + name + "'");
    }

    ir::VarId lowerExpr(const nix::v3::ast::Node * n)
    {
        namespace a = nix::v3::ast;
        switch (n->kind) {
        case a::Kind::Int:
            return addBinding(ir::LitInt{static_cast<const a::Int *>(n)->n});
        case a::Kind::Float:
            return addBinding(ir::LitFloat{static_cast<const a::Float *>(n)->f});
        case a::Kind::String: {
            // Long-lived backing store for the string_view (mirrors
            // lower.cc::lowerString's stringPool).
            static std::deque<std::string> stringPool;
            stringPool.emplace_back(static_cast<const a::String *>(n)->s);
            return addBinding(ir::LitString{stringPool.back()});
        }
        case a::Kind::Var:
            return lowerVar(static_cast<const a::Var *>(n));
        case a::Kind::Lambda:
            return lowerLambda(static_cast<const a::Lambda *>(n));
        case a::Kind::Let:
            return lowerLet(static_cast<const a::Let *>(n));
        case a::Kind::Attrs:
            return lowerAttrs(static_cast<const a::Attrs *>(n));
        case a::Kind::Select:
            return lowerSelect(static_cast<const a::Select *>(n));
        case a::Kind::OpHasAttr:
            return lowerHasAttr(static_cast<const a::OpHasAttr *>(n));
        case a::Kind::With:
            return lowerWith(static_cast<const a::With *>(n));

        case a::Kind::Call: {
            auto * c = static_cast<const a::Call *>(n);
            // App-chain (handles primops via LitPrimOp + OP_CALL).  Args
            // are thunked for laziness (thunkifyForAttr — thunk unless
            // a trivial literal/var/lambda) so e.g. `tryEval <x>` /
            // `const 1 (throw "y")` don't fire the arg eagerly.
            ir::VarId f = lowerExpr(c->fun);
            // `seq a b`  →  Force(a); return b  (b still lazy).  `seq` forces
            // its 1st arg to WHNF and returns the 2nd UNFORCED, so lowering it
            // as a Force on a plus the unchanged (thunked) b is semantically
            // identical — and drops the seq primop machinery (a LitPrimOp +
            // the partial-app closure for `seq a` + the saturating OP_CALL/
            // CALL_PRIMOP) in favour of one OP_FORCE.  b's laziness is
            // preserved (thunkifyForAttr, same as the generic arg path), so
            // this is safe in lazy contexts.  deepSeq is excluded (it deep-
            // forces, which a shallow Force does not implement).
            // Retirement: drop the NIX_V3_NO_SEQ_FORCE opt-out once this has
            // shipped byte-identical on --core + a nixpkgs sample across ≥10
            // runs (it is a pure local rewrite; the gate exists only as a
            // bisect handle during rollout).
            static const bool noSeqForce = std::getenv("NIX_V3_NO_SEQ_FORCE") != nullptr;
            if (!noSeqForce && c->args.size() == 2) {
                if (const v3::PrimOp * po = primopOfRecentBinding(f);
                    po && po->name == "seq") {
                    forceVal(lowerExpr(c->args[0]));     // force a (seq's effect)
                    return thunkifyForAttr(c->args[1]);  // return b, still lazy
                }
            }
            for (auto * arg : c->args)
                f = addBinding(ir::App{f, thunkifyForAttr(arg)});
            return f;
        }
        case a::Kind::If: {
            auto * i = static_cast<const a::If *>(n);
            ir::VarId cond = lowerExpr(i->cond);   // OP_BRANCH_FALSE forces
            auto thenB = m.freshBlock();
            auto elseB = m.freshBlock();
            blockStack.push_back(thenB); setReturn(lowerExpr(i->then_)); blockStack.pop_back();
            blockStack.push_back(elseB); setReturn(lowerExpr(i->else_)); blockStack.pop_back();
            return addBinding(ir::If{cond, thenB, elseB});
        }
        case a::Kind::OpEq: { auto * b = static_cast<const a::BinOp *>(n);
            return addBinding(ir::Eq{forceVal(lowerExpr(b->lhs)), forceVal(lowerExpr(b->rhs))}); }
        case a::Kind::OpNEq: { auto * b = static_cast<const a::BinOp *>(n);
            return addBinding(ir::NEq{forceVal(lowerExpr(b->lhs)), forceVal(lowerExpr(b->rhs))}); }
        case a::Kind::OpUpdate: { auto * b = static_cast<const a::BinOp *>(n);
            return addBinding(ir::Update{forceVal(lowerExpr(b->lhs)), forceVal(lowerExpr(b->rhs))}); }
        case a::Kind::OpConcatLists: { auto * b = static_cast<const a::BinOp *>(n);
            return addBinding(ir::ConcatLists{forceVal(lowerExpr(b->lhs)), forceVal(lowerExpr(b->rhs))}); }
        case a::Kind::OpNot:
            return addBinding(ir::Not{lowerExpr(static_cast<const a::OpNot *>(n)->e)});
        case a::Kind::ConcatStrings: {
            auto * cs = static_cast<const a::ConcatStrings *>(n);
            std::vector<ir::VarId> parts;
            parts.reserve(cs->es.size());
            for (auto * e : cs->es) parts.push_back(forceVal(lowerExpr(e)));
            return addBinding(ir::ConcatStrings{std::move(parts), cs->forceString});
        }
        case a::Kind::Assert: {
            auto * as = static_cast<const a::Assert *>(n);
            ir::VarId cond = forceVal(lowerExpr(as->cond));
            auto bodyB = m.freshBlock();
            blockStack.push_back(bodyB); setReturn(lowerExpr(as->body)); blockStack.pop_back();
            return addBinding(ir::Assert{cond, bodyB});
        }
        case a::Kind::List: {
            // Lazy elements — each non-trivial element thunked so building
            // the list fires no unused side-effects (`head [42 (throw "x")]`
            // → 42).  Mirrors lower.cc::lowerList.
            auto * l = static_cast<const a::List *>(n);
            std::vector<ir::VarId> elems;
            elems.reserve(l->elems.size());
            for (auto * el : l->elems) elems.push_back(thunkifyForAttr(el));
            return addBinding(ir::ListExpr{std::move(elems)});
        }
        case a::Kind::Path: {
            // The parser already resolved the path string (abs / SPATH /
            // relative-to-basePath).  accessor=nullptr → default root FS,
            // matching lower.cc::lowerPath.
            return addBinding(ir::LitPath{internStr(static_cast<const a::Path *>(n)->p), nullptr});
        }
        case a::Kind::OpAnd: case a::Kind::OpOr: case a::Kind::OpImpl: {
            // Short-circuit: lhs in the current block, rhs in a fresh block
            // entered only when lhs's value demands it (the VM's And/Or/Impl
            // op forces lhs).  Mirrors lower.cc::lowerShortCircuit.
            auto * b = static_cast<const a::BinOp *>(n);
            ir::VarId lhs = lowerExpr(b->lhs);
            auto rhsB = m.freshBlock();
            blockStack.push_back(rhsB); setReturn(lowerExpr(b->rhs)); blockStack.pop_back();
            if (n->kind == a::Kind::OpAnd) return addBinding(ir::And{lhs, rhsB});
            if (n->kind == a::Kind::OpOr)  return addBinding(ir::Or{lhs, rhsB});
            return addBinding(ir::Impl{lhs, rhsB});
        }
        case a::Kind::Pos:
            // `__curPos` → `{ file; line; column; }` from this node's offset.
            return lowerPosAttrs(n->pos);
        default:
            throw std::runtime_error("v3 native lower: unsupported kind "
                                     + std::to_string((int) n->kind));
        }
    }

    /// Phase 2a/2c: lambda.  Single-arg `x: body` (param bound by name)
    /// OR formals `{ a, b ? d, ... }[@arg]: body` (each formal is a
    /// thunk `if param?X then param.X else default` in a synthetic
    /// rec scope, so defaults can reference sibling formals; the body
    /// resolves formals via the rec attrset).  Mirrors lower.cc::
    /// lowerLambda.  No intrinsic recognition yet (Phase 4) — the
    /// fast-path is an optimization, intrinsicKind=0 is correct.
    ir::VarId lowerLambda(const nix::v3::ast::Lambda * lam)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();
        ir::VarId param = m.freshVar();
        m.functions[fid].entryBlock = entry;
        m.functions[fid].paramVar   = param;
        m.functions[fid].argName    = lam->arg.empty() ? ir::kInvalidSymbol : m.internSymbol(lam->arg);
        m.functions[fid].name       = lam->arg.empty() ? "<formals>" : lam->arg;

        if (!lam->hasFormals) {
            Scope inner;
            inner.byName[lam->arg] = param;
            // eval/apply (#3): collapse a curried chain `x: y: … : body` of
            // SIMPLE single-param lambdas (no formals, named arg) into ONE
            // arity-N Function — paramVar + extraParams[] all in scope, body
            // lowered once.  A later inner lambda's name shadows an outer one
            // (operator[] overwrites; the shadowed param still occupies its
            // slot, just unreferenced).  Gated; off → classic one-lambda-per-
            // arrow lowering (byte-identical).  The VM's OP_CALL_N + PAP make
            // partial/saturated/over-application of the arity-N function all
            // behave like the curried original.
            // WIP: this is the lowering half of eval/apply (bench doc §5.2).
            // Until the emit (multi-slot params + OP_CALL_N) and VM (PAP +
            // arity-match) halves land, NIX_V3_EVAL_APPLY=1 is INCOMPLETE —
            // keep it OFF (default) for correct results.
            // Retirement: once eval/apply is default-on (byte-identical on
            // --core + a nixpkgs sample + wall-positive on fold-add), drop the
            // gate and remove the curried fallback.
            static const bool s_evalApply =
                std::getenv("NIX_V3_EVAL_APPLY") != nullptr;
            const nix::v3::ast::Node * bodyToLower = lam->body;
            if (s_evalApply && !lam->arg.empty()) {
                // Cap arity at 16 (1 paramVar + ≤15 extraParams): the VM's
                // PAP saturate gathers into a fixed argbuf[16] and the
                // LambdaDescriptor::arity field is uint8_t.  Beyond the cap the
                // remaining lambdas stay curried (separate arity-1 Functions),
                // so a pathological N-ary lambda (e.g. the curry-apply-5000
                // test) degrades to the classic curried path rather than
                // overflowing.  Real multi-arg functions are far under 16.
                const nix::v3::ast::Node * b = lam->body;
                while (b->kind == nix::v3::ast::Kind::Lambda
                       && m.functions[fid].extraParams.size() < 15) {
                    auto * il = static_cast<const nix::v3::ast::Lambda *>(b);
                    if (il->hasFormals || il->arg.empty()) break;
                    ir::VarId ep = m.freshVar();
                    m.functions[fid].extraParams.push_back(ep);
                    inner.byName[il->arg] = ep;
                    b = il->body;
                }
                bodyToLower = b;
            }
            scopes.push_back(std::move(inner));
            blockStack.push_back(entry);
            setReturn(lowerExpr(bodyToLower));
            blockStack.pop_back();
            scopes.pop_back();
            {
            std::vector<ir::VarId> lws = collectLexicalWiths();
            m.functions[fid].nWithTargets = static_cast<uint16_t>(lws.size());
            return addBinding(ir::Lambda{fid, /*freeVars*/ {}, std::move(lws)});
        }
        }

        // Formals.  Record metadata for builtins.functionArgs.
        m.functions[fid].hasFormals = true;
        m.functions[fid].ellipsis   = lam->ellipsis;
        for (auto & f : lam->formals) {
            ir::Formal ifm;
            ifm.name = m.internSymbol(f.name);
            ifm.hasDefault = f.def != nullptr;
            ifm.pos = posHandle(f.pos);
            m.functions[fid].formals.push_back(ifm);
        }
        ir::VarId formalsRec = m.freshVar();
        m.functions[fid].formalsRecVar = formalsRec;
        m.recVarIds.push_back(formalsRec);

        // Rec scope: @-arg → the attrset (regular VarRef); each formal
        // → a rec slot (kInvalid) on formalsRec.
        Scope recScope;
        recScope.recVar = formalsRec;
        if (!lam->arg.empty()) recScope.byName.emplace(lam->arg, param);
        for (auto & f : lam->formals) recScope.byName.emplace(f.name, ir::kInvalid);

        // Canonical (by-name) order for FuncId stability (#815).
        std::vector<const nix::v3::ast::Formal *> fs;
        fs.reserve(lam->formals.size());
        for (auto & f : lam->formals) fs.push_back(&f);
        std::stable_sort(fs.begin(), fs.end(),
            [](const auto * a, const auto * b) { return a->name < b->name; });

        blockStack.push_back(entry);
        ir::LetRec lr;
        lr.recVar = formalsRec;
        lr.hasBody = true;
        lr.entries.reserve(fs.size());
        for (auto * f : fs) {
            ir::SymbolId sym = m.internSymbol(f->name);
            m.functions.emplace_back();
            ir::FuncId tfid = static_cast<ir::FuncId>(m.functions.size() - 1);
            auto teb = m.freshBlock();
            m.functions[tfid].entryBlock = teb;
            m.functions[tfid].name = f->name;
            // Thunk body: `if param ? X then param.X else <default>`
            // (or `param.X` when no default), lowered in the rec scope.
            blockStack.push_back(teb);
            scopes.push_back(recScope);
            ir::VarId paramForced = forceVal(addBinding(ir::VarRef{param}));
            ir::VarId rv;
            if (f->def) {
                ir::VarId hasIt = addBinding(ir::HasAttr{paramForced, sym});
                auto thenB = m.freshBlock();
                auto elseB = m.freshBlock();
                blockStack.push_back(thenB);
                setReturn(addBinding(ir::AttrSelect{paramForced, sym}));
                blockStack.pop_back();
                blockStack.push_back(elseB);
                setReturn(lowerExpr(f->def));
                blockStack.pop_back();
                rv = addBinding(ir::If{hasIt, thenB, elseB});
            } else
                rv = addBinding(ir::AttrSelect{paramForced, sym});
            setReturn(rv);
            scopes.pop_back();
            blockStack.pop_back();

            ir::LetRec::Entry en;
            en.name = sym;
            en.thunkBody = tfid;
            en.lexicalWiths = collectLexicalWiths();  // #530 with-chain
            m.functions[tfid].nWithTargets = static_cast<uint16_t>(en.lexicalWiths.size());
            lr.entries.push_back(std::move(en));
        }
        m.blocks[blockStack.back()].bindings.push_back({formalsRec, std::move(lr)});

        // Body in the rec scope (formals resolve via the rec attrset).
        scopes.push_back(recScope);
        setReturn(lowerExpr(lam->body));
        scopes.pop_back();
        blockStack.pop_back();

        {
            std::vector<ir::VarId> lws = collectLexicalWiths();
            m.functions[fid].nWithTargets = static_cast<uint16_t>(lws.size());
            return addBinding(ir::Lambda{fid, /*freeVars*/ {}, std::move(lws)});
        }
    }

    /// Trivial-for-value: cheap exprs that need no lazy thunk wrapper
    /// as an attr/list value (mirrors lower.cc isTrivialForLazy forValue
    /// — literals/lambda/var; NOT arithmetic, which is lazy for values).
    /// Would the name resolve through the `with`-chain (i.e. NOT lexically
    /// and NOT a base-env name)?  Mirrors lowerVarByName's resolution order
    /// WITHOUT emitting.  A with-resolved Var compiles to a runtime
    /// OP_WITH_LOOKUP that forces the with-target — so it is NOT trivial
    /// and must be thunked in lazy positions (the "delayed with" cycle:
    /// `with pkgs; { a = b; }` where pkgs is part of the same fixed point).
    bool resolvesViaWith(const std::string & name) const
    {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->byName.count(name)) return false;        // lexical / rec slot
        if (name == "true" || name == "false" || name == "null"
            || name == "builtins" || name == "__curPos") return false;  // base-env consts
        if (isBaseEnvPrimop(name)) return false;              // genuine TW base-env primop
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->withTargetVar != ir::kInvalid) return true;  // falls through to with
        return false;  // unbound (lowering will error) — not a with-lookup
    }

    /// Trivial-for-lazy (mirrors lower.cc::isTrivialForLazy): values whose
    /// "thunk" can be skipped because constructing them is side-effect-free
    /// and cheap.  A Var is trivial UNLESS it resolves via `with` (see
    /// resolvesViaWith).  Non-static: the Var case consults the scope stack.
    bool isTrivialForValue(const nix::v3::ast::Node * n) const
    {
        namespace a = nix::v3::ast;
        switch (n->kind) {
        case a::Kind::Int: case a::Kind::Float: case a::Kind::String:
        case a::Kind::Path: case a::Kind::Lambda:
            return true;
        case a::Kind::Var:
            return !resolvesViaWith(static_cast<const a::Var *>(n)->name);
        default: return false;
        }
    }

    /// Wrap `e` in a thunk Function (lowered in the CURRENT scopes, so it
    /// captures outer vars as upvalues — emit computes them).  Mirrors
    /// lower.cc::thunkify.
    ir::VarId thunkify(const nix::v3::ast::Node * e)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto eb = m.freshBlock();
        m.functions[fid].entryBlock = eb;
        m.functions[fid].name = "<thunk>";
        blockStack.push_back(eb);
        setReturn(lowerExpr(e));
        blockStack.pop_back();
        std::vector<ir::VarId> lws = collectLexicalWiths();
        m.functions[fid].nWithTargets = static_cast<uint16_t>(lws.size());
        return addBinding(ir::MkThunk{fid, /*freeVars*/ {}, std::move(lws)});
    }
    ir::VarId thunkifyForAttr(const nix::v3::ast::Node * e)
    {
        return isTrivialForValue(e) ? lowerExpr(e) : thunkify(e);
    }

    /// Thunk whose body is built by `bodyBuilder()` (returns the body
    /// VarId) — for synthetic bodies not backed by a v3 AST node (e.g.
    /// an `inherit (e) name` → AttrSelect(sharedSrc, name)).
    template<class F>
    ir::VarId thunkifyIR(F bodyBuilder)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto eb = m.freshBlock();
        m.functions[fid].entryBlock = eb;
        m.functions[fid].name = "<thunk>";
        blockStack.push_back(eb);
        setReturn(bodyBuilder());
        blockStack.pop_back();
        std::vector<ir::VarId> lws = collectLexicalWiths();
        m.functions[fid].nWithTargets = static_cast<uint16_t>(lws.size());
        return addBinding(ir::MkThunk{fid, /*freeVars*/ {}, std::move(lws)});
    }

    /// Shared rec-attrset / let-rec lowering (lower.cc::lowerLetRec).
    /// `hasBody` true → `let … in body` (returns body value); false →
    /// `rec { … }` (returns the rec attrset).  Plain bindings only —
    /// inherit / inherit-from / dynamic are Phase 4 (canLowerV3 rejects).
    ir::VarId lowerLetRec(const nix::v3::ast::Attrs * at, bool hasBody,
                          const nix::v3::ast::Node * body)
    {
        std::vector<const nix::v3::ast::Attrs::AttrDef *> bs;
        bs.reserve(at->attrs.size());
        for (auto & d : at->attrs) bs.push_back(&d);
        std::stable_sort(bs.begin(), bs.end(),
            [](const auto * a, const auto * b) { return a->name < b->name; });

        ir::VarId recVar = m.freshVar();
        m.recVarIds.push_back(recVar);
        Scope recScope;
        recScope.recVar = recVar;
        for (auto * d : bs) recScope.byName.emplace(d->name, ir::kInvalid);

        // inherit-from sources: one shared hidden thunk per inheritFromExprs
        // entry, lowered in the rec scope (it may reference siblings), so
        // each `inherit (e) a b;` forces `e` once (LetRec.hiddenEntries).
        std::vector<ir::VarId>  hiddenVars(at->inheritFromExprs.size(), ir::kInvalid);
        std::vector<ir::FuncId> hiddenFids(at->inheritFromExprs.size(), 0);
        for (size_t idx = 0; idx < at->inheritFromExprs.size(); ++idx) {
            m.functions.emplace_back();
            ir::FuncId hfid = static_cast<ir::FuncId>(m.functions.size() - 1);
            auto heb = m.freshBlock();
            m.functions[hfid].entryBlock = heb;
            m.functions[hfid].name = "<inherit-from>";
            blockStack.push_back(heb);
            scopes.push_back(recScope);
            setReturn(lowerExpr(at->inheritFromExprs[idx]));
            scopes.pop_back();
            blockStack.pop_back();
            hiddenVars[idx] = m.freshVar();
            hiddenFids[idx] = hfid;
        }

        std::vector<ir::FuncId> fids;
        fids.reserve(bs.size());
        // First block index belonging to the entry thunks (used by the
        // non-recursive demotion below to scan ONLY the entries' IR for
        // sibling references — the body isn't lowered yet).
        ir::BlockId entryBlockStart = static_cast<ir::BlockId>(m.blocks.size());
        for (auto * d : bs) {
            m.functions.emplace_back();
            ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
            auto eb = m.freshBlock();
            m.functions[fid].entryBlock = eb;
            m.functions[fid].name = d->name;
            fids.push_back(fid);
            blockStack.push_back(eb);
            if (d->kind == nix::v3::ast::Attrs::AttrKind::Inherited) {
                // `inherit x;` binds x to the PARENT-scope x (not the rec
                // slot) — lower the var WITHOUT the rec scope pushed.
                setReturn(lowerVarByName(d->name));
            } else if (d->kind == nix::v3::ast::Attrs::AttrKind::InheritedFrom) {
                // `inherit (e) x;` → e.x, sharing the hidden source thunk.
                ir::VarId src = addBinding(ir::VarRef{hiddenVars[d->fromIdx]});
                setReturn(addBinding(ir::AttrSelect{src, m.internSymbol(d->name)}));
            } else {  // Plain
                scopes.push_back(recScope);
                setReturn(lowerExpr(d->value));
                scopes.pop_back();
            }
            blockStack.pop_back();
        }

        // opt_letrec_demote (at-lowering, the clean fix): a NON-recursive
        // `let … in body` needs no rec-attrset.  Every `let … in …` lowers to
        // a LetRec (OP_ATTRS_LET_REC_INIT heap cell + per-binding thunk
        // function + RecBindingSlotRef slot lookups) — overhead only mutual
        // recursion needs.  For a non-recursive group we instead bind each
        // value as a plain lazy thunk and lower the body in a NON-rec scope,
        // so references become VarRef.  This is the fold-add-1M 17× root cause
        // (the bytecode foldl''s `let next = op acc elem; in …` allocated a
        // rec-attrset per iteration) and taxes EVERY non-recursive let in
        // nixpkgs.  Verified by test/ir-fixtures/letrecDemote-nonrec-pos.nix.
        //
        // Eligibility (conservative — any doubt keeps the LetRec):
        //   - hasBody (the `let … in body` shape; `rec { … }` must stay a
        //     Bindings value),
        //   - all entries Plain (no inherit / inherit-from),  no dynamics,
        //   - NON-recursive: no entry's lowered body references recVar via
        //     RecBindingSlotRef (a sibling/self reference) — scanned over the
        //     entry blocks only (the body isn't lowered yet).
        // Gate: NIX_V3_NO_LETREC_DEMOTE=1 (regression bisection / A-B).
        // RETIREMENT: fold into a shared helper if the TW-side lowerer ever
        // produces v3 IR, or delete if measurement falsifies the win.
        {
            static const bool noDemote =
                std::getenv("NIX_V3_NO_LETREC_DEMOTE") != nullptr;
            bool eligible = hasBody && !noDemote
                && at->inheritFromExprs.empty() && at->dynamicAttrs.empty();
            for (auto * d : bs)
                if (d->kind != nix::v3::ast::Attrs::AttrKind::Plain)
                    eligible = false;
            if (eligible) {
                bool recursive = false;
                for (ir::BlockId b = entryBlockStart;
                     b < (ir::BlockId)m.blocks.size() && !recursive; ++b)
                    for (auto & bd : m.blocks[b].bindings) {
                        auto * sr = std::get_if<ir::RecBindingSlotRef>(&bd.expr);
                        if (sr && sr->attrs == recVar) { recursive = true; break; }
                    }
                if (!recursive) {
                    std::vector<ir::VarId> lws = collectLexicalWiths();
                    Scope plainScope;
                    for (size_t i = 0; i < bs.size(); ++i) {
                        m.functions[fids[i]].nWithTargets =
                            static_cast<uint16_t>(lws.size());
                        ir::VarId vid =
                            addBinding(ir::MkThunk{fids[i], /*freeVars*/ {}, lws});
                        plainScope.byName[bs[i]->name] = vid;
                    }
                    // recVar was reserved (recVarIds) but is now unused — leave
                    // it; emit/computeFreeVars tolerate an unreferenced recVar.
                    scopes.push_back(std::move(plainScope));
                    ir::VarId rv = lowerExpr(body);
                    scopes.pop_back();
                    return addBinding(ir::VarRef{rv});
                }
            }
        }

        ir::LetRec lr;
        lr.recVar = recVar;
        lr.hasBody = hasBody;
        lr.entries.reserve(bs.size());
        std::vector<ir::VarId> lws = collectLexicalWiths();  // #530 with-chain
        for (size_t i = 0; i < bs.size(); ++i) {
            ir::LetRec::Entry en;
            en.name = m.internSymbol(bs[i]->name);
            en.thunkBody = fids[i];
            en.pos = posHandle(bs[i]->pos);
            en.lexicalWiths = lws;
            m.functions[fids[i]].nWithTargets = static_cast<uint16_t>(lws.size());
            lr.entries.push_back(std::move(en));
        }
        for (size_t idx = 0; idx < hiddenVars.size(); ++idx) {
            ir::LetRec::HiddenEntry he;
            he.hiddenVar = hiddenVars[idx];
            he.thunkBody = hiddenFids[idx];
            he.lexicalWiths = lws;
            m.functions[hiddenFids[idx]].nWithTargets = static_cast<uint16_t>(lws.size());
            lr.hiddenEntries.push_back(std::move(he));
        }
        m.blocks[blockStack.back()].bindings.push_back({recVar, std::move(lr)});

        if (hasBody) {  // `let … in body` — `let` never carries dynamics
            scopes.push_back(recScope);
            ir::VarId rv = lowerExpr(body);
            scopes.pop_back();
            return addBinding(ir::VarRef{rv});
        }

        // `rec { … }` — the value is the rec attrset itself.
        ir::VarId recValV = addBinding(ir::VarRef{recVar});

        // rec + dynamic keys (`rec { ${e} = v; ... }`): lower the dynamics
        // in the rec scope (they may reference siblings) into an AttrSetDyn
        // carrying ONLY the dynamics (statics already live in the rec
        // attrset), then merge via Update (dyn overrides rec).  Mirrors
        // lower.cc's rec+dyn branch.
        if (!at->dynamicAttrs.empty()) {
            scopes.push_back(recScope);
            ir::AttrSetDyn dyn;
            dyn.dynamics.reserve(at->dynamicAttrs.size());
            for (auto & da : at->dynamicAttrs) {
                ir::VarId nameV = forceVal(lowerExpr(da.nameExpr));
                ir::VarId valV  = thunkifyForAttr(da.valueExpr);
                dyn.dynamics.push_back({nameV, valV, /*pos*/ 0});
            }
            scopes.pop_back();
            recValV = addBinding(ir::Update{recValV, addBinding(std::move(dyn))});
        }
        return recValV;
    }

    ir::VarId lowerLet(const nix::v3::ast::Let * let)
    {
        return lowerLetRec(let->attrs, /*hasBody=*/true, let->body);
    }

    /// Lower one static (non-dynamic) attr def's VALUE (shared by AttrSet
    /// + AttrSetDyn statics): Plain → lazy thunk; `inherit x` → parent x
    /// (with-aware lazy; see the delayed-with note); `inherit (e) x` →
    /// `e.x` over the shared source thunk `srcVars[fromIdx]`.
    ir::VarId lowerStaticAttrValue(const nix::v3::ast::Attrs::AttrDef & d,
                                   const std::vector<ir::VarId> & srcVars)
    {
        namespace a = nix::v3::ast;
        if (d.kind == a::Attrs::AttrKind::Inherited)
            return resolvesViaWith(d.name)
                ? thunkifyIR([&] { return lowerVarByName(d.name); })
                : lowerVarByName(d.name);
        if (d.kind == a::Attrs::AttrKind::InheritedFrom) {
            ir::SymbolId sym = m.internSymbol(d.name);
            ir::VarId src = srcVars[d.fromIdx];
            return thunkifyIR([&] {
                return addBinding(ir::AttrSelect{addBinding(ir::VarRef{src}), sym});
            });
        }
        return thunkifyForAttr(d.value);  // Plain
    }

    /// Phase 3a/4c: attrset.  rec → lowerLetRec (handles rec + dynamics);
    /// non-rec → ir::AttrSet (static keys) or ir::AttrSetDyn (`${e}` keys).
    /// Values are lazy (thunkified); dynamic NAMES are forced (must be a
    /// string; `null` drops the entry — the VM's AttrSetDyn op handles it).
    ir::VarId lowerAttrs(const nix::v3::ast::Attrs * e)
    {
        namespace a = nix::v3::ast;
        if (e->recursive)
            return lowerLetRec(e, /*hasBody=*/false, /*body=*/nullptr);
        // inherit-from sources: one shared thunk per inheritFromExprs
        // entry (parent scope — non-rec doesn't see siblings), so each
        // `inherit (e) a b;` forces `e` once.
        std::vector<ir::VarId> srcVars(e->inheritFromExprs.size(), ir::kInvalid);
        for (size_t i = 0; i < e->inheritFromExprs.size(); ++i)
            srcVars[i] = thunkify(e->inheritFromExprs[i]);

        if (!e->dynamicAttrs.empty()) {
            // 4c: at least one `${expr} = v;` — emit an AttrSetDyn carrying
            // the statics + the dynamics (mirrors lower.cc's non-rec dyn).
            ir::AttrSetDyn dyn;
            dyn.statics.reserve(e->attrs.size());
            for (auto & d : e->attrs)
                dyn.statics.push_back({m.internSymbol(d.name),
                                       lowerStaticAttrValue(d, srcVars), posHandle(d.pos)});
            dyn.dynamics.reserve(e->dynamicAttrs.size());
            for (auto & da : e->dynamicAttrs) {
                ir::VarId nameV = forceVal(lowerExpr(da.nameExpr));  // string (or null → drop)
                ir::VarId valV  = thunkifyForAttr(da.valueExpr);     // lazy
                dyn.dynamics.push_back({nameV, valV, /*pos*/ 0});
            }
            return addBinding(std::move(dyn));
        }

        ir::AttrSet as;
        as.entries.reserve(e->attrs.size());
        for (auto & d : e->attrs) {
            ir::AttrSet::Entry en;
            en.name = m.internSymbol(d.name);
            en.pos = posHandle(d.pos);
            en.value = lowerStaticAttrValue(d, srcVars);
            as.entries.push_back(std::move(en));
        }
        return addBinding(std::move(as));
    }

    /// Phase 3a: `e.a.b.c [or default]` — AttrSelect chain (static or
    /// `${dyn}` keys), default thunkified once (lower.cc #755).
    ir::VarId lowerSelect(const nix::v3::ast::Select * e)
    {
        // `builtins.<primop>` → the primop value directly.
        if (!e->def && e->path.size() == 1 && e->path[0].expr == nullptr
            && e->e->kind == nix::v3::ast::Kind::Var
            && static_cast<const nix::v3::ast::Var *>(e->e)->name == "builtins")
        {
            if (const PrimOp * po = findPrimOp(e->path[0].symbol)) {
                if (po->arity == 0) return addBinding(ir::PrimOpCall{po, {}});
                return addBinding(ir::LitPrimOp{po});
            }
        }
        ir::VarId v = lowerExpr(e->e);
        ir::VarId def = e->def ? thunkifyForAttr(e->def) : ir::kInvalid;
        return emitSelectChain(v, e->path, def, 0);
    }

    ir::VarId emitSelectChain(ir::VarId attrs,
                              const std::vector<nix::v3::ast::AttrName> & path,
                              ir::VarId defaultVal, size_t idx)
    {
        if (idx == path.size()) return attrs;
        const auto & step = path[idx];
        bool dyn = step.expr != nullptr;
        ir::SymbolId nm = dyn ? 0 : m.internSymbol(step.symbol);
        ir::VarId nameVar = dyn ? lowerExpr(step.expr) : ir::kInvalid;
        if (defaultVal != ir::kInvalid) {
            ir::VarId hasIt = dyn ? addBinding(ir::HasAttrDyn{attrs, nameVar})
                                  : addBinding(ir::HasAttr{attrs, nm});
            auto thenB = m.freshBlock();
            auto elseB = m.freshBlock();
            blockStack.push_back(thenB);
            ir::VarId got = dyn ? addBinding(ir::AttrSelectDyn{attrs, nameVar})
                                : addBinding(ir::AttrSelect{attrs, nm});
            setReturn(emitSelectChain(got, path, defaultVal, idx + 1));
            blockStack.pop_back();
            blockStack.push_back(elseB);
            setReturn(forceVal(defaultVal));
            blockStack.pop_back();
            return addBinding(ir::If{hasIt, thenB, elseB});
        }
        ir::VarId v = dyn ? addBinding(ir::AttrSelectDyn{attrs, nameVar})
                          : addBinding(ir::AttrSelect{attrs, nm});
        return emitSelectChain(v, path, ir::kInvalid, idx + 1);
    }

    /// Phase 3a: `e ? a.b.c` — chain of HasAttr; short-circuit to false.
    ir::VarId lowerHasAttr(const nix::v3::ast::OpHasAttr * e)
    {
        ir::VarId attrs = lowerExpr(e->e);
        return hasAttrStep(attrs, e->path, 0);
    }
    ir::VarId hasAttrStep(ir::VarId cur, const std::vector<nix::v3::ast::AttrName> & path, size_t idx)
    {
        const auto & an = path[idx];
        bool dyn = an.expr != nullptr;
        ir::VarId nameVar = dyn ? lowerExpr(an.expr) : ir::kInvalid;
        ir::SymbolId nm = dyn ? 0 : m.internSymbol(an.symbol);
        ir::VarId hasIt = dyn ? addBinding(ir::HasAttrDyn{cur, nameVar})
                              : addBinding(ir::HasAttr{cur, nm});
        if (idx + 1 == path.size()) return hasIt;
        auto thenB = m.freshBlock();
        auto elseB = m.freshBlock();
        blockStack.push_back(thenB);
        ir::VarId got = dyn ? addBinding(ir::AttrSelectDyn{cur, nameVar})
                            : addBinding(ir::AttrSelect{cur, nm});
        setReturn(hasAttrStep(got, path, idx + 1));
        blockStack.pop_back();
        blockStack.push_back(elseB);
        setReturn(addBinding(ir::LitBool{false}));
        blockStack.pop_back();
        return addBinding(ir::If{hasIt, thenB, elseB});
    }

    /// Phase 3b: `with attrs; body`.  The attrs is thunked (lazy — TW
    /// only forces it when a WithLookup scans it; `with (throw "x"); 1`
    /// must return 1).  A `with` scope is pushed so unresolved names in
    /// body lower to WithLookup; the body runs in its own block.
    /// Mirrors lower.cc::lowerWith (minus the rec-slot optimization —
    /// a rec-bound with-target already lowers to RecBindingSlotRef via
    /// lowerVar, so the slot semantics hold naturally).
    ir::VarId lowerWith(const nix::v3::ast::With * e)
    {
        ir::VarId attrs = thunkifyForAttr(e->attrs);
        auto bodyB = m.freshBlock();
        blockStack.push_back(bodyB);
        Scope withScope;
        withScope.withTargetVar = attrs;
        scopes.push_back(withScope);
        ir::VarId rv = lowerExpr(e->body);
        scopes.pop_back();
        setReturn(rv);
        blockStack.pop_back();
        return addBinding(ir::With{attrs, bodyB, ir::kInvalid, ir::kInvalidSymbol});
    }

    ir::Module run(const nix::v3::ast::Node * e)
    {
        auto entry = m.freshBlock();
        m.functions[0].entryBlock = entry;
        blockStack.push_back(entry);
        ir::VarId rv = forceVal(lowerExpr(e));
        setReturn(rv);
        return std::move(m);
    }
};

/// Lower a v3 AST root to IR natively (no nix::Expr).  Precondition:
/// canLowerV3(e) is true.  `positions`/`origin` supply attr/formal
/// source positions (for unsafeGetAttrPos / functionArgs); pass a null
/// positions to omit them.  `baseEnvNames` (twBaseEnvGlobals) is TW's
/// bare base-env global set — required for correct free-name resolution
/// (a name not in it falls through to `with`, matching bindVars); pass
/// nullptr only when no TW EvalState is available.
inline ir::Module lowerV3Ast(const nix::SymbolTable & symbols, const nix::v3::ast::Node * e,
                             nix::PosTable * positions, nix::PosTable::Origin origin,
                             const std::set<std::string> * baseEnvNames = nullptr)
{
    LowererV3 L(symbols);
    L.positions = positions;
    if (positions) L.posOrigin.emplace(origin);
    L.baseEnvNames = baseEnvNames;
    return L.run(e);
}

} // namespace nix::v3

#pragma GCC diagnostic pop  // -Wswitch-enum (balanced with push at top)
