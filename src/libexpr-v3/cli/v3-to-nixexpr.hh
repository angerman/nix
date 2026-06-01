#pragma once
/// @file
/// Stage 2 bridge: v3 AST (`nix::v3::ast::Node`) -> `nix::Expr`.
///
/// PARSER_PROJECT_PLAN_2026-06-01.md §2 (Stage 2, integration step).
/// Lets the v3-native parser feed the EXISTING v3 pipeline
/// (`e->bindVars` + `lowerNixExpr` -> IR -> VM): parse natively, convert
/// to `nix::Expr`, then TW's bindVars + v3's own lower.cc handle
/// resolution + lowering unchanged.  This makes v3 own the PARSING while
/// reusing the proven lowering — the first integration milestone before
/// a fully-native AST->IR lowering replaces lower.cc's nix::Expr path.
///
/// Source positions: the v3 AST carries file-local byte offsets (the
/// determinism win vs TW's global PosIdx).  The bridge maps each offset
/// to a `nix::PosIdx` via the `PosTable::Origin` registered for the
/// source — so `__curPos` / `builtins.unsafeGetAttrPos` resolve to the
/// right line/column/file.  Only nodes that carry positions (PosExpr,
/// attr defs) get a real PosIdx; the rest are noPos.
///
/// Correctness is checkable by `show()` byte-equality (the bridged
/// nix::Expr show()s identically to TW's parser) AND eval parity (lang
/// test goldens).
///
/// Mirrors `parser.y`'s node construction exactly (the v3 AST was built
/// FROM parser.y's productions, so this is the inverse map).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ast/expr.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/expr/eval.hh"

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace nix::v3 {

/// Stateful converter: holds the EvalState + the PosTable origin for the
/// source, so byte offsets in the v3 AST become PosIdx.
struct Bridge {
    nix::EvalState & es;
    nix::PosTable::Origin po;

    nix::Exprs & X() { return es.mem.exprs; }

    /// v3 byte-offset -> nix::PosIdx (noPos when the node carries none;
    /// v3 noPos == offset 0, which we treat as "no position").
    nix::PosIdx pos(nix::v3::ast::Pos off)
    {
        return off == nix::v3::ast::noPos ? nix::noPos : es.positions.add(po, off);
    }

    std::vector<nix::AttrName> attrPath(const std::vector<nix::v3::ast::AttrName> & path)
    {
        std::vector<nix::AttrName> out;
        out.reserve(path.size());
        for (auto & a : path) {
            if (a.expr)
                out.emplace_back(expr(a.expr));            // dynamic ${e} key
            else
                out.emplace_back(es.symbols.create(a.symbol));  // static key
        }
        return out;
    }

    nix::ExprAttrs * attrs(const nix::v3::ast::Attrs * av)
    {
        namespace a = nix::v3::ast;
        auto * ea = X().add<nix::ExprAttrs>();
        ea->recursive = av->recursive;

        if (!av->inheritFromExprs.empty()) {
            ea->inheritFromExprs = std::make_unique<std::pmr::vector<nix::Expr *>>(X().alloc);
            for (auto * e : av->inheritFromExprs)
                ea->inheritFromExprs->push_back(expr(e));
        }

        for (auto & d : av->attrs) {
            nix::Symbol sym = es.symbols.create(d.name);
            nix::PosIdx p = pos(d.pos);
            switch (d.kind) {
            case a::Attrs::AttrKind::Plain:
                (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                    expr(d.value), p, nix::ExprAttrs::AttrDef::Kind::Plain);
                break;
            case a::Attrs::AttrKind::Inherited:
                // `inherit x;` => x bound to a Var of the same name (parser.y:494).
                (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                    X().add<nix::ExprVar>(p, sym), p,
                    nix::ExprAttrs::AttrDef::Kind::Inherited);
                break;
            case a::Attrs::AttrKind::InheritedFrom: {
                // `inherit (e) x;` => ExprSelect(ExprInheritFrom(displ), x),
                // displ = index into inheritFromExprs (parser.y:508).
                auto * from = X().add<nix::ExprInheritFrom>(
                    p, static_cast<nix::Displacement>(d.fromIdx));
                auto * sel = X().add<nix::ExprSelect>(X().alloc, p, from, sym);
                (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                    sel, p, nix::ExprAttrs::AttrDef::Kind::InheritedFrom);
                break;
            }
            }
        }

        for (auto & dyn : av->dynamicAttrs)
            ea->dynamicAttrs->push_back(nix::ExprAttrs::DynamicAttrDef(
                expr(dyn.nameExpr), expr(dyn.valueExpr), nix::noPos));

        return ea;
    }

    nix::Expr * expr(const nix::v3::ast::Node * n)
    {
        namespace a = nix::v3::ast;
        auto & X_ = X();
        switch (n->kind) {

        case a::Kind::Int:
            return X_.add<nix::ExprInt>(
                static_cast<nix::NixInt::Inner>(static_cast<const a::Int *>(n)->n));
        case a::Kind::Float:
            return X_.add<nix::ExprFloat>(static_cast<const a::Float *>(n)->f);
        case a::Kind::String:
            return X_.add<nix::ExprString>(X_.alloc, static_cast<const a::String *>(n)->s);
        case a::Kind::Path:
            // The v3 Path string is already resolved (abs / basePath / $HOME);
            // rootFS is the accessor for file-based eval.
            return X_.add<nix::ExprPath>(X_.alloc, es.rootFS, static_cast<const a::Path *>(n)->p);
        case a::Kind::Var:
            return X_.add<nix::ExprVar>(pos(n->pos), es.symbols.create(static_cast<const a::Var *>(n)->name));
        case a::Kind::Pos:
            return X_.add<nix::ExprPos>(pos(n->pos));

        case a::Kind::Call: {
            auto * c = static_cast<const a::Call *>(n);
            std::pmr::vector<nix::Expr *> args(X_.alloc);
            args.reserve(c->args.size());
            for (auto * arg : c->args) args.push_back(expr(arg));
            return X_.add<nix::ExprCall>(nix::noPos, expr(c->fun), std::move(args));
        }
        case a::Kind::Select: {
            auto * s = static_cast<const a::Select *>(n);
            auto path = attrPath(s->path);
            nix::Expr * def = s->def ? expr(s->def) : nullptr;
            return X_.add<nix::ExprSelect>(
                X_.alloc, nix::noPos, expr(s->e),
                std::span<const nix::AttrName>(path), def);
        }
        case a::Kind::OpHasAttr: {
            auto * h = static_cast<const a::OpHasAttr *>(n);
            auto path = attrPath(h->path);
            return X_.add<nix::ExprOpHasAttr>(
                X_.alloc, expr(h->e), std::span<nix::AttrName>(path));
        }
        case a::Kind::List: {
            auto * l = static_cast<const a::List *>(n);
            std::vector<nix::Expr *> elems;
            elems.reserve(l->elems.size());
            for (auto * e : l->elems) elems.push_back(expr(e));
            return X_.add<nix::ExprList>(X_.alloc, std::span<nix::Expr *>(elems));
        }
        case a::Kind::Lambda: {
            auto * lam = static_cast<const a::Lambda *>(n);
            if (!lam->hasFormals)
                return X_.add<nix::ExprLambda>(
                    pos(n->pos), es.symbols.create(lam->arg), expr(lam->body));
            nix::FormalsBuilder fb;
            fb.ellipsis = lam->ellipsis;
            fb.formals.reserve(lam->formals.size());
            for (auto & f : lam->formals)
                fb.formals.push_back(nix::Formal{
                    pos(f.pos), es.symbols.create(f.name), f.def ? expr(f.def) : nullptr});
            // FormalsBuilder @pre: sorted by Symbol (the ctor + has() use
            // Symbol-ordered lower_bound).  show() re-sorts lexicographically.
            std::sort(fb.formals.begin(), fb.formals.end(),
                      [](const nix::Formal & x, const nix::Formal & y) { return x.name < y.name; });
            nix::Symbol arg = lam->arg.empty() ? nix::Symbol() : es.symbols.create(lam->arg);
            return X_.add<nix::ExprLambda>(es.positions, X_.alloc, pos(n->pos), arg, fb, expr(lam->body));
        }

        case a::Kind::Attrs:
            return attrs(static_cast<const a::Attrs *>(n));
        case a::Kind::Let: {
            auto * l = static_cast<const a::Let *>(n);
            return X_.add<nix::ExprLet>(attrs(l->attrs), expr(l->body));
        }
        case a::Kind::With: {
            auto * w = static_cast<const a::With *>(n);
            return X_.add<nix::ExprWith>(pos(n->pos), expr(w->attrs), expr(w->body));
        }
        case a::Kind::If: {
            auto * i = static_cast<const a::If *>(n);
            return X_.add<nix::ExprIf>(
                nix::noPos, expr(i->cond), expr(i->then_), expr(i->else_));
        }
        case a::Kind::Assert: {
            auto * as = static_cast<const a::Assert *>(n);
            return X_.add<nix::ExprAssert>(nix::noPos, expr(as->cond), expr(as->body));
        }
        case a::Kind::OpNot:
            return X_.add<nix::ExprOpNot>(expr(static_cast<const a::OpNot *>(n)->e));
        case a::Kind::ConcatStrings: {
            auto * cs = static_cast<const a::ConcatStrings *>(n);
            std::vector<std::pair<nix::PosIdx, nix::Expr *>> es2;
            es2.reserve(cs->es.size());
            for (auto * e : cs->es) es2.emplace_back(nix::noPos, expr(e));
            return X_.add<nix::ExprConcatStrings>(
                X_.alloc, nix::noPos, cs->forceString,
                std::span<std::pair<nix::PosIdx, nix::Expr *>>(es2));
        }

        // Binary operators (the v3 node's Kind carries which one).
        case a::Kind::OpEq: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpEq>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpNEq: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpNEq>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpAnd: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpAnd>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpOr: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpOr>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpImpl: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpImpl>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpConcatLists: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpConcatLists>(nix::noPos, expr(b->lhs), expr(b->rhs)); }
        case a::Kind::OpUpdate: { auto * b = static_cast<const a::BinOp *>(n);
            return X_.add<nix::ExprOpUpdate>(nix::noPos, expr(b->lhs), expr(b->rhs)); }

        default:
            throw nix::Error("v3->nix::Expr bridge: unhandled v3 AST kind %d", (int) n->kind);
        }
    }
};

/// Convert a v3 AST root to a nix::Expr.  `po` is the PosTable origin
/// registered for the source (es.positions.addOrigin(origin, size)).
inline nix::Expr * toNixExpr(nix::EvalState & es, const nix::v3::ast::Node * n,
                             const nix::PosTable::Origin & po)
{
    Bridge b{es, po};
    return b.expr(n);
}

} // namespace nix::v3
