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

/// Capture the lower.cc call-site line for every `forceVal(v)` invocation
/// so the bytecode emitter can populate `CompilationUnit::forceEmitSites`
/// without each of the ~14 forceVal sites needing to spell out `__LINE__`
/// explicitly.  See `Lowerer::forceValAt` for the underlying helper.
#define forceVal(v) forceValAt((v), __LINE__)

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

    /// #458 step 1/6 — heap-stable rec-attrset slot capture.
    ///
    /// Parallel VarId allocated per let-rec scope holding a Tag::Slot
    /// pointing at heap-stable storage that contains the rec-attrset
    /// Tag::Attrs (allocated by the OP_REC_SLOT_PUBLISH the LetRec
    /// emitter inserts).  Inner closures crossing a function boundary
    /// to reach this scope's `recAttrsVar` capture `recSlotVar` instead
    /// of `recAttrsVar` so they observe the rec-attrset through a
    /// stable slot deref rather than via the wrap thunk's evaluated
    /// state.  Closes the BlackholeError that today fires when an
    /// inner thunk tries to force the wrap thunk while it is mid-
    /// construction across a different VMState.
    ///
    /// `kInvalid` when the let-rec emit path doesn't synthesise the
    /// slot (e.g., test-only builds compiled before the slot path).
    ir::VarId recSlotVar = ir::kInvalid;
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

    /// REVIEW MED-9: cache of shared rec-attrset name vectors keyed
    /// by recVar.  resolveVar copies recAttrsNames into every
    /// RecVarOrigin; pre-cache makes that copy O(1) once per recVar.
    std::unordered_map<ir::VarId, std::shared_ptr<std::vector<ir::SymbolId>>>
        recAttrsNamesCache;

    /// REVIEW HIGH-4: per-scope pre-lowered VarIds for the
    /// inheritFromExprs vector currently on top of inheritFromStack.
    /// Keyed by displ.  Pushed/popped in lockstep with inheritFromStack;
    /// populated lazily on first lookup and emptied otherwise.  When a
    /// displ entry is non-zero, lowerInheritFrom returns it directly
    /// (single shared thunk) instead of re-lowering the source
    /// expression -- side-effecting `e` (`builtins.trace`, IFD) fires
    /// once, matching tree-walker's buildInheritFromEnv semantics.
    std::vector<std::vector<ir::VarId>> inheritFromCacheStack;

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
        {
            ir::VarId v = scopes[scopeIdx].byDispl[displ];
            // CO-2 phase B: record (func, VarId -> level/displ) so the
            // force hook can walk tree-walker's env to populate
            // upvalues at force time.  Direct byDispl resolution only
            // — synthesized VarIds (rec-attrset, inheritFrom,
            // with-lookup) are intentionally skipped; Phase B only
            // handles direct refs.  The level/displ are recorded
            // relative to the CURRENT function's scope-stack — the
            // function we're emitting bytecode into right now, which
            // is what `funcStack.back()` points at.
            ir::FuncId f = funcStack.empty() ? ir::FuncId{0} : funcStack.back();
            m.varOrigins.push_back({f, v, level, displ});
            return v;
        }
        // Rec slot.
        if (scopes[scopeIdx].recAttrsVar != ir::kInvalid &&
            displ < scopes[scopeIdx].recAttrsNames.size())
        {
            ir::VarId rec = scopes[scopeIdx].recAttrsVar;
            ir::SymbolId nm = scopes[scopeIdx].recAttrsNames[displ];
            // WC-2-followup: record (currentFunc, recVar) → (level,
            // names).  Multiple resolveVar calls hitting the same
            // (func, rec) pair add duplicates; the cache populator
            // keeps the first.  `level` here matches the AST's
            // ExprVar::level, which is the same level tree-walker's
            // lookupVar will use to walk env->up.
            //
            // REVIEW MED-9: share one names vector per recVar across
            // all refs.  Pre-fix copied the full names vector per
            // ref, O(N^2) per let-rec on the names data alone.
            ir::FuncId f = funcStack.empty() ? ir::FuncId{0} : funcStack.back();
            auto sharedNames = recAttrsNamesCache.find(rec);
            if (sharedNames == recAttrsNamesCache.end()) {
                auto p = std::make_shared<std::vector<ir::SymbolId>>(
                    scopes[scopeIdx].recAttrsNames);
                sharedNames = recAttrsNamesCache.emplace(rec, std::move(p)).first;
            }
            ir::RecVarOrigin rvo;
            rvo.func   = f;
            rvo.recVar = rec;
            rvo.level  = level;
            rvo.names  = sharedNames->second;
            m.recVarOrigins.push_back(std::move(rvo));
            // #458 Phase B RecBuildSlot — also record an origin keyed
            // by recSlotVar (parallel to the recVar origin above) so
            // Phase B's recOriginLookup can find the (level, names)
            // shape via either VarId.  Same reasoning as the recVar
            // origin: the lowerer needs to tell Phase B "this freeVar
            // resolves to the rec-attrset whose entries are at TW env
            // depth `level` with these names".  Whether the freeVar
            // is recVar (legacy capture) or recSlotVar (slot capture)
            // changes only the upvalue's Tag at materialisation time,
            // not the env walk itself.
            if (scopes[scopeIdx].recSlotVar != ir::kInvalid) {
                ir::RecVarOrigin slotOrigin;
                slotOrigin.func   = f;
                slotOrigin.recVar = scopes[scopeIdx].recSlotVar;
                slotOrigin.level  = level;
                slotOrigin.names  = sharedNames->second;
                m.recVarOrigins.push_back(std::move(slotOrigin));
            }
            // WC-31: defer the AttrSelect by wrapping in a thunk.
            // Direct `AttrSelect{rec, nm}` runs at MAKE_CLOSURE time
            // and captures whatever's in the slot AT THAT MOMENT —
            // for closures created inside a rec body that reference
            // siblings, this is often a Suspended/Black thunk.  When
            // the closure is later called, forcing the captured Black
            // thunk cycles (WC-23 family).
            //
            // Tree-walker captures `Env *` by reference; var access
            // walks env at call time, by which point most rec slots
            // have transitioned Suspended→Evaluated.  The thunkified
            // AttrSelect achieves the same: the thunk's body runs
            // AttrSelect at force-time (i.e., closure-call time), so
            // the slot has had the chance to settle.
            //
            // Cost: one extra Thunk allocation per rec freeVar +
            // one extra force.  Worth it for the correctness fix.
            // (The prior NIX_V3_NO_THUNKIFY_REC A/B gate has been
            // removed -- thunkification is the verified-correct
            // default; REVIEW-COMP §8.6.)
            // #427 (Phase 5): when NIX_V3_INLINE_REC_SLOT=1, emit
            // RecBindingSlotRef directly, skipping the WC-31
            // thunkifyRecAttrSelect wrapper.  The slot is heap-stable
            // (Tag::Slot) so deferral behind a Thunk *should* be
            // redundant; eliminates the per-access Thunk allocation
            // that dominated nixpkgs eval (`recref-setType` alone was
            // forced 1.38M times under stages 0..3).
            //
            // Default-ON since 2026-05-04: cardano-node correctness
            // regression (#437) was root-caused to v3 closures with
            // formals escaping to tree-walker as `mkPrimOpApp(bridge1,
            // h)`, where bridge1's deep arg-conversion tripped
            // ExprBlackHole on the NixOS-module-system fixed-point's
            // `config`.  Fixed in:
            //   - 75dc45ebd: refuse to bridge `<formals>` closures
            //     out of `v3ToTreeWalker` (throw blackhole-shaped
            //     exception so existing `isBlackhole*` predicates
            //     route to fallbackExpr re-eval paths).
            //   - 599cb9745: skip v3 call hook for formals lambdas;
            //     bridge non-formals call-hook args lazily (shallow
            //     Bridge thunk instead of deep `treeWalkerToV3Public`).
            //
            // All four cardano-node modes pass with Phase 5 ON
            // (v3, v3+fhook, v3+P5, v3+P5+fhook); fib35 retains its
            // -21% min-vs-TW headline win.  Kill-switch
            // `NIX_V3_NO_INLINE_REC_SLOT=1` for fallback
            // (renamed from the prior opt-in `NIX_V3_INLINE_REC_SLOT`).
            static const bool inlineRecSlot =
                std::getenv("NIX_V3_NO_INLINE_REC_SLOT") == nullptr;
            // #458 step 3/6 — slot-capture path (default-on).
            //
            // Routes rec-attrset entry resolution through the let-rec's
            // recSlotVar (Tag::Slot pointing at heap-stable storage)
            // instead of the wrap thunk's recAttrsVar (Tag::Attrs /
            // wrapping thunk).  RecBindingSlotRef on Tag::Slot derefs
            // the slot in OP_REC_BINDING_SLOT_REF's runtime handler to
            // get the (possibly partial) Tag::Attrs without touching
            // the wrap thunk's state machine — sidesteps the
            // BlackholeError that fires when the wrap thunk is mid-
            // construction in another VMState's frame stack.
            //
            // Validated 2026-05-06 across:
            //   - run-lang-tests: 142/142.
            //   - run-cutover-parity-tests: 142/142 (slot mode flips
            //     the prior 2 divergences to parity).
            //   - run-456-chase-cycle-tests: 7/7.
            //   - With closure-result-refusal also removed: still
            //     142/142 across all three suites (confirms slot
            //     redesign closes the path that required the refusal).
            //   - nixpkgs#hello.name: parity, no perf regression.
            //
            // Disable via NIX_V3_NO_REC_SLOT_CAPTURE=1 (kill switch
            // pending Phase B RecBuildSlot integration).
            static const bool slotCapture =
                std::getenv("NIX_V3_NO_REC_SLOT_CAPTURE") == nullptr;
            ir::VarId source = rec;
            if (slotCapture && scopes[scopeIdx].recSlotVar != ir::kInvalid)
                source = scopes[scopeIdx].recSlotVar;
            if (inlineRecSlot)
                return addBinding(ir::RecBindingSlotRef{source, nm});
            return thunkifyRecAttrSelect(source, nm);
        }
        return ir::kInvalid;
    }

    /// WC-31: wrap a rec-attrset AttrSelect in a thunk so the select
    /// runs at force time (matching tree-walker's Env-pointer-by-
    /// reference semantics) instead of at MAKE_CLOSURE time.
    ///
    /// WC-38 Phase 5: the thunk's body now returns `Tag::Slot`
    /// (a heap-stable pointer into Bindings::entries[i].value) via
    /// `ir::RecBindingSlotRef`.  When the thunk is forced and its
    /// resolved value is consumed by callFunction or `with E;`, the
    /// slot identity is preserved through the call chain — sub-
    /// thunks captured-with the slot see the entry's mutated /
    /// memoized value, fixing the `with self;` blackhole over
    /// rec-attrset entries that's at the root of WC-38.
    ir::VarId thunkifyRecAttrSelect(ir::VarId rec, ir::SymbolId nm)
    {
        m.functions.emplace_back();
        ir::FuncId fid = static_cast<ir::FuncId>(m.functions.size() - 1);
        auto entry = m.freshBlock();
        m.functions[fid].entryBlock = entry;
        // Diagnostic name — appears in V3_DBG_OPCYCLE traces.
        const auto & gst = ir::globalSymbolTable();
        m.functions[fid].name = std::string("recref-")
            + (nm < gst.size() ? gst[nm] : std::string("?"));

        funcStack.push_back(fid);
        blockStack.push_back(entry);
        // WC-38 Phase 5: thunk body returns a Tag::Slot pointer
        // (heap-stable into Bindings::entries[i].value), not a
        // snapshot.  REVIEW-COMP §8.6: the prior
        // `NIX_V3_NO_REC_SLOT_REF` A/B gate is removed -- the slot
        // ref is the verified-correct default since Phase 5.
        ir::VarId selectVar = addBinding(ir::RecBindingSlotRef{rec, nm});
        setReturn(selectVar);
        blockStack.pop_back();
        funcStack.pop_back();

        return addBinding(ir::MkThunk{fid, /*freeVars*/ {}});
    }

    /// Wrap a VarId in a Force if it might not be in WHNF.  Cheap:
    /// Force on a non-thunk is a no-op at runtime.
    ///
    /// `srcLine` records the lower.cc line of the call site so the
    /// bytecode emitter can attribute the eventual OP_FORCE back to
    /// its emitter site for `V3_DBG_FORCE_SITE` traces.  Use the
    /// `forceVal()` macro below at call sites; it captures `__LINE__`
    /// automatically so individual call sites don't need to pass it.
    ir::VarId forceValAt(ir::VarId v, int srcLine)
    {
        return addBinding(ir::Force{v, srcLine});
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
            return addBinding(ir::WithLookup{sym});
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
            ir::VarId v = addBinding(ir::LitBuiltins{});
            // #425: track for the populate path so an inner function
            // capturing this var as a freeVar gets a LitBuiltins
            // upvalue source (no env walk -- builtins is a singleton).
            m.litBuiltinsVarIds.push_back(v);
            return v;
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
        // REVIEW MED-5: emit-time force dropped; OP_BRANCH_FALSE
        // (emitted via emit.cc::emitOne(ir::If)) forces the cond.
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
    /// #495: structurally match an ExprLambda's body against canonical
    /// nix-stdlib fix-point patterns.  Returns the intrinsic kind code
    /// (0=None, 1=Fix, ...) matching ir::Function::intrinsicKind.
    ///
    /// Currently recognises:
    ///   Fix:  `f: let x = f x; in x`
    ///         body = ExprLet(attrs={x = ExprCall(ExprVar(f), [ExprVar(x)])},
    ///                        body=ExprVar(x))
    ///
    /// Future kinds (Extends, ComposeExtensions, ...) added incrementally.
    /// Matchers are narrow on purpose -- if nixpkgs changes the canonical
    /// shape, the lambda silently falls through to the non-intrinsic v3
    /// dispatch (zero correctness loss; only optimization is lost).
    uint8_t recogniseIntrinsic(nix::ExprLambda * e)
    {
        static const bool s_dbgVerbose =
            std::getenv("V3_DBG_INTRINSIC_REJECT") != nullptr;
        auto reject = [&](const char * reason) -> uint8_t {
            if (s_dbgVerbose && e && e->arg)
                std::fprintf(stderr,
                    "v3 recogniseIntrinsic REJECT (arg=%s): %s\n",
                    std::string(symbols[e->arg]).c_str(), reason);
            return 0;
        };
        if (!e || !e->body || !e->arg) return reject("nullptr / no arg");
        if (e->getFormals()) return reject("has formals");

        if (s_dbgVerbose && e->arg
            && std::string_view(symbols[e->arg]) == "f")
            std::fprintf(stderr,
                "v3 recogniseIntrinsic: arg=f body.kind=%d (Let=%d Attrs=%d Var=%d Call=%d)\n",
                (int)e->body->exprKind,
                (int)nix::Expr::Kind::Let, (int)nix::Expr::Kind::Attrs,
                (int)nix::Expr::Kind::Var, (int)nix::Expr::Kind::Call);

        // Match Fix: body must be ExprLet with one binding `x = f x`,
        // body of let must be `x`.
        if (auto * letE = dynamic_cast<nix::ExprLet *>(e->body)) {
            if (!letE->attrs || !letE->attrs->attrs) return reject("let no attrs");
            const auto & defs = *letE->attrs->attrs;
            if (defs.size() != 1) return reject("let != 1 binding");
            if (letE->attrs->dynamicAttrs
                && !letE->attrs->dynamicAttrs->empty()) return reject("let has dyn");
            const auto & [bindSym, bindDef] = *defs.begin();
            if (bindDef.kind != nix::ExprAttrs::AttrDef::Kind::Plain) return reject("binding not Plain");

            // Binding's RHS must be ExprCall(f, [x]) -- one arg only.
            auto * callE = dynamic_cast<nix::ExprCall *>(bindDef.e);
            if (!callE || !callE->args.has_value()) return reject("RHS not ExprCall");
            if (callE->args->size() != 1) return reject("ExprCall args != 1");

            // callee must be ExprVar referencing the lambda's arg.
            auto * fVar = dynamic_cast<nix::ExprVar *>(callE->fun);
            if (!fVar) return reject("callee not ExprVar");
            if (fVar->name != e->arg) return reject("callee name != lambda arg");

            // arg must be ExprVar referencing the let binding.
            auto * xVar = dynamic_cast<nix::ExprVar *>((*callE->args)[0]);
            if (!xVar) return reject("call arg not ExprVar");
            if (xVar->name != bindSym) return reject("call arg name != binding sym");

            // Let body must be ExprVar referencing the binding.
            auto * bodyVar = dynamic_cast<nix::ExprVar *>(letE->body);
            if (!bodyVar) return reject("let body not ExprVar");
            if (bodyVar->name != bindSym) return reject("let body name != binding sym");

            return 1;  // Intrinsic::Fix
        }

        // Body is ExprLambda → peel curried lambdas and match Extends or
        // ComposeExtensions based on chain depth.
        //
        //   Extends:           overlay: f: final: <Let>
        //                      depth=2 inner lambdas (f, final), then Let.
        //
        //   ComposeExtensions: f: g: final: prev: <Let>
        //                      depth=3 inner lambdas (g, final, prev), then Let.
        //
        // We collect the lambda chain (e + all nested ExprLambdas) and
        // dispatch by length-and-inner-shape.
        if (dynamic_cast<nix::ExprLambda *>(e->body)) {
            std::vector<nix::ExprLambda *> chain{e};
            nix::Expr * cur = e->body;
            while (auto * inner = dynamic_cast<nix::ExprLambda *>(cur)) {
                if (!inner->arg) return reject("inner lambda has no arg");
                if (inner->getFormals()) return reject("inner lambda has formals");
                chain.push_back(inner);
                cur = inner->body;
            }
            // cur is the body after peeling all lambdas.
            auto * letE = dynamic_cast<nix::ExprLet *>(cur);
            if (!letE) return reject("inner-most body not Let");
            if (!letE->attrs || !letE->attrs->attrs) return reject("let no attrs");
            if (letE->attrs->dynamicAttrs && !letE->attrs->dynamicAttrs->empty())
                return reject("let has dyn");
            const auto & defs = *letE->attrs->attrs;

            // ---------- Extends: chain of length 3, single binding ----------
            if (chain.size() == 3 && defs.size() == 1) {
                auto * lamF = chain[1], * lamFinal = chain[2];
                const auto & [prevSym, prevDef] = *defs.begin();
                if (prevDef.kind != nix::ExprAttrs::AttrDef::Kind::Plain)
                    return reject("Extends: prev binding not Plain");

                // prev = f final
                auto * fCall = dynamic_cast<nix::ExprCall *>(prevDef.e);
                if (!fCall || !fCall->args.has_value()) return reject("Extends: prev RHS not ExprCall");
                if (fCall->args->size() != 1) return reject("Extends: f-call args != 1");
                auto * fVar = dynamic_cast<nix::ExprVar *>(fCall->fun);
                if (!fVar || fVar->name != lamF->arg) return reject("Extends: f-call callee != f");
                auto * fArgVar = dynamic_cast<nix::ExprVar *>((*fCall->args)[0]);
                if (!fArgVar || fArgVar->name != lamFinal->arg)
                    return reject("Extends: f-call arg != final");

                // Let body: prev // overlay final prev
                auto * upd = dynamic_cast<nix::ExprOpUpdate *>(letE->body);
                if (!upd) return reject("Extends: let body not OpUpdate");
                auto * lhsVar = dynamic_cast<nix::ExprVar *>(upd->e1);
                if (!lhsVar || lhsVar->name != prevSym) return reject("Extends: update LHS != prev");
                auto * rhsCall = dynamic_cast<nix::ExprCall *>(upd->e2);
                if (!rhsCall || !rhsCall->args.has_value())
                    return reject("Extends: update RHS not ExprCall");
                if (rhsCall->args->size() != 2)
                    return reject("Extends: overlay-call arity != 2");
                auto * ovVar = dynamic_cast<nix::ExprVar *>(rhsCall->fun);
                if (!ovVar || ovVar->name != e->arg)
                    return reject("Extends: overlay-call callee != overlay");
                auto * a0 = dynamic_cast<nix::ExprVar *>((*rhsCall->args)[0]);
                auto * a1 = dynamic_cast<nix::ExprVar *>((*rhsCall->args)[1]);
                if (!a0 || a0->name != lamFinal->arg)
                    return reject("Extends: overlay arg-0 != final");
                if (!a1 || a1->name != prevSym)
                    return reject("Extends: overlay arg-1 != prev");

                return 2;  // Intrinsic::Extends
            }

            // ---------- ComposeExtensions: chain length 4, two bindings ----
            if (chain.size() == 4 && defs.size() == 2) {
                auto * lamG = chain[1], * lamFinal = chain[2], * lamPrev = chain[3];

                // Identify bindings by RHS kind (one ExprCall, one ExprOpUpdate).
                nix::Symbol fAppliedSym, prevPrimeSym;
                nix::ExprCall * fApCall = nullptr;
                nix::ExprOpUpdate * pPrimeUpd = nullptr;
                for (const auto & [sym, def] : defs) {
                    if (def.kind != nix::ExprAttrs::AttrDef::Kind::Plain)
                        return reject("Compose: binding not Plain");
                    if (auto * c = dynamic_cast<nix::ExprCall *>(def.e)) {
                        fAppliedSym = sym; fApCall = c;
                    } else if (auto * u = dynamic_cast<nix::ExprOpUpdate *>(def.e)) {
                        prevPrimeSym = sym; pPrimeUpd = u;
                    } else {
                        return reject("Compose: binding RHS not Call/OpUpdate");
                    }
                }
                if (!fApCall || !pPrimeUpd)
                    return reject("Compose: missing call/update binding");

                // fApplied = f final prev
                if (!fApCall->args.has_value() || fApCall->args->size() != 2)
                    return reject("Compose: fApplied call arity != 2");
                auto * fv = dynamic_cast<nix::ExprVar *>(fApCall->fun);
                if (!fv || fv->name != e->arg)
                    return reject("Compose: fApplied callee != f");
                auto * a0 = dynamic_cast<nix::ExprVar *>((*fApCall->args)[0]);
                auto * a1 = dynamic_cast<nix::ExprVar *>((*fApCall->args)[1]);
                if (!a0 || a0->name != lamFinal->arg)
                    return reject("Compose: fApplied arg-0 != final");
                if (!a1 || a1->name != lamPrev->arg)
                    return reject("Compose: fApplied arg-1 != prev");

                // prev' = prev // fApplied
                auto * pLhs = dynamic_cast<nix::ExprVar *>(pPrimeUpd->e1);
                auto * pRhs = dynamic_cast<nix::ExprVar *>(pPrimeUpd->e2);
                if (!pLhs || pLhs->name != lamPrev->arg)
                    return reject("Compose: prev'-update LHS != prev");
                if (!pRhs || pRhs->name != fAppliedSym)
                    return reject("Compose: prev'-update RHS != fApplied");

                // Let body: fApplied // g final prev'
                auto * upd = dynamic_cast<nix::ExprOpUpdate *>(letE->body);
                if (!upd) return reject("Compose: let body not OpUpdate");
                auto * lhsVar = dynamic_cast<nix::ExprVar *>(upd->e1);
                if (!lhsVar || lhsVar->name != fAppliedSym)
                    return reject("Compose: body LHS != fApplied");
                auto * gCall = dynamic_cast<nix::ExprCall *>(upd->e2);
                if (!gCall || !gCall->args.has_value() || gCall->args->size() != 2)
                    return reject("Compose: g-call arity != 2");
                auto * gv = dynamic_cast<nix::ExprVar *>(gCall->fun);
                if (!gv || gv->name != lamG->arg)
                    return reject("Compose: g-call callee != g");
                auto * b0 = dynamic_cast<nix::ExprVar *>((*gCall->args)[0]);
                auto * b1 = dynamic_cast<nix::ExprVar *>((*gCall->args)[1]);
                if (!b0 || b0->name != lamFinal->arg)
                    return reject("Compose: g-call arg-0 != final");
                if (!b1 || b1->name != prevPrimeSym)
                    return reject("Compose: g-call arg-1 != prev'");

                return 3;  // Intrinsic::ComposeExtensions
            }

            return reject("lambda chain length doesn't match any intrinsic");
        }
        return reject("body not ExprLet or ExprLambda");
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
        m.functions[fid].posHandle  = posIdxToHandle(e->getPos());
        // #493 / #484 follow-on: capture the original ExprLambda* so emit
        // can carry it through to LambdaDescriptor and v3ToTreeWalker can
        // construct a proper TW Tag::tLambda for formals-closure bridges.
        m.functions[fid].astLambda  = static_cast<void *>(e);
        // #495: structural-match for nix-stdlib intrinsics (lib.fix, etc.).
        // OP_CALL on a closure with a non-zero intrinsicKind dispatches to
        // a v3-native impl that runs the fix-point machinery in v3
        // (eliminates the TW round-trip that today blocks lambda-skip
        // default-on for nixpkgs -- see project_493_step3d_with_stack memo).
        m.functions[fid].intrinsicKind = recogniseIntrinsic(e);
        {
            static const bool s_dbg =
                std::getenv("V3_DBG_INTRINSIC") != nullptr;
            if (s_dbg) {
                static const char * names[] = {
                    "None", "Fix", "Extends", "ComposeExtensions",
                    "ComposeManyExtensions",
                };
                uint8_t k = m.functions[fid].intrinsicKind;
                if (k != 0 || std::getenv("V3_DBG_INTRINSIC_ALL") != nullptr)
                    std::fprintf(stderr,
                        "v3 recogniseIntrinsic: fid=%u name='%s' kind=%s\n",
                        (unsigned)fid, m.functions[fid].name.c_str(),
                        k < (sizeof(names)/sizeof(names[0])) ? names[k] : "?");
            }
        }

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
                ifm.hasDefault = fm.def != nullptr;
                ifm.pos = posIdxToHandle(fm.pos);
                m.functions[fid].formals.push_back(ifm);
            }

            funcStack.push_back(fid);
            blockStack.push_back(entry);

            // EVAL-COMP §3.3 (REVERTED 2026-05-04): the
            // direct-AttrSelect optimisation broke laziness for
            // mapAttrs-style Tag::App formals because OP_ATTRS_SELECT
            // eagerly forces Tag::App entries via the Phase-13.3
            // mapAttrs memo path, even for formals the body never
            // references.  cardano-node hit infinite recursion because
            // an unused formal's lazy App was force-resolved at
            // function entry, exposing an eval-order cycle that
            // tree-walker resolves through its slot-pointer access
            // pattern (only forces formals the body actually demands).
            // Re-enable behind an env var only after either:
            //   (a) emitting an OP_ATTRS_SELECT_NO_FORCE variant for
            //       the formals path, or
            //   (b) deferring the AttrSelect to reference time so
            //       only used formals are touched.

            // Default-bearing formals: synthetic LetRec so each
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

            // #425 followup attempt (deferred): tried registering
            // formalsRec as a recVarId + emitting a level=0 origin
            // pointing at the formals env.  Reduced phaseB noUpv
            // events 60 -> 22 on hello.name and 484 -> 174 on
            // cardano-node default mode, but caused a v3-fhook
            // chase-cycle (Tag::Slot/Thunk indirection loop, vm.cc's
            // 4096-iter guard) on cardano-node when Phase 5 inlining
            // is also active.  Root cause is a level mismatch when
            // thunks are created inside a nested let/with within the
            // formals body -- captured env isn't env2 in zero levels.
            // Needs per-thunk level tracking (not just the rec scope's
            // immediate offset) to be safe across the matrix.

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

        // #426 / MED-21: register the lambda's body Function so the
        // tree-walker -> v3 callFunction cutover can find it.  The
        // ExprLambda* is the canonical key tree-walker uses (its
        // Value::lambda().fun field).  Stored alongside subExprFuncs
        // so the existing populateSubExprCacheLocal sees it and
        // produces a SubExprCacheEntry; the call-hook differentiates
        // by AST kind.
        m.subExprFuncs.push_back({static_cast<const void *>(e), fid});

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

            // Per-primop laziness rules are encoded uniformly in the
            // `po->lazyArgs` bitmask (bit i set ⇒ arg i passed lazily).
            // tryEval=0b1, foldl'=0b010, seq=0b10, deepSeq=0b10,
            // addErrorContext=0b10.  This mirrors tree-walker's per-
            // primop lazy-arg semantics.
            //
            // Strict (non-lazy) args are NOT compile-time-forced here
            // — they're passed through to OP_CALL_PRIMOP which forces
            // them at runtime via the C-recursive forceValue helper.
            // The C-helper does NOT set CFF_FORCE_RETRY, so it doesn't
            // chain-push thunks the way OP_FORCE bytecode does — each
            // thunk fully resolves before the next runs.  This was the
            // WC-38 fix: compile-time force at this site fired inner
            // sub-thunks during `lib.fix x`'s body via the bytecode
            // chain while x was still Black.

            std::vector<ir::VarId> args;
            args.reserve(po->arity);
            auto it = e->args->begin();
            for (uint32_t i = 0; i < po->arity; ++i, ++it) {
                if (po->lazyArgs & (1u << i)) {
                    // Lazy: wrap non-trivial expressions in a thunk so
                    // the primop sees a proper lazy value (callers may
                    // pass `throw` etc that should only fire if the
                    // primop actually demands the arg).
                    args.push_back(thunkifyForAttr(*it));
                } else {
                    // Strict: pass-through; OP_CALL_PRIMOP forces at
                    // runtime via the C-recursive forceValue helper.
                    args.push_back(lowerExpr(*it));
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
        // Generic application via OP_CALL.  Each argument must be
        // delivered as-is (Nix is lazy in arguments) — we wrap any
        // non-trivial expression in a thunk so that side-effects /
        // errors only fire if the callee actually forces the arg.
        //
        // REVIEW MED-5: dropped the emit-time forceVal on the callee
        // and between curried args.  OP_CALL's runtime handler
        // (vm.cc:1148-1151) already forces `fun` if it's Thunk/App/
        // Slot before dispatching, so the emit-time OP_FORCE was a
        // pure overhead.  Tree-walker's callFunction doesn't force
        // between curried args either; matching that here avoids
        // over-eager sub-thunk firing in nixpkgs' deeply-curried call
        // chains (callPackageWith / makeOverridable).
        ir::VarId f = lowerExpr(e->fun);
        for (auto * a : *e->args) {
            ir::VarId av = thunkifyForArg(a);
            f = addBinding(ir::App{f, av});
        }
        return f;
    }

    /// Two flavours of "is this expression cheap enough to skip the
    /// thunk wrapper?":
    ///
    ///   - `forArg`: used for **function-call arguments**.  An eagerly
    ///     evaluated ConcatStrings or arithmetic primop call passed as
    ///     an arg is fine — the callee will force the argument anyway,
    ///     and wrapping every `f (n + 1)` in a thunk dominates fib's
    ///     hot path.
    ///   - `forValue` (default — for attrset values and list elements):
    ///     stricter.  ConcatStrings and arithmetic primop calls do
    ///     **need** a thunk wrapper here because they may reference
    ///     rec siblings (`version = release + versionSuffix;` in
    ///     nixpkgs lib/trivial.nix).  Without the wrapper, the
    ///     surrounding rec-attrset construction eagerly forces the
    ///     sibling, deadlocking against the rec attrset that's still
    ///     being built (`lib.trivial`'s body forcing
    ///     `lib.trivial.release` cycle).
    bool isTrivialForLazy(nix::Expr * e, bool forArg) const
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
        // ConcatStrings (`a + b`) and known-pure arithmetic primop
        // calls — only cheap-to-skip in argument position.  In an
        // attr-value position they may reference rec siblings, so the
        // thunk wrapper is required for laziness.
        if (forArg) {
            if (k == nix::Expr::Kind::ConcatStrings) return true;
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
        }
        return false;
    }

    /// Wrap an attrset/list-element expression in a thunk if needed.
    /// Strict: ConcatStrings/arith get thunked here (rec-sibling
    /// laziness — see isTrivialForLazy comment).
    ir::VarId thunkifyForAttr(nix::Expr * e)
    {
        if (isTrivialForLazy(e, /*forArg=*/false)) return lowerExpr(e);
        return thunkify(e);
    }

    /// Wrap a function-call argument in a thunk if needed.  Looser:
    /// ConcatStrings + pure-arith calls pass through eagerly (fib
    /// hot-path optimisation).
    ir::VarId thunkifyForArg(nix::Expr * e)
    {
        if (isTrivialForLazy(e, /*forArg=*/true)) return lowerExpr(e);
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
        // Phase 13: record source position so V3_DBG_FORCE_TRACE +
        // periodic V3_DBG_FORCES progress can map hot anonymous
        // thunks back to their .nix file:line:column.
        m.functions[fid].posHandle = posIdxToHandle(e->getPos());

        // CO-3: record (Expr* -> FuncId) for the post-compile pass to
        // WC-2: register every per-thunk function in the cutover
        // cache, not just top-level ones.
        //
        // The earlier restriction (atTopLevel-only) was a safety
        // measure against the worry that "the same AST Expr* can be
        // reached from multiple call sites with different tree-walker
        // env shapes".  Closer reading shows Nix is purely lexical:
        // each AST Expr has ONE lexical scope, and a thunk's
        // freeVars (with their (level, displ) origins) describe THAT
        // scope's relation to the surrounding context.  Tree-walker
        // constructs envs lexically, so when it forces the thunk the
        // env-walking pattern from (level, displ) finds the right
        // values regardless of which dynamic call-site reached the
        // body.
        //
        // The remaining safety nets in v3_hook.cc still apply:
        //   - recVarSet excludes thunks whose freeVars include a
        //     rec-attrset VarId (those don't live in a single env
        //     cell).
        //   - upvalueSources.empty() causes Phase B to skip.
        //   - phaseBFailed remembers throws on a per-Expr basis.
        //
        // If a nested thunk turns out to misbehave at force time,
        // Phase B will catch the throw and mark phaseBFailed,
        // routing the same Expr through tree-walker on subsequent
        // forces — bounded blast radius.
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
        // REVIEW MED-5: emit-time force dropped; OP_NOT forces.
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
        // REVIEW HIGH-4: return the pre-lowered VarId so all N names
        // in `inherit (e) x y z` share one thunk for `e`.  The cache
        // is populated by the enclosing lowerAttrs / lowerLetRec
        // BEFORE per-attr thunk bodies are entered, so the VarId
        // lives in the outer function's scope and the thunk bodies
        // pick it up as an upvalue via computeFreeVars.
        auto & cache = inheritFromCacheStack.back();
        if (e->displ < cache.size() && cache[e->displ] != ir::kInvalid)
            return cache[e->displ];
        // Fallback: cache wasn't populated (shouldn't happen on the
        // happy path).  Lower fresh -- preserves correctness at the
        // cost of the multi-fire bug for this displ.
        return lowerExpr((*fromExprs)[e->displ]);
    }

    /// Pre-lower every from-expr in `inheritFromExprs` into the
    /// CURRENT (outer) function so per-attr thunk bodies share a
    /// single VarId per displ.  Caller must have just pushed the
    /// matching inheritFromStack entry; we push the matching cache
    /// entry here.
    ///
    /// #466 / #482: thunkify each from-expr so its evaluation is
    /// DEFERRED until at least one of the inherited names is actually
    /// demanded.  Mirrors tree-walker's `from->maybeThunk(state, up)`
    /// in `ExprAttrs::buildInheritFromEnv` (eval.cc:1520).  Eager
    /// `lowerExpr` was running during attrset construction; for
    /// `inherit (self.X) Y` inside a `self:` lambda passed to a
    /// fix-point helper (e.g. nixpkgs's `makeExtensible'`), eager
    /// evaluation tripped on `self` mid-construction and surfaced
    /// as `OP_ATTRS_SELECT: attribute not found` (the let-bindings
    /// scope ended up on the stack instead of the lambda's parameter
    /// when v3 owned the body — lambda-skip + nixpkgs lib pattern).
    /// Sharing across the N inherited names is preserved: a single
    /// MkThunk binding produces one VarId referenced by N AttrDefs.
    void pushInheritFromCache(std::pmr::vector<nix::Expr *> * fromExprs)
    {
        // #482 / Phase of #466: when lambda-skip is on, v3 owns lambda
        // body execution.  Eager `lowerExpr` emits the from-expr
        // directly into the lambda body's bytecode -- if the from-expr
        // touches a let-rec/rec-attrset value mid-construction (e.g.
        // `inherit (self.X) Y` inside a `self:` lambda passed to a
        // fix-point helper like nixpkgs's `makeExtensible'`), the
        // eager evaluation runs DURING attrset construction and trips
        // on the mid-construction self.  Tree-walker's
        // `from->maybeThunk(state, up)` (eval.cc:1520) defers via
        // thunk wrappers; mirror that here ONLY when lambda-skip is
        // active.  Default mode keeps eager (the call-hook's wrong-
        // shape pre-refusal routes Lambda Exprs to TW so eager from-
        // expr never gets v3-emitted under default semantics).
        // Override with NIX_V3_NO_INHERIT_FROM_THUNK=1.
        static const bool s_lambdaSkip =
            std::getenv("NIX_V3_LAMBDA_SKIP") != nullptr;
        static const bool s_noThunkify =
            std::getenv("NIX_V3_NO_INHERIT_FROM_THUNK") != nullptr;
        const bool useThunk = s_lambdaSkip && !s_noThunkify;
        std::vector<ir::VarId> cache;
        if (fromExprs) {
            cache.resize(fromExprs->size(), ir::kInvalid);
            for (size_t i = 0; i < fromExprs->size(); ++i) {
                if ((*fromExprs)[i])
                    cache[i] = useThunk
                        ? thunkifyForAttr((*fromExprs)[i])
                        : lowerExpr((*fromExprs)[i]);
            }
        }
        inheritFromCacheStack.push_back(std::move(cache));
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
        // Strict binary op: force both operands so the VM ops see
        // WHNF.  OP_ADD / OP_SUB / OP_LESS / etc. don't auto-force
        // (their fast path checks isInt and falls through to a
        // type-mismatch throw on Thunks), so the emit-time force
        // is required.  REVIEW-COMP §8.6: the prior
        // `NIX_V3_NO_BINOP_FORCE` A/B gate is removed -- the
        // forces were retained as the verified-correct default.
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
            // REVIEW MED-5: emit-time forces dropped; OP_ATTRS_UPDATE
            // forces both lhs and rhs at runtime.
            return addBinding(ir::Update{recV, dynV});
        }

        // Non-rec attrset.  All entries (Plain / Inherited / InheritedFrom)
        // are lowered eagerly in the parent scope — there is no rec env.
        // InheritedFrom AttrDefs have def.e = ExprSelect(ExprInheritFrom,
        // name); lowerExpr handles ExprInheritFrom by looking up the
        // current inheritFromStack.
        bool pushedInheritFrom = false;
        if (e->inheritFromExprs) {
            inheritFromStack.push_back(e->inheritFromExprs.get());
            pushInheritFromCache(e->inheritFromExprs.get());
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
                // WC-38 ROOT CAUSE: dynamic-attr VALUES must be lazy
                // (thunkified), matching:
                //   - the rec+dyn branch above (line 1047), which uses
                //     thunkifyForAttr;
                //   - the static-attr non-rec branch (line 1075/1097);
                //   - tree-walker's `attrs->maybeThunk(state, env)` for
                //     ExprAttrs values.
                // Pre-fix bug: v3 lowered the value eagerly via
                // lowerExpr, so `{ "${var}" = SOME_CALL; }` would fully
                // evaluate SOME_CALL during attrset construction.  In
                // nixpkgs darwin/default.nix:420 the value is
                // `overrideLlvmPackagesScope super."llvmPackages_${llvmVersion}" cb`
                // — a function call to lib.makeOverridable that runs
                // its let-bindings (mirrorArgs / decorate /
                // recoverMetadata) eagerly, chaining through to the
                // inner `callPackages ../llvm { }` thunk while pkgs
                // (= lib.fix x) is still Black.
                ir::VarId valV  = thunkifyForAttr(da.valueExpr);
                dyn.dynamics.push_back({nameV, valV, posIdxToHandle(da.pos)});
            }
            if (pushedInheritFrom) {
                inheritFromStack.pop_back();
                inheritFromCacheStack.pop_back();
            }
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
        if (pushedInheritFrom) {
            inheritFromStack.pop_back();
            inheritFromCacheStack.pop_back();
        }
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
        // CO-2 phase B: record this as a rec-attrset VarId so the
        // force-hook post-pass can detect when a per-thunk function's
        // freeVars include a rec-attrset (which we can't materialise
        // from tree-walker's env in one cell read) and skip the
        // entry's Phase B path before paying the lookup cost.
        m.recVarIds.push_back(recVar);

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
            m.functions[fid].posHandle = posIdxToHandle(kv.second.pos);
            // CO-3 + WC-11: register every Let/Attrs binding's def
            // expression, not just top-level ones.  Same rationale
            // as `thunkify` (above): Nix is purely lexical, so
            // freeVars (level, displ) origins describe the lexical
            // scope regardless of dynamic call-site depth.  Without
            // this, real-world workloads only register depth-0
            // bindings, leaving the bulk of forced thunks with no
            // v3 candidate flag and forceEntries=0.
            //
            // 2026-05-06 #457/#458 NOTE: investigated whether to skip
            // this registration for Lambda values (the lowerLambda
            // pass at line 790 also registers the Lambda Expr* but
            // with the BODY fid).  Under emplace's "first-wins"
            // semantics, this thunk-fid wins, and v3's call-hook
            // ends up calling the thunk function (which just
            // creates a Closure value) instead of the lambda body.
            // That's actually what produces the Tag::Closure result
            // that #436's closure-result refusal protects against.
            // FIXING this exposed a deeper Slot/Thunk chase cycle
            // (#456) on every nixpkgs workload (cardano-node, hello.
            // name, git.name all errored on default v3) -- the
            // closure-result refusal was implicitly masking that
            // bug.  Until the chase-cycle is fixed, leave the
            // first-wins behaviour in place; it's the lesser evil.
            // 2026-05-06 #457/#458 NOTE: investigated whether to skip
            // Lambda values here -- lowerLambda (line ~790) registers
            // (lambda_expr, body_fid).  emplace's first-wins semantics
            // makes the THUNK fid win for Lambda Exprs, so the call-
            // hook calls the thunk (which produces a Closure value)
            // instead of the body.  That triggers the closure-result
            // refusal at v3CallFunctionEntry.
            //
            // Skipping Lambda values here lets the body-fid win.
            // Tested with the #456 chase-cycle fix in place: still
            // exposes a separate self-referential force pattern --
            // a thunk gets Black-marked, its body recursively
            // requires forcing itself, BlackHole thrown.  Frame
            // dump (V3_DBG_OPCYCLE) shows the same "self" lambda
            // re-entering itself across nested frames.  Real bug,
            // not just a chase artifact -- v3's slot-threading for
            // let-rec / fix-point captures isn't producing the
            // lazy-attr-access path TW uses.  Tracked as a separate
            // self-referential-force issue beyond #456.
            //
            // Until that's fixed, leave the first-wins behaviour in
            // place; the closure-result refusal at v3CallFunctionEntry
            // remains the safety net.
            //
            // 2026-05-06 #458 step 6/6: opt-in via NIX_V3_LAMBDA_SKIP=1.
            // With the slot-capture redesign (steps 1-5) in place, the
            // self-referential-force pattern that previously broke
            // lambda-skip should be sidestepped: closures captured
            // inside per-attr Lambda bodies that reference the rec-
            // attrset now see Tag::Slot pointing at heap-stable
            // storage rather than the wrap thunk.  Validating in
            // tandem with the slot-capture flag in this session.
            static const bool lambdaSkip =
                std::getenv("NIX_V3_LAMBDA_SKIP") != nullptr;
            const bool isLambda = kv.second.e
                && kv.second.e->exprKind == nix::Expr::Kind::Lambda;
            if (kv.second.e && !(lambdaSkip && isLambda))
                m.subExprFuncs.push_back({static_cast<const void *>(kv.second.e), fid});
            pending.push_back({kv.first, kv.second.kind, kv.second.e, fid, eb,
                                posIdxToHandle(kv.second.pos)});
        }

        Scope recScope;
        recScope.recAttrsVar = recVar;
        // #458 step 1/6: allocate a parallel VarId that will hold a
        // Tag::Slot pointing at heap-stable storage for the rec-
        // attrset Bindings.  Registered into Module::recVarToSlotVar
        // so the LetRec emitter can detect it and emit
        // OP_REC_SLOT_PUBLISH + OP_SET_LOCAL recSlotVar after
        // OP_ATTRS_REC_INIT.  Future increments (resolveVar) will
        // route inner-closure freeVars through recSlotVar instead of
        // recAttrsVar.  In this initial increment, recSlotVar is
        // ALLOCATED but only USED by the emitter (no resolveVar
        // change yet), so behaviour is unchanged but the runtime
        // infrastructure is exercised.
        ir::VarId recSlotVar = m.freshVar();
        recScope.recSlotVar = recSlotVar;
        m.recVarToSlotVar.emplace(recVar, recSlotVar);
        // #458 Phase B RecBuildSlot — register recSlotVar so v3_hook.cc
        // can detect its appearance as a freeVar of a sub-thunk and
        // emit a `Kind::RecBuildSlot` UpvalueSource that materialises
        // Tag::Slot from a TW env walk.  Without this set, lambda-
        // body freeVars referencing recSlotVar fall through Phase B's
        // varOrigins lookup with no match -> v3 refuses Phase B.
        m.recSlotVarIds.push_back(recSlotVar);
        recScope.recAttrsNames.reserve(pending.size());
        for (auto & p : pending) {
            recScope.recAttrsNames.push_back(internSym(p.sym));
            recScope.byDispl.push_back(ir::kInvalid);
            recScope.byName.emplace(std::string(symbols[p.sym]), ir::kInvalid);
        }

        // REVIEW HIGH-4 follow-up: hidden from-expr thunks built
        // for the rec case (see below).  Holds (hiddenVar, FuncId)
        // pairs that the LetRec emit will turn into a MkThunk
        // bound to hiddenVar's local slot, ordered after the
        // OP_DUP/SET_LOCAL recSlot prologue.
        struct PendingHidden {
            ir::VarId hiddenVar;
            ir::FuncId thunkBody;
        };
        std::vector<PendingHidden> pendingHidden;

        // Push the inheritFromExprs onto the stack so any ExprInheritFrom
        // encountered while lowering def.e resolves correctly.
        bool pushedInheritFrom = false;
        if (inheritFromExprs) {
            inheritFromStack.push_back(inheritFromExprs);
            // REVIEW HIGH-4: pre-lower from-exprs into the OUTER
            // function so all N names in `inherit (e) ...` share one
            // VarId.  Only safe when `isRec=false` (the from-exprs
            // don't reference rec-bindings); for `isRec=true` we use
            // the hidden-thunk mechanism (REVIEW HIGH-4 follow-up):
            // each from-expr lowers into a Function with the rec
            // scope, the function's MkThunk is emitted by emit.cc
            // immediately AFTER OP_DUP/SET_LOCAL recSlot (so recVar
            // is bound), and the resulting Tag::Thunk Value is stored
            // in a hidden VarId in the LetRec's containing block.
            // Per-attr bodies capture hiddenVar as a regular upvalue;
            // each `inherit (e) name` lowers to AttrSelect(hidden, name)
            // so all N names share one force of `e`.
            if (!isRec) {
                pushInheritFromCache(inheritFromExprs);
            } else {
                // #444: address by INDEX, not by reference -- the
                // recursive `lowerExpr(fromE)` call below can re-enter
                // `lowerLetRecCapture` and emplace_back onto the
                // SAME `inheritFromCacheStack`, invalidating any
                // outstanding `back()` reference.  Manifested as
                // a write to a poisoned address (`cache[displ] = ...`)
                // when a nested let-rec contained another let-rec
                // with `inherit (e) ...`.  Hard to trigger from
                // user code today (eval-hook only sees a flat root
                // Expr) but lethal under parse-time / on-demand
                // precompile patterns where the lowerer runs on
                // the FULL nested AST in one go.
                inheritFromCacheStack.emplace_back();
                size_t cacheIdx = inheritFromCacheStack.size() - 1;
                inheritFromCacheStack[cacheIdx].resize(
                    inheritFromExprs->size(), ir::kInvalid);
                for (size_t displ = 0; displ < inheritFromExprs->size(); ++displ) {
                    nix::Expr * fromE = (*inheritFromExprs)[displ];
                    if (!fromE) continue;
                    // Synthesize a Function for the from-expr body,
                    // lowered in the rec scope.
                    m.functions.emplace_back();
                    ir::FuncId hfid = static_cast<ir::FuncId>(m.functions.size() - 1);
                    auto heb = m.freshBlock();
                    m.functions[hfid].entryBlock = heb;
                    m.functions[hfid].name = "<inherit-from>";
                    funcStack.push_back(hfid);
                    blockStack.push_back(heb);
                    scopes.push_back(recScope);
                    ir::VarId hrv = lowerExpr(fromE);
                    setReturn(hrv);
                    scopes.pop_back();
                    blockStack.pop_back();
                    funcStack.pop_back();
                    // Hidden Var lives in the LetRec's containing
                    // block; the LetRec emit code wires the
                    // OP_MAKE_THUNK + OP_SET_LOCAL after recVar is
                    // bound.
                    ir::VarId hiddenVar = m.freshVar();
                    // Re-fetch by index; vector may have re-allocated
                    // during the recursive lower above.
                    inheritFromCacheStack[cacheIdx][displ] = hiddenVar;
                    pendingHidden.push_back({hiddenVar, hfid});
                }
            }
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

        if (pushedInheritFrom) {
            inheritFromStack.pop_back();
            inheritFromCacheStack.pop_back();
        }

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
        // REVIEW HIGH-4 follow-up: attach hidden from-expr thunks.
        letRec.hiddenEntries.reserve(pendingHidden.size());
        for (auto & ph : pendingHidden) {
            ir::LetRec::HiddenEntry he;
            he.hiddenVar = ph.hiddenVar;
            he.thunkBody = ph.thunkBody;
            letRec.hiddenEntries.push_back(std::move(he));
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
        // REVIEW MED-5: dropped emit-time forces.  OP_ATTRS_SELECT /
        // OP_ATTRS_HAS / *_DYN all force their attrs operand (and the
        // dynamic name operand) on the runtime fast path.

        // Static name: regular AttrSelect / HasAttr.  Dynamic name
        // (`attrs.${expr}`): evaluate expr to a string at runtime and
        // use OP_ATTRS_SELECT_DYN / OP_ATTRS_HAS_DYN.
        const bool dyn = step.expr != nullptr;
        ir::SymbolId nm = dyn ? 0 : internSym(step.symbol);
        ir::VarId nameVar = ir::kInvalid;
        if (dyn) nameVar = lowerExpr(step.expr);

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
        // REVIEW MED-5: emit-time force dropped; OP_ATTRS_HAS / SELECT
        // force at runtime.
        ir::VarId attrs = lowerExpr(e->e);
        ir::VarId result = ir::kInvalid;
        // Build the chain bottom-up.  We model it inline here using
        // nested If blocks: `if hasAttr(attrs, p0) then if has(attrs.p0, p1) ... else false else false`.
        std::function<ir::VarId(ir::VarId, size_t)> step =
            [&](ir::VarId cur, size_t idx) -> ir::VarId {
                const auto & an = e->attrPath[idx];
                bool dyn = an.expr != nullptr;
                ir::VarId nameVar = ir::kInvalid;
                // REVIEW MED-5: dyn name force dropped; OP_ATTRS_*_DYN forces.
                if (dyn) nameVar = lowerExpr(an.expr);
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
                // REVIEW MED-5: chain-step force dropped; the next
                // OP_ATTRS_HAS forces.
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
        //
        // WC-38 SECD-style slot aliasing.
        //
        // When `e->attrs` is an ExprVar that resolves to a rec-attrset
        // entry, emit OP_REC_BINDING_SLOT_REF — a stable pointer into
        // the Bindings::entries[i].value heap memory.  This is
        // crucial for `with self;` patterns over let-rec / rec
        // attrsets (e.g., nixpkgs's `lib.fix toFix` and overlay-
        // extend patterns) so that sub-thunks captured inside the
        // with-body observe the entry's mutated/memoized value
        // through the slot rather than a snapshot taken before the
        // entry was forced.
        //
        // Direct value-stack slot references (lambda params etc.)
        // remain unhandled here — they have lifetime issues (the
        // slot's storage is reused after the surrounding frame
        // returns) and would dangle when sub-thunks escape.
        ir::VarId attrs = lowerExpr(e->attrs);
        ir::VarId withRecAttrsVar = ir::kInvalid;
        ir::SymbolId withRecAttrsName = ir::kInvalidSymbol;
        if (auto * ev = dynamic_cast<nix::ExprVar *>(e->attrs)) {
            if (!ev->fromWith && ev->level < scopes.size()) {
                size_t scopeIdx = scopes.size() - 1 - ev->level;
                // Check the rec-attrset entry path: if the var lives
                // in a rec scope and `byDispl[displ]` is invalid
                // (i.e., resolveVar would synthesize an AttrSelect),
                // we have a heap-stable slot target.
                bool byDisplDirect =
                    ev->displ < scopes[scopeIdx].byDispl.size()
                    && scopes[scopeIdx].byDispl[ev->displ] != ir::kInvalid;
                bool inRecScope =
                    scopes[scopeIdx].recAttrsVar != ir::kInvalid
                    && ev->displ < scopes[scopeIdx].recAttrsNames.size();
                if (!byDisplDirect && inRecScope) {
                    withRecAttrsVar = scopes[scopeIdx].recAttrsVar;
                    withRecAttrsName = scopes[scopeIdx].recAttrsNames[ev->displ];
                    // #458 step 3/6: route through recSlotVar in slot-
                    // capture mode so With's emit doesn't force the
                    // wrap thunk.  emit pushes recSlotVar (Tag::Slot)
                    // and OP_REC_BINDING_SLOT_REF derefs internally.
                    static const bool slotCapture =
                        std::getenv("NIX_V3_NO_REC_SLOT_CAPTURE") == nullptr;
                    if (slotCapture
                        && scopes[scopeIdx].recSlotVar != ir::kInvalid)
                    {
                        withRecAttrsVar = scopes[scopeIdx].recSlotVar;
                    }
                }
            }
        }
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
        return addBinding(ir::With{
            attrs, bodyB,
            withRecAttrsVar, withRecAttrsName});
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

// Scope the forceVal-line-capture macro to this TU only.
#undef forceVal

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
