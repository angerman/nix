#pragma once
/// @file
/// Intermediate Representation (IR) for the Nix evaluator.
///
/// The IR sits between the AST (nixexpr.hh) and bytecode (bytecode.hh),
/// providing a flat, A-normal-form representation suitable for analysis
/// and optimization passes before final code generation.
///
/// Design principles:
///   1. FLAT -- not a tree.  Each IR "block" is a linear sequence of
///      bindings (VarId = IRExpr) terminated by a single Terminal.
///   2. EXPLICIT free variable lists on every Lambda and MkThunk, so the
///      bytecode emitter knows exactly which upvalues to capture.
///   3. THUNK vs VALUE distinction at the type level -- MkThunk wraps a
///      lazy body, while all other IRExpr forms produce values.
///   4. DESUGARED -- inherit, with, let, rec, or-default, and string
///      interpolation are all lowered to primitive IR operations before
///      any optimization pass runs.
///   5. A-NORMAL FORM -- every compound sub-expression is bound to a
///      VarId, so optimization passes can reason about individual bindings
///      without recursive tree traversal.
///   6. SOURCE POSITIONS -- every binding and terminal carries a PosIdx
///      for error messages and profiling.
///
/// The IR is constructed by a single post-order walk of the AST
/// (after bindVars()), then optimization passes transform the IR
/// in-place, and finally the bytecode emitter consumes it.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/symbol-table.hh"
#include "nix/util/pos-idx.hh"

#include <boost/container/small_vector.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace nix {

struct ExprVar;
struct ExprLambda;
struct PrimOp;
struct Value;

} // namespace nix

namespace nix::ir {

// ============================================================================
// Core identifier types
// ============================================================================

/// Unique identifier for an IR variable.  Variables are numbered
/// sequentially during lowering; the ID space is per-IRModule.
/// ID 0 is reserved as "invalid/uninitialized".
using VarId = uint32_t;

/// Sentinel value: no variable.
static constexpr VarId kInvalidVar = 0;

/// Index into IRModule::blocks.  Blocks are the unit of control flow:
/// every lambda body, thunk body, if-branch, etc. is a separate block.
using BlockId = uint32_t;

/// Sentinel value: no block.
static constexpr BlockId kInvalidBlock = 0;


// ============================================================================
// Free variable set
// ============================================================================

/// Sorted list of VarIds that are free in a lambda or thunk body.
/// The bytecode emitter uses this to emit upvalue capture instructions
/// in the enclosing scope when creating a closure or thunk object.
///
/// Sorted so that:
///   (a) set operations (union, intersection, difference) are O(n).
///   (b) the bytecode emitter can assign deterministic upvalue slots.
struct FreeVars
{
    /// Use small_vector to keep tiny FreeVars sets (the common case —
    /// most bindings reference 0-8 free variables) inline without
    /// heap allocation.  Set ops still treat this as a sorted list.
    boost::container::small_vector<VarId, 8> vars;  ///< Sorted, no duplicates.

    bool empty() const noexcept { return vars.empty(); }
    size_t size() const noexcept { return vars.size(); }

    /// Insert a variable, maintaining sorted order.
    void insert(VarId v);

    /// Merge another FreeVars set into this one (set union).
    void merge(const FreeVars & other);

    /// Remove a variable (e.g., when a binding covers it).
    void erase(VarId v);

    /// Remove all variables in `bound` from this set.
    void subtract(const FreeVars & bound);

    /// Test membership.
    bool contains(VarId v) const noexcept;
};


// ============================================================================
// Lambda formal parameters (desugared)
// ============================================================================

/// A single formal parameter in a lambda's pattern match.
/// Desugared from the AST Formal: default expressions are referenced
/// by BlockId (their bodies are separate blocks) rather than by Expr*.
struct IRFormal
{
    Symbol name;                        ///< Parameter name.
    BlockId defaultBody = kInvalidBlock; ///< Block for default value, or kInvalidBlock if required.
    PosIdx pos;                         ///< Source position of this formal.
    FreeVars defaultFreeVars;           ///< Free vars of the default block (populated by computeFreeVars).
};

/// Desugared lambda parameter specification.
/// Covers all three AST forms:
///   - `x: body`          -> arg set, no formals
///   - `{ a, b }: body`   -> formals set, no arg
///   - `x@{ a, b }: body` -> both arg and formals
struct IRFormals
{
    Symbol arg;                          ///< Bound name for the entire argument (may be empty).
    std::vector<IRFormal> formals;       ///< Pattern-match fields, sorted by name.
    bool ellipsis = false;               ///< Whether `...` was present.
};


// ============================================================================
// IR expression variants (the "operations" in A-normal form bindings)
// ============================================================================
//
// Each variant represents a single primitive operation.  All operands
// are VarIds (referencing previous bindings) or constants -- never
// nested expressions.  This is the A-normal form invariant.

/// Literal integer value.
struct IRLitInt
{
    int64_t value;
};

/// Literal floating-point value.
struct IRLitFloat
{
    double value;
};

/// Literal string value.  The string data is owned by the AST arena
/// (BumpMemoryResource) and outlives the IR.
struct IRLitString
{
    std::string_view value;
};

/// Literal path value.
struct IRLitPath
{
    std::string_view path;
    /// Pointer to the SourceAccessor for this path.  Borrowed from the
    /// AST ExprPath node; valid for the lifetime of the Exprs arena.
    void * accessor; // SourceAccessor*, opaque here to avoid header dep
};

/// Literal boolean value.
struct IRLitBool
{
    bool value;
};

/// Literal null.
struct IRLitNull {};

/// Reference to a previously-bound IR variable.
struct IRVarRef
{
    VarId var;
};

/// Create a closure (lambda with captured environment).
///
/// The FreeVars list tells the bytecode emitter exactly which
/// variables from the enclosing scope to capture as upvalues.
/// The body is a separate block containing the desugared lambda body.
struct IRLambda
{
    FreeVars freeVars;                   ///< Variables captured from enclosing scope.
    IRFormals params;                    ///< Desugared parameter specification.
    BlockId bodyBlock;                   ///< Block containing the lambda body.
    Symbol name;                         ///< Optional lambda name (for profiling/errors).
    PosIdx pos;                          ///< Source position of the lambda.
    ExprLambda * sourceExpr = nullptr;   ///< Original AST node (for callFunction compat).
};

/// Function application.  Both operands must be VarIds.
struct IRApp
{
    VarId func;
    VarId arg;
};

/// Force evaluation of a thunk.  The operand may be a thunk-typed
/// VarId; after forcing, the result is a value.
struct IRForce
{
    VarId thunk;
};

/// Create a lazy thunk.  The body is evaluated on demand when forced.
///
/// FreeVars tells the emitter which enclosing bindings to capture
/// so the thunk body can reference them when eventually forced.
struct IRMkThunk
{
    FreeVars freeVars;                   ///< Captured variables.
    BlockId bodyBlock;                   ///< Block containing the thunk body.
    PosIdx pos;                          ///< Source position (for error reporting).
    Expr * sourceExpr = nullptr;         ///< Original AST expr (for isTrivial() compat).
};

/// Select a static attribute from an attrset: `attrs.name`.
struct IRAttrSelect
{
    VarId attrs;
    Symbol name;
};

/// Select a dynamic attribute from an attrset: `attrs."${nameExpr}"`.
/// The name is computed at runtime from a VarId that evaluates to a string.
struct IRAttrSelectDynamic
{
    VarId attrs;
    VarId nameVar;   ///< VarId that evaluates to the attribute name string.
};

/// Test whether an attrset has a given attribute: `attrs ? name`.
struct IRHasAttr
{
    VarId attrs;
    Symbol name;
};

/// Check whether an attribute exists using a dynamically-computed name.
struct IRHasAttrDynamic
{
    VarId attrs;
    VarId nameVar;   ///< VarId that evaluates to the attribute name string.
};

/// Construct a non-recursive attribute set from sorted (name, value) pairs.
///
/// All attribute names are statically known Symbol values.  Dynamic
/// attribute names are handled by IRAttrSetDynamic.
struct IRAttrSet
{
    /// Pairs are sorted by Symbol for deterministic output and
    /// efficient lookup.  PosIdx tracks the definition site of each attr.
    struct Entry
    {
        Symbol name;
        VarId value;
        PosIdx pos;
    };
    std::vector<Entry> entries;
};

/// Construct an attribute set with dynamic attribute names.
///
/// Dynamic entries have their name computed at runtime from a VarId.
/// Static entries are included alongside so the emitter can use the
/// mixed OP_ATTRS_DYN_INIT instruction.
struct IRAttrSetDynamic
{
    struct StaticEntry
    {
        Symbol name;
        VarId value;
        PosIdx pos;
    };

    struct DynamicEntry
    {
        VarId nameVar;   ///< VarId that evaluates to the attribute name string.
        VarId value;
        PosIdx pos;
    };

    std::vector<StaticEntry> staticEntries;    ///< Sorted by Symbol.
    std::vector<DynamicEntry> dynamicEntries;
};

/// Construct a recursive attribute set.
///
/// The `selfVar` is a VarId that bindings in `entries` may reference
/// to access sibling attributes (the "self" environment in rec {}).
/// The lowering pass introduces explicit thunks for forward references
/// within the rec scope.
struct IRRecAttrSet
{
    struct Entry
    {
        Symbol name;
        VarId value;       ///< Typically an IRMkThunk wrapping the expression.
        PosIdx pos;
    };
    VarId selfVar;         ///< VarId for the self-referencing attrset.
    std::vector<Entry> entries;
};

/// Construct a list from a known set of element VarIds.
struct IRList
{
    std::vector<VarId> elems;
};

/// Conditional branch.  The condition must be a VarId (already bound).
/// Then and else are separate blocks; the result of whichever branch
/// executes becomes the value of this binding.
struct IRIf
{
    VarId cond;
    BlockId thenBlock;
    BlockId elseBlock;
};

/// Direct primop call with all arguments available.
///
/// This is the result of resolving a call where the callee is a known
/// built-in function and all arguments are present.  This enables the
/// bytecode emitter to generate a direct call without going through
/// the generic OP_CALL path.
struct IRPrimOpCall
{
    const PrimOp * primOp;               ///< Pointer to the PrimOp descriptor.
    std::vector<VarId> args;             ///< Exactly `primOp->arity` arguments.
    PosIdx pos;                          ///< Call site position.
};

/// Introduce a `with` scope.  Within the body block, unresolved
/// variable references first consult the attrset `attrs` before
/// falling back to the enclosing scope.
///
/// This is NOT desugared away because `with` has dynamic scoping
/// semantics that cannot be statically resolved in general (the
/// attribute set is computed at runtime).  The bytecode emitter
/// generates OP_PUSH_WITH + OP_GET_WITH for lookups in the body.
struct IRWith
{
    VarId attrs;                         ///< The attrset to bring into scope.
    BlockId bodyBlock;                   ///< Body evaluated with the with-scope active.
    PosIdx pos;                          ///< Position of the `with` keyword.
};

/// With-scope variable lookup.  This is the IR-level representation
/// of a variable that was resolved to a `with` scope during bindVars().
/// The bytecode emitter generates OP_GET_WITH for these.
struct IRWithLookup
{
    Symbol name;                         ///< Attribute name to look up in with-scopes.
    PosIdx pos;                          ///< Position of the variable reference.
    ExprVar * sourceVar = nullptr;       ///< Original AST ExprVar with fromWith chain (for OP_GET_WITH).
    uint32_t v2Level = 0;               ///< Adjusted level for v2 env chain (accounts for missing let envs).
};

/// String interpolation / concatenation.
///
/// The parts are evaluated left-to-right and concatenated.
/// `forceString` indicates whether the result must be coerced to a
/// string (true for `"${...}"`, false for path interpolation).
struct IRConcatStrings
{
    std::vector<VarId> parts;
    bool forceString;
};

/// Assertion: evaluate `cond`; if false, throw an error at `pos`.
/// The result of the assertion expression is `body`.
/// Assert: check condition, then evaluate body block.
/// Body is a separate block to prevent condition-side inline blocks
/// from interfering with body evaluation.
struct IRAssert
{
    VarId cond;
    BlockId bodyBlock;
};

/// Boolean negation.
struct IRNot
{
    VarId operand;
};

// -- Arithmetic and comparison operators --
// All binary operators take two VarIds and produce a value.

struct IRAdd { VarId lhs; VarId rhs; };
struct IRSub { VarId lhs; VarId rhs; };
struct IRMul { VarId lhs; VarId rhs; };
struct IRDiv { VarId lhs; VarId rhs; };
struct IRNegate { VarId operand; };

struct IREq     { VarId lhs; VarId rhs; };
struct IRNEq    { VarId lhs; VarId rhs; };
struct IRLess   { VarId lhs; VarId rhs; };

/// Short-circuiting logical AND.
/// Lowered to: if lhs then rhs else false.
/// Represented explicitly so optimization passes can recognize the pattern
/// and the bytecode emitter can use JUMP_IF_FALSE for short-circuit.
/// Short-circuiting AND.  rhs is a separate block (only evaluated
/// when lhs is true) to avoid eagerly executing side effects.
struct IRAnd    { VarId lhs; BlockId rhsBlock; };

/// Short-circuiting logical OR.  rhs in a separate block.
struct IROr     { VarId lhs; BlockId rhsBlock; };

/// Logical implication: `lhs -> rhs` == `!lhs || rhs`.
/// rhs in a separate block.
struct IRImpl   { VarId lhs; BlockId rhsBlock; };

/// Attrset update (merge): `lhs // rhs`.
struct IRUpdate { VarId lhs; VarId rhs; };

/// List concatenation: `lhs ++ rhs`.
struct IRConcatLists { VarId lhs; VarId rhs; };

/// `__curPos` -- produces the source position as an attrset.
struct IRPos
{
    PosIdx pos;
};


// ============================================================================
// The IRExpr sum type
// ============================================================================

/// A single IR operation.  Every binding in a block is:
///   `VarId = IRExpr`
/// in A-normal form.
using IRExpr = std::variant<
    // Literals
    IRLitInt,
    IRLitFloat,
    IRLitString,
    IRLitPath,
    IRLitBool,
    IRLitNull,
    // Variable reference
    IRVarRef,
    // Closures and thunks
    IRLambda,
    IRApp,
    IRForce,
    IRMkThunk,
    // Attribute operations
    IRAttrSelect,
    IRAttrSelectDynamic,
    IRHasAttr,
    IRHasAttrDynamic,
    IRAttrSet,
    IRAttrSetDynamic,
    IRRecAttrSet,
    // List
    IRList,
    // Control flow
    IRIf,
    // Primop
    IRPrimOpCall,
    // With scope
    IRWith,
    IRWithLookup,
    // String interpolation
    IRConcatStrings,
    // Assertion
    IRAssert,
    // Logical operators
    IRNot,
    IRAnd,
    IROr,
    IRImpl,
    // Arithmetic
    IRAdd,
    IRSub,
    IRMul,
    IRDiv,
    IRNegate,
    // Comparison
    IREq,
    IRNEq,
    IRLess,
    // Attrset update
    IRUpdate,
    // List concatenation
    IRConcatLists,
    // Position
    IRPos
>;


// ============================================================================
// Binding: the fundamental unit of an IR block
// ============================================================================

/// A single A-normal-form binding: `result = expr`.
/// Every compound sub-expression in the original Nix source becomes
/// one Binding, with its result named by `result`.
struct Binding
{
    VarId result;    ///< The variable this binding defines.
    IRExpr expr;     ///< The operation that computes the value.
    PosIdx pos;      ///< Source position for error messages.

    /// M5: direct references in `expr` (NOT including sub-block free
    /// vars).  Populated by computeFreeVars/blockFreeVars (ir.cc) so
    /// the bytecode emitter's forward-ref + use-count pre-pass
    /// (ir-emit.cc:emitBlock) can reuse them instead of running
    /// collectRefs() a second time over the same expression.  ~10-15ms
    /// saving on nixpkgs-scale CUs (260K+ bindings) at the cost of
    /// ~14MB extra IR memory while compiling — both temporary, freed
    /// when the IRModule is dropped (or kept under NIX_VM_V2_LAZY_EMIT
    /// for re-entry).  Mutable so blockFreeVars can write through a
    /// `const IRBlock &`.
    mutable FreeVars directRefs;
};


// ============================================================================
// Block terminal instructions
// ============================================================================

/// How a block ends.  Every block has exactly one terminal.

/// Return the value of a VarId from this block.
/// In the top-level block, this is the final result of evaluation.
/// In lambda/thunk bodies, this is the return value.
struct TermReturn
{
    VarId value;
    PosIdx pos;
};

/// Tail call: re-enter a function without allocating a new frame.
/// Only valid in tail position of a lambda body.
struct TermTailCall
{
    VarId func;
    VarId arg;
    PosIdx pos;
};

/// Conditional branch to one of two blocks.
/// After the chosen block completes, its result becomes the value of
/// the enclosing binding that references this block's output.
struct TermBranch
{
    VarId cond;
    BlockId thenBlock;
    BlockId elseBlock;
    PosIdx pos;
};

using Terminal = std::variant<
    TermReturn,
    TermTailCall,
    TermBranch
>;


// ============================================================================
// IRBlock: a linear sequence of bindings + terminal
// ============================================================================

/// A basic block in the IR.  Blocks are the unit of control flow.
///
/// Every lambda body, thunk body, if-then branch, if-else branch,
/// and default-value expression is a separate IRBlock.  This makes
/// the IR flat: no nested expression trees, just sequences of
/// bindings ending in a terminal.
struct IRBlock
{
    BlockId id;                          ///< This block's ID in IRModule::blocks.

    /// Parameters: VarIds that are "inputs" to this block.
    /// For a lambda body: the argument VarId.
    /// For an if-branch: empty (inherits enclosing scope).
    /// For a thunk body: captured free variables are mapped via
    /// the FreeVars list on the IRMkThunk that references this block.
    std::vector<VarId> params;

    /// The sequence of A-normal-form bindings.
    std::vector<Binding> bindings;

    /// How this block terminates.
    Terminal terminal;

    /// Source position of the block start (for diagnostics).
    PosIdx pos;
};


// ============================================================================
// IRModule: the top-level IR container
// ============================================================================

/// The complete IR for one parsed file or top-level evaluation.
///
/// Contains all blocks (one per lambda body, thunk body, branch, etc.)
/// and a single entry block.  The entry block is always blocks[0].
///
/// Analogous to bytecode::CompilationUnit but at the IR level.
/// Env-chain coordinates for a variable from the enclosing runtime scope.
/// Used by the bytecode emitter to access baseEnv variables that are
/// referenced by the entry block but not defined in any IR block.
struct ExternalVarRef
{
    uint32_t level;       ///< Environment chain depth.
    uint32_t displacement; ///< Slot index within that env.
};

struct IRModule
{
    /// All blocks in the module.  blocks[0] is the entry block.
    /// Block IDs are indices into this vector.
    std::vector<IRBlock> blocks;

    /// Next available VarId.  Lowering increments this to allocate
    /// fresh variable names.
    VarId nextVar = 1;  // 0 is kInvalidVar.

    /// Map from VarId to runtime env coordinates for variables that
    /// originate from the enclosing scope (e.g., baseEnv builtins).
    /// The bytecode emitter uses this to emit OP_GET_LOCAL instructions
    /// for entry-block free variables that aren't defined in any IR block.
    std::unordered_map<VarId, ExternalVarRef> externalVars;

    /// Debug: map from VarId to human-readable name (populated during lowering).
    std::unordered_map<VarId, std::string> varNames;

    /// Allocate a fresh VarId.
    VarId freshVar() { return nextVar++; }

    /// Allocate a fresh BlockId and append an empty block.
    /// Returns the BlockId (index) of the new block.
    BlockId freshBlock(PosIdx pos);

    /// Convenience: the entry block.
    IRBlock & entryBlock() { return blocks[0]; }
    const IRBlock & entryBlock() const { return blocks[0]; }
};


// ============================================================================
// IR utilities
// ============================================================================

/// Collect all VarIds directly referenced by an IRExpr (its operands).
/// Used by free variable analysis and the emitter's forward-reference detection.
void collectRefs(const IRExpr & expr, FreeVars & refs);


// ============================================================================
// AST -> IR lowering
// ============================================================================

/// Lower a Nix AST expression (which must have been through bindVars())
/// into an IRModule.
///
/// This performs:
///   1. Desugaring of all syntactic sugar (inherit, with, rec, etc.)
///   2. A-normalization (all sub-expressions bound to VarIds)
///   3. Free variable analysis (FreeVars computed for every Lambda/MkThunk)
///
/// The returned IRModule is self-contained and ready for optimization
/// passes or direct bytecode emission.
IRModule lower(EvalState & state, Expr * expr);

/// Per-phase timing breakdown filled in by lower() when
/// lowerPhaseTiming is non-null.  Microsecond resolution.
/// Used by the NIX_VM_COMPILE_PROFILE diagnostic.
struct LowerPhaseTiming
{
    uint64_t lowerCoreUs = 0;     ///< AST traversal + IR construction
    uint64_t freeVarsUs = 0;      ///< computeFreeVars()
    uint64_t strictnessUs = 0;    ///< runStrictnessPass() (only if enabled)
    uint64_t freeVarsRecomputeUs = 0; ///< Re-run after strictness pass

    /// Aggregated work units, useful for normalising the timings.
    uint64_t numBlocks = 0;
    uint64_t numBindings = 0;
    uint64_t numVarIds = 0;
    uint64_t numThunks = 0;       ///< IRMkThunk bindings
    uint64_t numLambdas = 0;      ///< IRLambda bindings
};

/// Optional per-call observer.  When non-null, lower() populates the
/// referenced struct with a phase-by-phase microsecond breakdown.
/// Pass nullptr to disable.
extern thread_local LowerPhaseTiming * lowerPhaseTiming;


// ============================================================================
// Free variable analysis (post-lowering fixup)
// ============================================================================

/// Recompute FreeVars for all IRLambda and IRMkThunk nodes in the module.
///
/// This is called:
///   (a) At the end of lower() to populate the initial FreeVars.
///   (b) After any optimization pass that may invalidate FreeVars
///       (e.g., inlining, dead code elimination).
///
/// Algorithm:
///   For each block, walk bindings forward collecting defined VarIds
///   into a "bound" set.  Any VarId referenced but not in the bound
///   set is free.  For blocks referenced by IRLambda/IRMkThunk, the
///   free set is stored back on the Lambda/MkThunk node.
///
///   Because blocks can reference other blocks (nested lambdas),
///   analysis proceeds bottom-up: leaf blocks first, then their
///   enclosing blocks.  A block's free vars = union of all referenced
///   VarIds not defined in that block, plus the free vars of any
///   sub-blocks (transitively).
void computeFreeVars(IRModule & module);


} // namespace nix::ir
