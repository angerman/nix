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

#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace nix::v3 {

/// Whole-program gate: true iff `n`'s entire subtree uses only
/// Phase-1-supported constructs (so the native lowerer handles it; else
/// the caller falls back to the bridge).  Conservative by construction.
inline bool canLowerV3(const nix::v3::ast::Node * n)
{
    namespace a = nix::v3::ast;
    if (!n) return false;
    switch (n->kind) {
    case a::Kind::Int:
    case a::Kind::Float:
    case a::Kind::String:
    case a::Kind::Var:
        return true;
    case a::Kind::Lambda: {
        // Phase 2a: single-arg only.  Formals (Phase 2b) → bridge.
        auto * lam = static_cast<const a::Lambda *>(n);
        if (lam->hasFormals) return false;
        return canLowerV3(lam->body);
    }
    case a::Kind::Call: {
        auto * c = static_cast<const a::Call *>(n);
        if (!canLowerV3(c->fun)) return false;
        for (auto * arg : c->args) {
            if (!canLowerV3(arg)) return false;
            // Trivial-for-lazy: a non-thunked arg must be a literal /
            // var / lambda / primop-call (no control flow that would
            // need a lazy thunk).  Phase 2b lifts this once thunkify
            // lands.
            switch (arg->kind) {
            case a::Kind::Int: case a::Kind::Float: case a::Kind::String:
            case a::Kind::Var: case a::Kind::Lambda: case a::Kind::Call: break;
            default: return false;
            }
        }
        return true;
    }
    case a::Kind::If: {
        auto * i = static_cast<const a::If *>(n);
        return canLowerV3(i->cond) && canLowerV3(i->then_) && canLowerV3(i->else_);
    }
    case a::Kind::OpEq: case a::Kind::OpNEq:
    case a::Kind::OpUpdate: case a::Kind::OpConcatLists: {
        auto * b = static_cast<const a::BinOp *>(n);
        return canLowerV3(b->lhs) && canLowerV3(b->rhs);
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
        return false;  // Lambda/Let/Attrs/Select/With/Path/List/&&/||/->/... -> bridge
    }
}

struct LowererV3 {
    ir::Module m = ir::makeModule();
    const nix::SymbolTable & symbols;  // unused (names are inline in the v3 AST)
    std::vector<ir::BlockId> blockStack;

    /// Lexical scope stack (innermost at the back), name → VarId.  A Var
    /// resolves against this (innermost-first); cross-function refs become
    /// upvalues — emit's computeFreeVars derives those from the IR, so the
    /// lowerer only needs correct VarRefs (freeVars passed empty).
    struct Scope { std::map<std::string, ir::VarId> byName; };
    std::vector<Scope> scopes;

    explicit LowererV3(const nix::SymbolTable & symbols) : symbols(symbols) {}

    ir::VarId addBinding(ir::Expr e)
    {
        auto v = m.freshVar();
        m.blocks[blockStack.back()].bindings.push_back({v, std::move(e)});
        return v;
    }
    void setReturn(ir::VarId v) { m.blocks[blockStack.back()].terminal = ir::TermReturn{v}; }
    ir::VarId forceVal(ir::VarId v) { return addBinding(ir::Force{v}); }

    /// Resolve a Var: lexical scope (innermost-first) → VarRef; else the
    /// base env (literal const / primop / builtins); else unbound error
    /// (mirrors lower.cc::lowerVar).
    ir::VarId lowerVar(const nix::v3::ast::Var * v)
    {
        const std::string & name = v->name;
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->byName.find(name);
            if (f != it->byName.end()) return addBinding(ir::VarRef{f->second});
        }
        if (name == "true")  return addBinding(ir::LitBool{true});
        if (name == "false") return addBinding(ir::LitBool{false});
        if (name == "null")  return addBinding(ir::LitNull{});
        if (name == "builtins") {
            ir::VarId bv = addBinding(ir::LitBuiltins{});
            m.litBuiltinsVarIds.push_back(bv);
            return bv;
        }
        if (name == "__curPos")
            throw std::runtime_error("v3 native lower: __curPos unsupported in Phase 1");
        if (const PrimOp * po = findPrimOp(name)) {
            if (po->arity == 0) return addBinding(ir::PrimOpCall{po, {}});
            return addBinding(ir::LitPrimOp{po});
        }
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

        case a::Kind::Call: {
            auto * c = static_cast<const a::Call *>(n);
            // App-chain (handles primops via LitPrimOp + OP_CALL).  Args
            // are Phase-1-trivial (canLowerV3 guarantees), so no thunk.
            ir::VarId f = lowerExpr(c->fun);
            for (auto * arg : c->args)
                f = addBinding(ir::App{f, lowerExpr(arg)});
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
        default:
            throw std::runtime_error("v3 native lower: Phase-1-unsupported kind "
                                     + std::to_string((int) n->kind));
        }
    }

    /// Phase 2a: single-arg lambda (no formals — canLowerV3 rejects
    /// those).  Sub-func with the param bound by name; body lowered in
    /// its own scope/block; emit computes upvalues.  No intrinsics
    /// (Phase 4) — a single-arg lambda with no `let`/attrs body can't
    /// form a Fix/Extends/Compose shape, so intrinsicKind=0 is correct.
    ir::VarId lowerLambda(const nix::v3::ast::Lambda * lam)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();
        ir::VarId param = m.freshVar();
        m.functions[fid].entryBlock = entry;
        m.functions[fid].paramVar   = param;
        m.functions[fid].argName    = m.internSymbol(lam->arg);
        m.functions[fid].name       = lam->arg;

        Scope inner;
        inner.byName.emplace(lam->arg, param);
        scopes.push_back(std::move(inner));
        blockStack.push_back(entry);
        ir::VarId rv = lowerExpr(lam->body);
        setReturn(rv);
        blockStack.pop_back();
        scopes.pop_back();

        return addBinding(ir::Lambda{fid, /*freeVars*/ {}, /*lexicalWiths*/ {}});
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
/// canLowerV3(e) is true.
inline ir::Module lowerV3Ast(const nix::SymbolTable & symbols, const nix::v3::ast::Node * e)
{
    LowererV3 L(symbols);
    return L.run(e);
}

} // namespace nix::v3
