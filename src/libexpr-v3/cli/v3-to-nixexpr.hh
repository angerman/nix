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
/// Correctness is checkable by `show()` byte-equality: the constructed
/// `nix::Expr` must `show()` identically to TW's parser (which equals
/// the v3 AST `show()`, already proven byte-equal to TW on 547/547
/// nixpkgs files).  Any construction bug surfaces immediately on the
/// real-file sweep.
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

/// Convert one v3 AST node to a `nix::Expr` allocated in `es.mem.exprs`.
/// Positions are `noPos` (the v3 parser does not yet track file-local
/// spans — a separate Stage 2 item); `bindVars` + eval do not require
/// positions, only error messages degrade.
inline nix::Expr * toNixExpr(nix::EvalState & es, const nix::v3::ast::Node * n);

/// Convert a v3 attr-path (static symbol XOR dynamic `${expr}` element)
/// to a `nix::AttrName` vector.
inline std::vector<nix::AttrName>
toNixAttrPath(nix::EvalState & es, const std::vector<nix::v3::ast::AttrName> & path)
{
    std::vector<nix::AttrName> out;
    out.reserve(path.size());
    for (auto & a : path) {
        if (a.expr)
            out.emplace_back(toNixExpr(es, a.expr));         // dynamic key
        else
            out.emplace_back(es.symbols.create(a.symbol));   // static key
    }
    return out;
}

/// Convert a v3 Attrs node to a `nix::ExprAttrs` (also used for `let`).
inline nix::ExprAttrs * toNixAttrs(nix::EvalState & es, const nix::v3::ast::Attrs * av)
{
    namespace a = nix::v3::ast;
    auto & X = es.mem.exprs;
    auto * ea = X.add<nix::ExprAttrs>();
    ea->recursive = av->recursive;

    // inherit-from source exprs (ExprInheritFrom::displ indexes this).
    if (!av->inheritFromExprs.empty()) {
        ea->inheritFromExprs = std::make_unique<std::pmr::vector<nix::Expr *>>(X.alloc);
        for (auto * e : av->inheritFromExprs)
            ea->inheritFromExprs->push_back(toNixExpr(es, e));
    }

    for (auto & d : av->attrs) {
        nix::Symbol sym = es.symbols.create(d.name);
        switch (d.kind) {
        case a::Attrs::AttrKind::Plain:
            (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                toNixExpr(es, d.value), nix::noPos, nix::ExprAttrs::AttrDef::Kind::Plain);
            break;
        case a::Attrs::AttrKind::Inherited:
            // `inherit x;` => x bound to a Var of the same name (parser.y:494).
            (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                X.add<nix::ExprVar>(nix::noPos, sym), nix::noPos,
                nix::ExprAttrs::AttrDef::Kind::Inherited);
            break;
        case a::Attrs::AttrKind::InheritedFrom: {
            // `inherit (e) x;` => ExprSelect(ExprInheritFrom(displ), x),
            // displ = index into inheritFromExprs (parser.y:508).
            auto * from = X.add<nix::ExprInheritFrom>(
                nix::noPos, static_cast<nix::Displacement>(d.fromIdx));
            auto * sel = X.add<nix::ExprSelect>(X.alloc, nix::noPos, from, sym);
            (*ea->attrs)[sym] = nix::ExprAttrs::AttrDef(
                sel, nix::noPos, nix::ExprAttrs::AttrDef::Kind::InheritedFrom);
            break;
        }
        }
    }

    for (auto & dyn : av->dynamicAttrs)
        ea->dynamicAttrs->push_back(nix::ExprAttrs::DynamicAttrDef(
            toNixExpr(es, dyn.nameExpr), toNixExpr(es, dyn.valueExpr), nix::noPos));

    return ea;
}

inline nix::Expr * toNixExpr(nix::EvalState & es, const nix::v3::ast::Node * n)
{
    namespace a = nix::v3::ast;
    auto & X = es.mem.exprs;
    switch (n->kind) {

    case a::Kind::Int:
        return X.add<nix::ExprInt>(
            static_cast<nix::NixInt::Inner>(static_cast<const a::Int *>(n)->n));
    case a::Kind::Float:
        return X.add<nix::ExprFloat>(static_cast<const a::Float *>(n)->f);
    case a::Kind::String:
        return X.add<nix::ExprString>(X.alloc, static_cast<const a::String *>(n)->s);
    case a::Kind::Path:
        // The v3 Path string is already resolved (abs / basePath / $HOME);
        // rootFS is the accessor for file-based eval.
        return X.add<nix::ExprPath>(X.alloc, es.rootFS, static_cast<const a::Path *>(n)->p);
    case a::Kind::Var:
        return X.add<nix::ExprVar>(nix::noPos, es.symbols.create(static_cast<const a::Var *>(n)->name));
    case a::Kind::Pos:
        return X.add<nix::ExprPos>(nix::noPos);

    case a::Kind::Call: {
        auto * c = static_cast<const a::Call *>(n);
        std::pmr::vector<nix::Expr *> args(X.alloc);
        args.reserve(c->args.size());
        for (auto * arg : c->args) args.push_back(toNixExpr(es, arg));
        return X.add<nix::ExprCall>(nix::noPos, toNixExpr(es, c->fun), std::move(args));
    }
    case a::Kind::Select: {
        auto * s = static_cast<const a::Select *>(n);
        auto path = toNixAttrPath(es, s->path);
        nix::Expr * def = s->def ? toNixExpr(es, s->def) : nullptr;
        return X.add<nix::ExprSelect>(
            X.alloc, nix::noPos, toNixExpr(es, s->e),
            std::span<const nix::AttrName>(path), def);
    }
    case a::Kind::OpHasAttr: {
        auto * h = static_cast<const a::OpHasAttr *>(n);
        auto path = toNixAttrPath(es, h->path);
        return X.add<nix::ExprOpHasAttr>(
            X.alloc, toNixExpr(es, h->e), std::span<nix::AttrName>(path));
    }
    case a::Kind::List: {
        auto * l = static_cast<const a::List *>(n);
        std::vector<nix::Expr *> elems;
        elems.reserve(l->elems.size());
        for (auto * e : l->elems) elems.push_back(toNixExpr(es, e));
        return X.add<nix::ExprList>(X.alloc, std::span<nix::Expr *>(elems));
    }
    case a::Kind::Lambda: {
        auto * lam = static_cast<const a::Lambda *>(n);
        if (!lam->hasFormals)
            return X.add<nix::ExprLambda>(
                nix::noPos, es.symbols.create(lam->arg), toNixExpr(es, lam->body));
        nix::FormalsBuilder fb;
        fb.ellipsis = lam->ellipsis;
        fb.formals.reserve(lam->formals.size());
        for (auto & f : lam->formals)
            fb.formals.push_back(nix::Formal{
                nix::noPos, es.symbols.create(f.name), f.def ? toNixExpr(es, f.def) : nullptr});
        // FormalsBuilder @pre: sorted by Symbol (the ctor + has() use
        // Symbol-ordered lower_bound).  show() re-sorts lexicographically.
        std::sort(fb.formals.begin(), fb.formals.end(),
                  [](const nix::Formal & x, const nix::Formal & y) { return x.name < y.name; });
        nix::Symbol arg = lam->arg.empty() ? nix::Symbol() : es.symbols.create(lam->arg);
        return X.add<nix::ExprLambda>(es.positions, X.alloc, nix::noPos, arg, fb, toNixExpr(es, lam->body));
    }

    case a::Kind::Attrs:
        return toNixAttrs(es, static_cast<const a::Attrs *>(n));
    case a::Kind::Let: {
        auto * l = static_cast<const a::Let *>(n);
        return X.add<nix::ExprLet>(toNixAttrs(es, l->attrs), toNixExpr(es, l->body));
    }
    case a::Kind::With: {
        auto * w = static_cast<const a::With *>(n);
        return X.add<nix::ExprWith>(nix::noPos, toNixExpr(es, w->attrs), toNixExpr(es, w->body));
    }
    case a::Kind::If: {
        auto * i = static_cast<const a::If *>(n);
        return X.add<nix::ExprIf>(
            nix::noPos, toNixExpr(es, i->cond), toNixExpr(es, i->then_), toNixExpr(es, i->else_));
    }
    case a::Kind::Assert: {
        auto * as = static_cast<const a::Assert *>(n);
        return X.add<nix::ExprAssert>(nix::noPos, toNixExpr(es, as->cond), toNixExpr(es, as->body));
    }
    case a::Kind::OpNot:
        return X.add<nix::ExprOpNot>(toNixExpr(es, static_cast<const a::OpNot *>(n)->e));
    case a::Kind::ConcatStrings: {
        auto * cs = static_cast<const a::ConcatStrings *>(n);
        std::vector<std::pair<nix::PosIdx, nix::Expr *>> es2;
        es2.reserve(cs->es.size());
        for (auto * e : cs->es) es2.emplace_back(nix::noPos, toNixExpr(es, e));
        return X.add<nix::ExprConcatStrings>(
            X.alloc, nix::noPos, cs->forceString,
            std::span<std::pair<nix::PosIdx, nix::Expr *>>(es2));
    }

    // Binary operators (the v3 node's Kind carries which one).
    case a::Kind::OpEq: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpEq>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpNEq: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpNEq>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpAnd: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpAnd>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpOr: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpOr>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpImpl: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpImpl>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpConcatLists: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpConcatLists>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }
    case a::Kind::OpUpdate: { auto * b = static_cast<const a::BinOp *>(n);
        return X.add<nix::ExprOpUpdate>(nix::noPos, toNixExpr(es, b->lhs), toNixExpr(es, b->rhs)); }

    default:
        throw nix::Error("v3->nix::Expr bridge: unhandled v3 AST kind %d", (int) n->kind);
    }
}

} // namespace nix::v3
