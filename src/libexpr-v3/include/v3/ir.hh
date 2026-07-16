#pragma once
/// @file
/// v3 IR — block-based A-normal-form representation of Nix programs.
///
/// Design (mirrors v2 ir.hh; deliberate so the v2 → v3 IR mapping is easy):
///   - FLAT.  No nested expression trees; every compound sub-expression is
///     bound to a VarId in a Block's `bindings` vector.
///   - BLOCK.  Unit of control flow.  Each Block has a sequence of bindings
///     and exactly one Terminal (Return / Branch).  if-branches, lambda
///     bodies, thunk bodies, and short-circuit RHS are all separate Blocks.
///   - MODULE.  Owns all Blocks and Functions.  Block IDs / Function IDs are
///     indices into the module's vectors.
///   - SYMBOL.  IR-local SymbolId: uint32 index into Module::symbolTable.
///     Cheap to compare; lowered to an external symbol table at emit time.
///   - DESUGARED.  inherit, with, let, rec, or-default, string interpolation,
///     and assert are lowered to primitive IR operations during AST → IR.
///   - LAZINESS EXPLICIT.  Use MkThunk to introduce a deferred computation;
///     use Force when a strict context demands a value.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <memory>   // std::shared_ptr (libstdc++/Linux needs it explicitly; libc++ pulls it in transitively)
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace nix::v3 {
struct PrimOp;
}

namespace nix::v3::ir {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

/// A variable in the IR (defined by exactly one Binding).  Within one Block
/// VarIds are linear; across the Module they are unique.
using VarId = uint32_t;
constexpr VarId kInvalid = 0;

/// Index into Module::blocks.  Blocks own bindings + a Terminal.
using BlockId = uint32_t;
constexpr BlockId kInvalidBlock = 0;

/// Index into Module::functions.  Functions own a body Block + parameter
/// metadata (used by Lambda / MkThunk to point at the callable code).
using FuncId = uint32_t;

/// Index into Module::symbolTable.  Used for attribute names, formals,
/// with-lookup names, etc.  Comparison is O(1) (integer compare).
using SymbolId = uint32_t;
constexpr SymbolId kInvalidSymbol = 0;

// ---------------------------------------------------------------------------
// IR expression variants
// ---------------------------------------------------------------------------

// --- Literals ---
struct LitInt    { int64_t value; };
struct LitFloat  { double  value; };
struct LitBool   { bool    value; };
struct LitNull   {};
/// String literal.  `value` is borrowed from a long-lived buffer (the AST
/// arena, or a Module-owned string pool).  Lifetime must outlive the IR.
struct LitString { std::string_view value; };
struct LitPath   { std::string_view path; void * accessor; };

// --- Var reference ---
/// References a previously-defined VarId in the enclosing function/block scope.
struct VarRef    { VarId var; };

/// Reference to a free variable resolved against a `with` scope at runtime.
/// Lookup walks the runtime with-stack from innermost to outermost.
struct WithLookup {
    SymbolId name;
};

// --- Lambdas, application, thunks ---

/// Lambda formal parameter (in `{ a ? def, b, ... }: body`).
struct Formal {
    SymbolId name;
    /// True if this formal has a default expression.  The default's
    /// own block is wired into the lambda body's prologue at lower
    /// time; we don't carry the BlockId here because the only consumer
    /// (`builtins.functionArgs`) just needs the presence flag.
    bool hasDefault = false;
    /// AST position handle for the formal name; 0 = unknown.  Recorded
    /// in the per-attr side-table when `builtins.functionArgs` builds
    /// its result attrset, so `unsafeGetAttrPos` works.
    uint32_t pos = 0;
};

/// Construct a closure value.  At runtime, captures the free variables
/// (in `freeVars` order) into a Closure object and tags the result.
struct Lambda {
    FuncId             funcIdx;
    /// Free vars of the body, in the order the body expects to read them
    /// via OP_GET_UPVALUE.  Populated by computeFreeVars before emit.
    std::vector<VarId> freeVars;

    /// #530 lexical-with chain — outermost-first VarIds of every
    /// enclosing `with X;` in lexical scope at this Lambda's creation
    /// site.  Materialised by the lowerer at lambda-creation time
    /// from `Scope::Kind::With` entries on the scopes stack.  Emit
    /// pushes these values BEFORE freeVars; OP_MAKE_CLOSURE consumes
    /// them and stuffs them into the resulting Closure's
    /// `capturedWiths` ListVec.  Order matches the order
    /// OP_WITH_LOOKUP walks (outermost-first), so a name lookup inside
    /// the closure's body finds the with-target whose attrset binds
    /// the name innermost-first when scanned in reverse.
    std::vector<VarId> lexicalWiths;
};

/// Strict (single-arg) function application.  In v3, OP_CALL takes one arg;
/// curried application is achieved by chaining App nodes.
struct App   { VarId fun; VarId arg; };

/// Force evaluation of a thunk in a strict context.  No-op on already-WHNF
/// values.
///
/// `srcLine` (0 = unknown) records the lower.cc line that synthesised the
/// node, so the bytecode emitter can populate
/// `CompilationUnit::forceEmitSites` for `V3_DBG_FORCE_SITE` traces.
/// Default 0 keeps existing aggregate-init call sites compiling.
struct Force { VarId thunk; int srcLine = 0; };

/// Construct a deferred computation.  When forced, runs the body block in
/// the captured environment.
struct MkThunk {
    FuncId             funcIdx;
    std::vector<VarId> freeVars;

    /// #530 lexical-with chain — outermost-first VarIds of every
    /// enclosing `with X;` in lexical scope at this MkThunk's
    /// creation site.  Materialised by the lowerer at thunkify time
    /// from `Scope::Kind::With` entries on the scopes stack.  Emit
    /// pushes these values BEFORE freeVars; OP_MAKE_THUNK consumes
    /// them and stuffs them into the resulting Thunk's
    /// `suspended.capturedWiths` ListVec, replacing the runtime
    /// snapshot of the with-stack as the source of truth for the
    /// thunk's lexical with-environment.
    std::vector<VarId> lexicalWiths;
};

// --- Attribute sets ---

struct AttrSelect    { VarId attrs; SymbolId name; };
struct AttrSelectDyn { VarId attrs; VarId nameVar; };
struct HasAttr       { VarId attrs; SymbolId name; };
struct HasAttrDyn    { VarId attrs; VarId nameVar; };

/// SECD-style heap-stable slot reference for a rec-attrset entry.
/// Lowers to `OP_FORCE` of `attrs` (a Tag::Attrs) followed by
/// `OP_REC_BINDING_SLOT_REF name` — the result is a Tag::Slot Value
/// pointing at `Bindings::entries[i].value` (stable as long as the
/// Bindings is alive).  Used by `thunkifyRecAttrSelect` so that
/// rec-attrset entry references propagate as slot pointers through
/// callFunction: when `f x` is called and `x` is a rec entry, the
/// callee's parameter slot inherits Tag::Slot, and `with self;` over
/// the parameter sees the entry's mutated/memoized value via the
/// slot.  This is the WC-38 fix for the `with self;` blackhole in
/// lib.fix-style patterns.
struct RecBindingSlotRef { VarId attrs; SymbolId name; };

/// Construct a non-recursive attrset from sorted (name, value) pairs.
/// `pos` is the AST PosIdx for the attribute *name* token (or 0 = none),
/// used by `builtins.unsafeGetAttrPos`.
///
/// #558 emit-order restructure: when an attrset has `inherit (E) y;`
/// entries (`isInheritFrom = true`), the from-expr `E` is lowered as
/// SEPARATE parent-block bindings AFTER this AttrSet binding (instead
/// of before it, which would force `E` while `pkgs`-style with-sources
/// are still mid-construction).  emitOne(AttrSet) emits REC_INIT +
/// regular SETs only; the inherit-from REC_SETs are emitted by a
/// trailing `AttrSetSetInheritFrom` binding once cache vars exist.
/// IF entries' `value` is `kInvalid` here (placeholder).
struct AttrSet {
    struct Entry {
        SymbolId name;
        VarId    value;          // kInvalid for isInheritFrom entries
        uint32_t pos = 0;
        bool     isInheritFrom = false;
    };
    std::vector<Entry> entries; // sorted ascending by SymbolId

    /// #558 (2026-05-10): true iff this AttrSet is the eventual return
    /// value of its enclosing function — i.e. forcing the surrounding
    /// thunk produces this AttrSet's Bindings (modulo wrappers like
    /// `with` or `assert` that don't change the value).
    ///
    /// Set by `markTailReturnAttrSets` (lower.cc) as a post-lower
    /// analysis pass.  Consumed by `emitOne(AttrSet)` (emit.cc): true →
    /// `OP_ATTRS_REC_INIT_TAIL` (publishes partial Bindings to all
    /// thunk frames on the call stack, first-wins, so `with self;`-
    /// style lookups through any outer thunk find this AttrSet's
    /// in-progress Bindings); false → `OP_ATTRS_REC_INIT` (publishes
    /// only to the innermost Black thunk, the legacy non-tail-return
    /// behavior, which is conservative for sub-expression attrsets
    /// that aren't the function's return value).
    ///
    /// Why this distinction matters: in lib.fix-style fix-points
    /// (`let x = f x; in x`), the entire chain of thunks waiting for
    /// `f x` to return is conceptually waiting for the function
    /// body's tail-return AttrSet.  Sub-attrsets within that body
    /// (let-bindings, function args) don't represent the chain's
    /// expected value, so registering them with outer thunks would
    /// falsely advertise sub-expression shapes via the partial-
    /// Bindings peek path (vm.cc:withLookup).
    bool isFunctionReturn = false;

    /// 2026-06-16 (foldl lever): true iff this AttrSet was lowered from a
    /// NON-recursive, non-dynamic attrset literal (`{...}`, not `rec {...}`,
    /// no `${e}` dynamic keys).  Such an attrset can never have inter-entry
    /// references or `with self;` lookups during construction (both require
    /// `rec`), so the OP_ATTRS_REC_INIT slot/publish/cell-update machinery is
    /// pure overhead.  emit.cc demotes flagged sets to the cheaper
    /// OP_ATTRS_INIT (pop-N build) under the NIX_V3_NONREC_ATTRS_INIT gate.
    /// Default false → conservative REC_INIT (rec sets + synthesized sets stay
    /// on the safe path).
    bool nonRecursive = false;
};

/// #558 emit-order restructure: companion to `AttrSet` that emits the
/// `inherit (E) y;` per-entry REC_SETs.  Lowered AFTER the `AttrSet`
/// binding and AFTER the from-expr cache + IF entry value bindings, so
/// the from-expr's runtime evaluation (e.g. `OP_WITH_LOOKUP libsForQt5`)
/// fires AFTER the attrset's regular entries are visible via the
/// partial-Bindings registry.
///
/// Emit:
///   emitVarRef(attrSetVar)          # push attrset on stack
///   for each entry:
///     emitVarRef(valueVar)          # push value
///     OP_ATTRS_REC_SET sortedSlot   # consumes value, leaves attrset
///   # binding's slot ends up holding the attrset (idempotent — same
///   # heap Bindings*; the IR var is a discardable alias).
struct AttrSetSetInheritFrom {
    VarId attrSetVar;
    struct IFEntry {
        uint32_t sortedSlot;
        VarId    valueVar;
    };
    std::vector<IFEntry> entries; // sorted ascending by sortedSlot
};

/// Attrset with one or more dynamic-name attributes.
struct AttrSetDyn {
    struct StaticEntry  { SymbolId name; VarId value; uint32_t pos = 0; };
    struct DynamicEntry { VarId nameVar; VarId value; uint32_t pos = 0; };
    std::vector<StaticEntry>  statics;
    std::vector<DynamicEntry> dynamics;
};

// --- Lists ---

struct ListExpr    { std::vector<VarId> elems; };
struct ConcatLists { VarId lhs; VarId rhs; };

// --- Control flow / scoping ---

/// Conditional.  `cond` is forced; `thenBlock` or `elseBlock` runs, and its
/// return value becomes the value of this binding's slot.
struct If    { VarId cond; BlockId thenBlock; BlockId elseBlock; };

/// `with attrs; body`.  Pushes `attrs` onto the runtime with-stack, runs
/// `bodyBlock`, then pops.  Inside the body, WithLookup resolves names.
///
/// `recAttrsVar` + `recAttrsName` are set when `attrs` resolves to a
/// rec-attrset entry: the emitter pushes a Tag::Slot pointing into
/// `recAttrsVar`'s Bindings::entries[i].value (heap-stable).  This is
/// the production path for `with self;` over rec-attrsets and is what
/// makes `lib.fix` patterns work in v3.  Both fields are kInvalid when
/// unused (the back-compat OP_GET_LOCAL + OP_WITH_PUSH path).
struct With  {
    VarId attrs;
    BlockId bodyBlock;
    VarId recAttrsVar = kInvalid;
    SymbolId recAttrsName = kInvalidSymbol;
};

/// `assert cond; body`.  Forces `cond`; if false, raises an error; otherwise
/// runs `bodyBlock` and yields its return value.
struct Assert { VarId cond; BlockId bodyBlock; };

// --- String interpolation / coercion ---

struct ConcatStrings { std::vector<VarId> parts; bool forceString; };

// --- Boolean / comparison / arithmetic ---

struct Not    { VarId operand; };

struct Add  { VarId lhs; VarId rhs; };
struct Sub  { VarId lhs; VarId rhs; };
struct Mul  { VarId lhs; VarId rhs; };
struct Div  { VarId lhs; VarId rhs; };

struct Eq   { VarId lhs; VarId rhs; };
struct NEq  { VarId lhs; VarId rhs; };
struct Less { VarId lhs; VarId rhs; };

/// Short-circuit logical operators.  `rhsBlock` is only run when needed.
struct And  { VarId lhs; BlockId rhsBlock; };
struct Or   { VarId lhs; BlockId rhsBlock; };
struct Impl { VarId lhs; BlockId rhsBlock; };

/// Attrset update: lhs // rhs.
///
/// #558 (2026-05-10) `isFunctionReturn` mirrors AttrSet's analogous
/// flag.  When this Update is in tail-return position of an enclosing
/// function (= the function's body's terminal-return value resolves
/// through transparent IR wrappers to this Update), it emits as
/// OP_ATTRS_UPDATE_TAIL instead of OP_ATTRS_UPDATE.  The tail variant
/// publishes the merged Bindings to all THUNK_RETURN frames on the
/// call stack via publishToAllThunkFrames — analog of STG's
/// "constructor allocation reaches WHNF" but for // results that are
/// the actual fix-point cell, not partial sub-AttrSets.
///
/// Static (markTailReturnAttrSets) — see lower.cc.
struct Update { VarId lhs; VarId rhs; bool isFunctionReturn = false; };

/// Direct primop call.  All arguments must be available; the primop's
/// arity must match args.size().  Faster than going through OP_CALL since
/// no Closure / PrimOpApp allocation is needed.
struct PrimOpCall {
    const v3::PrimOp * primop;
    std::vector<VarId> args;
};

/// Push a Tag::PrimOp value (for partial application or first-class use
/// of primops).  Used when a primop is referenced as a value rather than
/// the callee of a sufficiently-applied call site.
struct LitPrimOp {
    const v3::PrimOp * primop;
};

/// Push the singleton `builtins` attrset.  The VM lazily materialises one
/// process-wide Tag::Attrs Value containing every registered primop, then
/// reuses it for every emit.  Saves the lower phase from constructing N
/// LitPrimOp + AttrSet bindings on every occurrence of the bare
/// `builtins` symbol.  No VarId refs — pushes a constant.
struct LitBuiltins {};

/// Recursive let / rec attrset built via the env-carrier pattern:
/// allocate a Bindings(n) with placeholder values, allocate one Thunk per
/// entry capturing the Bindings as its first upvalue, then patch the
/// Bindings.  References to siblings inside thunk bodies (and in the
/// surrounding `let ... in body`) traverse `AttrSelect + Force` on the
/// rec attrset.
struct LetRec {
    /// The VarId this binding produces (= the rec attrset value).
    /// Stored here so emit can reference it without the Binding context
    /// and so computeFreeVars can subtract it from each thunk body's
    /// freeVars to get `outerUpvalues`.  Set by the lowerer.
    VarId recVar = kInvalid;

    struct Entry {
        SymbolId            name;
        FuncId              thunkBody;     // body Function, evaluated on Force
        uint32_t            pos = 0;       // AST PosIdx for the attr name
        /// VarIds the thunk body needs from the surrounding scope, NOT
        /// counting the rec attrset (which is implicitly upvalue 0).
        /// Populated by computeFreeVars.
        std::vector<VarId>  outerUpvalues;

        /// #530 lexical-with chain — outermost-first VarIds of every
        /// enclosing `with X;` in lexical scope at this LetRec entry's
        /// thunk-body creation site.  Materialised by the lowerer.
        std::vector<VarId>  lexicalWiths;
    };
    std::vector<Entry> entries;

    /// REVIEW HIGH-4 follow-up: hidden from-expr thunks for
    /// `let inherit (e) a b c; in body` shape.  Each hidden entry is
    /// a thunk function whose body lowers `e` (the from-expr) once,
    /// in the rec scope, capturing recVar + any other free vars.
    /// The thunk's resulting Value is bound to `hiddenVar` -- a
    /// regular VarId in the LetRec's containing block -- so each
    /// `inherit (e) name` shares one force.  Emitted between OP_DUP /
    /// OP_SET_LOCAL recSlot and the regular per-attr thunks so the
    /// per-attr thunks can capture hiddenVar as an upvalue.
    struct HiddenEntry {
        VarId               hiddenVar;
        FuncId              thunkBody;
        std::vector<VarId>  outerUpvalues;

        /// #530 lexical-with chain — outermost-first VarIds of every
        /// enclosing `with X;` in lexical scope at this hidden
        /// from-expr thunk's creation site.  Materialised by the
        /// lowerer.
        std::vector<VarId>  lexicalWiths;
    };
    std::vector<HiddenEntry> hiddenEntries;

    /// True when this LetRec was lowered from `let ... in body` (i.e.
    /// the rec-attrset is INTERMEDIATE state, the binding's value is
    /// `body`).  False when lowered from `rec { ... }` (where the
    /// rec-attrset IS the binding's value).
    ///
    /// Drives the choice of init-opcode in emit:
    ///   hasBody=false -> OP_ATTRS_REC_INIT      (publishes recAttrs
    ///                                              to nearest Black
    ///                                              thunk -- legitimate
    ///                                              self-reference path)
    ///   hasBody=true  -> OP_ATTRS_LET_REC_INIT  (no publish -- the
    ///                                              thunk's return value
    ///                                              is `body`, not the
    ///                                              rec-attrs)
    ///
    /// Without this distinction, a `let prev = f final; in prev //
    /// overlay final prev` (the lib.extends shape) publishes the
    /// `{prev}` placeholder recAttrs to the surrounding thunk, which
    /// later participates as a wrong-shape value in with-scope lookups
    /// (the v3-direct callPackage-with-scope bug; see
    /// CALLPACKAGE_BUG_2026-05-09.md).
    bool hasBody = false;
};

// ---------------------------------------------------------------------------
// IRExpr sum
// ---------------------------------------------------------------------------

using Expr = std::variant<
    LitInt, LitFloat, LitBool, LitNull, LitString, LitPath,
    VarRef, WithLookup,
    Lambda, App, Force, MkThunk,
    AttrSelect, AttrSelectDyn, HasAttr, HasAttrDyn, AttrSet, AttrSetSetInheritFrom, AttrSetDyn,
    RecBindingSlotRef,
    ListExpr, ConcatLists,
    If, With, Assert,
    ConcatStrings,
    Not, Add, Sub, Mul, Div, Eq, NEq, Less,
    And, Or, Impl,
    Update,
    PrimOpCall,
    LitPrimOp,
    LitBuiltins,
    LetRec
>;

// ---------------------------------------------------------------------------
// Binding / Terminal / Block
// ---------------------------------------------------------------------------

struct Binding {
    VarId var;
    Expr  expr;
};

/// Final operation of a Block.

/// Yield `value` as the Block's result.
struct TermReturn { VarId value; };

using Terminal = std::variant<TermReturn>;

/// A linear sequence of bindings + a terminal.  Owned by Module::blocks.
struct Block {
    std::vector<Binding> bindings;
    Terminal             terminal{TermReturn{kInvalid}};
};

// ---------------------------------------------------------------------------
// Function descriptor (logical lambda / thunk)
// ---------------------------------------------------------------------------

struct Function {
    /// Identifier for the entry Block that is run when this function is
    /// applied / forced.
    BlockId  entryBlock = kInvalidBlock;
    /// Optional argument name (for `x: body`).  kInvalidSymbol if no arg
    /// or formals-only.
    SymbolId argName    = kInvalidSymbol;
    /// `arg` VarId in the body's scope (if argName is set).
    VarId    paramVar   = kInvalid;

    /// eval/apply (#3): params beyond the first, for an uncurried multi-arity
    /// function formed by collapsing a curried chain `x: y: … : body`.  Empty
    /// for the common arity-0/1 case.  When non-empty the function has arity
    /// 1 + extraParams.size(); the body is lowered with paramVar + every
    /// extraParam in scope, and a saturated N-arg call enters once with the
    /// args in local slots 0..N-1 (no per-arg partial-application closure).
    /// Populated by the curried-chain collapse in lowerLambda, gated by
    /// NIX_V3_EVAL_APPLY.  These VarIds are PARAMETERS (bound), so
    /// computeFreeVars subtracts them from the body's freeVars exactly like
    /// paramVar.
    std::vector<VarId> extraParams;

    /// Formals (`{ a ? def, b }: body`).  Empty if no formals.
    std::vector<Formal> formals;
    bool                hasFormals = false;
    bool                ellipsis   = false;

    /// P2.1 step-0 measure (2026-07-02, TEMPORARY instrument): true for a
    /// per-formal WRAPPER thunk Function minted at lower_v3.hh:599-635
    /// (body `if param ? X then param.X else <default>`).  Propagated to
    /// LambdaDescriptor::isFormalWrapper so the NIX_VM_STATS dump can size
    /// the wrapper share of runtime thunk allocations (audit §4.1 / A.4
    /// P2.1 step 0).  Remove with the instrument once P2.1 is decided.
    bool                isFormalWrapper = false;

    /// P2.1-a (NIX_V3_RAW_FORMALS): set on a NO-DEFAULT demoted formal wrapper.
    /// `formalSym` is the formal's SymbolId; when `rawFormalEligible`, emit
    /// prefixes this wrapper's OP_MAKE_THUNK with OP_RAW_FORMAL(formalSym) so the
    /// runtime can raw-bind the formal (plain arg) instead of allocating the
    /// wrapper.  emit reads these off the IR Function directly (no descriptor
    /// propagation needed).  Only the DEMOTED-plainScope path emits the wrapper
    /// via emitOne(MkThunk); the LetRec path is untouched.
    bool                rawFormalEligible = false;
    SymbolId            formalSym = 0;

    /// P2.3 step-0 measure (2026-07-02, TEMPORARY instrument): tag the two
    /// other "per-evaluation wrapper thunk TW doesn't allocate" classes from
    /// audit §4.3, so the NIX_VM_STATS dump can size their share vs the ≥2 %
    /// pre-commit.  isOrDefault = the `x.y or DEFAULT` default thunk minted at
    /// lower_v3.hh (lowerSelect), allocated in the PARENT block even when the
    /// attr is present; isInheritWrapper = the per-`inherit x;` wrapper Function
    /// minted in the rec-attrset path.  Remove with the instrument once decided.
    bool                isOrDefault = false;
    bool                isInheritWrapper = false;

    /// Free vars referenced by the body block (and recursively by any
    /// sub-blocks / nested functions reachable from the body), in the
    /// order they appear as upvalues at runtime.  Populated by
    /// computeFreeVars before emit.
    std::vector<VarId>  freeVars;

    /// #530 lexical-with chain — count of with-target VarIds the
    /// MAKE_THUNK / MAKE_CLOSURE opcode for this Function pushes
    /// before its upvalue block.  Mirrors the size of the
    /// `lexicalWiths` field on the IR node (ir::Lambda /
    /// ir::MkThunk / ir::LetRec::Entry / ir::LetRec::HiddenEntry)
    /// that creates this Function.  Carried through to
    /// LambdaDescriptor::nWithTargets so the runtime can pop the
    /// right number of values.  Populated at the IR-creation site
    /// (lowerer) by simply mirroring `lexicalWiths.size()`; emit
    /// reads it back into LambdaDescriptor.
    uint16_t            nWithTargets = 0;

    /// Optional name for diagnostics (e.g. lambda or attribute name).
    std::string         name;

    /// #669 follow-up: contextual binding name (let-bound / attr-bound)
    /// as set by TW's parser on `ExprLambda::name` via `setName`.  Empty
    /// for anonymous lambdas, even when `name` (above) carries an
    /// arg-name fallback for diagnostic dumps.  Used by
    /// `printNixValueRich` (the `nix eval` printer) to decide whether
    /// to emit `«lambda <name> @ pos»` — TW only emits the name when
    /// this contextual binding is set.
    std::string         contextualName;

    /// Source position handle (1-based index into posSnapshotPool, 0 = unknown).
    /// Used by V3_DBG_FORCE_TRACE to print file:line:col per force,
    /// matching tree-walker's TW_DBG_FORCE format for direct trace diff.
    uint32_t            posHandle = 0;

    /// #493 / #484 follow-on: original `nix::ExprLambda *` this IR Function
    /// was lowered from, or nullptr if synthesised internally (per-formal
    /// default thunks).  Held as `void *` so ir.hh stays decoupled from
    /// libnixexpr's AST headers.  Carried through to LambdaDescriptor at
    /// emit time so v3ToTreeWalker can construct a proper TW Tag::tLambda
    /// when bridging a formals closure back to TW (autoCallFunction needs
    /// the original ExprLambda for formals introspection).
    void *              astLambda = nullptr;

    /// #495: native-intrinsic kind, mirrors LambdaDescriptor::Intrinsic
    /// (enumerated as uint8_t here to keep ir.hh decoupled from
    /// closure.hh's enum class).  Set by lower.cc's lowerLambda
    /// structural-match pass; carried through to LambdaDescriptor at
    /// emit time so OP_CALL can dispatch to the v3-native impl.
    /// Values:
    ///   0 = None
    ///   1 = Fix
    ///   2 = Extends
    ///   3 = ComposeExtensions
    ///   4 = ComposeManyExtensions
    ///   5 = ExtendsBody  (STG-13a #509/#510 — chain[2] of extends)
    ///   6 = ComposeBody  (STG-13a #509/#510 — chain[3] of compose)
    uint8_t             intrinsicKind = 0;

    /// STG-13a (#509/#510): for ExtendsBody / ComposeBody, the captured
    /// VarIds we'll read from the closure as upvalues at native dispatch
    /// time.  Resolved by lowerLambda when it processes chain[2]/chain[3]
    /// against the live scope stack: scopes still contain chain[0]/chain[1]
    /// (and chain[2] for ComposeBody) with their byName/byDispl, so we
    /// can find the VarId for `overlay`/`f`/`g`/`final` directly.
    ///
    /// At emit time, these VarIds are looked up in `freeVars` to compute
    /// the upvalue indices stored on LambdaDescriptor.
    ///
    /// Roles per intrinsic:
    ///   ExtendsBody : intrinsicVar0 = overlay, intrinsicVar1 = f
    ///   ComposeBody : intrinsicVar0 = f, intrinsicVar1 = g,
    ///                 intrinsicVar2 = final
    /// kInvalid sentinel = unused.
    VarId intrinsicVar0 = kInvalid;
    VarId intrinsicVar1 = kInvalid;
    VarId intrinsicVar2 = kInvalid;

    /// #740 Stage 4 v3 (2026-05-21) formal-rec attrset VarId.
    ///
    /// For formals-style lambdas (`{a, b ? def}: body`), the lowerer
    /// synthesises a LetRec attrset whose entries are per-formal
    /// thunks.  Body references to formals lower to
    /// `RecBindingSlotRef{formalsRecVar, name}` bindings.  Recording
    /// the formalsRecVar here lets the Stage 4 v3 strictness pass
    /// (opt_func_strictness.cc) identify formal-reference bindings
    /// in the body and trace which formals are forced before
    /// branching.
    ///
    /// `kInvalid` for single-arg lambdas (no formals) and any
    /// formals-style lambda whose lowering doesn't go through the
    /// synthetic LetRec path (none currently).
    VarId formalsRecVar = kInvalid;

    /// #737 Stage 4 v2 (2026-05-21) per-Function strictness signature.
    ///
    /// One entry per formal argument the function accepts at the
    /// emit-time call ABI level.  Order matches `formals` (for
    /// formals-style lambdas) or `[paramVar]` (for single-arg `x:
    /// body`).  Computed by `computeFunctionStrictness` (in
    /// opt_func_strictness.cc) AFTER `optimise` and BEFORE
    /// `computeFreeVars`.
    ///
    /// `strictArgs[i] == true` means: EVERY execution path through
    /// the body forces formal #i before any branching point.  Such
    /// formals are SAFE to pre-force at the call site (no thunk
    /// allocation needed; direct value push).
    ///
    /// `strictArgs[i] == false` means: at least one path through
    /// the body does NOT force formal #i — the formal MUST stay
    /// lazy at the call site (thunkify-for-arg as today).
    ///
    /// This is INFORMATION ONLY for v2 — the call-site emitter does
    /// not yet consume the signature.  Future v3 wiring through
    /// OP_CALL_STRICT (or equivalent) will skip MkThunk on strict
    /// positions when the callee is statically known.
    ///
    /// The vector is sized once by `computeFunctionStrictness`;
    /// empty if the pass hasn't run yet.
    std::vector<bool> strictArgs;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

/// Global v3 symbol table — shared across all Modules / CompilationUnits
/// in a process so that SymbolIds are stable across imports.  Lazily
/// populated by Module::internSymbol via globalInternSymbol().
const std::vector<std::string> & globalSymbolTable();
SymbolId globalInternSymbol(std::string_view s);

/// WS5-D2a — id-preferring intern used ONLY by the AOT-borrow deserialize
/// path (serialize::deserializeCUBorrowed).  Interns `name` PREFERRING the
/// slot `preferredId` (the writer process's SymbolId), so that a CU whose
/// read-only bytecode is BORROWED in place from the AOT mmap keeps its
/// symbol operands valid WITHOUT rewriting the (read-only, shared) code
/// pages.  Semantics:
///   * `name` already interned          → returns its existing id (unchanged).
///   * slot `preferredId` free (a hole / past the end) and `name` unseen
///                                       → places `name` at `preferredId`,
///                                         returns `preferredId` (SEEDED).
///   * slot `preferredId` occupied by a different name
///                                       → interns `name` normally (append),
///                                         returns the fresh id (CONFLICT).
/// The caller detects identity as `result == preferredId`.  Because both
/// the writer and reader register builtins/primops deterministically at
/// startup, and an AOT-hit reader does NOT re-intern the symbols of the
/// imported files it skips, the writer's ids are almost always free in the
/// reader → seeding succeeds → the borrow stays clean (Shared_Clean).
/// A conflict is always SAFE: it just forces that one CU to own+remap its
/// code (never wrong, only unshared).  Off the AOT path this is never
/// called, so the normal `globalInternSymbol` id assignment is unchanged.
SymbolId globalSeedSymbol(SymbolId preferredId, std::string_view name);

/// WS5-D2a — reserve the global symbol-id range [0, maxId] so that subsequent
/// `globalInternSymbol` calls append ABOVE it.  Called once by
/// aot_cache::init (with the max SymbolId across all AOT CU blobs) BEFORE any
/// borrowed CU is loaded: it grows the table with empty holes so the reader's
/// own fresh interns can't land on a writer id that a later borrowed CU needs
/// to seed — the collision that otherwise forces `code` to be owned+remapped
/// instead of borrowed.  A no-op if the table is already larger.  Off the AOT
/// path this is never called, so normal id assignment is unchanged.
void reserveSymbolCapacity(SymbolId maxId);

/// Sub-Expr -> (FuncId) entry recorded by the lowerer.  The lower
/// pre-creates a per-thunk Function for every nontrivial Expr that
/// would be wrapped in a thunk (let bindings, lazy attrset values,
/// etc.).  We expose the (AST Expr* -> FuncId) mapping here so the
/// post-compile pass can populate a runtime force-hook cache —
/// when tree-walker calls forceValue with that Expr*, v3 can
/// resolve it back to a CompilationUnit + FuncId and run the
/// pre-compiled code directly.
struct SubExprEntry {
    const void * astExpr;     // nix::Expr* — opaque here to avoid the include
    FuncId       funcIdx;
};

/// CO-2 phase B: per-VarId origin recorded at lower time when an
/// ExprVar resolves to an outer-scope binding via the direct
/// `byDispl` path.  Used at force time to reconstruct upvalues
/// from tree-walker's `Env`: walk env up `level` parents and read
/// `values[displ]`.  Synthesized VarIds (rec-attrset access,
/// inheritFrom, with-lookup) are NOT recorded — Phase B skips
/// functions whose freeVars include unrecorded VarIds.
///
/// `level` and `displ` are relative to the SCOPE of the function
/// they were recorded in (`func`).  The same VarId referenced from
/// different functions may have different (level, displ) values,
/// so we key by (func, var) rather than var alone.
struct VarOrigin {
    FuncId   func;
    VarId    var;
    uint32_t level;
    uint32_t displ;
};

/// WC-2-followup: rec-attrset self-reference origin.  v3 carries
/// the rec attrset as a single VarId; tree-walker spreads its
/// bindings across env cells at displacement 0..N-1.  The force
/// hook materialises a Bindings* from the env range at force time
/// using the recorded (level, names) pair.
struct RecVarOrigin {
    FuncId                func;
    VarId                 recVar;
    uint32_t              level;
    /// Shared with the originating Scope::recAttrsNames so multiple
    /// rec-binding refs can record their origin without copying the
    /// names vector per ref (REVIEW MED-9: was an O(N^2) hot path on
    /// nixpkgs-scale let-recs).
    std::shared_ptr<const std::vector<SymbolId>> names;
};

struct Module {
    /// All blocks; blocks[0] is unused (kInvalidBlock sentinel).
    std::vector<Block> blocks;
    /// All functions; functions[0] is the top-level entry.
    std::vector<Function> functions;

    /// Local view into the global symbol table; kept for diagnostics.
    /// internSymbol returns ids from the global table directly so they
    /// remain stable across imports/CUs.
    std::vector<std::string> symbols;

    /// Per-thunk function provenance recorded by the lowerer.  Each
    /// entry is `(AST Expr*, IR FuncId)` for a thunk-body function.
    /// Consumed by the post-compile cache populator (CO-3) to wire
    /// up the forceValue cutover.
    std::vector<SubExprEntry> subExprFuncs;

    /// CO-2 phase B: origin map populated by `resolveVar` whenever an
    /// ExprVar takes the direct (byDispl) path.  Multiple references
    /// to the same VarId may produce duplicate entries; the post-pass
    /// dedupes by keeping only one per (VarId).
    std::vector<VarOrigin> varOrigins;

    /// CO-2 phase B: VarIds the lowerer allocated as v3-internal "rec
    /// attrset" values (recVar of every let-rec / rec attrset).  These
    /// are NOT representable as a single tree-walker env cell — to
    /// reconstruct them at force time we'd have to walk every binding
    /// in the rec scope and assemble a Bindings*.  Until that lands
    /// (a future Phase B refinement) we skip per-thunk functions whose
    /// freeVars intersect this set.
    std::vector<VarId> recVarIds;

    /// #458 step 1/6 — heap-stable rec-attrset slot capture.
    ///
    /// Per let-rec scope, the lowerer allocates a parallel `recSlotVar`
    /// alongside the regular `recVar`.  At runtime, `recSlotVar` holds a
    /// Tag::Slot pointing at a heap-stable Value (allocated by
    /// OP_REC_SLOT_PUBLISH) that contains the rec-attrset's Tag::Attrs.
    /// The Bindings storage is the SAME storage as recVar's Tag::Attrs,
    /// so OP_ATTRS_REC_SET writes are visible through both.
    ///
    /// Inner closures whose freeVars resolve to the rec-attrset capture
    /// recSlotVar (Tag::Slot) instead of recVar (which today is the
    /// wrap-thunk that triggers blackhole when forced mid-construction).
    /// Forcing a Tag::Slot derefs to the (possibly partial) Tag::Attrs
    /// without involving the wrap thunk's state machine.
    ///
    /// Map is keyed by recVar; each let-rec scope inserts one entry.
    /// Empty under the legacy lowering path.
    std::unordered_map<VarId, VarId> recVarToSlotVar;

    /// #458 Phase B RecBuildSlot — VarIds the lowerer allocated as
    /// recSlotVar (Tag::Slot pointing at heap-stable rec-attrset
    /// storage).  Companion to `recVarIds`.  Phase B's UpvalueSource
    /// populator detects freeVars in this set and emits a
    /// `Kind::RecBuildSlot` source — same env walk as RecBuild but
    /// the result is wrapped as Tag::Slot pointing at a freshly-
    /// allocated heap Value (so the lambda body's slot-capture refs
    /// work uniformly across both lower-emit and call-hook paths).
    std::vector<VarId> recSlotVarIds;

    /// #425: VarIds the lowerer bound to `LitBuiltins` (the singleton
    /// `builtins` attrset).  When a sub-Expr captures one of these as
    /// a freeVar, the populate path generates a special UpvalueSource
    /// that just hands back the v3 vBuiltins singleton at hook time --
    /// no env walk needed since builtins is process-wide constant.
    /// Closes the LitBuiltins subset of the noUpvSrc failure mode.
    std::vector<VarId> litBuiltinsVarIds;

    /// WC-2-followup companion to recVarIds.  For each (function,
    /// recVar) pair where the recVar appears as a freeVar, records
    /// the level + names so the force hook can synthesise a v3
    /// Bindings* from tree-walker's env range.
    std::vector<RecVarOrigin> recVarOrigins;

    VarId   nextVar   = 1;
    BlockId nextBlock = 1;

    /// Allocate a fresh VarId.
    VarId freshVar() { return nextVar++; }

    /// Create a new empty Block.  Returns its BlockId.
    BlockId freshBlock();

    /// Intern a symbol.  Returns SymbolId; same input -> same id.
    SymbolId internSymbol(std::string_view s);

    /// Convenience: get a symbol's textual name.
    std::string_view symbolName(SymbolId id) const;
};

inline Module makeModule()
{
    Module m;
    // Reserve slot 0 for the kInvalid sentinels.
    m.blocks.emplace_back();          // blocks[0] = unused
    m.functions.emplace_back();       // functions[0] = top-level (filled later)
    m.symbols.emplace_back("");       // symbols[0]  = empty / invalid
    return m;
}

// ---------------------------------------------------------------------------
// Free-vars analysis
// ---------------------------------------------------------------------------

/// Compute Function::freeVars and Lambda/MkThunk::freeVars for every function
/// in the module.  Must be run after lowering and before emit.
void computeFreeVars(Module & m);

/// Insert into `refs` every VarId referenced *directly* by `e` (operand
/// position).  Does NOT recurse into sub-blocks (If/With/Assert bodies)
/// nor into nested functions (Lambda/MkThunk bodies).  Lambda/MkThunk
/// freeVars vectors ARE included -- they're the captures the closing
/// expression needs at MAKE_CLOSURE / MAKE_THUNK time.
///
/// Used by DCE and other IR passes that need to know which VarIds a
/// binding consumes.
void collectExprRefs(const Expr & e, std::unordered_set<VarId> & refs);

// ---------------------------------------------------------------------------
// Optimisation passes
// ---------------------------------------------------------------------------

/// Constant fold arithmetic / comparison / boolean operations whose every
/// operand is a literal in the same Block.  Replaces the right-hand-side of
/// the binding with the folded LitInt / LitFloat / LitBool.  Skips cases
/// where the runtime would throw (div-by-zero, INT64_MIN / -1, integer
/// overflow on Add/Sub/Mul) so eval-time semantics are preserved.
///
/// Safe to run before computeFreeVars: never introduces new VarRefs and
/// never removes a VarRef that the surrounding scope might still consume.
/// Returns the number of bindings whose expr was replaced.
size_t constantFold(Module & m);

/// Erase bindings whose VarId is referenced nowhere else in the Module
/// AND whose RHS is obviously pure (Lit*/VarRef/Lambda/MkThunk/AttrSet/
/// ListExpr/LitPrimOp/LitBuiltins).  Bindings that may force a thunk,
/// invoke a primop, or throw at evaluation time (Force, App, Add, ...)
/// are preserved unconditionally to keep eval-order semantics intact.
/// Iterates to a fixed point.  Returns the total number of bindings
/// removed across all iterations.
size_t deadBindingElim(Module & m);

/// Body-clear functions unreachable from the entry function (func 0).
/// Reachability follows every in-block funcIdx carrier (Lambda, MkThunk,
/// LetRec entry/hidden thunkBodies) and every intra-function block edge
/// (If/With/Assert/And/Or/Impl sub-blocks).  An unreachable function's
/// entry block is replaced with a trivial `return null` stub; the function
/// is NOT removed from `m.functions`, so every existing FuncId stays valid.
/// Idempotent — safe to run more than once.  Returns the count cleared.
///
/// Runs inside `optimise()`, but ALSO after `applyStrictnessPasses` (which
/// de-thunks strict call args and orphans their thunk bodies — residue the
/// in-`optimise` sweep, running before strictness, cannot see).
size_t deadFunctionElim(Module & m);

/// OPT_OCCUR Phase B variant of deadBindingElim.  Consults the OccMap
/// produced by `analyseOccurrence` instead of re-walking the reference
/// graph each iteration.  Two passes (per OPT_OCCUR_PLAN_2026-05-08.md
/// §B): first pass removes Dead+pure bindings; second pass picks up
/// any newly-Dead bindings whose sole consumer was removed by the
/// first.  Equivalent result to `deadBindingElim` (8-round fixed point)
/// but bounded at exactly 2 occurrence-info walks.  Returns total
/// bindings removed.
///
/// gate: NIX_V3_OCCUR_DCE — opt-in switch to use this variant in
/// place of deadBindingElim.  Retire once side-by-side validation
/// (NIX_V3_OCCUR_DCE_VALIDATE) confirms identical removal sets
/// across the full functional test suite for one release.
size_t deadBindingElimViaOccur(Module & m);

/// Collapse VarRef alias bindings.  For every `v = VarRef{u}`, rewrite
/// every operand `v` to `u` across the whole Module and drop the
/// alias binding.  Path-compresses chains so a chain of N aliases
/// resolves in one rewrite.  Strictly safe: a VarRef is a pure rename
/// — replacing it changes nothing observable.  Returns the number of
/// alias bindings removed.
size_t inlineTrivialBindings(Module & m);

/// Block-local common subexpression elimination.  Within each Block,
/// merges identical-shape arithmetic / comparison / boolean / static
/// HasAttr bindings: the second occurrence becomes `VarRef{firstSeen}`
/// so the alias-collapse pass folds it away.  Strict whitelist (see
/// opt_cse.cc) keeps observable side effects intact.  Returns the
/// number of bindings rewritten to aliases.
size_t commonSubexprElim(Module & m);

/// IR Phase A (2026-05-18): 1-shot beta reduction.  Inlines
/// `App(VarRef→Lambda, arg)` patterns when SAFE — i.e. the Lambda is
/// same-block, OnceLinear, single-arg (no formals), no intrinsic,
/// body has no nested Function/Block-carrying Exprs (Lambda/MkThunk/
/// LetRec/If/With/Assert/And/Or/Impl).  See opt_beta_reduce.cc for
/// the exact predicate and the clone-with-substitution algorithm.
/// Gate: NIX_V3_NO_BETA_REDUCE=1 disables.  Returns the number of
/// App bindings rewritten.
size_t betaReduce(Module & m);

/// IR Phase B (2026-05-18): pure-primop constant folding.  Recognises
/// PrimOpCall shapes where the operands are statically known
/// (ListExpr / LitInt / LitString / LitBool / AttrSet) AND the
/// primop has a known compile-time semantics (length / stringLength /
/// head / tail / elemAt / toString / attrNames).  Rewrites to a
/// literal / VarRef / ListExpr.  Conservative — only folds patterns
/// whose runtime result is statically computable without throwing.
/// Gate: NIX_V3_NO_PRIMOP_FOLD=1 disables.  Returns the number of
/// PrimOpCall bindings folded.
size_t primOpFold(Module & m);

/// IR Phase C (2026-05-18): stream fusion.  Recognises
/// `foldl'(op, init, map(f, xs))` IR patterns and rewrites to
/// `__foldlMap(op, init, f, xs)` — a single-pass FFI leaf primop.
/// Eliminates the intermediate map result list + N callClosure
/// invocations + one list traversal.  Use-once safety check on the
/// map's result VarId prevents work duplication.
/// Gate: NIX_V3_NO_STREAM_FUSION=1 disables.  Returns the number of
/// foldl' bindings rewritten.
size_t streamFusion(Module & m);

/// Detection-only probe (measure-twice gate) for the
/// `foldl' (acc: x: acc ++ G) [] xs` O(n²) accumulation idiom + the
/// `if C then acc ++ G else acc` filter shape.  Counts occurrences without
/// rewriting; gated logging via V3_DBG_FOLDL_APPEND=1.  Used to decide
/// whether the (risky) IR-surgery rewrite to `concatLists (map (x: G) xs)`
/// is worth it given that nixpkgs lib avoids this antipattern.
size_t detectFoldlAppendIdiom(const Module & m);

/// IR Phase G (2026-05-18): pure if-then-else folding.  Recognises
/// `If(LitBool, thenBlock, elseBlock)` patterns and rewrites the
/// binding to inline the chosen block's bindings + a VarRef to the
/// chosen block's TermReturn target.  Eliminates the OP_BRANCH_FALSE
/// emit + the discarded branch's bytecode.  Gate: NIX_V3_NO_IF_FOLD=1
/// disables.  Returns the number of If bindings folded.
size_t ifThenFold(Module & m);

/// IR Phase H (2026-05-18): static genList unrolling.  Recognises
/// `PrimOpCall("genList", [f, n])` where `n` resolves to a LitInt
/// in 0..8 and rewrites to an N-element ListExpr of per-element
/// MkThunk bindings (each thunk's body is `App(f, LitInt i)`).
/// Laziness preserved: each MkThunk forces its body only on demand.
/// Gate: NIX_V3_NO_GENLIST_UNROLL=1 disables.  Returns the number of
/// genList calls unrolled.
size_t genListUnroll(Module & m);

/// IR Phase F (2026-05-18): static App-spine folding.  Recognises
/// curried call chains `App(App(...App(f, a0), ...), a_N-1)` where
/// `f` resolves to a Lambda whose body is an N-deep canonical
/// curried-Lambda chain and all `a_i` are PURE.  Substitutes all N
/// args into the deepest body in one shot, eliminating N-1
/// PartialApp allocations.  Gate: NIX_V3_NO_APP_SPINE_FOLD=1
/// disables.  Returns the number of spines folded.
size_t appSpineFold(Module & m);

/// IR (2026-06-05): de-thunk USE-ONCE MkThunk args to STRICT arithmetic
/// primops (__add/__sub/__mul/__div/__lessThan, which force every arg).
/// The lowerer thunks every primop arg for laziness, but a strict primop
/// forces its args when its block runs, so a use-once arg-thunk's deferral is
/// redundant — inlining the thunk body is byte-identical and exposes nested
/// arithmetic to constant folding (e.g. `x*y*z`'s inner `x*y` thunk, which
/// otherwise blocks appSpineFold).  Runs BEFORE appSpineFold.  Gate:
/// NIX_V3_NO_DETHUNK_STRICT=1 disables (bisect handle; retire once shipped
/// byte-identical on --core + a nixpkgs sample).  Returns the number de-thunked.
size_t deThunkForcedStrictArgs(Module & m);

/// #429: fuse App-chains over LitPrimOp into a single PrimOpCall.
/// Detects the let/inherit-from indirection pattern that escapes
/// lowerCall's direct-recognition (e.g. `let inherit (builtins) map;
/// in map f xs`) and rewrites the saturated tail App to PrimOpCall.
/// Intermediate partial-Apps become orphan bindings that the next
/// DCE pass sweeps.  Skips primops with non-zero lazyArgs to keep
/// per-arg laziness semantics intact.  Returns the number of App
/// bindings rewritten.
size_t fusePrimOpApps(Module & m);

/// #423: eliminate redundant `Force{v}` bindings via local strictness
/// analysis.  Lower emits Force defensively at every strict-context
/// use; this pass detects the cases where `v` is provably already in
/// WHNF (literals, lambdas, attrsets, lists, primitive arithmetic,
/// etc.) and rewrites the Force as a VarRef.  Subsequent
/// `inlineTrivialBindings` collapses the alias and `deadBindingElim`
/// removes the orphan binding, so the OP_FORCE bytecode never gets
/// emitted.  Block-local; chases VarRef chains within the same
/// block.  Returns the number of Force bindings rewritten.  Disable
/// with `NIX_V3_NO_OPT_STRICT=1`.
size_t elimRedundantForce(Module & m);

/// Run the standard optimisation pipeline.  Currently:
/// constantFold -> commonSubexprElim -> inlineTrivialBindings ->
/// fusePrimOpApps -> deadBindingElim.  Always called between lower
/// and computeFreeVars by the v3 hook, the import primop, and the
/// wrapper-source primop.  No-op when `NIX_V3_NO_OPT` is set (escape
/// hatch for debugging).
void optimise(Module & m);

/// #737 Stage 4 v2 (2026-05-21) per-Function strictness inference.
///
/// For each `ir::Function` in `m.functions`, computes the
/// `strictArgs` bitmap (one bit per formal argument).  The bit is
/// set iff EVERY execution path through the body forces the formal
/// before any branching point.
///
/// Forward dataflow over body blocks:
///   - maintain a "definitely forced" VarId set.
///   - each binding `var = e` adds e's strict operands to the set.
///   - terminal flow: `TermReturn{v}` does NOT force v (the caller
///     forces the return value, not the function itself).
///   - branching ops (If/With/Assert) force their scrutinee but
///     branch bodies are NOT walked recursively in v2 — only the
///     pre-branch linear prefix is considered.  This is conservative
///     (under-marks strictness) and safe.
///
/// Result is INFORMATION ONLY for this commit (Stage 4 v2): the
/// emitter does not yet apply the signature at call sites.  Future
/// v3 will wire caller-side use through OP_CALL_STRICT.  When
/// `NIX_V3_DBG_STRICTNESS=1` is set, emits a one-line summary at
/// the end of the pass.
void computeFunctionStrictness(Module & m);

/// #742 Stage 4 v4 (2026-05-21) — caller-side application of the
/// strictness signature.  Walks every App binding; if `fun`
/// statically resolves to a Lambda whose target Function has
/// strictArgs[0] == true, AND the App's arg is a MkThunk in the
/// same block with exactly one use and a simple body, inline the
/// thunk body into the calling block and replace the App's arg
/// with the inlined tail.  Defined in opt_strict_call_unthunk.cc.
///
/// Returns the number of MkThunk wraps elided.
///
/// Runs AFTER `computeFunctionStrictness` (so strictArgs is set)
/// and BEFORE `computeFreeVars` (so the inlined bindings are
/// visible to freeVars).  Gate: NIX_V3_NO_STRICT_CALL_UNTHUNK=1
/// disables.  Telemetry: NIX_V3_DBG_STRICT_CALL_UNTHUNK=1 prints
/// elision count.
size_t applyStrictnessAtCallSites(Module & m);

/// Run the caller-side strictness passes as a unit: computeFunctionStrictness
/// followed by applyStrictnessAtCallSites iterated to a fixpoint (max 8).
/// This is the EXACT sequence the production eval path (run.cc) runs after
/// `optimise`, factored out so the `--emit-bytecode` dump runs the SAME
/// passes — otherwise the disassembly shows a pre-strictness form (with the
/// arg-thunks still present) that does NOT match what eval executes, which
/// has misled bytecode review.  Defined in opt_strict_call_unthunk.cc.
void applyStrictnessPasses(Module & m);

// ---------------------------------------------------------------------------
// #540: occurrence analysis (per lode/OPT_OCCUR_PLAN_2026-05-08.md)
// ---------------------------------------------------------------------------

/// Per-binder occurrence kind.  Mirrors GHC `OccurAnal`'s coarse
/// classification minus loop-breakers (no inliner yet) and minus
/// branch-aware OneOcc refinement (deferred to v2 of the pass).
///
/// Authorization table for downstream consumers:
///
///   | Kind          | Substitute RHS at use? | Drop binding?           |
///   |---------------|------------------------|-------------------------|
///   | Unknown       | NEVER                  | NEVER                   |
///   | Param         | NEVER                  | NEVER (externally bound)|
///   | Dead          | n/a                    | YES (subject to purity) |
///   | OnceLinear    | YES (any RHS)          | YES (after substitution)|
///   | OnceCaptured  | ONLY trivial RHS       | NEVER automatically     |
///   | Many          | ONLY trivial RHS       | NEVER                   |
///
/// Trivial RHS = LitInt/Float/Bool/Null/String/Path, VarRef,
/// LitPrimOp, LitBuiltins.  Zero evaluation cost so duplicating is
/// free.
enum class OccKind : uint8_t {
    /// Out-of-range, not a defined VarId, or not analysed.  Consumers
    /// MUST treat as conservative ("don't touch").
    Unknown = 0,
    /// Function paramVar / LetRec recVar / hidden-entry hiddenVar.
    /// Externally bound (the runtime supplies the value); count is
    /// meaningful but consumers should not drop the binding.
    Param,
    /// Zero references across the whole module.  Subject to existing
    /// purity check, droppable.
    Dead,
    /// One reference, in the same function as the def, NOT through a
    /// captured-list entry (Lambda::freeVars / MkThunk::freeVars /
    /// LetRec::*::outerUpvalues / lexicalWiths).  Substituting the
    /// RHS at the use site is safe; the binding can be dropped after.
    OnceLinear,
    /// One reference, but the use is inside a different function.
    /// The binding's value crosses a closure capture, so substitution
    /// is only safe for trivial RHSes (zero evaluation cost).
    OnceCaptured,
    /// Two or more references (saturating count).  Substitution only
    /// safe for trivial RHSes; binding stays.
    Many,
};

/// Per-VarId occurrence record.
struct OccInfo {
    OccKind  kind        = OccKind::Unknown;
    /// Saturating count: capped at 2 (for "Many" we don't need the
    /// exact value).  Direct uses only — captured-list entries are
    /// excluded (those refs are derived data populated by
    /// computeFreeVars and would double-count).
    uint16_t count       = 0;
    /// True if any direct use is inside a different Function than the
    /// definition.  Drives OnceLinear vs OnceCaptured classification
    /// for count==1; for count>=2 it's still meaningful info for
    /// future passes (e.g., a Many-with-captured RHS may still be
    /// safe to inline at certain use kinds).
    bool     capturedUse = false;
};

/// Module-wide occurrence map.  `data[v] = OccInfo` for every VarId
/// `v < nextVar`.  Out-of-range lookups return Unknown.
struct OccMap {
    std::vector<OccInfo> data;
    OccInfo lookup(VarId v) const noexcept
    {
        if (v == kInvalid || v >= data.size()) return {};
        return data[v];
    }
};

/// Compute the occurrence map for `m`.
///
/// Algorithm:
///   1. Compute funcOfBlock[bid] -> fid via reachability from each
///      Function::entryBlock; sub-blocks (If/With/Assert bodies,
///      And/Or/Impl rhsBlocks) inherit the parent's fid.
///   2. defFunc[var] = fid for every binding's var, paramVar, recVar,
///      hiddenVar.
///   3. Initialise kind = Dead for binding vars, Param for the
///      lambda/letrec-bound params and synthesised hidden vars.
///   4. Forward-walk every block; for every binding's expr and the
///      terminal, accumulate operand uses.  CAPTURED-LIST entries
///      (Lambda::freeVars / MkThunk::freeVars / LetRec::*::outerUpvalues
///      / Lambda::lexicalWiths / MkThunk::lexicalWiths /
///      LetRec::*::lexicalWiths) are SKIPPED — those VarIds are
///      already counted via the body's direct operand uses, and
///      counting them too would double-count.
///   5. Finalise: count==0 -> Dead; count==1 && !captured -> OnceLinear;
///      count==1 && captured -> OnceCaptured; count>=2 -> Many.
///
/// O(N) in the size of the IR (one pass per binding/terminal).
OccMap analyseOccurrence(const Module & m);

/// Convenience: return true iff the given Expr is a "trivial" RHS
/// — zero evaluation cost, safe to duplicate at any use site.
bool isTrivialRhs(const Expr & e) noexcept;

} // namespace nix::v3::ir
