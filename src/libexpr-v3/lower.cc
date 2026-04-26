/// @file
/// Lower nix::Expr* AST → v3::ir::Module.
///
/// Coverage (initial): literals (int, float, bool via primops, null, string),
/// arithmetic (+, -, *), comparison, logical, if-then-else, lambda (single
/// arg, no formals), application, let (linear; no rec yet), select.
///
/// Out of scope (yet): rec, with, formals, let-rec mutual references,
/// dynamic attrs, primop calls (need primop wiring), inheritance, paths,
/// ConcatStrings.
///
/// Lowering algorithm:
///   - DFS the AST.
///   - Maintain a scope stack: each scope maps Symbol → VarId.
///   - For each AST node, emit IR bindings into the current Block; the node
///     "returns" a VarId that callers can refer to.
///   - For ExprVar with level/displ resolution, look up the appropriate
///     scope on the stack (level enters from inner; displ indexes within).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/lower.hh"
#include "v3/ir.hh"
#include "v3/primop.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"

#include <deque>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

namespace nix::v3 {

namespace {

[[noreturn]] inline void unsupported(const char * what)
{
    throw std::runtime_error(std::string("v3 lower: unsupported AST node — ") + what);
}

/// One frame on the scope stack — a per-Function map of Symbol → VarId.
/// A static-env "level" walks from this frame toward the back of the stack;
/// "displ" is the position within the frame.  We mirror nix's bindVars
/// convention: the closest enclosing env is level=0.
struct Scope
{
    /// Vars in declaration order; index = displacement.
    std::vector<ir::VarId> byDispl;
    /// Optional name lookup (rare).
    std::unordered_map<std::string, ir::VarId> byName;
};

struct Lowerer
{
    ir::Module m = ir::makeModule();
    const nix::SymbolTable & symbols;

    /// Stack of scopes, innermost at the back.
    std::vector<Scope> scopes;

    /// Current function being lowered (for nested lambdas).
    std::vector<ir::FuncId> funcStack;

    /// Block we're currently appending bindings into.
    std::vector<ir::BlockId> blockStack;

    explicit Lowerer(const nix::SymbolTable & st) : symbols(st) {}

    ir::SymbolId internSym(nix::Symbol s)
    {
        std::string_view sv = symbols[s];
        return m.internSymbol(std::string(sv));
    }

    ir::VarId addBinding(ir::Expr e)
    {
        auto v = m.freshVar();
        m.blocks[blockStack.back()].bindings.push_back({v, std::move(e)});
        return v;
    }

    void setReturn(ir::VarId v)
    {
        m.blocks[blockStack.back()].terminal = ir::TermReturn{v};
    }

    /// Returns the resolved VarId, or kInvalid if the variable lives in the
    /// base env (i.e., is a primop).  Caller should fall back to primop
    /// lookup by symbol name in the kInvalid case.
    ir::VarId resolveVar(uint32_t level, uint32_t displ)
    {
        if (level >= scopes.size()) return ir::kInvalid;
        const Scope & s = scopes[scopes.size() - 1 - level];
        if (displ >= s.byDispl.size()) return ir::kInvalid;
        return s.byDispl[displ];
    }

    /// True iff `e` is an ExprVar whose level resolves outside any user
    /// scope — i.e., it's a base-env primop reference.
    bool isPrimOpRef(nix::Expr * e, const PrimOp ** outPO = nullptr) const
    {
        if (!e || e->exprKind != nix::Expr::Kind::Var) return false;
        auto * ev = static_cast<nix::ExprVar *>(e);
        if (ev->fromWith) return false;
        if (ev->level < scopes.size()) return false;
        std::string name(symbols[ev->name]);
        const PrimOp * po = findPrimOp(name);
        if (po && outPO) *outPO = po;
        return po != nullptr;
    }

    // Top-level entry.
    ir::Module run(nix::Expr * e)
    {
        // Top-level function = functions[0].  Allocate its entry block.
        auto entry = m.freshBlock();
        m.functions[0].entryBlock = entry;
        funcStack.push_back(0);
        blockStack.push_back(entry);
        // We deliberately do NOT push an initial scope here.  The
        // staticBaseEnv (containing primops) corresponds to "level 0
        // outside any user scope" — i.e., scopes.empty() means we should
        // look up the symbol as a primop, not as a user binding.

        ir::VarId rv = lowerExpr(e);
        setReturn(rv);
        return std::move(m);
    }

    // Dispatcher.
    ir::VarId lowerExpr(nix::Expr * e)
    {
        if (!e) throw std::runtime_error("v3 lower: nullptr expr");

        switch (e->exprKind) {
        case nix::Expr::Kind::Int:    return lowerInt(static_cast<nix::ExprInt *>(e));
        case nix::Expr::Kind::Float:  return lowerFloat(static_cast<nix::ExprFloat *>(e));
        case nix::Expr::Kind::String: return lowerString(static_cast<nix::ExprString *>(e));
        case nix::Expr::Kind::Var:    return lowerVar(static_cast<nix::ExprVar *>(e));
        case nix::Expr::Kind::If:     return lowerIf(static_cast<nix::ExprIf *>(e));
        case nix::Expr::Kind::Lambda: return lowerLambda(static_cast<nix::ExprLambda *>(e));
        case nix::Expr::Kind::Call:   return lowerCall(static_cast<nix::ExprCall *>(e));
        case nix::Expr::Kind::OpEq:   return lowerBinOp(static_cast<nix::ExprOpEq *>(e),  ir::Eq{});
        case nix::Expr::Kind::OpNEq:  return lowerBinOp(static_cast<nix::ExprOpNEq *>(e), ir::NEq{});
        case nix::Expr::Kind::OpAnd:  return lowerShortCircuit(static_cast<nix::ExprOpAnd *>(e), /*kind*/0);
        case nix::Expr::Kind::OpOr:   return lowerShortCircuit(static_cast<nix::ExprOpOr  *>(e), /*kind*/1);
        case nix::Expr::Kind::OpImpl: return lowerShortCircuit(static_cast<nix::ExprOpImpl*>(e), /*kind*/2);
        case nix::Expr::Kind::OpNot:  return lowerNot(static_cast<nix::ExprOpNot *>(e));
        case nix::Expr::Kind::ConcatStrings: return lowerConcatStrings(static_cast<nix::ExprConcatStrings *>(e));
        case nix::Expr::Kind::Unknown:
        case nix::Expr::Kind::Path:
        case nix::Expr::Kind::InheritFrom:
        case nix::Expr::Kind::Select:
        case nix::Expr::Kind::OpHasAttr:
        case nix::Expr::Kind::Attrs:
        case nix::Expr::Kind::List:
        case nix::Expr::Kind::Let:
        case nix::Expr::Kind::With:
        case nix::Expr::Kind::Assert:
        case nix::Expr::Kind::OpUpdate:
        case nix::Expr::Kind::Pos:
        case nix::Expr::Kind::BlackHole:
        case nix::Expr::Kind::OpConcatLists:
        default: break;
        }
        // Many node kinds aren't supported yet.  Surface a clear message.
        unsupported(("Kind " + std::to_string(static_cast<int>(e->exprKind))).c_str());
    }

    ir::VarId lowerInt(nix::ExprInt * e)
    {
        // ExprInt's Value already stores the int.
        return addBinding(ir::LitInt{e->v.integer().value});
    }
    ir::VarId lowerFloat(nix::ExprFloat * e)
    {
        return addBinding(ir::LitFloat{e->v.fpoint()});
    }
    ir::VarId lowerString(nix::ExprString * e)
    {
        // Long-lived backing store.  std::deque never reallocates existing
        // elements on push_back, so std::string_view into the deque's
        // strings stays valid for the program's lifetime.
        static std::deque<std::string> stringPool;
        stringPool.emplace_back(e->v.string_view());
        return addBinding(ir::LitString{stringPool.back()});
    }
    ir::VarId lowerVar(nix::ExprVar * e)
    {
        if (e->fromWith) {
            auto sym = internSym(e->name);
            return addBinding(ir::WithLookup{sym, /*depth*/0});
        }
        ir::VarId v = resolveVar(e->level, e->displ);
        if (v != ir::kInvalid) return addBinding(ir::VarRef{v});

        // Base-env reference.  Three subcases:
        //   1. true / false / null — literal constants
        //   2. a primop (must currently appear in a call site)
        //   3. unbound — error
        std::string name(symbols[e->name]);
        if (name == "true")  return addBinding(ir::LitBool{true});
        if (name == "false") return addBinding(ir::LitBool{false});
        if (name == "null")  return addBinding(ir::LitNull{});
        if (findPrimOp(name)) {
            throw std::runtime_error(
                "v3 lower: bare primop reference '" + name +
                "' (only direct calls supported in bring-up)");
        }
        throw std::runtime_error("v3 lower: unbound variable '" + name + "'");
    }
    ir::VarId lowerIf(nix::ExprIf * e)
    {
        ir::VarId cond = lowerExpr(e->cond);

        auto thenB = m.freshBlock();
        auto elseB = m.freshBlock();
        // Lower the then branch in its own block.
        blockStack.push_back(thenB);
        ir::VarId thenV = lowerExpr(e->then);
        setReturn(thenV);
        blockStack.pop_back();
        // Lower the else branch.
        blockStack.push_back(elseB);
        ir::VarId elseV = lowerExpr(e->else_);
        setReturn(elseV);
        blockStack.pop_back();

        return addBinding(ir::If{cond, thenB, elseB});
    }
    ir::VarId lowerLambda(nix::ExprLambda * e)
    {
        if (e->getFormals().has_value())
            unsupported("ExprLambda with formals");

        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();

        ir::VarId param = m.freshVar();
        auto sym = internSym(e->arg);
        std::string argName(symbols[e->arg]);

        m.functions[fid].entryBlock = entry;
        m.functions[fid].argName    = sym;
        m.functions[fid].paramVar   = param;
        m.functions[fid].name       = argName;

        Scope inner;
        inner.byDispl.push_back(param);
        inner.byName.emplace(argName, param);
        scopes.push_back(std::move(inner));
        funcStack.push_back(fid);
        blockStack.push_back(entry);

        ir::VarId rv = lowerExpr(e->body);
        setReturn(rv);

        blockStack.pop_back();
        funcStack.pop_back();
        scopes.pop_back();

        return addBinding(ir::Lambda{ fid, /*freeVars*/ {} });
    }
    ir::VarId lowerCall(nix::ExprCall * e)
    {
        if (!e->args.has_value()) unsupported("ExprCall without args");
        // Direct primop call: callee is a base-env var that names a primop.
        const PrimOp * po = nullptr;
        if (isPrimOpRef(e->fun, &po) && e->args->size() == po->arity) {
            std::vector<ir::VarId> args;
            args.reserve(po->arity);
            for (auto * a : *e->args) args.push_back(lowerExpr(a));
            return addBinding(ir::PrimOpCall{po, std::move(args)});
        }
        // Generic application via OP_CALL.
        ir::VarId f = lowerExpr(e->fun);
        for (auto * a : *e->args) {
            ir::VarId av = lowerExpr(a);
            f = addBinding(ir::App{f, av});
        }
        return f;
    }
    ir::VarId lowerNot(nix::ExprOpNot * e)
    {
        return addBinding(ir::Not{lowerExpr(e->e)});
    }
    ir::VarId lowerConcatStrings(nix::ExprConcatStrings * e)
    {
        std::vector<ir::VarId> parts;
        parts.reserve(e->es.size());
        for (auto & p : e->es) parts.push_back(lowerExpr(p.second));
        return addBinding(ir::ConcatStrings{std::move(parts), e->forceString});
    }
    template<class AstNode, class IRNode>
    ir::VarId lowerBinOp(AstNode * e, IRNode)
    {
        ir::VarId a = lowerExpr(e->e1);
        ir::VarId b = lowerExpr(e->e2);
        IRNode op{a, b};
        return addBinding(op);
    }
    template<class AstNode>
    ir::VarId lowerShortCircuit(AstNode * e, int kind /*0=And, 1=Or, 2=Impl*/)
    {
        ir::VarId lhs = lowerExpr(e->e1);
        auto rhsB = m.freshBlock();
        blockStack.push_back(rhsB);
        ir::VarId rhsV = lowerExpr(e->e2);
        setReturn(rhsV);
        blockStack.pop_back();
        switch (kind) {
        case 0: return addBinding(ir::And { lhs, rhsB });
        case 1: return addBinding(ir::Or  { lhs, rhsB });
        case 2: return addBinding(ir::Impl{ lhs, rhsB });
        }
        throw std::logic_error("v3 lower: unknown short-circuit kind");
    }
};

} // namespace

ir::Module lowerNixExpr(nix::Expr * e, const nix::SymbolTable & symbols)
{
    Lowerer L(symbols);
    return L.run(e);
}

} // namespace nix::v3
