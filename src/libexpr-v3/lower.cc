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
#include "v3/bytecode_primops.hh"

#include "nix/expr/nixexpr.hh"
#include "nix/expr/symbol-table.hh"
#include "nix/util/position.hh"

#include <deque>
#include <functional>
#include <sstream>
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
    /// #495 follow-on: tag a scope as introduced by a Lambda (param +
    /// formals).  pushInheritFromCache uses this to decide whether a
    /// from-expr's head ExprVar resolves to a lambda parameter -- such
    /// vars can be mid-construction (e.g. fix0's `let x = f x; in x`
    /// passes the not-yet-evaluated x as the lambda arg), so eager
    /// `lowerExpr` of `param.X` triggers a recursion.  Other scopes
    /// (Let, Rec, With) are tagged with their respective kinds.
    enum class Kind : uint8_t {
        Lambda,   ///< function body — byDispl[0..N] are param + formals
        Let,      ///< plain let body — byDispl[i] are bindings
        Rec,      ///< rec attrset / let-rec — recAttrsVar set
        With,     ///< inside `with E;` body
        Other,    ///< unknown / irrelevant for our heuristic
    };
    Kind kind = Kind::Other;

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

    /// #530 lexical-with chain — the IR VarId of this `with` scope's
    /// `attrs` expression.  Set only on `Kind::With` scopes pushed by
    /// `lowerWith`; collectLexicalWiths() walks the scopes stack and
    /// gathers these VarIds (outermost-first) into MkThunk / Lambda
    /// `lexicalWiths`, so that thunks and closures created inside a
    /// `with X;` body materialise their captured-with chain at make
    /// time from the *static* lexical structure rather than from a
    /// runtime snapshot of the with-stack.
    ///
    /// `kInvalid` on every non-With-kind scope.
    ir::VarId withTargetVar = ir::kInvalid;
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

    /// STG-13a (#509/#510): deferred intrinsic-kind assignments for
    /// inner lambdas of a recognised chain.  When recogniseIntrinsic
    /// matches the OUTER lambda of an Extends or ComposeExtensions
    /// chain, it walks to chain[2]/chain[3] and records the desired
    /// inner intrinsic kind here.  lowerLambda checks this map after
    /// running recogniseIntrinsic on the inner lambda; if the inner
    /// lambda is in the map, the deferred kind overrides the inner's
    /// own recognition (which would return None for a chain[2]/[3]
    /// body in isolation).
    ///
    /// The mapped value also carries the SymbolIds of the captured
    /// vars (overlay/f for ExtendsBody; f/g/final for ComposeBody) so
    /// emit can compute upvalue indices later.
    struct DeferredIntrinsic {
        uint8_t  kind;          ///< ExtendsBody=5 or ComposeBody=6
        ir::SymbolId sym0;      ///< overlay (Extends) or f (Compose)
        ir::SymbolId sym1;      ///< f       (Extends) or g (Compose)
        ir::SymbolId sym2;      ///< unused  (Extends) or final (Compose)
    };
    std::unordered_map<nix::ExprLambda *, DeferredIntrinsic>
        deferredIntrinsics;

    explicit Lowerer(const nix::SymbolTable & st) : symbols(st) {}
    Lowerer(const nix::SymbolTable & st, const nix::PosTable & pt)
        : symbols(st), positions(&pt) {}

    /// #530 lexical-with chain — collect the IR VarIds of every
    /// enclosing `with X;` scope, in outermost-first order, that lies
    /// **within the current function frame**.  We stop walking outward
    /// at the first Kind::Lambda boundary because a closure / thunk
    /// emitted in this frame has its own freeVars list for vars that
    /// crossed the lambda boundary; the lexical-with chain we attach
    /// here is consumed by emit at MAKE time using emitVarRef, which
    /// itself handles upvalue translation if a `with`-target VarId
    /// resolves to an upvalue of the maker function.  In other words
    /// emit doesn't care whether the var is local or upvalue — but to
    /// stay symmetric with how freeVars are populated we collect all
    /// With scopes regardless of whether they cross a Lambda
    /// boundary; computeFreeVars's collectExprRefs will route the
    /// VarIds into the maker function's freeVars set if needed.
    ///
    /// Order is outermost-first: the first VarId in the returned
    /// vector is the OUTERMOST enclosing `with` (closest to file
    /// root); the last is the INNERMOST.  This matches the order
    /// `pushCapturedWiths` pushes onto the runtime with-stack so
    /// OP_WITH_LOOKUP's reverse scan finds innermost-binds-first.
    std::vector<ir::VarId> collectLexicalWiths() const
    {
        std::vector<ir::VarId> out;
        out.reserve(scopes.size());
        // Walk outermost-first (front-to-back), keeping every
        // Kind::With scope's withTargetVar.  We do NOT stop at the
        // Kind::Lambda boundary — the With-chain is a global lexical
        // property of the source location.  emit / runtime handle the
        // upvalue translation if a with-target VarId came from a
        // surrounding function.
        for (const Scope & s : scopes) {
            if (s.kind == Scope::Kind::With && s.withTargetVar != ir::kInvalid)
                out.push_back(s.withTargetVar);
        }
        return out;
    }

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

        // #530 lexical-with chain — capture the lexical with-targets
        // visible at the thunkify site so the resulting thunk's
        // capturedWiths are populated from the static structure.
        std::vector<ir::VarId> lws = collectLexicalWiths();
        m.functions[fid].nWithTargets =
            static_cast<uint16_t>(lws.size());
        return addBinding(
            ir::MkThunk{fid, /*freeVars*/ {}, lws});
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
        // #558: post-lower pass — mark each function's tail-return
        // AttrSet so emit can choose the right opcode.
        markTailReturnAttrSets();
        return std::move(m);
    }

    // ---------------------------------------------------------------
    // #558 post-lower analysis: tag the AttrSet that is each function's
    // tail-return value (the value forcing the surrounding thunk
    // produces) with `isFunctionReturn = true`.  This drives emit's
    // choice of OP_ATTRS_REC_INIT_TAIL (publishes to all thunk frames)
    // vs OP_ATTRS_REC_INIT (publishes only to innermost Black thunk).
    //
    // The analysis walks the entry block's terminal-return value
    // through transparent wrappers (With, Assert, If) until it finds
    // the underlying AttrSet binding.  For If, both branches are
    // potential returns and both are marked.  For LetRec with
    // hasBody=false (i.e. `rec { ... }` IS the value), the rec-attrs
    // already publishes via its own OP_ATTRS_REC_INIT path; we don't
    // need to mark it (LetRec doesn't have an AttrSet IR node, just a
    // direct LetRec node which emit handles separately).
    //
    // Edge cases NOT yet handled:
    //   - Tail return through a function call (e.g. `f x`); the
    //     call's result is the return value but no AttrSet is built
    //     in *this* function — the AttrSet is built in `f`'s body
    //     (which gets its own marking pass).
    //   - Tail return via Update (`A // B`); the result is a fresh
    //     Bindings allocated by OP_ATTRS_UPDATE, not via REC_INIT,
    //     so the publish path doesn't apply.
    // ---------------------------------------------------------------
    void markTailReturnAttrSets()
    {
        for (ir::FuncId fid = 0;
             fid < (ir::FuncId)m.functions.size(); ++fid) {
            ir::BlockId be = m.functions[fid].entryBlock;
            if (be == ir::kInvalidBlock) continue;
            markTailAttrSetInBlock(be);
        }
    }

    void markTailAttrSetInBlock(ir::BlockId blkId)
    {
        if (blkId == ir::kInvalidBlock
            || blkId >= m.blocks.size()) return;
        auto & blk = m.blocks[blkId];
        auto * ret = std::get_if<ir::TermReturn>(&blk.terminal);
        if (!ret || ret->value == ir::kInvalid) return;
        markTailVarInBlock(blkId, ret->value, /*depth=*/0);
    }

    // Recursively follow a VarId through transparent IR wrappers
    // (VarRef, With, Assert, If) inside a single block, marking the
    // ultimate AttrSet binding as the function's tail-return.
    //
    // Depth bound prevents pathological cycles (the IR shouldn't have
    // any, but defensive — VarRef chains in particular could loop if a
    // future optimizer creates them).
    void markTailVarInBlock(ir::BlockId blkId, ir::VarId target,
                            int depth)
    {
        if (depth > 16) return;  // pathological-loop guard
        if (blkId == ir::kInvalidBlock
            || blkId >= m.blocks.size()) return;
        if (target == ir::kInvalid) return;
        auto & blk = m.blocks[blkId];
        for (auto & bd : blk.bindings) {
            if (bd.var != target) continue;
            std::visit([&](auto & e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ir::AttrSet>) {
                    e.isFunctionReturn = true;
                } else if constexpr (std::is_same_v<T, ir::Update>) {
                    // #558 (2026-05-10) Update in tail position is the
                    // STG analog of "constructor allocation reaches
                    // WHNF" for // results: the merged Bindings IS
                    // this function's eventual return value.  Mark it
                    // so emit picks OP_ATTRS_UPDATE_TAIL.
                    //
                    // Gated by NIX_V3_NO_UPDATE_TAIL=1 for bisecting.
                    static const bool s_disabled =
                        std::getenv("NIX_V3_NO_UPDATE_TAIL") != nullptr;
                    if (!s_disabled)
                        e.isFunctionReturn = true;
                } else if constexpr (std::is_same_v<T, ir::With>) {
                    // `with X; body` is transparent — body's
                    // terminal-return is the function's return.
                    markTailAttrSetInBlock(e.bodyBlock);
                } else if constexpr (std::is_same_v<T, ir::Assert>) {
                    markTailAttrSetInBlock(e.bodyBlock);
                } else if constexpr (std::is_same_v<T, ir::If>) {
                    // Both branches are potential returns.
                    markTailAttrSetInBlock(e.thenBlock);
                    markTailAttrSetInBlock(e.elseBlock);
                } else if constexpr (std::is_same_v<T, ir::VarRef>) {
                    // VarRef is an alias.  `let X = ...; in body`
                    // wraps the body's value in a VarRef binding (see
                    // lowerLetRec), so following the VarRef finds the
                    // actual body AttrSet (in the same block).
                    markTailVarInBlock(blkId, e.var, depth + 1);
                }
                // Other expr kinds (App, Update, ConcatLists, etc.)
                // produce values that aren't AttrSet REC_INIT outputs;
                // the publish path doesn't apply.
            }, bd.expr);
            break;
        }
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
        // #516 follow-on: NIX_V3_NO_INTRINSIC_RECOGNISE=1 disables ALL
        // intrinsic recognition (Fix / Extends / Compose / etc.).
        // Without this, even when the runtime dispatch
        // (NIX_V3_INTRINSIC_DISPATCH) is OFF, the recognition still
        // sets `deferredIntrinsics` for chain[2]/chain[3] which carries
        // through to ir::Function::intrinsicKind.  Used to bisect
        // whether the recognition itself has a side effect on
        // lowering of inner lambdas (`final:` body of Extends has
        // `let prev = ...; in prev // overlay final prev`).
        static const bool s_disableRecognise =
            std::getenv("NIX_V3_NO_INTRINSIC_RECOGNISE") != nullptr;
        if (s_disableRecognise) return 0;
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

                // STG-13a (#509/#510): mark chain[2] (the final-lambda)
                // as ExtendsBody so OP_CALL/callClosure dispatches to
                // the v3-native impl.  Carry the SymbolIds of overlay
                // and f (chain[0]'s arg + chain[1]'s arg) so emit can
                // compute upvalue indices for the native dispatch.
                deferredIntrinsics[lamFinal] = DeferredIntrinsic{
                    /*kind*/ 5u,  // Intrinsic::ExtendsBody
                    /*sym0 overlay*/ internSym(e->arg),
                    /*sym1 f*/ internSym(lamF->arg),
                    /*sym2 unused*/ 0,
                };

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

                // STG-13a (#509/#510): mark chain[3] (the prev-lambda)
                // as ComposeBody so OP_CALL/callClosure dispatches to
                // the v3-native impl.  Carry the SymbolIds of f, g and
                // final (chain[0..2]'s args) so emit can compute
                // upvalue indices.  prev is the runtime arg (local 0),
                // not an upvalue.
                deferredIntrinsics[lamPrev] = DeferredIntrinsic{
                    /*kind*/ 6u,  // Intrinsic::ComposeBody
                    /*sym0 f*/ internSym(e->arg),
                    /*sym1 g*/ internSym(lamG->arg),
                    /*sym2 final*/ internSym(lamFinal->arg),
                };

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
        // STG-13a (#509/#510): inner-lambda marker.  recogniseIntrinsic
        // populates deferredIntrinsics for chain[2]/chain[3] when matching
        // Extends/ComposeExtensions on the outer lambda; here we apply the
        // marker to the inner lambda's ir::Function as it gets lowered
        // recursively.  The outer chain's recogniseIntrinsic-on-inner call
        // would return 0 (chain[2] in isolation isn't a known shape); the
        // deferred map overrides that.
        //
        // We also resolve the captured-var SymbolIds (overlay/f/... for
        // Extends; f/g/final for Compose) to their VarIds via the live
        // scope stack: at this point in lowerLambda, scopes still
        // contains the parent chain[0..n-1] with byName populated, so a
        // simple top-down search finds each VarId.  The VarIds are
        // recorded on the inner ir::Function and looked up at emit time
        // to fill in LambdaDescriptor::intrinsicVar0/1/2 (upvalue
        // indices into the closure).
        if (auto it = deferredIntrinsics.find(e); it != deferredIntrinsics.end()) {
            m.functions[fid].intrinsicKind = it->second.kind;
            // Resolve each named capture to a VarId by walking scopes.
            // The outermost scope (chain[0]'s lambda body context) is
            // closest to the back end of the vector; chain[2]/chain[3]
            // hasn't pushed its inner scope yet.  Search innermost-first
            // (= highest index) which honours shadowing if any.
            auto findVarBySymbol = [&](ir::SymbolId sid) -> ir::VarId {
                if (sid == 0) return ir::kInvalid;
                const auto & tbl = ir::globalSymbolTable();
                if (sid >= tbl.size()) return ir::kInvalid;
                const std::string & nm = tbl[sid];
                for (auto sIt = scopes.rbegin(); sIt != scopes.rend(); ++sIt) {
                    auto bIt = sIt->byName.find(nm);
                    if (bIt != sIt->byName.end()) return bIt->second;
                }
                return ir::kInvalid;
            };
            m.functions[fid].intrinsicVar0 = findVarBySymbol(it->second.sym0);
            m.functions[fid].intrinsicVar1 = findVarBySymbol(it->second.sym1);
            m.functions[fid].intrinsicVar2 = findVarBySymbol(it->second.sym2);
        }
        {
            static const bool s_dbg =
                std::getenv("V3_DBG_INTRINSIC") != nullptr;
            if (s_dbg) {
                static const char * names[] = {
                    "None", "Fix", "Extends", "ComposeExtensions",
                    "ComposeManyExtensions",
                    "ExtendsBody", "ComposeBody",
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
        inner.kind = Scope::Kind::Lambda;

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
            recScope.kind = Scope::Kind::Lambda;
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
            // Synthetic LetRec for default-bearing formals: the rec
            // attrs is INTERMEDIATE state -- the lambda body
            // (lowered below) is what produces the lambda's return
            // value.  Mark hasBody=true so the emitter selects
            // OP_ATTRS_LET_REC_INIT (no publish-to-Black-thunk),
            // matching the lowerLet path.  Without this, every
            // default-bearing formals lambda publishes the placeholder
            // formals attrs onto its surrounding Black thunk -- a
            // latent variant of the v3-direct callPackage with-scope
            // bug (CALLPACKAGE_BUG_2026-05-09.md).
            letRec.hasBody = true;
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

        // #530 lexical-with chain — capture the lexical with-targets
        // visible at this lambda's creation site so the resulting
        // closure's capturedWiths are populated from the static
        // structure.
        std::vector<ir::VarId> lws = collectLexicalWiths();
        m.functions[fid].nWithTargets =
            static_cast<uint16_t>(lws.size());
        return addBinding(
            ir::Lambda{ fid, /*freeVars*/ {}, lws });
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
            && e->args->size() >= po->arity
            // A12b T0b: skip the static PrimOpCall emission for primops
            // that have a bytecode-closure replacement installed.  The
            // call falls through to the App-chain path below, where
            // each App dispatches via OP_CALL on the closure Value
            // pushed by OP_LIT_PRIMOP's redirect.  Inline-and-check
            // (NOT a separate `if (po) { ... }` outside) so a primop
            // ref with size<arity (partial application like
            // `builtins.foldl' op acc`) cleanly falls through — the
            // size<arity short-circuit MUST gate the primop emission,
            // otherwise the inner loop's `++it` walks past end() (UB
            // — produces "nullptr expr" lower errors on patterns like
            // `(import <nixpkgs>) {}`).
            && !lookupPrimopReplacement(po))
        {
            const std::string_view name(po->name);

            // 2026-05-17 seq fast-path: `builtins.seq x y` emits as an
            // OP_FORCE on x's bytecode form + lowerExpr(y).  Saves the
            // OP_CALL_PRIMOP + the C-recursive forceValue(args[0]) the
            // arg-prep loop would otherwise do (vm.cc:2876).  OP_FORCE
            // uses op_force_slow + CFF_FORCE_RETRY which drives the
            // force iteratively via vm.frames pushes — no C-recursion.
            //
            // Used by the primDerivation* hybrid wrapper to pre-force
            // arbitrary-typed attrs at bytecode level before passing
            // them to the C leaf primop (Option 4 in the strategic
            // primDerivation* note).
            //
            // Mechanism: the force result is captured in a fresh VarId
            // that emit.cc allocates a local slot for; it's never
            // referenced downstream, so the slot stays unused (but the
            // OP_FORCE side effect fires).  DCE preserves ir::Force
            // (it's classified Impure — see opt_dce.cc).
            if (po->arity == 2 && (name == "seq" || name == "__seq")
                && e->args->size() >= 2)
            {
                auto it = e->args->begin();
                ir::VarId xv = lowerExpr(*it);
                (void)forceVal(xv);  // force x; result intentionally unused
                ++it;
                ir::VarId result = lowerExpr(*it);
                ++it;
                // Extra args (Nix's `builtins.seq A B C D` parses as
                // (((seq A) B) C) D): seq returns B, which is then
                // applied to C, D, ... Mirror the App-chain handling
                // from the generic primop path below so curried-extras
                // semantics is preserved.
                for (; it != e->args->end(); ++it) {
                    result = forceVal(result);
                    ir::VarId av = lowerExpr(*it);
                    result = addBinding(ir::App{result, av});
                }
                return result;
            }

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
        //
        // BR-3 bug-1 (2026-05-13): Nix's parser produces a single
        // `ExprConcatStrings` node for BOTH `a + b` arithmetic and
        // `"...${expr}..."` string interpolation.  The two are
        // distinguished by the `forceString` flag (false for the
        // former, true for the latter).  Treating an interpolation as
        // "trivial for arg" is WRONG — the interpolated parts may
        // contain attribute access (`${pkg.missing}`) which would
        // throw at lower-time when the function it's passed to never
        // actually uses the arg (e.g. lib.optionalString's `else`
        // branch).  Repro: `optionalString false "${obj.missing}"`
        // throws under v3-direct but returns "" under TW.  Arithmetic
        // `n + 1` stays eager because (a) Nix `+` on Int/Float types
        // is itself strict in both operands and (b) hot benchmarks
        // (fib) chain many `f (n - 1)` calls where the thunk overhead
        // would dominate.
        if (forArg) {
            if (k == nix::Expr::Kind::ConcatStrings) {
                auto * cs = static_cast<nix::ExprConcatStrings *>(e);
                // String interpolation — must thunk so unused branches
                // don't evaluate the parts.
                if (cs->forceString) return false;
                // BR-3 bug-2 (2026-05-18): `''str ${X} str'' + Y` is
                // parsed as an OUTER ExprConcatStrings(forceString=false,
                // [(p1, inner-interp), (p2, Y)]).  The OUTER fS=false
                // suggested "arithmetic — safe eager" historically, but
                // the INNER operand may be an ExprConcatStrings with
                // forceString=true (the `''str ${X} str''` interpolation)
                // whose eager lowering forces ${X} immediately.
                //
                // Repro: forcing `stdenv.cc.drvAttrs.postFixup` on aarch64-
                // darwin nixpkgs hits `targetPackages.stdenv.cc.isGNU`
                // (null) because the optionalString call at cc-wrapper.nix:
                // 665-682 has body `''shell ${gccForLibs}...'' + optionals
                // (!isArocc) (...)` whose outer `+` matched fS=false here
                // and the inner ${gccForLibs} got force-emitted in the
                // outer block (see project_cc_wrapper_bisection_2026-05-18.md
                // for the trace divergence and IR evidence).
                //
                // Fix: recursively check operands.  If any operand is
                // itself a string-interpolation (fS=true) ConcatStrings,
                // the whole `+` expression is not safe-eager; thunkify
                // the outer.  Arithmetic `n + 1` (all operands trivial
                // Int/Var/etc.) still bypasses thunkification — fib's
                // hot-path `f (n - 1)` chain stays cheap.
                for (auto & p : cs->es) {
                    nix::Expr * sub = p.second;
                    if (!sub) continue;
                    if (sub->exprKind == nix::Expr::Kind::ConcatStrings) {
                        auto * subCs = static_cast<nix::ExprConcatStrings *>(sub);
                        if (subCs->forceString) return false;
                    }
                }
                return true;  // pure arithmetic — safe eager
            }
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
        // (Historical: prior Phase B safety nets — recVarSet,
        // upvalueSources.empty(), phaseBFailed — lived in the now-
        // deleted v3_hook.cc.  v3-direct relies on the lexical
        // correctness argument above; bugs surface as cycles or
        // wrong-value, not silently routed to tree-walker.)
        m.subExprFuncs.push_back({static_cast<const void *>(e), fid});

        funcStack.push_back(fid);
        blockStack.push_back(entry);
        ir::VarId rv = lowerExpr(e);
        setReturn(rv);
        blockStack.pop_back();
        funcStack.pop_back();

        // #530 lexical-with chain — capture the lexical with-targets
        // visible at this thunkify site so the resulting thunk's
        // capturedWiths are populated from the static structure.
        std::vector<ir::VarId> lws = collectLexicalWiths();
        m.functions[fid].nWithTargets =
            static_cast<uint16_t>(lws.size());
        return addBinding(
            ir::MkThunk{fid, /*freeVars*/ {}, lws});
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
        //
        // #495 follow-on: targeted thunkify for the `self.X`-from-expr
        // pattern even in DEFAULT mode -- simple-arg `self:` lambdas
        // route to v3 even in default mode, and eager lowering of
        // `inherit (self.X) Y` forces `self` mid-construction and
        // recurses (reproducer:
        // src/libexpr-v3/test/run-fix-inherit-from-self-tests.sh).
        //
        // Detection: the from-expr is an ExprSelect whose head is an
        // ExprVar that refers to the immediately-enclosing lambda's
        // parameter (heuristic: ExprVar with level==0).  An always-on
        // blanket thunkify of every non-Var from-expr broke
        // rec-sibling resolution under v3 (`callPackage` undefined in
        // nixpkgs all-packages.nix:2276), so keep the existing
        // LAMBDA_SKIP-gated full thunkify and only ADD the targeted
        // self-X thunkify on top.
        //
        // Override via:
        //   NIX_V3_NO_INHERIT_FROM_THUNK=1   -- disable all thunkify
        //   NIX_V3_INHERIT_FROM_THUNK_ALL=1  -- force thunkify all
        //
        // STG-5 (#547): under NIX_V3_STG=1 (explicit opt-in for the
        // lower-time behavior change), thunkify ALL inherit-from
        // from-exprs unconditionally — matches TW's
        // `from->maybeThunk(state, up)` in
        // ExprAttrs::buildInheritFromEnv (libexpr/eval.cc:1520).
        //
        // Note: this stays opt-in even after #547 flipped the
        // *runtime* STG gates default-on -- the lower-time blanket
        // thunkify changes the static IR (more MkThunk nodes), and
        // empirical testing showed it triggers different forcing
        // behaviours that aren't yet hardened.  Tracked separately
        // for re-flip once cell-update + slot semantics are
        // universally wired (CALLPACKAGE_BUG_2026-05-09.md context).
        static const bool s_stgMode =
            std::getenv("NIX_V3_STG") != nullptr;
        static const bool s_lambdaSkip =
            std::getenv("NIX_V3_LAMBDA_SKIP") != nullptr;
        static const bool s_noThunkify =
            std::getenv("NIX_V3_NO_INHERIT_FROM_THUNK") != nullptr;
        // #558 Phase 3 (2026-05-12): flipped DEFAULT-ON.  THUNK_ALL is
        // required for cell-update-everywhere correctness — under
        // NIX_V3_NO_PARTIAL_BINDINGS=1 (also default-on), the libsForQt5
        // emit-order tests fail without THUNK_ALL because partial-
        // Bindings was the legacy workaround for v3's eager inherit-from
        // lowering.  With THUNK_ALL, inherit-from from-exprs are
        // unconditionally lazy (matching TW's `from->maybeThunk`), so
        // the cycle never arises.
        //
        // Opt back to the legacy "thunkify only complex from-exprs"
        // mode via NIX_V3_NO_INHERIT_FROM_THUNK_ALL=1.
        static const bool s_noThunkifyAll =
            std::getenv("NIX_V3_NO_INHERIT_FROM_THUNK_ALL") != nullptr;
        static const bool s_thunkifyAll = !s_noThunkifyAll;
        const bool useThunkBlanket = (s_lambdaSkip || s_thunkifyAll) && !s_noThunkify;

        // Heuristic for the `self.X` shape: ExprSelect whose head is
        // an ExprVar that resolves directly to the IMMEDIATELY enclosing
        // simple-arg lambda's parameter (level 0, scope tagged Lambda,
        // no formals, not from-with).  This catches `self: { inherit
        // (self.X) Y }` directly; nixpkgs lib's nested-let case
        // (level >= 1 self-dot via intermediate `let`) is a known-fail
        // -- a broader heuristic (any Lambda-scope var, regardless of
        // level) regressed nixpkgs hello.name with `OP_ATTRS_SELECT:
        // attribute not found` errors that point at upvalue-capture
        // bugs in the thunkify path under deep nesting.  Solving that
        // is a separate investigation (#495 follow-on).
        // The maximum level offset we'll thunkify a self-dot-pattern at.
        // 0 = only the immediately-enclosing simple-arg lambda's parameter
        // (the fix-inherit-from-self repro shape).  Higher = walk N scope
        // levels to reach a Lambda scope (catches the nixpkgs lib.fix
        // pattern: `lambda_param: let X = ...; in { inherit
        // (lambda_param.Y) Z }`, where the Let bumps the level).  But
        // higher also currently triggers a wrong-upvalue-capture bug in
        // nixpkgs all-packages (`OP_ATTRS_SELECT: attribute not found
        // "isx86"` -- platform closure receives the wrong attrset).  Keep
        // default at 0; opt into the broader gate via
        // NIX_V3_SELF_DOT_MAX_LEVEL=N for testing.  TODO: root-cause and
        // fix the upvalue capture, then default to a safe higher level.
        //
        // NOTE: NIX_V3_INTRINSIC_DISPATCH=1 alone does NOT auto-bump
        // this, because intrinsic dispatch + nixpkgs (e.g. evaluating
        // hello.name with NIX_V3_INTRINSIC_DISPATCH=1) trips the same
        // upvalue bug.  Users who want the full lib.fix path must opt
        // into BOTH NIX_V3_INTRINSIC_DISPATCH=1 and
        // NIX_V3_SELF_DOT_MAX_LEVEL=N explicitly.
        // #528: default-bumped to 4 (was 0).  Level=N walks N scope
        // levels to find a Lambda scope (so an `inherit (lambda.X) Y`
        // wrapped in N nested `let`s gets thunkified).
        //
        // Why default 4 (not 1 or 0):
        //   - level=0 (prior default) only catches `self: { inherit
        //     (self.X) Y }` directly.  Any inner `let` bumps level≥1
        //     and falls back to eager.
        //   - level=1 catches the `let helper = ...; in {inherit
        //     (self.X) Y}` shape (our minimal regression repro).
        //   - level=2-4 covers `let A = ...; in let B = ...; in {...}`
        //     with multiple intermediate lets, which appears in
        //     nixpkgs lib (nested let bindings in module evaluation).
        //
        // The thunkify is correctness-driven (defers `self.X`
        // evaluation until `Y` is accessed, matching TW's
        // `from->maybeThunk(state, up)`).  The narrow gate exists
        // only because earlier broader sweeps regressed nixpkgs (the
        // `OP_ATTRS_SELECT 'isx86' not found` and `callPackage
        // missing` issues).  Under NIX_V3_DIRECT_EVAL (the inversion
        // path) those issues stem from upstream eval-order divergences
        // that the bridge cycle had been masking; default-on at
        // level=4 is verified safe by run-self-dot-thunkify-tests.sh
        // (9/9), run-lang-tests (142/142), run-cutover-parity
        // (140/142, same pre-existing diffs), run-direct-eval-tests
        // (26/26).
        //
        // Override via NIX_V3_SELF_DOT_MAX_LEVEL=N (set to 0 to
        // revert to the prior gate for bisection).
        static const unsigned s_maxLevel = []() -> unsigned {
            const char * s = std::getenv("NIX_V3_SELF_DOT_MAX_LEVEL");
            return s ? unsigned(std::atoi(s)) : 4u;
        }();
        auto isSelfDotPattern = [this](nix::Expr * fx) -> bool {
            auto * sel = dynamic_cast<nix::ExprSelect *>(fx);
            if (!sel) return false;
            auto * v = dynamic_cast<nix::ExprVar *>(sel->e);
            if (!v) return false;
            if (v->fromWith) return false;
            if (v->level > s_maxLevel) return false;
            if (v->level >= scopes.size()) return false;
            size_t idx = scopes.size() - 1 - v->level;
            const auto & sc = scopes[idx];
            if (sc.kind != Scope::Kind::Lambda) return false;
            if (sc.recAttrsVar != ir::kInvalid) return false;
            return true;
        };

        // #497 follow-on: TW always thunkifies inherit-from from-exprs
        // (`from->maybeThunk(state, up)` in ExprAttrs::buildInheritFromEnv,
        // eval.cc:1520).  v3 was lowering them eagerly EXCEPT for the
        // narrow self-dot pattern, which broke any from-expr that
        // references a let/rec sibling mid-construction (e.g.
        // lib/systems/default.nix's `inherit ({...} // platforms.select
        // final) ...` -- the eager evaluation forces `final` while
        // final's let-binding thunk is still Black, throwing
        // BlackholeError).
        //
        // Rule: thunkify any from-expr whose shape can re-enter the
        // enclosing scope's let/rec bindings.  Conservative shape list:
        //   - ExprOpUpdate (`A // B`): both sides may capture rec/let
        //     siblings, common in fix-point-style elaborate code.
        //   - ExprCall: function calls of any flavour can force the
        //     enclosing rec/let by reading its bindings as args or
        //     captured upvalues; `inherit (callPackages ...) ...` is
        //     the canonical nixpkgs shape.
        //   - ExprIf: branch-dependent reads of the surrounding scope.
        // ExprVar / ExprSelect-on-ExprVar / literals stay eager
        // (ExprVar via isTrivialForLazy in thunkifyForAttr; ExprSelect-
        // on-ExprVar handled by the self-dot rule above).
        // Other shapes (ExprWith, ExprOpConcatLists, etc.) are rare in
        // practice; thunkify them too for parity with TW.
        auto isComplexFromExpr = [](nix::Expr * fx) -> bool {
            if (!fx) return false;
            const auto k = fx->exprKind;
            // Targeted-only: ExprOpUpdate (`A // B`) is the canonical
            // shape that captures rec/let siblings via a `//` merge
            // (e.g. lib/systems/default.nix's `inherit ({...} //
            // platforms.select final) ...`).  Other shapes were tried
            // (ExprCall, broader sweep) but regress nixpkgs paths
            // (callPackage undefined at all-packages.nix:2276) — the
            // freeVar capture across thunk wrap interacts poorly with
            // `with self;`-driven environments under default eval.
            // Stay narrow until that's root-caused (#497 follow-on).
            if (k == nix::Expr::Kind::OpUpdate) return true;
            // #529: `inherit (X) Y` where X is a with-resolved Var
            // (e.g. `with pkgs; ... inherit (nix-update) script;` from
            // pkgs/top-level/all-packages.nix:196).  Eager lowering
            // emits OP_WITH_LOOKUP on `nix-update` immediately during
            // attrset construction; the with-scope (`pkgs` =
            // mid-construction `self`) is still Black, so the lookup
            // throws "cycle while resolving 'nix-update'" / blackhole.
            // Thunkifying defers the with-lookup until `Y` is forced,
            // matching TW's lazy `inherit (X) Y` semantics.
            //
            // Bare ExprVar with fromWith (no Select) is the canonical
            // form; the self-dot heuristic above already covers
            // ExprSelect-on-fromWith-Var (its v->fromWith check
            // explicitly excludes those — they fall through to here).
            if (k == nix::Expr::Kind::Var) {
                auto * v = static_cast<nix::ExprVar *>(fx);
                if (v->fromWith) return true;
            }
            // #529 follow-up: ExprCall whose head is a fromWith Var
            // (e.g. `inherit (callPackages ../some/path { }) name1
            // name2;` from pkgs/top-level/all-packages.nix:532).  The
            // eager lowering of the call forces `callPackages` via
            // OP_WITH_LOOKUP; the with-scope is `pkgs` mid-construction
            // so the lookup throws "cycle while resolving 'callPackages'".
            // Same fix as the bare-Var case: thunkify so the call runs
            // at the consumer's force time, by which point `pkgs` has
            // settled.
            //
            // #548 (2026-05-09): also handle CURRIED calls — the AST
            // for `callPackagesWith {} ./path` is
            //   ExprCall{ fun = ExprCall{ fun = ExprVar(callPackagesWith),
            //                             arg = {} },
            //             arg = ./path }
            // so the outer ExprCall's `fun` is an inner ExprCall, not a
            // Var.  Walk the chain to the root callee and thunkify if
            // it's a Var (regardless of fromWith — let-bound function
            // calls in `inherit (E) ...` from-expr position must also
            // be deferred to match TW's `from->maybeThunk(state, up)`).
            // This catches the v3-direct nixpkgs cycle on `hello.name`
            // where stage.nix's allPackages has
            //   inherit (callPackagesWith pkgs ./top-level/...) Y Z;
            // and v3's eager lowering trips OP_WITH_LOOKUP-cycle.
            if (k == nix::Expr::Kind::Call) {
                nix::Expr * head = fx;
                int hops = 0;
                while (head && head->exprKind == nix::Expr::Kind::Call
                       && hops < 8) {
                    head = static_cast<nix::ExprCall *>(head)->fun;
                    ++hops;
                }
                if (head && head->exprKind == nix::Expr::Kind::Var)
                    return true;
                // #548 follow-on (2026-05-09): call-on-Select-on-Var
                // (`libsForQt5.callPackage path`).  Closes the
                // libsForQt5 cycle in nixpkgs all-packages.nix.
                // GATED behind NIX_V3_THUNK_CALL_ON_SELECT_VAR
                // because enabling it default-on triggered a runtime
                // force-count explosion (~6M forces of
                // lib/systems/parse.nix:64 in 30s) — the broader
                // thunkify breaks TW's sharing of `lib.systems.*`
                // computations under v3's freeVar-capture semantics.
                // Same bug class as project_498 always-thunkify
                // regression: the thunk wrap's upvalue capture
                // doesn't propagate the same memoization that TW's
                // env-driven thunks do.  Tracked separately; flip
                // default-on once the upvalue-capture root-cause is
                // fixed.
                if (head && head->exprKind == nix::Expr::Kind::Select) {
                    auto * sel = static_cast<nix::ExprSelect *>(head);
                    if (sel->e && sel->e->exprKind == nix::Expr::Kind::Var) {
                        static const bool s_callOnSelectVar =
                            std::getenv("NIX_V3_THUNK_CALL_ON_SELECT_VAR") != nullptr;
                        if (s_callOnSelectVar) return true;
                    }
                }
            }
            // #548 (2026-05-09): ExprSelect from-expr where head is a
            // Var (e.g. `inherit (lib.systems) X`).  Lazy in TW via
            // `from->maybeThunk`.  Eager v3 lowering forces the
            // surrounding rec/let scope mid-construction.
            if (k == nix::Expr::Kind::Select) {
                auto * sel = static_cast<nix::ExprSelect *>(fx);
                if (sel->e && sel->e->exprKind == nix::Expr::Kind::Var)
                    return true;
            }
            return false;
        };

        // Diagnostic / bisect knobs (v3 #495 follow-on, broader-thunkify
        // upvalue investigation):
        //   V3_DBG_SELF_DOT_FIRES=1     -- log each self-dot fire's
        //                                  source position
        //   NIX_V3_SELF_DOT_LIMIT=N     -- gate fires to the first N
        //                                  module-wide (LIMIT=41 works,
        //                                  LIMIT=42 trips the bug).
        //   NIX_V3_SELF_DOT_SKIP_NTH=N  -- skip JUST the Nth fire
        //                                  (1-indexed); other fires
        //                                  proceed normally.  Helps
        //                                  distinguish "the Nth clause
        //                                  is special" from "any extra
        //                                  fire past N triggers".
        static const bool s_dbgFires =
            std::getenv("V3_DBG_SELF_DOT_FIRES") != nullptr;
        static const int s_fireLimit = []() -> int {
            const char * s = std::getenv("NIX_V3_SELF_DOT_LIMIT");
            return s ? std::atoi(s) : -1;
        }();
        static const int s_fireSkipNth = []() -> int {
            const char * s = std::getenv("NIX_V3_SELF_DOT_SKIP_NTH");
            return s ? std::atoi(s) : -1;
        }();
        static int s_fireCount = 0;
        std::vector<ir::VarId> cache;
        if (fromExprs) {
            cache.resize(fromExprs->size(), ir::kInvalid);
            for (size_t i = 0; i < fromExprs->size(); ++i) {
                nix::Expr * fx = (*fromExprs)[i];
                if (!fx) continue;
                bool selfDotMatches = isSelfDotPattern(fx);
                int thisOrdinal = selfDotMatches ? (++s_fireCount) : -1;
                bool selfDot = selfDotMatches;
                if (selfDot && s_fireLimit >= 0
                    && s_fireCount > s_fireLimit) selfDot = false;
                if (selfDot && s_fireSkipNth >= 1
                    && thisOrdinal == s_fireSkipNth) selfDot = false;
                // #497: always-default-on opt-in for the broader complex-
                // from-expr rule.  Set NIX_V3_NO_COMPLEX_FROM_THUNK=1 to
                // disable for bisection.
                static const bool s_noComplex =
                    std::getenv("NIX_V3_NO_COMPLEX_FROM_THUNK") != nullptr;
                bool complexFromExpr = !s_noComplex && isComplexFromExpr(fx);
                bool useThunk = !s_noThunkify
                    && (useThunkBlanket || selfDot || complexFromExpr);
                // #558 Phase 2 perf: even under blanket THUNK_ALL,
                // skip thunkify for TRIVIAL from-exprs (Int, String,
                // Path, Lambda, ExprVar without fromWith).  This
                // matches TW's `from->maybeThunk(state, up)` which
                // dispatches via Expr::maybeThunk virtual overrides
                // that inline trivial values directly.  Under blanket
                // mode we were wrapping every inherit-from regardless,
                // which created millions of useless wrapper thunks
                // under nixpkgs.  Trivial values are inherently lazy
                // (no allocation, no force needed), so wrapping them
                // adds pure overhead.
                //
                // Override via NIX_V3_THUNK_ALL_TRIVIAL=1 to restore
                // the pre-optimization "wrap everything" behavior for
                // bisection.
                if (useThunkBlanket && useThunk) {
                    static const bool s_thunkAllTrivial =
                        std::getenv("NIX_V3_THUNK_ALL_TRIVIAL") != nullptr;
                    if (!s_thunkAllTrivial
                        && isTrivialForLazy(fx, /*forArg=*/false))
                        useThunk = false;
                }
                if (selfDotMatches) {
                    if (s_dbgFires) {
                        auto * sel = dynamic_cast<nix::ExprSelect *>(fx);
                        auto * v = sel ? dynamic_cast<nix::ExprVar *>(sel->e) : nullptr;
                        if (positions) {
                            auto pos = (*positions)[fx->getPos()];
                            std::ostringstream oss;
                            oss << pos;
                            std::fprintf(stderr,
                                "v3 self-dot fires #%d: var=%s level=%u at %s\n",
                                s_fireCount,
                                v ? std::string(symbols[v->name]).c_str() : "?",
                                v ? v->level : 0, oss.str().c_str());
                        }
                    }
                }
                // V3_DBG_INHERIT_FROM_THUNK=1 logs each inherit-from
                // from-expr's lower-time decision (THUNK vs EAGER), the
                // AST kind, and source position.  Used to bisect which
                // shape regresses under broader thunkify rules.
                // NIX_V3_INHERIT_FROM_THUNK_FILTER=substring restricts
                // thunkify to from-exprs whose source position contains
                // the given substring; useful for bisecting which file's
                // from-exprs are the problem under always-thunkify.
                static const char * s_filter =
                    std::getenv("NIX_V3_INHERIT_FROM_THUNK_FILTER");
                if (useThunk && s_filter && positions) {
                    auto pos = (*positions)[fx->getPos()];
                    std::ostringstream oss; oss << pos;
                    if (oss.str().find(s_filter) == std::string::npos)
                        useThunk = false;
                }
                if (std::getenv("V3_DBG_INHERIT_FROM_THUNK")) {
                    auto * sel = dynamic_cast<nix::ExprSelect *>(fx);
                    auto * v = sel ? dynamic_cast<nix::ExprVar *>(sel->e) : dynamic_cast<nix::ExprVar *>(fx);
                    int kind = (int)fx->exprKind;
                    const char * pos_str = "<no-pos>";
                    std::string pos_buf;
                    if (positions) {
                        auto pos = (*positions)[fx->getPos()];
                        std::ostringstream oss; oss << pos;
                        pos_buf = oss.str();
                        pos_str = pos_buf.c_str();
                    }
                    std::fprintf(stderr,
                        "v3 inherit-from %s: kind=%d %s%s%s at %s\n",
                        useThunk ? "THUNK" : "EAGER",
                        kind,
                        v ? "var=" : "",
                        v ? std::string(symbols[v->name]).c_str() : "",
                        v && v->fromWith ? "(fromWith)" : "",
                        pos_str);
                }
                cache[i] = useThunk ? thunkifyForAttr(fx) : lowerExpr(fx);
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
        //
        // #558 emit-order restructure (non-rec, non-dyn branch only —
        // see below).  When `e->inheritFromExprs` is non-null (i.e.
        // there are `inherit (FROM_EXPR) name1 name2 ...;` clauses),
        // we LOWER IN A SPECIFIC ORDER so that the runtime emission
        // closes the libsForQt5-style cycle architecturally:
        //
        //   1. Regular (non-IF) entry value bindings.
        //   2. The `AttrSet` binding (REC_INIT + regular SETs only at
        //      emit time — IF entries' `value` is kInvalid placeholder).
        //   3. From-expr cache bindings (pushInheritFromCache).
        //   4. IF entry value bindings (Select(cache_var, name)).
        //   5. The `AttrSetSetInheritFrom` binding (IF SETs).
        //
        // Why: at runtime, step 2 publishes the partial Bindings to the
        // outer Black thunk's registry AND populates regular slots;
        // when step 3's bytecode runs (e.g. `OP_WITH_LOOKUP libsForQt5`
        // for an `inherit (libsForQt5.callPackage ...) wt4` clause),
        // the with-source's mid-construction Bindings now has
        // libsForQt5 visible via the partial-Bindings peek path.
        // Cycle closed without thunkifying the from-expr (the prior
        // `NIX_V3_THUNK_CALL_ON_SELECT_VAR` workaround which produced
        // divergent eval semantics — see project_libsForQt5_deferred
        // and #557 root-cause analysis).
        bool pushedInheritFrom = false;
        // Non-non-rec/non-dyn paths still take the legacy order: the
        // dyn branch builds an `AttrSetDyn` which has its own emit
        // pipeline (no REC_INIT split), and rec attrsets go through
        // lowerLetRecCapture which has its own lazy/cell story.
        if (e->inheritFromExprs && hasDyn) {
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

        // #558 emit-order restructure: phase 1 — lower REGULAR entry
        // values FIRST (independent of any from-expr cache).  Build
        // entries vector with isInheritFrom flags; IF entries get
        // kInvalid value placeholder (filled in phase 4).
        //
        // We iterate the attrs map TWICE: once to lower regulars +
        // build entry shape, then later for IF values.  Cost: a second
        // map walk; nixpkgs attrsets are small enough that this is in
        // the noise.
        std::vector<ir::AttrSet::Entry> entries;
        entries.reserve(e->attrs->size());

        // Track which entry slots correspond to IF entries — we'll
        // backfill their `value` field in phase 4 after pushing the
        // inherit-from cache.  Pair = (entries[] index, AST kv pointer).
        std::vector<std::pair<size_t, decltype(e->attrs->begin())>>
            ifEntrySlots;
        for (auto it = e->attrs->begin(); it != e->attrs->end(); ++it) {
            const auto & sym = it->first;
            const auto & def = it->second;
            const bool isIF =
                def.kind == nix::ExprAttrs::AttrDef::Kind::InheritedFrom;
            if (isIF) {
                // Placeholder value — filled in phase 4 once the
                // from-expr cache is available.
                ifEntrySlots.emplace_back(entries.size(), it);
                entries.push_back(
                    {internSym(sym), ir::kInvalid,
                     posIdxToHandle(def.pos), /*isInheritFrom=*/true});
            } else {
                // Lazy entries — see comment on the dyn branch above.
                ir::VarId vv = thunkifyForAttr(def.e);
                entries.push_back(
                    {internSym(sym), vv, posIdxToHandle(def.pos),
                     /*isInheritFrom=*/false});
            }
        }

        // Sort entries by SymbolId (the AttrSet IR invariant: entries
        // sorted ascending — see ir.hh).  Capture the post-sort index
        // for each entry so the IF SET binding (and the IF entry value
        // backfill below) can reference the correct slot.
        //
        // Note: emit.cc historically also re-sorted at emit time via a
        // local sortedIdx — that's redundant once the IR is sorted, but
        // we keep it as a defensive idempotent sort in emitOne(AttrSet).
        std::vector<size_t> oldToNew(entries.size());
        {
            std::vector<std::pair<ir::SymbolId, size_t>> idxOrder;
            idxOrder.reserve(entries.size());
            for (size_t i = 0; i < entries.size(); ++i)
                idxOrder.emplace_back(entries[i].name, i);
            std::stable_sort(idxOrder.begin(), idxOrder.end(),
                             [](const auto & a, const auto & b) {
                                 return a.first < b.first;
                             });
            std::vector<ir::AttrSet::Entry> sorted;
            sorted.reserve(entries.size());
            for (size_t newIdx = 0; newIdx < idxOrder.size(); ++newIdx) {
                size_t oldIdx = idxOrder[newIdx].second;
                oldToNew[oldIdx] = newIdx;
                sorted.push_back(entries[oldIdx]);
            }
            entries = std::move(sorted);
        }

        // Phase 2: add the AttrSet binding NOW.  Its emit at runtime
        // does REC_INIT + regular SETs.  IF SETs are deferred to the
        // AttrSetSetInheritFrom binding emitted in phase 5.
        ir::VarId attrSetVar =
            addBinding(ir::AttrSet{std::move(entries)});

        // If there are no inherit-from clauses, we're done — emit the
        // attrset as before (no IF SETs needed).
        if (!e->inheritFromExprs) {
            return attrSetVar;
        }

        // Phase 3: push the from-expr cache.  This adds bindings to the
        // current parent block (in lower-time order, AFTER the AttrSet
        // binding).  At runtime, these bindings' bytecode runs AFTER the
        // AttrSet's REC_INIT + regular SETs — partial-Bindings now has
        // sibling regular entries visible to OP_WITH_LOOKUP from inside
        // the from-expr.
        inheritFromStack.push_back(e->inheritFromExprs.get());
        pushInheritFromCache(e->inheritFromExprs.get());
        pushedInheritFrom = true;

        // Phase 4: lower IF entry values.  Each IF entry's `def.e` is
        // typically `ExprSelect(ExprInheritFrom, name)`; thunkifyForAttr
        // produces a thunk binding wrapping a Select on cache_var.
        std::vector<ir::AttrSetSetInheritFrom::IFEntry> ifSets;
        ifSets.reserve(ifEntrySlots.size());
        for (auto & [oldIdx, it] : ifEntrySlots) {
            const auto & def = it->second;
            ir::VarId vv = thunkifyForAttr(def.e);
            const size_t newIdx = oldToNew[oldIdx];
            ifSets.push_back(
                {static_cast<uint32_t>(newIdx), vv});
        }
        // The AttrSet IR's IF entries still hold kInvalid placeholders
        // — they're not consumed by emit (emitOne(AttrSet) only emits
        // REC_INIT names+pos for IF entries; the SETs come from the
        // AttrSetSetInheritFrom binding).  Leaving the placeholder in
        // place keeps optimizer passes (DCE / occur) consistent: an IF
        // entry's value var is referenced ONLY by the IF SET binding,
        // not by the AttrSet itself.

        // Sort IF SETs by sortedSlot for deterministic emit order
        // (matches the regular SET pass which also walks slots in
        // ascending order).  Stable so equal slots — which can't
        // happen for distinct names — preserve.
        std::stable_sort(ifSets.begin(), ifSets.end(),
                         [](const auto & a, const auto & b) {
                             return a.sortedSlot < b.sortedSlot;
                         });

        // Phase 5: pop inherit-from stack BEFORE emitting the
        // AttrSetSetInheritFrom binding (mirrors the legacy pop site).
        if (pushedInheritFrom) {
            inheritFromStack.pop_back();
            inheritFromCacheStack.pop_back();
            pushedInheritFrom = false;
        }

        // Phase 6: add the AttrSetSetInheritFrom binding.  Its var is
        // a fresh discardable that aliases the attrset (the SET ops
        // mutate the same heap Bindings).  Future bindings in the
        // parent block (or the terminal-return) reference attrSetVar
        // directly — they don't need the alias.
        addBinding(ir::AttrSetSetInheritFrom{
            attrSetVar, std::move(ifSets)});

        return attrSetVar;
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
        // #458 Phase B RecBuildSlot — register recSlotVar so the
        // lowerer's downstream upvalue-translation pass can detect its
        // appearance as a freeVar of a sub-thunk and emit a
        // `Kind::RecBuildSlot` UpvalueSource (materialises Tag::Slot
        // from the env walk).  Without this set, lambda-body freeVars
        // referencing recSlotVar fall through varOrigins lookup with
        // no match.
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
        // Track whether this is `let ... in body` (intermediate
        // recAttrs) vs `rec { ... }` (recAttrs IS the value).  emit.cc
        // selects OP_ATTRS_LET_REC_INIT vs OP_ATTRS_REC_INIT based on
        // this.  See ir::LetRec::hasBody comment + bytecode.hh's
        // OP_ATTRS_LET_REC_INIT documentation for why this matters.
        letRec.hasBody = hasBody;
        letRec.entries.reserve(pending.size());
        // #530 lexical-with chain — capture the chain ONCE at the
        // LetRec construction site; every per-entry thunk and every
        // hidden from-expr thunk created here lives at the same
        // lexical position (the source `let-rec` form), so they share
        // the chain.  Per-entry inner `with` scopes are seen INSIDE
        // the entry body's lowering and would attach to thunks built
        // there.
        std::vector<ir::VarId> letRecWiths = collectLexicalWiths();
        const uint16_t nWiths =
            static_cast<uint16_t>(letRecWiths.size());
        for (auto & p : pending) {
            ir::LetRec::Entry en;
            en.name = internSym(p.sym);
            en.thunkBody = p.funcIdx;
            en.pos = p.posHandle;
            en.lexicalWiths = letRecWiths;
            // Mirror count into Function::nWithTargets so the
            // descriptor-build pass sees a consistent value.
            if (p.funcIdx < m.functions.size())
                m.functions[p.funcIdx].nWithTargets = nWiths;
            letRec.entries.push_back(std::move(en));
        }
        // REVIEW HIGH-4 follow-up: attach hidden from-expr thunks.
        letRec.hiddenEntries.reserve(pendingHidden.size());
        for (auto & ph : pendingHidden) {
            ir::LetRec::HiddenEntry he;
            he.hiddenVar = ph.hiddenVar;
            he.thunkBody = ph.thunkBody;
            he.lexicalWiths = letRecWiths;
            if (ph.thunkBody < m.functions.size())
                m.functions[ph.thunkBody].nWithTargets = nWiths;
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
        // Push a Kind::With scope that records the with-target VarId
        // (#530 lexical-with chain).  nix's bindVars counts the with's
        // env as a level when resolving ExprVar in the body, so v3's
        // `scopes` stack must match that depth or resolveVar's (level,
        // displ) lookup falls off the end.  In addition, the scope
        // carries `withTargetVar` so collectLexicalWiths() can collect
        // a static outermost-first chain for thunks / closures created
        // anywhere in the body.
        //
        // `withTargetVar` records the TARGET expression's VarId — this
        // is what gets re-emitted at MAKE_THUNK / MAKE_CLOSURE time so
        // the thunk/closure's `capturedWiths` are populated from the
        // lexical with-chain rather than from a runtime snapshot of
        // the with-stack.
        //
        // Always use the lowered `attrs` VarId (the result of
        // `lowerExpr(e->attrs)`).  When `attrs` is a rec-attrset
        // entry, the lowerer already produces a Tag::Slot reference
        // (RecBindingSlotRef on `recSlotVar`) for it — heap-stable —
        // so capturing `attrs` directly gives the right value.  We
        // intentionally do NOT route through `withRecAttrsVar` here:
        // that var points at the WHOLE rec-attrset, and combining it
        // with `withRecAttrsName` would require re-running an
        // OP_REC_BINDING_SLOT_REF at MAKE time, which doesn't compose
        // with the simple `emitVarRef` push-and-pop protocol the
        // lexical-with chain uses.  The slot tag carried by `attrs`
        // is what makes formals + `with` work (the slot persists
        // beyond the maker frame's lifetime via Bindings allocation).
        Scope withScope;
        withScope.kind = Scope::Kind::With;
        withScope.withTargetVar = attrs;
        scopes.emplace_back(std::move(withScope));
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
    static const bool s_dbg = std::getenv("V3_DBG_LOWER_CALLS") != nullptr;
    if (s_dbg) {
        auto pos = positions[e->getPos()];
        std::ostringstream oss;
        oss << pos;
        std::fprintf(stderr,
            "v3 lowerNixExpr: e=%p kind=%d at %s\n",
            (void*)e, (int)e->exprKind, oss.str().c_str());
    }
    Lowerer L(symbols, positions);
    return L.run(e);
}

} // namespace nix::v3
