#pragma once

// Standard headers this header's declarations need directly.  libc++ (macOS)
// pulls these in transitively via other <...> includes, but libstdc++ (Linux
// GCC) correctly does not — so a Linux build fails with "std::string does not
// name a type" without them.  All no-ops where already included.
#include <cstddef>   // std::size_t
#include <cstdint>   // uint8_t..uint64_t, int8_t
#include <string>    // std::string (LambdaDescriptor::name / contextualName)
#include <vector>    // std::vector
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
#include "v3/cu_registry.hh"  // WS5-D1: desc→CU reverse map (closureCU/thunkCU)

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
    /// P1b (2026-07-06): the per-closure `const CompilationUnit * cu` field was
    /// REMOVED — derived from desc->cu via closureCU(c), mirroring FP-2a's
    /// per-thunk cu removal (thunkCU).  A closure's CU is always its
    /// descriptor's owning CU (OP_MAKE_CLOSURE sets desc = &cu->lambdas[i], so
    /// the descriptor LIVES IN that cu's lambdas vector); OP_MAKE_CLOSURE +
    /// fakeClo set `desc->cu = cu` idempotently (always the authoritative
    /// value).  Shrinks the header 40B → 32B (8B × every closure — ~2.2M on M5).
    /// Snapshot of the `with`-stack visible at MAKE_CLOSURE.  null when
    /// no enclosing `with` is in scope at definition time.  When the
    /// closure is invoked, the dispatcher re-pushes these onto the
    /// runtime with-stack so OP_WITH_LOOKUP inside the body finds them.
    ListVec *                capturedWiths;
    /// Env-sharing: when non-null, the
    /// upvalues live in this shared (tenured) Env's values[] instead of the
    /// inline FAM below — multiple closures from the same capture-set share one
    /// Env, cutting the per-closure upvalue-copy alloc. GET_UPVALUE reads
    /// `upvalEnv->values[n]` when set, else `upvalues[n]`. The inline-FAM path
    /// remains available via NIX_V3_NO_ENV_SHARING / NIX_V3_ENV_SHARING=0.
    Env *                    upvalEnv;
    uint16_t                 nUpvalues;
    uint16_t                 _pad;
    Value                    upvalues[]; // FAM (unused when upvalEnv != null)
};

/// Env-sharing upvalue accessors: read upvalue `i` from the shared Env when one
/// was built (gate-on), else from the inline FAM.  Centralizes the null-check so
/// every reader is consistent; until a gate builds an Env (`upvalEnv` always
/// null) these are exactly the inline-FAM path (byte-identical).
inline Value closureUpvalue(const Closure * c, uint32_t i) noexcept
{
    return c->upvalEnv ? c->upvalEnv->values[i] : c->upvalues[i];
}
inline Value * closureUpvaluePtr(Closure * c, uint32_t i) noexcept
{
    return c->upvalEnv ? &c->upvalEnv->values[i] : &c->upvalues[i];
}
/// const overload: diagnostic/trace readers hold a `const Closure *` and only
/// need a `const Value *` (e.g. dbgLogForceSite).  Mirrors the mutable variant.
inline const Value * closureUpvaluePtr(const Closure * c, uint32_t i) noexcept
{
    return c->upvalEnv ? &c->upvalEnv->values[i] : &c->upvalues[i];
}

[[gnu::always_inline]] inline std::size_t closureScanSize(const Closure * c) noexcept
{
    return sizeof(Closure)
         + (c->upvalEnv ? 0 : sizeof(Value) * c->nUpvalues);
}

[[gnu::always_inline]] inline std::size_t closureAllocatedSize(const Closure * c) noexcept
{
    // Real env-shared closures are allocated with a zero-length FAM.  Fake
    // closures keep their pool bucket capacity in the FAM even when upvalEnv is
    // set, but that tail is semantically dead and must not be scanned for roots.
    const bool hasInlineStorage = !c->upvalEnv || c->_pad != 0;
    return sizeof(Closure)
         + (hasInlineStorage ? sizeof(Value) * c->nUpvalues : 0);
}

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
    /// FP-2b (2026-06-14): repurposed from `_pad0` (which had no readers).  1
    /// iff this thunk reserved a trailing capturedWiths slot at tail[nUpvalues]
    /// (a raw `ListVec*`, NOT a NaN-boxed Value).  Set at allocThunkSuspended
    /// from `willHaveWiths` (= the thunk WILL capture a non-null with-list),
    /// which is computed before alloc and EXACTLY predicts capturedWiths!=null.
    /// Only Suspended/Blackhole thunks carry the slot; read via thunkCapturedWiths.
    uint8_t    hasWithsSlot;
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

    // M-8 (CODEBASE_REVIEW_2026-06-11): the Phase D `Bindings * cellContainer`
    // field was REMOVED.  It cached the owning Bindings of `cell` purely so the
    // GC marker could precisely walk it and the nursery write-barrier could
    // dirty-mark it.  Both are now DERIVED on demand from `cell` via
    // Arena::findContainingCellStart (+ a Bindings type-check) at the marker
    // (mark_sweep walkThunk), and the OP_RETURN barrier tracks the single cell
    // write via the standalone-cell registry (cellWrite(cell, v, nullptr)).
    // Dropping it (with shapeCell) shrinks the Thunk header 56 B -> 40 B.

    // M-8 (CODEBASE_REVIEW_2026-06-11): the #558 Phase 1.5 "Cell-Update
    // Everywhere" `Value * shapeCell` field was REMOVED here.  It was an 8-byte
    // per-thunk slot used only by the NIX_V3_CELL_EVERYWHERE experiment, which
    // was DEFAULT-OFF since #558 Phase 1.5 (shapeCell stayed nullptr in every
    // production eval — allocThunkSuspended only allocated it under the gate).
    // Removing it (together with `cellContainer`, derived on demand) shrinks
    // the Thunk header 56 B -> 40 B, which — because Arena::alloc rounds to a
    // 16 B boundary — drops a consistent 16 B per thunk for ALL upvalue counts
    // (thunks were ~320 MB on HNE-class evals -> order 80 MB).  The VM-13
    // retirement criterion (delete the gate + publish) is hereby satisfied:
    // the experiment is abandoned, not flipped on.

    union {
        // ThunkState::Suspended
        struct {
            // Stored as `LambdaDescriptor *` directly -- the prior
            // `ThunkDescriptor` placeholder type was only ever
            // reinterpret_cast back to LambdaDescriptor at every read
            // site.  Storing the real type kills ~10 reinterpret_casts.
            const LambdaDescriptor * desc;
            // FP-2a (2026-06-14): the per-thunk `const CompilationUnit * cu`
            // field was REMOVED — derived from desc->cu via thunkCU(t).
            // FP-2b (2026-06-14): the per-thunk `ListVec * capturedWiths` field
            // was REMOVED from the header.  It is now stored in the FAM tail at
            // tail[nUpvalues] (a raw ListVec*) ONLY when the thunk actually
            // captures a non-null with-list (hasWithsSlot==1) — 74-97% of thunks
            // capture none and pay 0.  Accessed via thunkCapturedWiths(t) /
            // thunkSetCapturedWiths(t,w).  This empties the Suspended union arm
            // to just {desc} = 8 B, taking the header 32 B -> 24 B.
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

// FP-2 (2026-06-14): thunk-header shrink complete.  FP-2a removed `suspended.cu`
// (derived from desc->cu via thunkCU); FP-2b removed `suspended.capturedWiths`
// (relocated to the FAM tail at tail[nUpvalues], present only when
// hasWithsSlot==1).  The Suspended union arm is now just {desc} = 8 B; the union
// floors at sizeof(Value)==8 (the Evaluated arm), so the header is
// 8 (state/hasWithsSlot/nUpvalues/forces) + 8 (cell) + 8 (union) = 24 B (was 40).
// Arena's 16 B rounding then yields a clean −16 B per null-withs thunk for ALL
// upvalue counts (≈97% of M5's 17.1 M thunks; M5 arena peak is thunk-bound).
// This assert pins the layout so a stray field re-grows it visibly.  Every GC
// size computation MUST go through thunkScanSize() (below) so the optional withs
// slot is never dropped on evac copy.  See lode/MEMORY_FORWARD_PLAN_2026-06-14.md.
static_assert(sizeof(Thunk) == 24,
    "FP-2: Thunk header must be 24 B (state-word 8 + cell 8 + union 8). The "
    "optional capturedWiths lives at tail[nUpvalues] when hasWithsSlot==1.");

// env-sharing: the `hasWithsSlot` byte is repurposed as a
// FLAGS bitfield rather than adding a field — FP-2 keeps the header at 24 B, so
// the thunk-side Env reference must NOT grow it.  Bit 0 (THUNK_WITHS_SLOT) is the
// original capturedWiths-slot flag; bit 1 (THUNK_ENV_SHARED) marks env-sharing,
// where the thunk's upvalues live in a shared tenured Env (`tail[0]` holds the
// raw Env*) instead of inline in tail[0..nUpvalues).  When env-shared the tail is
// just [Env* @ tail[0]] + [capturedWiths @ tail[1] iff THUNK_WITHS_SLOT] — so the
// withs slot RELOCATES from tail[nUpvalues] to tail[1] (nUpvalues stays the
// LOGICAL upvalue count, read from the Env).  This lets the per-force fakeClo
// share the Env (fakeClo->upvalEnv = thunkUpvalEnv(t)) with NO upvalue copy.
enum : uint8_t {
    THUNK_WITHS_SLOT = 1,
    THUNK_ENV_SHARED = 2,
    // (bit 4 was THUNK_ENV_CAPTURE — the env-pointer-capture experiment,
    // KILLed at Gate C 2026-07-04 and deleted.  Retired, not reusable while
    // pre-deletion arenas could be replayed; safe to reuse after a schema
    // bump — 19 already gates the disk cache.)
};
[[gnu::always_inline]] inline bool thunkHasWithsSlot(const Thunk * t) noexcept
{ return (t->hasWithsSlot & THUNK_WITHS_SLOT) != 0; }
[[gnu::always_inline]] inline bool thunkEnvShared(const Thunk * t) noexcept
{ return (t->hasWithsSlot & THUNK_ENV_SHARED) != 0; }
/// Shared upvalue Env (env-sharing), or null on the default inline-tail path.
[[gnu::always_inline]] inline Env * thunkUpvalEnv(const Thunk * t) noexcept
{
    return thunkEnvShared(t)
        ? *reinterpret_cast<Env * const *>(&t->tail[0])
        : nullptr;
}

// FP-2b SINGLE SOURCE OF TRUTH for a thunk's scanned/copied byte size.  EVERY GC
// size computation (evac copy in Cheney/scavenge, line-marking, byte accounting)
// MUST use this — if any under-counts, the evac copy drops the trailing withs
// slot and the forwarded thunk reads a stale pointer = use-after-free.  Mirrors
// the prior per-state logic (Suspended/Blackhole/Native carry the FAM tail;
// Evaluated's union holds a Value with no live tail) and adds the 8 B withs slot
// for Suspended/Blackhole when hasWithsSlot==1.
[[gnu::always_inline]] inline std::size_t thunkScanSize(const Thunk * t) noexcept
{
    switch (t->state) {
    case ThunkState::Suspended:
    case ThunkState::Blackhole:
        // env-sharing: tail is [Env* @ 0] + [withs @ 1 iff WITHS_SLOT] — fixed
        // 1-or-2 slots (upvalues live in the Env).
        if (thunkEnvShared(t))
            return sizeof(Thunk)
                 + sizeof(Value) * (1 + (thunkHasWithsSlot(t) ? 1 : 0));
        return sizeof(Thunk) + sizeof(Value) * t->nUpvalues
             + (thunkHasWithsSlot(t) ? sizeof(Value) : 0);
    case ThunkState::Native:
        return sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
    case ThunkState::Evaluated:
    default:
        return sizeof(Thunk);
    }
}

// FP-2b capturedWiths accessors.  The with-list lives at tail[nUpvalues] as a RAW
// ListVec* (not a NaN-boxed Value), present iff hasWithsSlot.  Read returns null
// when absent; the setter is valid only when the slot was reserved at alloc (its
// callers — OP_MAKE_THUNK and the GC evac forward — only write when present).
[[gnu::always_inline]] inline ListVec * thunkCapturedWiths(const Thunk * t) noexcept
{
    // Gate on state as well as hasWithsSlot: the bit is set at alloc and never
    // cleared on the Suspended→Evaluated transition, and an evac-copied
    // Evaluated thunk is relocated as sizeof(Thunk) (tail dropped) — so reading
    // tail[nUpvalues] would be out of bounds.  Only Suspended/Blackhole carry a
    // live slot.  (All real callers are already in those states; this is
    // defence-in-depth in a UAF-prone area, same cache line, zero behaviour change.)
    if (!thunkHasWithsSlot(t)
        || !(t->state == ThunkState::Suspended
             || t->state == ThunkState::Blackhole))
        return nullptr;
    // env-sharing relocates the withs slot to tail[1] (tail[0] is the Env*);
    // the default inline-tail path keeps it at tail[nUpvalues].
    const std::size_t idx = thunkEnvShared(t) ? 1 : t->nUpvalues;
    return *reinterpret_cast<ListVec * const *>(&t->tail[idx]);
}
[[gnu::always_inline]] inline void thunkSetCapturedWiths(Thunk * t, ListVec * w) noexcept
{
    const std::size_t idx = thunkEnvShared(t) ? 1 : t->nUpvalues;
    *reinterpret_cast<ListVec **>(&t->tail[idx]) = w;
}

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
    // WS5-D1 (2026-07-16): the runtime-mutable per-descriptor fields
    // `forceCount`, `allocCount`, `callCount` (diagnostic counters) and
    // `cachedSingletonClosure` (IR Phase D closure-free lambda-lift interning
    // slot) were MOVED OUT of LambdaDescriptor into the per-process side array
    // `CompilationUnit::Runtime::lambdaState[funcId]` (bytecode.hh).  They are
    // runtime state, not part of the descriptor's logical (compiled) identity;
    // removing them lets the `lambdas[]` array become read-only-after-load so
    // its pages can be shared across processes (WS5-D2b).  Write sites all have
    // (cu, funcIdx) in hand; the two debug-gated sites that only hold `desc`
    // recover funcId via cuForDesc + pointer subtraction.  The lambda-lift
    // singleton's Closure* still lives at an address-stable side-array slot so
    // the singletonClosureRegistry GC walk is unchanged.

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

    /// MapAttrs/callClosure2 projection fast path.  True only for a simple
    /// arity-2 lambda whose body is exactly `name: value: value`.
    bool secondArgIdentityLambda = false;

    /// P2.1 step-0 measure (2026-07-02, TEMPORARY): mirrors
    /// ir::Function::isFormalWrapper — true for a per-formal wrapper thunk
    /// body.  Summed against total allocCount under NIX_VM_STATS to size the
    /// wrapper share of runtime thunk allocations (audit §4.1).  NOT
    /// serialized — the P2.1 measure runs cache-off (fresh emit).  Remove
    /// with the instrument once P2.1 is decided.
    // CACHE-COHERENCE-EXEMPT: isFormalWrapper is diagnostic-only and is NOT
    // part of the LambdaDescriptor serialize/deserialize round-trip
    // (serialize.cc untouched) — the on-disk cache format is unchanged, so no
    // kSchemaVersion bump is warranted; deserialized descriptors default it to
    // false, which is correct for the cache-off P2.1 measure.
    bool isFormalWrapper = false;

    /// P2.3 step-0 measure (2026-07-02, TEMPORARY): mirror
    /// ir::Function::isOrDefault / isInheritWrapper for the audit §4.3 thunk
    /// classes.  Same measure-only, cache-off discipline as isFormalWrapper.
    // CACHE-COHERENCE-EXEMPT: diagnostic-only, not serialized (same rationale
    // as isFormalWrapper above); deserialized descriptors default to false.
    bool isOrDefault = false;
    bool isInheritWrapper = false;

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

    // WS5-D1 (2026-07-16): the runtime-mutable owning-CU backpointer `cu` was
    // REMOVED from LambdaDescriptor (it was STAMPED at every closure/thunk
    // creation, dirtying the descriptor page and blocking cross-process sharing
    // of the `lambdas[]` array — WS5-D2b).  A descriptor always lives inside its
    // owning CU's `lambdas` vector, so the CU is recovered STRUCTURALLY from the
    // descriptor's address via the process-global interval registry in
    // cu_registry.hh (registered idempotently at the former stamp sites).  This
    // is byte-identical to the old field: for any descriptor a live closure /
    // thunk points at, its CU's interval has been registered.
};

/// A suspended thunk's owning CU.  WS5-D1: derived from its descriptor's address
/// via the desc→CU interval registry (was `t->suspended.desc->cu`).  Returns
/// nullptr when the thunk has no descriptor (no CU) — callers that previously
/// fell back to the executing frame's `cu` keep that `?: cu` fallback.
[[gnu::always_inline]] inline const CompilationUnit * thunkCU(const Thunk * t) noexcept
{
    return cuForDesc(t->suspended.desc);
}

/// A closure's owning CU.  WS5-D1: derived from its descriptor's address via the
/// desc→CU interval registry (was `c->desc->cu`).  Returns nullptr when the
/// closure has no descriptor; callers that fell back to the executing frame's
/// `cu` keep that `?: cu` fallback (closureCU(c) is null iff the old cu was
/// null, since the CU interval is registered at closure creation).
[[gnu::always_inline]] inline const CompilationUnit * closureCU(const Closure * c) noexcept
{
    return cuForDesc(c->desc);
}

// `struct ThunkDescriptor` removed -- was a placeholder type only ever
// reinterpret_cast to LambdaDescriptor at use sites.  Thunk::suspended
// now stores `LambdaDescriptor *` directly.

} // namespace nix::v3
