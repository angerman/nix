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
struct AttrSet {
    struct Entry { SymbolId name; VarId value; uint32_t pos = 0; };
    std::vector<Entry> entries; // sorted ascending by SymbolId
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
struct Update { VarId lhs; VarId rhs; };

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
    };
    std::vector<HiddenEntry> hiddenEntries;
};

// ---------------------------------------------------------------------------
// IRExpr sum
// ---------------------------------------------------------------------------

using Expr = std::variant<
    LitInt, LitFloat, LitBool, LitNull, LitString, LitPath,
    VarRef, WithLookup,
    Lambda, App, Force, MkThunk,
    AttrSelect, AttrSelectDyn, HasAttr, HasAttrDyn, AttrSet, AttrSetDyn,
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

    /// Formals (`{ a ? def, b }: body`).  Empty if no formals.
    std::vector<Formal> formals;
    bool                hasFormals = false;
    bool                ellipsis   = false;

    /// Free vars referenced by the body block (and recursively by any
    /// sub-blocks / nested functions reachable from the body), in the
    /// order they appear as upvalues at runtime.  Populated by
    /// computeFreeVars before emit.
    std::vector<VarId>  freeVars;

    /// Optional name for diagnostics (e.g. lambda or attribute name).
    std::string         name;

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
    uint8_t             intrinsicKind = 0;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

/// Global v3 symbol table — shared across all Modules / CompilationUnits
/// in a process so that SymbolIds are stable across imports.  Lazily
/// populated by Module::internSymbol via globalInternSymbol().
const std::vector<std::string> & globalSymbolTable();
SymbolId globalInternSymbol(std::string_view s);

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

} // namespace nix::v3::ir
