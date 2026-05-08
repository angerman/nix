#pragma once
/// @file
/// v3 Closure / Thunk / Env representation.
///
/// Design (per doc/v3-design/v3-design.md §3.2-§3.5):
///
///   Closure {
///     LambdaDescriptor * desc;     // shared blueprint (code, formals, etc.)
///     Env *              withEnv;  // null when no enclosing `with`
///     Value              upvalues[FAM];
///   }
///
///   No carrier Env around upvalues.  Closures with nUpvalues == 0 and no
///   enclosing `with` are 16 bytes total — one alloc per closure.
///
///   Thunk {
///     State        state;          // Suspended / Blackhole / Evaluated / Native
///     ...
///   }
///
///   Env (only for `let` / `with` scopes — NOT for closure upvalues):
///     parent + values[FAM]
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"

#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>

namespace nix::v3 {

struct LambdaDescriptor;
struct PrimOp;

// ---------------------------------------------------------------------------
// Env: only for `let`/`with` scopes, NOT for closure upvalues.
// ---------------------------------------------------------------------------

/// Env is allocated by OP_ENTER_LET / OP_PUSH_WITH / OP_INHERIT_FROM_INIT.
/// Closures use Closure directly (FAM upvalues); thunks use Thunk directly.
/// This dramatically reduces the env-allocation count vs v2.
struct Env
{
    Env *  parent;        // outer scope, or nullptr at the base.
    bool   isWithEnv;     // true if values[0] holds a with-attrset.
    uint16_t nValues;     // size of FAM (slot count).
    Value  values[];      // FAM
};

// ---------------------------------------------------------------------------
// Closure
// ---------------------------------------------------------------------------

struct CompilationUnit;

struct Closure
{
    const LambdaDescriptor * desc;        // shared blueprint
    /// CompilationUnit owning desc + the bytecode it points into.
    /// Required for cross-CU calls (e.g., closures returned by
    /// `builtins.import` from another file).  When null, the dispatch
    /// loop uses the caller's CU — fine for intra-CU calls.
    const CompilationUnit *  cu;
    /// Snapshot of the `with`-stack visible at MAKE_CLOSURE.  null when
    /// no enclosing `with` is in scope at definition time.  When the
    /// closure is invoked, the dispatcher re-pushes these onto the
    /// runtime with-stack so OP_WITH_LOOKUP inside the body finds them.
    ListVec *                capturedWiths;
    uint16_t                 nUpvalues;
    uint16_t                 _pad;
    Value                    upvalues[]; // FAM
};

// ---------------------------------------------------------------------------
// Thunk
// ---------------------------------------------------------------------------

enum class ThunkState : uint8_t {
    Suspended = 0,
    Blackhole = 1,
    Evaluated = 2,
    Native    = 3,
    /// WC-10 (Option 1): a thunk that, when forced, calls back into
    /// tree-walker for a single nix::Value*, then bridges the
    /// already-forced result to a v3 Value via treeWalkerToV3.
    /// Used by the rec-attrset materialisation path: instead of
    /// eagerly bridging every rec entry's body up-front, we
    /// allocate a Bridge thunk per entry — only entries the v3
    /// thunk's body actually accesses pay the bridge cost.
    Bridge    = 4,
};

struct Thunk
{
    ThunkState state;
    uint8_t    _pad0;
    uint16_t   nUpvalues;   // for Suspended state
    /// Phase 13 instrumentation (was `_pad1`).  Counts Suspended →
    /// Blackhole transitions for *this* thunk instance.  Each force
    /// transitions the thunk once per its lifetime (Suspended →
    /// Blackhole → Evaluated, then OP_FORCE returns the cached
    /// value), so a value > 1 indicates the thunk was reset to
    /// Suspended by some control-flow path — a real memoization
    /// regression.  Only meaningfully populated when V3_DBG_FORCES
    /// is set (overhead is one int increment per force, so cheap
    /// enough to leave on, but we gate the per-descriptor map
    /// dumping behind the env var).
    uint32_t   forces;

    /// STG-8 (#498): heap-stable cell where this thunk Value was
    /// originally stored (typically `&Bindings::entries[i].value`).
    /// When the thunk's body completes via OP_RETURN, the result is
    /// written back to *cell, mirroring tree-walker's in-place
    /// `forceValue` cell update.  Sub-thunks holding a Tag::Slot to
    /// the same cell see the result via single-deref instead of
    /// needing thunk-chase, AND foreign-VM observers that captured
    /// a slot to the cell stop seeing the (possibly leaked) Black
    /// thunk after the body completes.
    ///
    /// nullptr for thunks NOT stored at a heap-stable cell (e.g.
    /// thunks on the value-stack, lazy primop args, captured upvals).
    /// Set at OP_ATTRS_REC_SET / OP_REC_SLOT_PUBLISH / equivalent
    /// storing sites.  Cleared (read-and-zero) at OP_RETURN so the
    /// write happens exactly once per cell-binding.
    Value * cell;

    union {
        // ThunkState::Suspended
        struct {
            // Stored as `LambdaDescriptor *` directly -- the prior
            // `ThunkDescriptor` placeholder type was only ever
            // reinterpret_cast back to LambdaDescriptor at every read
            // site.  Storing the real type kills ~10 reinterpret_casts.
            const LambdaDescriptor * desc;
            /// Same semantics as Closure::capturedWiths.
            ListVec * capturedWiths;
            /// Same semantics as Closure::cu.
            const CompilationUnit * cu;
        } suspended;
        // ThunkState::Evaluated — the cached value.
        Value evaluated;
        // ThunkState::Native — primop wrapper.
        struct {
            const PrimOp * fn;
            // args follow as FAM
        } native;
        // ThunkState::Bridge — pointer to a tree-walker nix::Value
        // that the OP_FORCE handler will forceValue + bridge on
        // access.  Stored as `void *` so closure.hh stays free of
        // nix:: includes; cast to nix::Value * at the use site.
        void * bridgeSrc;
    };

    // FAM: upvalues[nUpvalues] for Suspended; args[fn->arity] for Native.
    Value tail[];
};

// ---------------------------------------------------------------------------
// LambdaDescriptor (shared blueprint)
// ---------------------------------------------------------------------------

struct LambdaDescriptor
{
    uint32_t codeOffset;    // start of the body in CompilationUnit::code
    uint32_t prologueOffset; // for formals; same as codeOffset for simple lambdas
    uint16_t nUpvalues;     // count of upvalues this closure captures
    uint16_t nLocals;       // stack slots needed in the body's frame
    uint8_t  arity;         // 1 for simple `x: ...`; >1 for currying (later)
    uint8_t  hasFormals;    // 0 = simple arg, 1 = formals attrset
    uint8_t  ellipsis;      // formals with `...` accept extra args; otherwise reject

    /// #530 lexical-with chain — the count of with-target VarIds this
    /// closure / thunk captures into its `capturedWiths` ListVec at
    /// MAKE time.  Set by emit from the lowerer-populated
    /// ir::Lambda::lexicalWiths / ir::MkThunk::lexicalWiths /
    /// ir::LetRec::Entry::lexicalWiths chains.  OP_MAKE_CLOSURE /
    /// OP_MAKE_THUNK pop `nUpvalues + nWithTargets` values; the with-
    /// target block is consumed first (it sits BELOW the upvalue
    /// block on the stack — pushed first by the maker frame), then
    /// the upvalue block.
    uint16_t nWithTargets = 0;

    /// Formal parameters (`{a, b ? def}: body`).  Each entry is
    /// (name SymbolId, hasDefault, posHandle).  posHandle is an index
    /// into the global posSnapshotPool; 0 means unknown.  Used by
    /// `builtins.functionArgs` (the bool drives the result value, the
    /// pos feeds the per-attr side-table so `unsafeGetAttrPos` works).
    struct Formal {
        uint32_t name;
        bool     hasDefault;
        uint32_t pos;
    };
    std::vector<Formal> formals;
    /// WC-17.1 diagnostic name (lambda or rec-attrset attr name).
    /// Mirrors ir::Function::name; populated by compile().  Used by
    /// V3_DBG_OPCYCLE / disassembler dumps to map LambdaDescriptor
    /// pointers back to the original AST scope.
    std::string name;
    /// Source position handle for the lambda body (1-based posSnapshotPool
    /// index; 0 = unknown).  Mirrors ir::Function::posHandle so
    /// V3_DBG_FORCE_TRACE can print file:line:col per thunk-force,
    /// matching tree-walker's TW_DBG_FORCE format.
    uint32_t posHandle = 0;
    /// Phase 13 instrumentation: total Suspended → Blackhole
    /// transitions of any thunk whose `suspended.desc` points at
    /// this descriptor.  Bumped from OP_FORCE.  Marked `mutable`
    /// because OP_FORCE only sees a `const LambdaDescriptor *` —
    /// the field is statistical, not part of the descriptor's
    /// logical identity.  Single-threaded VM, no atomics needed.
    mutable uint64_t forceCount = 0;

    /// #424: selector lambda specialisation.  When non-zero, the
    /// lambda body is exactly `paramVar.<selectorSym>` -- the emit-
    /// time peephole detected the canonical bytecode shape:
    ///   OP_GET_LOCAL_FORCE 0
    ///   OP_ATTRS_SELECT [sym]
    ///   OP_RETURN
    /// OP_CALL takes a fast path on these: force arg, check attrset,
    /// project the field directly, push -- no frame allocation, no
    /// inner dispatch.  `Map (p: p.name) [...]` patterns are dominant
    /// in nixpkgs and now actually flow through v3 since #426.
    uint32_t selectorSym = 0;

    /// #495: native intrinsic kind.  When recognised at lower-time,
    /// the lambda's body matches a canonical Nix-stdlib pattern (lib.fix,
    /// lib.extends, lib.composeExtensions, ...) and OP_CALL dispatches
    /// to a v3-native implementation that evaluates the entire fix-
    /// point machinery in v3 -- no TW round-trips.  Eliminates the
    /// captured-env / with-stack mismatch that today blocks lambda-skip
    /// default-on for nixpkgs (project_493_step3d_with_stack memo).
    ///
    /// Detection is structural AST match in lower.cc lowerLambda;
    /// matchers are narrow (one canonical shape per kind), so a
    /// nixpkgs change to fix.nix that alters the shape silently
    /// falls through to the non-intrinsic v3 dispatch.  No
    /// correctness loss -- intrinsics are PURE optimization.
    enum class Intrinsic : uint8_t {
        None                  = 0,
        Fix                   = 1,  ///< fix = f: let x = f x; in x
        Extends               = 2,  ///< extends = overlay: f: (final: ...)
        ComposeExtensions     = 3,  ///< composeExtensions = f: g: final: prev: ...
        ComposeManyExtensions = 4,  ///< composeManyExtensions = lib.foldr ...
        /// STG-13a (#509/#510): innermost lambda of an `extends` chain,
        /// i.e. chain[2] = `final: let prev = f final; in prev // overlay
        /// final prev`.  Recognised when chain[0] (`overlay:`) is matched
        /// as Extends; lower.cc threads the marker via a deferred map so
        /// chain[2]'s ir::Function gets this kind set when it is lowered
        /// recursively.  Native dispatch in OP_CALL/callClosure executes
        /// the body without going through bytecode -- the call path that
        /// today bridges the recursive rattrs through TW and trips the
        /// STG-12 BlackholeError when arg chases to a Black v3 thunk.
        ExtendsBody           = 5,
        /// STG-13a (#509/#510): innermost lambda of a
        /// `composeExtensions` chain, i.e. chain[3] = `prev: <body>`
        /// where the body computes `f final prev // g final prev'` (with
        /// the intermediate `f final prev`/`prev // f final prev`
        /// bindings).  Same recognition + deferred-marker scheme as
        /// ExtendsBody.
        ComposeBody           = 6,
    };
    Intrinsic intrinsicKind = Intrinsic::None;

    /// STG-13b (#509/#511): for ExtendsBody / ComposeBody dispatch, the
    /// upvalue indices of the captured `f` / `overlay` / `g` / `final`
    /// vars (or -1 if unused).  Native dispatch reads these to load the
    /// right closure->upvalues[i] without name-matching at runtime.
    /// Populated by emit.cc after freeVars are finalised; ordered by the
    /// natural roles of each intrinsic:
    ///   ExtendsBody : intrinsicVar0 = overlay, intrinsicVar1 = f
    ///   ComposeBody : intrinsicVar0 = f, intrinsicVar1 = g,
    ///                 intrinsicVar2 = final
    /// (final is the runtime arg in both cases; prev is local.)
    int8_t intrinsicVar0 = -1;
    int8_t intrinsicVar1 = -1;
    int8_t intrinsicVar2 = -1;

    /// #493 / #484 follow-on: the original tree-walker `nix::ExprLambda *`
    /// this v3 LambdaDescriptor was lowered from, or nullptr if the
    /// lambda was synthesised internally (e.g., the per-formal default-
    /// expression thunks lowerLambda emits at line 717-789).
    ///
    /// Used by v3ToTreeWalker when bridging a v3 Closure-with-formals
    /// back to TW: instead of refusing (the pre-#493 behaviour, which
    /// triggered the by-name-overlay.nix:54 cascade -- see
    /// project_484_lambda_skip_formals memory), construct a real TW
    /// Tag::tLambda Value pointing at this `astLambda`.  TW's
    /// autoCallFunction can then introspect formals via `lambda.fun`;
    /// the actual call is intercepted by v3CallFunctionEntry which
    /// dispatches to v3's compiled body via the body_fid path.
    ///
    /// Forward-declared type pointer: not all callers include
    /// libnixexpr's ExprLambda definition.  Held as `void *` to avoid
    /// pulling the AST header into closure.hh; cast at use sites.
    void * astLambda = nullptr;
};

// `struct ThunkDescriptor` removed -- was a placeholder type only ever
// reinterpret_cast to LambdaDescriptor at use sites.  Thunk::suspended
// now stores `LambdaDescriptor *` directly.

} // namespace nix::v3
