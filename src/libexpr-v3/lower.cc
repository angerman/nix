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
#include "v3/alloc.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

#include <deque>
#include <functional>
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
    const nix::PosTable * positions = nullptr;

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

    /// nix::Symbol -> ir::SymbolId interning cache (per Lowerer).
    /// Avoids the std::string allocation + global-table hashmap lookup
    /// on every lowerVar/lowerAttrs/etc. access to a repeat symbol.
    /// nix::Symbol is just a uint32_t, so the std::unordered_map keyed
    /// on its int id is cheap.
    std::unordered_map<uint32_t, ir::SymbolId> symbolCache;

    explicit Lowerer(const nix::SymbolTable & st) : symbols(st) {}
    Lowerer(const nix::SymbolTable & st, const nix::PosTable & pt)
        : symbols(st), positions(&pt) {}

    ir::SymbolId internSym(nix::Symbol s)
    {
        // Per-Lowerer cache keyed on nix::Symbol's uint32 id — repeats
        // within one lower call are common (every attrset references the
        // same field names) and the global-table fallback below allocates
        // a std::string we don't need on hits.
        uint32_t sid = s.getId();
        auto it = symbolCache.find(sid);
        if (it != symbolCache.end()) return it->second;
        std::string_view sv = symbols[s];
        ir::SymbolId id = m.internSymbol(sv);
        symbolCache.emplace(sid, id);
        return id;
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
    /// For rec scopes, returns AttrSelect on the rec attrset WITHOUT
    /// forcing.  Callers in strict contexts (arithmetic, comparison,
    /// AttrSelect of `.x`, etc.) wrap the result in Force themselves.
    /// Lazy contexts (function call arguments) leave the value as a
    /// thunk so patterns like `fix = f: let x = f x; in x;` work.
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
            return addBinding(ir::AttrSelect{rec, nm});
        }
        return ir::kInvalid;
    }

    /// Wrap a VarId in a Force if it might not be in WHNF.  Cheap:
    /// Force on a non-thunk is a no-op at runtime.
    ir::VarId forceVal(ir::VarId v) { return addBinding(ir::Force{v}); }

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

        ir::VarId rv = forceVal(lowerExpr(e));
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
        case nix::Expr::Kind::Pos:    return lowerPos(static_cast<nix::ExprPos *>(e));
        case nix::Expr::Kind::Unknown:
        case nix::Expr::Kind::BlackHole:
        default: break;
        }
        // Many node kinds aren't supported yet.  Surface a clear message.
        unsupported(("Kind " + std::to_string(static_cast<int>(e->exprKind))).c_str());
    }

    /// Build an attrset value `{ file = STR; line = N; column = N; }`
    /// for `__curPos` and other position-aware primops.
    ir::VarId lowerPosAttrs(nix::PosIdx posIdx)
    {
        if (!positions || !posIdx) {
            // No PosTable wired or no position info — emit `null`.
            return addBinding(ir::LitNull{});
        }
        auto pos = (*positions)[posIdx];
        std::string file;
        if (auto * s = std::get_if<nix::SourcePath>(&pos.origin)) {
            file = s->path.abs();
        } else if (std::holds_alternative<nix::Pos::Stdin>(pos.origin)) {
            file = "<stdin>";
        } else if (std::holds_alternative<nix::Pos::String>(pos.origin)) {
            file = "<string>";
        } else {
            file = "<unknown>";
        }
        ir::VarId fileV   = addBinding(ir::LitString{interpStr(file)});
        ir::VarId lineV   = addBinding(ir::LitInt{static_cast<int64_t>(pos.line)});
        ir::VarId columnV = addBinding(ir::LitInt{static_cast<int64_t>(pos.column)});
        std::vector<ir::AttrSet::Entry> entries;
        entries.push_back({m.internSymbol("file"),   fileV});
        entries.push_back({m.internSymbol("line"),   lineV});
        entries.push_back({m.internSymbol("column"), columnV});
        return addBinding(ir::AttrSet{std::move(entries)});
    }

    ir::VarId lowerPos(nix::ExprPos * e)
    {
        return lowerPosAttrs(e->pos);
    }

    /// Stable string pool for runtime-derived strings (positions, etc.).
    /// Mirrors lowerString's stringPool (deque never invalidates pointers).
    std::string_view interpStr(const std::string & s)
    {
        static std::deque<std::string> pool;
        pool.emplace_back(s);
        return pool.back();
    }

    /// Resolve a parser PosIdx into a handle into the global pos-snapshot
    /// pool.  Returns 0 when there's no PosTable wired or the PosIdx is
    /// missing — that handle uniformly means "no position info known".
    /// The handle is what we store on each ir::AttrSet::Entry so the VM
    /// can populate the per-attr position side-table consulted by
    /// `builtins.unsafeGetAttrPos`.
    ///
    /// We don't cache by PosIdx — measurement showed the parser
    /// assigns a distinct PosIdx to every attribute, so the cache
    /// almost never hits and the hash overhead regresses lower time
    /// by ~10%.  The bigger win lives in caching the file-origin
    /// std::string across attrs from the same source (below).
    ///
    /// Cache the std::string for the most-recently-seen origin.
    /// Within an attrset, all attrs share one origin variant +
    /// SourcePath, so the file string is identical for hundreds of
    /// consecutive calls.  A 1-entry MRU cache catches this.
    nix::Pos::Origin lastOrigin;
    std::string      lastOriginFile;
    bool             haveLastOrigin = false;

    uint32_t posIdxToHandle(nix::PosIdx posIdx)
    {
        if (!positions || !posIdx) return 0;
        auto pos = (*positions)[posIdx];
        std::string file;
        if (haveLastOrigin && pos.origin == lastOrigin) {
            file = lastOriginFile;
        } else {
            if (auto * s = std::get_if<nix::SourcePath>(&pos.origin))
                file = s->path.abs();
            else if (std::holds_alternative<nix::Pos::Stdin>(pos.origin))
                file = "<stdin>";
            else if (std::holds_alternative<nix::Pos::String>(pos.origin))
                file = "<string>";
            else
                file = "<unknown>";
            lastOrigin = pos.origin;
            lastOriginFile = file;
            haveLastOrigin = true;
        }
        return recordPosSnapshot({std::move(file),
                                   static_cast<uint32_t>(pos.line),
                                   static_cast<uint32_t>(pos.column)});
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
        if (name == "builtins") {
            // Emit a LitBuiltins opcode — the VM lazily materialises a
            // single process-wide `vBuiltins` attrset containing every
            // registered primop and reuses it on every reference.
            // Saves the per-occurrence cost of building N LitPrimOp +
            // AttrSet IR bindings, and avoids the cross-function
            // VarId-reuse trap that an IR-level cache would hit.
            return addBinding(ir::LitBuiltins{});
        }
        if (auto * po = findPrimOp(name)) {
            // Arity-0 primops behave as constants — invoke immediately
            // so e.g. `__nixPath`, `__currentSystem` yield their value.
            if (po->arity == 0)
                return addBinding(ir::PrimOpCall{po, {}});
            // Higher-arity bare reference: emit a Tag::PrimOp value
            // that callers can apply args to.
            return addBinding(ir::LitPrimOp{po});
        }
        throw std::runtime_error("v3 lower: unbound variable '" + name + "'");
    }
    ir::VarId lowerIf(nix::ExprIf * e)
    {
        ir::VarId cond = forceVal(lowerExpr(e->cond));

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
                ifm.pos = posIdxToHandle(fm.pos);
                m.functions[fid].formals.push_back(ifm);
            }

            funcStack.push_back(fid);
            blockStack.push_back(entry);

            // Route formal-extraction through a synthetic LetRec so each
            // formal becomes a thunk that captures the formals scope.
            // This makes default expressions lazy — references to other
            // formals from a default body resolve via the rec attrset
            // (AttrSelect + Force on the sibling thunk), so mutually
            // recursive defaults like `{ a ? b, b ? a }` work the same
            // way they do in tree-walker: only the formals actually
            // demanded by the body get forced; provided values short the
            // default branch entirely.

            // Reserve sym + var for each formal.
            const size_t nF = formals->formals.size();
            std::vector<ir::SymbolId> formalSyms;
            std::vector<ir::FuncId>   thunkFids;
            std::vector<ir::BlockId>  thunkEntries;
            formalSyms.reserve(nF); thunkFids.reserve(nF); thunkEntries.reserve(nF);
            for (auto & f : formals->formals) {
                formalSyms.push_back(internSym(f.name));
                m.functions.emplace_back();
                ir::FuncId tfid = static_cast<ir::FuncId>(m.functions.size() - 1);
                auto teb = m.freshBlock();
                m.functions[tfid].entryBlock = teb;
                m.functions[tfid].name = std::string(symbols[f.name]);
                thunkFids.push_back(tfid);
                thunkEntries.push_back(teb);
            }

            ir::VarId formalsRec = m.freshVar();

            // Build a single scope holding both @arg (if any) at
            // displ 0 plus the formals as rec slots starting at the
            // appropriate offset.  Nix's bindVars assigns @arg displ
            // 0 and formals displ 1..N, so the rec scope's
            // recAttrsNames vector is padded with a sentinel for the
            // @arg slot so AttrSelect lookups land on the right
            // formal name.
            Scope recScope;
            recScope.recAttrsVar = formalsRec;
            if (e->arg) {
                recScope.byDispl.push_back(param);
                recScope.byName.emplace(std::string(symbols[e->arg]), param);
                recScope.recAttrsNames.push_back(ir::kInvalidSymbol);  // pad slot 0
            }
            for (size_t i = 0; i < nF; ++i) {
                recScope.byDispl.push_back(ir::kInvalid);
                recScope.byName.emplace(std::string(symbols[formals->formals[i].name]), ir::kInvalid);
                recScope.recAttrsNames.push_back(formalSyms[i]);
            }

            // Lower each thunk body: `if hasAttr(param, X) then param.X
            // else <default>` (or pure AttrSelect when no default).
            // The thunk body sees the recScope so default expressions
            // can reference sibling formals via AttrSelect on the rec
            // attrset (those AttrSelects yield sibling thunks; force
            // them to materialize the value).
            for (size_t i = 0; i < nF; ++i) {
                auto & f = formals->formals[i];
                ir::SymbolId nm = formalSyms[i];

                funcStack.push_back(thunkFids[i]);
                blockStack.push_back(thunkEntries[i]);
                scopes.push_back(recScope);

                ir::VarId paramRef   = addBinding(ir::VarRef{param});
                ir::VarId paramForced = forceVal(paramRef);

                ir::VarId rv;
                if (f.def) {
                    ir::VarId hasIt = addBinding(ir::HasAttr{paramForced, nm});
                    auto thenB = m.freshBlock();
                    auto elseB = m.freshBlock();
                    blockStack.push_back(thenB);
                    ir::VarId got = addBinding(ir::AttrSelect{paramForced, nm});
                    setReturn(got);
                    blockStack.pop_back();
                    blockStack.push_back(elseB);
                    ir::VarId defv = lowerExpr(f.def);
                    setReturn(defv);
                    blockStack.pop_back();
                    rv = addBinding(ir::If{hasIt, thenB, elseB});
                } else {
                    rv = addBinding(ir::AttrSelect{paramForced, nm});
                }
                setReturn(rv);

                scopes.pop_back();
                blockStack.pop_back();
                funcStack.pop_back();
            }

            // Now build the LetRec binding inside the lambda body.  Each
            // entry's thunkBody is the function we just lowered.
            ir::LetRec letRec;
            letRec.recVar = formalsRec;
            letRec.entries.reserve(nF);
            for (size_t i = 0; i < nF; ++i) {
                ir::LetRec::Entry en;
                en.name = formalSyms[i];
                en.thunkBody = thunkFids[i];
                letRec.entries.push_back(std::move(en));
            }
            m.blocks[blockStack.back()].bindings.push_back({formalsRec, std::move(letRec)});

            // Lower the body with the rec scope so formal references
            // resolve via the rec attrset (and @arg, if any, via
            // recScope.byDispl[0]).
            scopes.push_back(recScope);
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
        // Handle both exact-arity and extra-args (`(import path) attrSet`
        // parses as one ExprCall with two args; we emit the primop call
        // for the first `arity` args, then App-chain the remainder).
        const PrimOp * po = nullptr;
        if ((isPrimOpRef(e->fun, &po) || isBuiltinsPrimOp(e->fun, &po))
            && e->args->size() >= po->arity)
        {
            const std::string_view name(po->name);

            // Fast path: arithmetic / comparison primops (`a * b`,
            // `a < b`, ... lower as ExprCall(__mul, a, b) etc.).  Emit
            // direct VM ops instead of OP_CALL_PRIMOP.  Saves the
            // primop-call dispatch + the per-arg OP_FORCE — those VM
            // ops force inline.
            if (po->arity == 2) {
                auto eachArg = [&]() {
                    auto it = e->args->begin();
                    ir::VarId a = forceVal(lowerExpr(*it));
                    ++it;
                    ir::VarId b = forceVal(lowerExpr(*it));
                    return std::pair{a, b};
                };
                if (name == "__sub" || name == "sub") {
                    auto [a, b] = eachArg();
                    return addBinding(ir::Sub{a, b});
                }
                if (name == "__mul" || name == "mul") {
                    auto [a, b] = eachArg();
                    return addBinding(ir::Mul{a, b});
                }
                if (name == "__div" || name == "div") {
                    auto [a, b] = eachArg();
                    return addBinding(ir::Div{a, b});
                }
                if (name == "__lessThan" || name == "lessThan") {
                    auto [a, b] = eachArg();
                    return addBinding(ir::Less{a, b});
                }
            }

            // Per-primop laziness rules: which positional arg indices
            // should be passed as thunks (or left lazy without forcing)
            // instead of force-evaluated up-front.
            //
            //  - `tryEval x` — x is the whole point of the primop; wrap
            //    in a thunk so an error during forcing is caught.
            //  - `foldl' op nul list` — `nul` is not strict; tree-walker
            //    documents that explicitly.  Pass it lazy so a `throw`
            //    that the operator never demands doesn't fire.
            //  - `seq a b` / `deepSeq a b` — `b` is returned untouched;
            //    only `a` gets forced.
            const bool isTryEval = name == "tryEval";
            auto isLazyArg = [&](uint32_t idx) -> bool {
                if (name == "foldl'") return idx == 1;     // nul
                if (name == "seq" || name == "deepSeq") return idx == 1;
                return false;
            };

            std::vector<ir::VarId> args;
            args.reserve(po->arity);
            auto it = e->args->begin();
            for (uint32_t i = 0; i < po->arity; ++i, ++it) {
                if (isTryEval) {
                    args.push_back(thunkify(*it));
                } else if (isLazyArg(i)) {
                    // Lazy: lower without forcing, and wrap non-trivial
                    // expressions in a thunk so the primop sees a
                    // proper lazy value (callers may pass `throw` etc).
                    args.push_back(thunkifyForAttr(*it));
                } else {
                    args.push_back(forceVal(lowerExpr(*it)));
                }
            }
            ir::VarId result = addBinding(ir::PrimOpCall{po, std::move(args)});
            // Any extra args become an App-chain on the primop's result
            // (`(import path) attrs` shape).  We force between Apps so
            // each step calls a real callable.
            for (; it != e->args->end(); ++it) {
                result = forceVal(result);
                ir::VarId av = lowerExpr(*it);
                result = addBinding(ir::App{result, av});
            }
            return result;
        }
        // Generic application via OP_CALL.  Force the callee (must be
        // a closure / primop / PrimOpApp).  Each argument must be
        // delivered as-is (Nix is lazy in arguments) — we wrap any
        // non-trivial expression in a thunk so that side-effects /
        // errors only fire if the callee actually forces the arg.
        ir::VarId f = forceVal(lowerExpr(e->fun));
        for (auto * a : *e->args) {
            ir::VarId av = thunkifyForAttr(a);
            f = addBinding(ir::App{f, av});
            // After this App, the result might be a closure (curried)
            // or the applied value.  Force before the next App so the
            // chain calls a real closure each step.
            if (a != e->args->back())
                f = forceVal(f);
        }
        return f;
    }

    /// Decide whether an attrset/list element expression needs a thunk
    /// wrapper: trivial nodes (literals, plain var refs, lambdas)
    /// carry no risk of failing or doing observable work, so skip the
    /// wrapping cost.  Anything non-trivial (calls, selects, arith,
    /// attrsets-of-attrsets) gets thunkified so that building the
    /// outer attrset doesn't eagerly force its contents — this is what
    /// gives Nix attrsets their lazy-by-attribute semantics.
    bool isTrivialForLazy(nix::Expr * e) const
    {
        if (!e) return true;
        const auto k = e->exprKind;
        if (k == nix::Expr::Kind::Int
            || k == nix::Expr::Kind::Float
            || k == nix::Expr::Kind::String
            || k == nix::Expr::Kind::Path
            || k == nix::Expr::Kind::Lambda) return true;
        // ExprVar is mostly trivial — but a `fromWith` reference
        // performs a runtime OP_WITH_LOOKUP that forces the with-stack
        // entries.  Inside `with pkgs; { a = b; }` where pkgs is part
        // of the same letRec, eagerly forcing the with entry would
        // blackhole.  Wrap fromWith vars in thunks so the lookup
        // happens lazily at access time.
        if (k == nix::Expr::Kind::Var) {
            auto * v = static_cast<nix::ExprVar *>(e);
            return !v->fromWith;
        }
        // ConcatStrings (`a + b`) is treated as trivial: the operands
        // are forced lazily by the VM's OP_STR_CONCAT path, and
        // wrapping the entire add in an extra thunk is the dominant
        // per-call cost on arithmetic-heavy benchmarks like fib.
        if (k == nix::Expr::Kind::ConcatStrings) return true;
        // Calls to known-pure arithmetic / comparison primops also
        // skip the wrapper.  These never throw / have side effects on
        // valid inputs, and tree-walker effectively treats them the
        // same way (its App value is much lighter than a v3 Thunk).
        if (k == nix::Expr::Kind::Call) {
            auto * c = static_cast<nix::ExprCall *>(e);
            if (c && c->fun && c->fun->exprKind == nix::Expr::Kind::Var) {
                auto * fv = static_cast<nix::ExprVar *>(c->fun);
                if (fv->fromWith) return false;
                std::string n(symbols[fv->name]);
                if (n == "__sub" || n == "__mul" || n == "__div"
                    || n == "__lessThan") return true;
            }
        }
        return false;
    }

    ir::VarId thunkifyForAttr(nix::Expr * e)
    {
        if (isTrivialForLazy(e)) return lowerExpr(e);
        return thunkify(e);
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

        // CO-3: record (Expr* -> FuncId) so the post-compile pass can
        // wire this thunk into the forceValue cutover cache.  Tree-
        // walker's forceValue gets called with `expr->thunk().expr`
        // (= `e` here); a cache hit lets v3 run the thunk body
        // directly instead of falling through to expr->eval.
        m.subExprFuncs.push_back({static_cast<const void *>(e), fid});

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
        return addBinding(ir::Not{forceVal(lowerExpr(e->e))});
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
        for (auto & p : e->es) parts.push_back(forceVal(lowerExpr(p.second)));
        return addBinding(ir::ConcatStrings{std::move(parts), e->forceString});
    }
    template<class AstNode, class IRNode>
    ir::VarId lowerBinOp(AstNode * e, IRNode)
    {
        // Strict binary op: force both operands so the VM ops see WHNF.
        ir::VarId a = forceVal(lowerExpr(e->e1));
        ir::VarId b = forceVal(lowerExpr(e->e2));
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
        // Lazy list elements — wrap non-trivial element exprs in
        // thunks so building a list doesn't fire side-effects in
        // unused entries (matches tree-walker; e.g. `head [42 (throw
        // "x")]` returns 42 without firing the throw).
        for (auto * el : e->elems) elems.push_back(thunkifyForAttr(el));
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

        if (e->recursive) {
            // Build the rec part, capturing its rec scope so dynamic
            // name + value expressions can be lowered while siblings
            // are visible (rec-references in dyn values, plus level
            // numbering for outer-scope vars in dyn names).
            Scope recScopeOut;
            ir::VarId recV = lowerLetRecCapture(
                e->attrs.value(),
                e->inheritFromExprs ? e->inheritFromExprs.get() : nullptr,
                /*isRec=*/true,
                /*hasBody=*/false, /*body=*/nullptr,
                &recScopeOut);
            if (!hasDyn) return recV;

            scopes.push_back(recScopeOut);
            ir::AttrSetDyn dyn;
            dyn.dynamics.reserve(e->dynamicAttrs->size());
            for (auto & da : *e->dynamicAttrs) {
                ir::VarId nameV = forceVal(lowerExpr(da.nameExpr));
                ir::VarId valV  = thunkifyForAttr(da.valueExpr);
                dyn.dynamics.push_back({nameV, valV});
            }
            scopes.pop_back();
            ir::VarId dynV = addBinding(std::move(dyn));
            // Merge: dyn entries override matching rec entries.
            return addBinding(ir::Update{forceVal(recV), forceVal(dynV)});
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
                // Lazy attrset values: wrap each entry in a thunk so
                // sibling attrs aren't eagerly evaluated when the
                // attrset is built.  Skips trivial expressions (literal
                // / var ref) that need no thunk for correctness.
                ir::VarId vv = thunkifyForAttr(kv.second.e);
                dyn.statics.push_back({internSym(kv.first), vv,
                                        posIdxToHandle(kv.second.pos)});
            }
            dyn.dynamics.reserve(e->dynamicAttrs->size());
            for (auto & da : *e->dynamicAttrs) {
                // Dynamic-name expression must evaluate to a string —
                // strict context, force.
                ir::VarId nameV = forceVal(lowerExpr(da.nameExpr));
                ir::VarId valV  = lowerExpr(da.valueExpr);
                dyn.dynamics.push_back({nameV, valV, posIdxToHandle(da.pos)});
            }
            if (pushedInheritFrom) inheritFromStack.pop_back();
            return addBinding(std::move(dyn));
        }

        std::vector<ir::AttrSet::Entry> entries;
        for (auto & kv : *e->attrs) {
            const auto & sym = kv.first;
            const auto & def = kv.second;
            // Lazy entries — see comment on the dyn branch above.
            ir::VarId vv = thunkifyForAttr(def.e);
            entries.push_back({internSym(sym), vv, posIdxToHandle(def.pos)});
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
        return lowerLetRecCapture(attrDefs, inheritFromExprs, isRec, hasBody, body, nullptr);
    }

    /// Like lowerLetRec but also writes the constructed rec Scope to
    /// `*outRecScope` so callers (lowerAttrs for `rec + dyn`) can
    /// re-push it when lowering follow-up expressions.
    ir::VarId lowerLetRecCapture(nix::ExprAttrs::AttrDefs & attrDefs,
                                 std::pmr::vector<nix::Expr *> * inheritFromExprs,
                                 bool isRec,
                                 bool hasBody, nix::Expr * body,
                                 Scope * outRecScope)
    {
        ir::VarId recVar = m.freshVar();

        struct Pending {
            nix::Symbol sym;
            nix::ExprAttrs::AttrDef::Kind kind;
            nix::Expr * defE;
            ir::FuncId funcIdx;
            ir::BlockId entryBlock;
            uint32_t    posHandle;
        };
        std::vector<Pending> pending;
        pending.reserve(attrDefs.size());

        for (auto & kv : attrDefs) {
            m.functions.emplace_back();
            ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
            auto eb = m.freshBlock();
            m.functions[fid].entryBlock = eb;
            m.functions[fid].name = std::string(symbols[kv.first]);
            // CO-3: register the LetRec / rec-attrset binding's def
            // expression with the thunk's function.  Tree-walker stores
            // these as `let { x = E; }` thunks whose `expr` field is E
            // — a cache lookup at force time hits this entry.
            if (kv.second.e)
                m.subExprFuncs.push_back({static_cast<const void *>(kv.second.e), fid});
            pending.push_back({kv.first, kv.second.kind, kv.second.e, fid, eb,
                                posIdxToHandle(kv.second.pos)});
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
            en.pos = p.posHandle;
            letRec.entries.push_back(std::move(en));
        }
        m.blocks[blockStack.back()].bindings.push_back(
            {recVar, std::move(letRec)});

        if (outRecScope) *outRecScope = recScope;

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
        const auto & step = path[pathIdx];
        // attrs must be in WHNF for AttrSelect/HasAttr to work.  Force
        // it once at each chain step.
        attrs = forceVal(attrs);

        // Static name: regular AttrSelect / HasAttr.  Dynamic name
        // (`attrs.${expr}`): evaluate expr to a string at runtime and
        // use OP_ATTRS_SELECT_DYN / OP_ATTRS_HAS_DYN.
        const bool dyn = step.expr != nullptr;
        ir::SymbolId nm = dyn ? 0 : internSym(step.symbol);
        ir::VarId nameVar = ir::kInvalid;
        if (dyn) nameVar = forceVal(lowerExpr(step.expr));

        if (defaultExpr) {
            ir::VarId hasIt = dyn
                ? addBinding(ir::HasAttrDyn{attrs, nameVar})
                : addBinding(ir::HasAttr{attrs, nm});
            auto thenB = m.freshBlock();
            auto elseB = m.freshBlock();

            blockStack.push_back(thenB);
            ir::VarId got = dyn
                ? addBinding(ir::AttrSelectDyn{attrs, nameVar})
                : addBinding(ir::AttrSelect{attrs, nm});
            ir::VarId rest = emitSelectChain(got, path, defaultExpr, pathIdx + 1);
            setReturn(rest);
            blockStack.pop_back();

            blockStack.push_back(elseB);
            ir::VarId defv = lowerExpr(defaultExpr);
            setReturn(defv);
            blockStack.pop_back();

            return addBinding(ir::If{hasIt, thenB, elseB});
        }

        ir::VarId v = dyn
            ? addBinding(ir::AttrSelectDyn{attrs, nameVar})
            : addBinding(ir::AttrSelect{attrs, nm});
        return emitSelectChain(v, path, nullptr, pathIdx + 1);
    }

    ir::VarId lowerHasAttr(nix::ExprOpHasAttr * e)
    {
        // `attrs ? a.b.c` is true iff each successive lookup hits an
        // existing attrset entry along the path.  Lower as a chain of
        // HasAttr followed by AttrSelect: at each step, if the current
        // attr exists go on, else short-circuit to false.
        ir::VarId attrs = forceVal(lowerExpr(e->e));
        ir::VarId result = ir::kInvalid;
        // Build the chain bottom-up.  We model it inline here using
        // nested If blocks: `if hasAttr(attrs, p0) then if has(attrs.p0, p1) ... else false else false`.
        std::function<ir::VarId(ir::VarId, size_t)> step =
            [&](ir::VarId cur, size_t idx) -> ir::VarId {
                const auto & an = e->attrPath[idx];
                bool dyn = an.expr != nullptr;
                ir::VarId nameVar = ir::kInvalid;
                if (dyn) nameVar = forceVal(lowerExpr(an.expr));
                ir::SymbolId nm = dyn ? 0 : internSym(an.symbol);

                ir::VarId hasIt = dyn
                    ? addBinding(ir::HasAttrDyn{cur, nameVar})
                    : addBinding(ir::HasAttr{cur, nm});
                if (idx + 1 == e->attrPath.size()) return hasIt;

                auto thenB = m.freshBlock();
                auto elseB = m.freshBlock();
                blockStack.push_back(thenB);
                ir::VarId got = dyn
                    ? addBinding(ir::AttrSelectDyn{cur, nameVar})
                    : addBinding(ir::AttrSelect{cur, nm});
                got = forceVal(got);
                ir::VarId rest = step(got, idx + 1);
                setReturn(rest);
                blockStack.pop_back();
                blockStack.push_back(elseB);
                setReturn(addBinding(ir::LitBool{false}));
                blockStack.pop_back();
                return addBinding(ir::If{hasIt, thenB, elseB});
            };
        result = step(attrs, 0);
        return result;
    }

    ir::VarId lowerAssert(nix::ExprAssert * e)
    {
        ir::VarId cond = forceVal(lowerExpr(e->cond));
        auto bodyB = m.freshBlock();
        blockStack.push_back(bodyB);
        ir::VarId rv = lowerExpr(e->body);
        setReturn(rv);
        blockStack.pop_back();
        return addBinding(ir::Assert{cond, bodyB});
    }

    ir::VarId lowerWith(nix::ExprWith * e)
    {
        // The attrs expression is evaluated OUTSIDE the with's scope —
        // do this before pushing the placeholder so its var lookups use
        // the surrounding level numbering.
        //
        // Do NOT eagerly force the attrs here: tree-walker delays
        // forcing until OP_WITH_LOOKUP first needs to scan the entry,
        // which is required for `with pkgs; ...` to work inside the
        // recursive group that defines pkgs (otherwise we'd blackhole).
        ir::VarId attrs = lowerExpr(e->attrs);
        auto bodyB = m.freshBlock();
        blockStack.push_back(bodyB);
        // Push an empty placeholder scope: nix's bindVars counts the
        // with's env as a level when resolving ExprVar in the body, so
        // v3's `scopes` stack must match that depth or resolveVar's
        // (level, displ) lookup falls off the end.
        scopes.emplace_back();
        ir::VarId rv = lowerExpr(e->body);
        scopes.pop_back();
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

ir::Module lowerNixExpr(nix::Expr * e, const nix::SymbolTable & symbols, const nix::PosTable & positions)
{
    Lowerer L(symbols, positions);
    return L.run(e);
}

} // namespace nix::v3
