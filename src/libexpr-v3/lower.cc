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

/// One frame on the scope stack.  A static-env "level" walks from this
/// frame toward the back of the stack; "displ" is the position within
/// the frame.  We mirror nix's bindVars convention: closest enclosing
/// env is level=0.
///
/// Two flavors:
///   - Regular scope: byDispl[displ] is a VarId (params, lambda
///     argName, simple let bindings).
///   - Rec scope: recAttrsVar is a VarId holding the rec attrset, and
///     recAttrsNames[displ] is the SymbolId for that displ.  When a
///     binding lookup hits a rec slot (byDispl[displ] == kInvalid),
///     resolve via AttrSelect(recAttrsVar, recAttrsNames[displ]) +
///     Force.  Used for `let ... in body` (let-rec) so mutual
///     references work.
struct Scope
{
    std::vector<ir::VarId> byDispl;
    std::unordered_map<std::string, ir::VarId> byName;

    ir::VarId recAttrsVar = ir::kInvalid;
    std::vector<ir::SymbolId> recAttrsNames;
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

    /// Stack of inheritFromExprs for the enclosing ExprAttrs / ExprLet
    /// constructs that have any.  ExprInheritFrom resolves its displ
    /// against the topmost entry — they never nest in practice (the
    /// parser only synthesizes them inside an ExprAttrs/ExprLet that
    /// owns the inheritFromExprs vector), so a simple stack suffices.
    std::vector<std::pmr::vector<nix::Expr *> *> inheritFromStack;

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

    /// Resolve a (level, displ) pair to an IR VarId.  Returns kInvalid if
    /// the variable lives in the base env (i.e., is a primop) — caller
    /// should fall back to primop lookup by symbol name.
    ///
    /// For rec scopes, this emits AttrSelect+Force on the rec attrset
    /// and returns the forced value's VarId.
    ir::VarId resolveVar(uint32_t level, uint32_t displ)
    {
        if (level >= scopes.size()) return ir::kInvalid;
        size_t scopeIdx = scopes.size() - 1 - level;
        if (displ < scopes[scopeIdx].byDispl.size() &&
            scopes[scopeIdx].byDispl[displ] != ir::kInvalid)
            return scopes[scopeIdx].byDispl[displ];
        // Rec slot.
        if (scopes[scopeIdx].recAttrsVar != ir::kInvalid &&
            displ < scopes[scopeIdx].recAttrsNames.size())
        {
            ir::VarId rec = scopes[scopeIdx].recAttrsVar;
            ir::SymbolId nm = scopes[scopeIdx].recAttrsNames[displ];
            ir::VarId sel = addBinding(ir::AttrSelect{rec, nm});
            return addBinding(ir::Force{sel});
        }
        return ir::kInvalid;
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

    /// True iff `e` is `builtins.<name>` where <name> is a known primop
    /// and `builtins` resolves to the base env (the standard meaning).
    bool isBuiltinsPrimOp(nix::Expr * e, const PrimOp ** outPO = nullptr) const
    {
        if (!e || e->exprKind != nix::Expr::Kind::Select) return false;
        auto * sel = static_cast<nix::ExprSelect *>(e);
        if (sel->def) return false;
        auto path = sel->getAttrPath();
        if (path.size() != 1 || path[0].expr) return false;
        if (sel->e->exprKind != nix::Expr::Kind::Var) return false;
        auto * ev = static_cast<nix::ExprVar *>(sel->e);
        if (ev->fromWith) return false;
        if (ev->level < scopes.size()) return false;
        if (std::string(symbols[ev->name]) != "builtins") return false;
        std::string name(symbols[path[0].symbol]);
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
        case nix::Expr::Kind::Path:   return lowerPath(static_cast<nix::ExprPath *>(e));
        case nix::Expr::Kind::Var:    return lowerVar(static_cast<nix::ExprVar *>(e));
        case nix::Expr::Kind::If:     return lowerIf(static_cast<nix::ExprIf *>(e));
        case nix::Expr::Kind::Lambda: return lowerLambda(static_cast<nix::ExprLambda *>(e));
        case nix::Expr::Kind::Call:   return lowerCall(static_cast<nix::ExprCall *>(e));
        case nix::Expr::Kind::Let:    return lowerLet (static_cast<nix::ExprLet  *>(e));
        case nix::Expr::Kind::List:   return lowerList(static_cast<nix::ExprList *>(e));
        case nix::Expr::Kind::Attrs:  return lowerAttrs(static_cast<nix::ExprAttrs *>(e));
        case nix::Expr::Kind::Select: return lowerSelect(static_cast<nix::ExprSelect *>(e));
        case nix::Expr::Kind::OpHasAttr: return lowerHasAttr(static_cast<nix::ExprOpHasAttr *>(e));
        case nix::Expr::Kind::OpUpdate: return lowerBinOp(static_cast<nix::ExprOpUpdate *>(e), ir::Update{});
        case nix::Expr::Kind::OpConcatLists: return lowerBinOp(static_cast<nix::ExprOpConcatLists *>(e), ir::ConcatLists{});
        case nix::Expr::Kind::Assert: return lowerAssert(static_cast<nix::ExprAssert *>(e));
        case nix::Expr::Kind::With:   return lowerWith(static_cast<nix::ExprWith *>(e));
        case nix::Expr::Kind::OpEq:   return lowerBinOp(static_cast<nix::ExprOpEq *>(e),  ir::Eq{});
        case nix::Expr::Kind::OpNEq:  return lowerBinOp(static_cast<nix::ExprOpNEq *>(e), ir::NEq{});
        case nix::Expr::Kind::OpAnd:  return lowerShortCircuit(static_cast<nix::ExprOpAnd *>(e), /*kind*/0);
        case nix::Expr::Kind::OpOr:   return lowerShortCircuit(static_cast<nix::ExprOpOr  *>(e), /*kind*/1);
        case nix::Expr::Kind::OpImpl: return lowerShortCircuit(static_cast<nix::ExprOpImpl*>(e), /*kind*/2);
        case nix::Expr::Kind::OpNot:  return lowerNot(static_cast<nix::ExprOpNot *>(e));
        case nix::Expr::Kind::ConcatStrings: return lowerConcatStrings(static_cast<nix::ExprConcatStrings *>(e));
        case nix::Expr::Kind::InheritFrom: return lowerInheritFrom(static_cast<nix::ExprInheritFrom *>(e));
        case nix::Expr::Kind::Unknown:
        case nix::Expr::Kind::Pos:
        case nix::Expr::Kind::BlackHole:
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
    ir::VarId lowerPath(nix::ExprPath * e)
    {
        static std::deque<std::string> pathPool;
        pathPool.emplace_back(e->v.pathStrView());
        // accessor lifetime: tied to ExprPath::accessor, anchored in the
        // AST arena.
        return addBinding(ir::LitPath{pathPool.back(), nullptr});
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
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();

        ir::VarId param = m.freshVar();
        m.functions[fid].entryBlock = entry;
        m.functions[fid].paramVar   = param;
        m.functions[fid].name       = e->arg ? std::string(symbols[e->arg]) : "<formals>";

        Scope inner;

        if (auto formals = e->getFormals()) {
            // Param is the attrset.  Formals get extracted from it.
            m.functions[fid].argName    = e->arg ? internSym(e->arg) : ir::kInvalidSymbol;
            m.functions[fid].hasFormals = true;
            m.functions[fid].ellipsis   = formals->ellipsis;
            // Record formals for builtins.functionArgs introspection.
            m.functions[fid].formals.reserve(formals->formals.size());
            for (auto & fm : formals->formals) {
                ir::Formal ifm;
                ifm.name = internSym(fm.name);
                // We don't yet propagate default-block IDs through;
                // for functionArgs we only need the has-default flag.
                ifm.defaultBlock = fm.def ? 1u : ir::kInvalidBlock;
                m.functions[fid].formals.push_back(ifm);
            }

            funcStack.push_back(fid);
            blockStack.push_back(entry);

            // Order in newEnv before sort: arg (if any), then formals in
            // declaration order; displ assigned 0..N.  We mirror that.
            if (e->arg) {
                inner.byDispl.push_back(param);
                inner.byName.emplace(std::string(symbols[e->arg]), param);
            }
            for (auto & f : formals->formals) {
                ir::VarId v;
                ir::SymbolId nm = internSym(f.name);
                if (f.def) {
                    // if (param ? f.name) then param.f.name else default
                    ir::VarId hasIt = addBinding(ir::HasAttr{param, nm});
                    auto thenB = m.freshBlock();
                    auto elseB = m.freshBlock();
                    blockStack.push_back(thenB);
                    ir::VarId got = addBinding(ir::AttrSelect{param, nm});
                    setReturn(got);
                    blockStack.pop_back();
                    blockStack.push_back(elseB);
                    // Default expressions are evaluated in the new env (formals scope),
                    // so we evaluate them after the scope is set up below.
                    // For now, lower them with the partial scope (best-effort).
                    scopes.push_back(inner);
                    ir::VarId defv = lowerExpr(f.def);
                    setReturn(defv);
                    scopes.pop_back();
                    blockStack.pop_back();
                    v = addBinding(ir::If{hasIt, thenB, elseB});
                } else {
                    v = addBinding(ir::AttrSelect{param, nm});
                }
                inner.byDispl.push_back(v);
                inner.byName.emplace(std::string(symbols[f.name]), v);
            }

            scopes.push_back(std::move(inner));

            ir::VarId rv = lowerExpr(e->body);
            setReturn(rv);

            scopes.pop_back();
            blockStack.pop_back();
            funcStack.pop_back();
        } else {
            // Plain `x: body` — no formals.
            m.functions[fid].argName    = internSym(e->arg);
            std::string argName(symbols[e->arg]);

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
        }

        return addBinding(ir::Lambda{ fid, /*freeVars*/ {} });
    }
    ir::VarId lowerCall(nix::ExprCall * e)
    {
        if (!e->args.has_value()) unsupported("ExprCall without args");
        // Direct primop call (1): callee is a base-env var that names a primop.
        const PrimOp * po = nullptr;
        if ((isPrimOpRef(e->fun, &po) || isBuiltinsPrimOp(e->fun, &po))
            && e->args->size() == po->arity)
        {
            // tryEval needs its arg evaluated lazily (the whole point is
            // to catch errors raised during forcing).  Wrap the arg in a
            // MkThunk that defers its evaluation until tryEval forces it.
            const bool isTryEval = std::string_view(po->name) == "tryEval";

            std::vector<ir::VarId> args;
            args.reserve(po->arity);
            for (auto * a : *e->args) {
                if (isTryEval) {
                    args.push_back(thunkify(a));
                } else {
                    args.push_back(lowerExpr(a));
                }
            }
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

    /// Lower an Expr into a separate thunk-body Function and emit a
    /// MkThunk binding that, when forced, evaluates the expression in
    /// the captured-upvalues context.  Used to defer evaluation for
    /// primops with lazy argument semantics (e.g., tryEval).
    ir::VarId thunkify(nix::Expr * e)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();
        m.functions[fid].entryBlock = entry;
        m.functions[fid].name = "<thunk>";

        funcStack.push_back(fid);
        blockStack.push_back(entry);
        ir::VarId rv = lowerExpr(e);
        setReturn(rv);
        blockStack.pop_back();
        funcStack.pop_back();

        return addBinding(ir::MkThunk{fid, /*freeVars*/ {}});
    }
    ir::VarId lowerNot(nix::ExprOpNot * e)
    {
        return addBinding(ir::Not{lowerExpr(e->e)});
    }

    /// ExprInheritFrom — synthesized by the parser to represent the
    /// `from` part of `inherit (from) name1 name2 ...`.  Each AttrDef
    /// emitted by such a clause has its def.e set to ExprSelect(this,
    /// name).  The `displ` field is an index into the enclosing
    /// ExprAttrs/ExprLet's inheritFromExprs vector — we look it up via
    /// inheritFromStack which the lowerer maintains while traversing
    /// ExprAttrs / ExprLet nodes.
    ir::VarId lowerInheritFrom(nix::ExprInheritFrom * e)
    {
        if (inheritFromStack.empty())
            unsupported("ExprInheritFrom outside of an inheritFromExprs scope");
        auto * fromExprs = inheritFromStack.back();
        if (!fromExprs || e->displ >= fromExprs->size())
            unsupported("ExprInheritFrom: displ out of range");
        return lowerExpr((*fromExprs)[e->displ]);
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
    /// `let`: full mutual-recursion via the env-carrier pattern.
    ///
    /// Each binding becomes a Thunk inside a rec attrset.  References
    /// from sibling thunk bodies and the let body resolve to
    /// `AttrSelect(__rec, name) + Force` on the rec attrset.  Mutual
    /// recursion works because the rec attrset is allocated up-front
    /// and patched after the thunks are built (each thunk captures the
    /// same Bindings pointer, which is fully populated by the time any
    /// thunk's body runs).
    ///
    /// Inherits are bound in the parent env (per nix's bindVars).  We
    /// route them through the same rec attrset by lowering each
    /// inherit's RHS as a thunk body that references its outer var via
    /// the function's normal upvalue mechanism.
    ir::VarId lowerLet(nix::ExprLet * e)
    {
        return lowerLetRec(
            e->attrs->attrs.value(),
            e->attrs->inheritFromExprs ? e->attrs->inheritFromExprs.get() : nullptr,
            /*isRec=*/true, /*hasBody=*/true, e->body);
    }

    ir::VarId lowerList(nix::ExprList * e)
    {
        std::vector<ir::VarId> elems;
        elems.reserve(e->elems.size());
        for (auto * el : e->elems) elems.push_back(lowerExpr(el));
        return addBinding(ir::ListExpr{std::move(elems)});
    }

    /// `{ a = 1; b = 2; }` — non-recursive: each value is evaluated
    /// eagerly with the surrounding scope.
    ///
    /// `rec { a = 1; b = a + 1; }` — recursive: routed through the
    /// same env-carrier path as `let` (lowerLetOrRec).  The result is
    /// the rec attrset value.
    ir::VarId lowerAttrs(nix::ExprAttrs * e)
    {
        bool hasDyn = e->dynamicAttrs && !e->dynamicAttrs->empty();

        if (e->recursive && hasDyn)
            unsupported("recursive attrset with dynamic attrs");

        if (e->recursive) {
            return lowerLetRec(
                e->attrs.value(),
                e->inheritFromExprs ? e->inheritFromExprs.get() : nullptr,
                /*isRec=*/true,
                /*hasBody=*/false, /*body=*/nullptr);
        }

        // Non-rec attrset.  All entries (Plain / Inherited / InheritedFrom)
        // are lowered eagerly in the parent scope — there is no rec env.
        // InheritedFrom AttrDefs have def.e = ExprSelect(ExprInheritFrom,
        // name); lowerExpr handles ExprInheritFrom by looking up the
        // current inheritFromStack.
        bool pushedInheritFrom = false;
        if (e->inheritFromExprs) {
            inheritFromStack.push_back(e->inheritFromExprs.get());
            pushedInheritFrom = true;
        }

        if (hasDyn) {
            ir::AttrSetDyn dyn;
            dyn.statics.reserve(e->attrs->size());
            for (auto & kv : *e->attrs) {
                ir::VarId vv = lowerExpr(kv.second.e);
                dyn.statics.push_back({internSym(kv.first), vv});
            }
            dyn.dynamics.reserve(e->dynamicAttrs->size());
            for (auto & da : *e->dynamicAttrs) {
                ir::VarId nameV = lowerExpr(da.nameExpr);
                ir::VarId valV  = lowerExpr(da.valueExpr);
                dyn.dynamics.push_back({nameV, valV});
            }
            if (pushedInheritFrom) inheritFromStack.pop_back();
            return addBinding(std::move(dyn));
        }

        std::vector<ir::AttrSet::Entry> entries;
        for (auto & kv : *e->attrs) {
            const auto & sym = kv.first;
            const auto & def = kv.second;
            ir::VarId vv = lowerExpr(def.e);
            entries.push_back({internSym(sym), vv});
        }
        if (pushedInheritFrom) inheritFromStack.pop_back();
        return addBinding(ir::AttrSet{std::move(entries)});
    }

    /// Shared between ExprLet and ExprAttrs (both rec and non-rec when
    /// non-rec needs the env-carrier path for InheritedFrom).
    ///
    /// `isRec` tracks whether the new env should be visible while
    /// lowering inherit-from source expressions:
    ///   - ExprLet and rec attrsets: from-exprs are bound in newEnv,
    ///     so we push the rec scope before lowering.
    ///   - non-rec attrsets: from-exprs are bound in env (parent), so
    ///     we lower them with the surrounding scope only.
    ///
    /// `inheritFromExprs` is the ExprAttrs's `inherit (from) ...`
    /// sources (or nullptr / empty if none).  InheritedFrom bindings'
    /// def.e is an ExprInheritFrom whose displ indexes into this list.
    ///
    /// Returns the body's VarId when hasBody=true, otherwise the rec
    /// attrset's VarId.
    ir::VarId lowerLetRec(nix::ExprAttrs::AttrDefs & attrDefs,
                          std::pmr::vector<nix::Expr *> * inheritFromExprs,
                          bool isRec,
                          bool hasBody, nix::Expr * body)
    {
        ir::VarId recVar = m.freshVar();

        struct Pending {
            nix::Symbol sym;
            nix::ExprAttrs::AttrDef::Kind kind;
            nix::Expr * defE;
            ir::FuncId funcIdx;
            ir::BlockId entryBlock;
        };
        std::vector<Pending> pending;
        pending.reserve(attrDefs.size());

        for (auto & kv : attrDefs) {
            m.functions.emplace_back();
            ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
            auto eb = m.freshBlock();
            m.functions[fid].entryBlock = eb;
            m.functions[fid].name = std::string(symbols[kv.first]);
            pending.push_back({kv.first, kv.second.kind, kv.second.e, fid, eb});
        }

        Scope recScope;
        recScope.recAttrsVar = recVar;
        recScope.recAttrsNames.reserve(pending.size());
        for (auto & p : pending) {
            recScope.recAttrsNames.push_back(internSym(p.sym));
            recScope.byDispl.push_back(ir::kInvalid);
            recScope.byName.emplace(std::string(symbols[p.sym]), ir::kInvalid);
        }

        // Push the inheritFromExprs onto the stack so any ExprInheritFrom
        // encountered while lowering def.e resolves correctly.
        bool pushedInheritFrom = false;
        if (inheritFromExprs) {
            inheritFromStack.push_back(inheritFromExprs);
            pushedInheritFrom = true;
        }

        for (auto & p : pending) {
            funcStack.push_back(p.funcIdx);
            blockStack.push_back(p.entryBlock);
            switch (p.kind) {
            case nix::ExprAttrs::AttrDef::Kind::Plain:
            case nix::ExprAttrs::AttrDef::Kind::InheritedFrom: {
                // Plain: bound in newEnv (rec scope).
                // InheritedFrom: def.e = ExprSelect(ExprInheritFrom,
                // name); the from-expr is bound in newEnv (rec) when
                // isRec=true, else in env.
                if (isRec) scopes.push_back(recScope);
                ir::VarId rv = lowerExpr(p.defE);
                setReturn(rv);
                if (isRec) scopes.pop_back();
                break;
            }
            case nix::ExprAttrs::AttrDef::Kind::Inherited: {
                // def.e = ExprVar bound in env (parent), no rec push.
                ir::VarId rv = lowerExpr(p.defE);
                setReturn(rv);
                break;
            }
            }
            blockStack.pop_back();
            funcStack.pop_back();
        }

        if (pushedInheritFrom) inheritFromStack.pop_back();

        ir::LetRec letRec;
        letRec.recVar = recVar;
        letRec.entries.reserve(pending.size());
        for (auto & p : pending) {
            ir::LetRec::Entry en;
            en.name = internSym(p.sym);
            en.thunkBody = p.funcIdx;
            letRec.entries.push_back(std::move(en));
        }
        m.blocks[blockStack.back()].bindings.push_back(
            {recVar, std::move(letRec)});

        if (!hasBody) {
            return addBinding(ir::VarRef{recVar});
        }

        scopes.push_back(std::move(recScope));
        ir::VarId rv = lowerExpr(body);
        scopes.pop_back();
        return addBinding(ir::VarRef{rv});
    }

    /// `expr.attr.path or default` — chain of static attribute selects,
    /// each followed by an implicit Force (auto-forcing nix semantics).
    /// With a default, ANY missing attr in the chain short-circuits to
    /// the default.  We build nested if-then-else: at each level, if
    /// the current attr exists, recurse into the rest of the path; else
    /// return the default.
    ///
    /// Special case: `builtins.<name>` where <name> is a known primop
    /// emits OP_LIT_PRIMOP directly (a Tag::PrimOp value).  This allows
    /// partial application like `builtins.foldl' f nul` and use of
    /// primops as first-class values (e.g., `map builtins.toString xs`).
    ir::VarId lowerSelect(nix::ExprSelect * e)
    {
        auto path = e->getAttrPath();
        const PrimOp * po = nullptr;
        if (!e->def && path.size() == 1 && !path[0].expr
            && e->e->exprKind == nix::Expr::Kind::Var)
        {
            auto * ev = static_cast<nix::ExprVar *>(e->e);
            if (!ev->fromWith && ev->level >= scopes.size()
                && std::string(symbols[ev->name]) == "builtins")
            {
                std::string name(symbols[path[0].symbol]);
                po = findPrimOp(name);
                if (po) {
                    // 0-arity primops (currentSystem, nixVersion, etc.)
                    // are values: call directly so the access yields the
                    // constant.  Higher-arity primops produce a Tag::PrimOp
                    // value that participates in PrimOpApp.
                    if (po->arity == 0)
                        return addBinding(ir::PrimOpCall{po, {}});
                    return addBinding(ir::LitPrimOp{po});
                }
            }
        }

        ir::VarId v = lowerExpr(e->e);
        return emitSelectChain(v, path, e->def, 0);
    }

    ir::VarId emitSelectChain(ir::VarId attrs,
                              std::span<const nix::AttrName> path,
                              nix::Expr * defaultExpr,
                              size_t pathIdx)
    {
        if (pathIdx == path.size()) return attrs;
        if (path[pathIdx].expr)
            unsupported("dynamic attribute name in select");
        ir::SymbolId nm = internSym(path[pathIdx].symbol);

        if (defaultExpr) {
            ir::VarId hasIt = addBinding(ir::HasAttr{attrs, nm});
            auto thenB = m.freshBlock();
            auto elseB = m.freshBlock();

            blockStack.push_back(thenB);
            ir::VarId got = addBinding(ir::AttrSelect{attrs, nm});
            ir::VarId forced = addBinding(ir::Force{got});
            ir::VarId rest = emitSelectChain(forced, path, defaultExpr, pathIdx + 1);
            setReturn(rest);
            blockStack.pop_back();

            blockStack.push_back(elseB);
            ir::VarId defv = lowerExpr(defaultExpr);
            setReturn(defv);
            blockStack.pop_back();

            return addBinding(ir::If{hasIt, thenB, elseB});
        }

        ir::VarId v = addBinding(ir::AttrSelect{attrs, nm});
        v = addBinding(ir::Force{v});
        return emitSelectChain(v, path, nullptr, pathIdx + 1);
    }

    ir::VarId lowerHasAttr(nix::ExprOpHasAttr * e)
    {
        if (e->attrPath.size() != 1) unsupported("has-attr with multi-element path");
        auto & an = e->attrPath[0];
        if (an.expr) unsupported("dynamic attribute name in hasAttr");
        ir::VarId v = lowerExpr(e->e);
        return addBinding(ir::HasAttr{v, internSym(an.symbol)});
    }

    ir::VarId lowerAssert(nix::ExprAssert * e)
    {
        ir::VarId cond = lowerExpr(e->cond);
        auto bodyB = m.freshBlock();
        blockStack.push_back(bodyB);
        ir::VarId rv = lowerExpr(e->body);
        setReturn(rv);
        blockStack.pop_back();
        return addBinding(ir::Assert{cond, bodyB});
    }

    ir::VarId lowerWith(nix::ExprWith * e)
    {
        ir::VarId attrs = lowerExpr(e->attrs);
        auto bodyB = m.freshBlock();
        blockStack.push_back(bodyB);
        ir::VarId rv = lowerExpr(e->body);
        setReturn(rv);
        blockStack.pop_back();
        return addBinding(ir::With{attrs, bodyB});
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
