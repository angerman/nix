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
constexpr FuncId kInvalidFunc = 0xFFFFFFFFu;

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
/// `depth` = how many enclosing with-scopes to skip before lookup (0 = innermost).
struct WithLookup {
    SymbolId name;
    uint32_t depth;
};

// --- Lambdas, application, thunks ---

/// Lambda formal parameter (in `{ a ? def, b, ... }: body`).
struct Formal {
    SymbolId name;
    /// Default-value block (kInvalidBlock if formal is required).  Free vars
    /// of the default are part of the enclosing closure's upvalues.
    BlockId  defaultBlock = kInvalidBlock;
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
struct Force { VarId thunk; };

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

/// Recursive attrset (`rec { ... }`).  Each entry's value can reference
/// any sibling via the synthetic `selfVar` (lowering rewrites such refs
/// to AttrSelect on selfVar).
struct RecAttrSet {
    VarId selfVar;
    struct Entry { SymbolId name; VarId value; uint32_t pos = 0; };
    std::vector<Entry> entries;
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
/// `slotRef` is set when `attrs` was lowered from a simple `ExprVar`
/// (a direct slot reference): in that case the emitter should push a
/// `Tag::Slot` Value pointing at the resolved local slot rather than
/// a snapshot of the slot's contents.  This preserves SECD-style
/// pointer aliasing for `with self;` patterns where `self` is a
/// let-rec binding that may be mutated mid-evaluation (WC-38).
/// When `slotRef = kInvalid`, `attrs` is used as a regular value
/// source (back-compat with the original `OP_GET_LOCAL +
/// OP_WITH_PUSH` path).
///
/// `recAttrsVar` + `recAttrsName` are set when `attrs` resolves to a
/// rec-attrset entry: the emitter should push a Tag::Slot pointing
/// into `recAttrsVar`'s Bindings::entries[i].value (heap-stable).
/// This is the production path for `with self;` over rec-attrsets and
/// is what makes `lib.fix` patterns work in v3.  Both fields are
/// kInvalid when unused.
struct With  {
    VarId attrs;
    BlockId bodyBlock;
    VarId slotRef = kInvalid;
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
struct Negate { VarId operand; };

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

/// `__curPos` — position attrset of the call site.
struct PosExpr {};

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
};

// ---------------------------------------------------------------------------
// IRExpr sum
// ---------------------------------------------------------------------------

using Expr = std::variant<
    LitInt, LitFloat, LitBool, LitNull, LitString, LitPath,
    VarRef, WithLookup,
    Lambda, App, Force, MkThunk,
    AttrSelect, AttrSelectDyn, HasAttr, HasAttrDyn, AttrSet, AttrSetDyn, RecAttrSet,
    ListExpr, ConcatLists,
    If, With, Assert,
    ConcatStrings,
    Not, Negate, Add, Sub, Mul, Div, Eq, NEq, Less,
    And, Or, Impl,
    Update,
    PosExpr,
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
    /// Parameters: VarIds defined "by entry" — for a function body Block,
    /// this is the parameter Var (or the fresh slots for matched formals);
    /// for a thunk body, empty.
    std::vector<VarId>   params;
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
    std::vector<SymbolId> names;
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

} // namespace nix::v3::ir
