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
    // (4 was ThunkState::Bridge — retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
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

    /// Phase D (Stage 3) write-barrier metadata, 2026-05-21.  If the
    /// `cell` above points INTO a `Bindings::entries[i].value` slot,
    /// `cellContainer` is the owning `Bindings *`.  At OP_RETURN's
    /// cell-write, the write barrier appends `cellContainer` to the
    /// thread-local dirty-container list (see `v3/barrier.hh`) so
    /// the next scavenge walks the Bindings and forwards any
    /// nursery payload the cell-write installed.
    ///
    /// nullptr when:
    ///   - The cell is null (no in-place update planned), OR
    ///   - The cell is a standalone `Alloc::allocValue()` cell
    ///     that doesn't sit inside a Bindings.  In that case the
    ///     barrier uses the thread-local standalone-cell registry
    ///     (also in `v3/barrier.hh`) instead.
    ///
    /// Set at the same MAKE-thunk / publish sites that set `cell`;
    /// cleared alongside `cell` at OP_RETURN.
    Bindings * cellContainer;

    /// #558 Phase 1.5 (2026-05-12) Cell-Update Everywhere: separate
    /// heap-stable Value* used for IN-PROGRESS shape publishing during
    /// body execution.  Distinct from `cell` (which is the
    /// parent-entry-slot pointer for STG-8 in-place updates).
    ///
    /// Lifecycle:
    ///   - At MAKE_THUNK / allocThunkSuspended: allocated, initialized
    ///     to *shapeCell = Tag::Thunk(this).  The sentinel value
    ///     "this thunk has not published anything yet."
    ///   - During body execution, OP_ATTRS_REC_INIT (and friends)
    ///     update *shapeCell with the in-progress Bindings as the
    ///     body constructs them.
    ///   - At OP_RETURN: *shapeCell = retVal; shapeCell = nullptr
    ///     (read-once).
    ///
    /// forceValue's Black branch reads *shapeCell BEFORE consulting
    /// the partial-Bindings registry.  If shapeCell has been updated
    /// past the pre-body sentinel, return its contents — this is the
    /// precise per-thunk in-progress state, free of the cross-thunk
    /// pollution that publishToAllThunkFrames' registry-wide search
    /// introduces.
    ///
    /// Gated by NIX_V3_CELL_EVERYWHERE=1 for safe rollout.
    /// nullptr if not allocated (cell-everywhere off, or thunk not
    /// of a kind that benefits).
    Value * shapeCell;

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
        // (bridgeSrc retired; TW_VALUE_ERADICATION F4, 2026-06-02.)
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
    /// #669 follow-up: contextual binding name (set when this lambda
    /// originated from a `let foo = ...` / `{ foo = ...; }` binding).
    /// Empty for anonymous lambdas — `name` may still have an arg-name
    /// fallback for diagnostics, but `contextualName` only carries the
    /// real binding name.  Used by `printNixValueRich` to match TW's
    /// `«lambda <name>? @ pos»` exactly (TW omits the `<name>` slot
    /// unless `ExprLambda::name` was set by the parser via setName).
    std::string contextualName;
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

    /// #548c diagnostic (2026-05-10): bumped at every OP_MAKE_THUNK
    /// whose funcIdx points at this descriptor.  When dumped at
    /// process exit (atexit), allocCount reveals descriptors that
    /// are re-instantiated many times — i.e., `let x = E` bindings
    /// where the surrounding scope is entered many times despite
    /// `x` being conceptually a fix-point.  A high allocCount with
    /// high forceCount but allocCount > forceCount indicates a
    /// sharing failure: the binding is being re-instantiated more
    /// times than its results are used.  When allocCount ≈ forceCount
    /// AND both are huge, the surrounding scope is hot-looped (the
    /// real failure to investigate).
    ///
    /// `mutable` for the same reason as forceCount: only OP_MAKE_THUNK
    /// has a `LambdaDescriptor *` (not `const`); other sites see it
    /// as `const` so the field is statistical only.
    mutable uint64_t allocCount = 0;

    /// #583 (2026-05-15): bumped at every OP_CALL whose callee is a
    /// Closure pointing at this descriptor.  Reveals which lambdas are
    /// repeatedly invoked — the hello.name re-evaluation loop suspects
    /// matchAttrs/matchAnyAttrs/elaborate's `final` builder.  Statistical
    /// only; gated behind V3_DBG_ALLOC_DUMP for zero cost otherwise.
    mutable uint64_t callCount = 0;

    /// IR Phase D (2026-05-18): closure-free lambda lifting.
    ///
    /// When `nUpvalues == 0` AND `nWithTargets == 0`, every
    /// OP_MAKE_CLOSURE invocation for this descriptor produces a
    /// semantically identical Closure: same `desc`, same `cu`,
    /// `nUpvalues=0`, no upvalues, and `capturedWiths=nullptr` (the
    /// body has no lexically captured `with` to look up — guaranteed
    /// by the lowerer's `ir::Lambda::lexicalWiths.empty()` check).
    ///
    /// We intern by caching the FIRST allocated Closure here; every
    /// subsequent OP_MAKE_CLOSURE returns the cached pointer.  Skips
    /// one `Alloc::allocClosure(0)` + `snapshotCurrentWiths(vm)`
    /// per creation site, which adds up under loop-heavy patterns
    /// like `map (x: x * 2) ...` or `foldl' (a: b: a + b) ...` where
    /// the inner lambda's outer captureless layer re-allocates per
    /// element under broken sharing.
    ///
    /// `mutable` for the same reason as the other instrumentation
    /// fields below: OP_MAKE_CLOSURE sees `cu` as `const` so reaches
    /// the descriptor as `const &`; the cache slot is runtime state,
    /// not part of the descriptor's logical identity.  Single-threaded
    /// VM — no atomics needed.  Lifetime is bounded by the
    /// CompilationUnit: when the CU is dropped, the descriptor goes
    /// with it and the cached Closure becomes unreachable (Boehm GC
    /// collects it on the next sweep).
    ///
    /// Disabled via NIX_V3_NO_LAMBDA_LIFT=1 (A/B gate).
    mutable Closure * cachedSingletonClosure = nullptr;

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

    /// Phase 1.2 (2026-05-16): identity-lambda specialisation.  When
    /// `true`, the lambda body is exactly `x: x` -- the emit-time
    /// peephole detected the canonical 2-instruction body:
    ///   OP_GET_LOCAL[_FORCE] 0
    ///   OP_RETURN
    /// OP_CALL / callClosure / OP_FORCE's Tag::App apply step take a
    /// fast path: substitute the arg directly, skip the frame push.
    /// Critical for deep App spines like `id (id (id ... 0))` where
    /// every level otherwise pushes a frame and hits the
    /// kMaxCallDepth=5000 guard.  Also pays on nixpkgs callPackage
    /// chains where `let foo = bar: bar; in foo (foo ...)` patterns
    /// occur — the elaboration helpers in lib have many of these.
    /// Set in emit.cc next to the selectorSym detection block.
    bool identityLambda = false;

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
