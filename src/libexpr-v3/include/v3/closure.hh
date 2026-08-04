#pragma once

// Standard headers this header's declarations need directly.  libc++ (macOS)
// pulls these in transitively via other <...> includes, but libstdc++ (Linux
// GCC) correctly does not — so a Linux build fails with "std::string does not
// name a type" without them.  All no-ops where already included.
#include <cstddef>   // std::size_t
#include <cstdint>   // uint8_t..uint64_t, int8_t
#include <cstring>   // std::memcmp (FlatStr comparison)
#include <string>    // std::string (LambdaBuild::name / contextualName)
#include <string_view>  // FlatStr::operator string_view
#include <type_traits>  // std::is_trivially_copyable_v (LambdaDescriptor POD assert)
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
    uint16_t                 nUpvalues;
    uint16_t                 _pad;      // fakeClo magic marker (kFakeCloMagic)
    Value                    upvalues[]; // FAM: the captured upvalues
};

// The env-sharing / env-tuple interning path (a `Closure::upvalEnv` field that,
// when non-null, held the upvalues in a shared tenured Env instead of the inline
// FAM) was RETIRED 2026-08: interning was default-disabled and shared ~nothing on
// real workloads, so the field was provably always null.  Upvalues now always
// live in the inline FAM; the accessors below are the plain FAM reads.

inline Value closureUpvalue(const Closure * c, uint32_t i) noexcept
{
    return c->upvalues[i];
}
inline Value * closureUpvaluePtr(Closure * c, uint32_t i) noexcept
{
    return &c->upvalues[i];
}
/// const overload: diagnostic/trace readers hold a `const Closure *` and only
/// need a `const Value *` (e.g. dbgLogForceSite).  Mirrors the mutable variant.
inline const Value * closureUpvaluePtr(const Closure * c, uint32_t i) noexcept
{
    return &c->upvalues[i];
}

[[gnu::always_inline]] inline std::size_t closureScanSize(const Closure * c) noexcept
{
    return sizeof(Closure) + sizeof(Value) * c->nUpvalues;
}

[[gnu::always_inline]] inline std::size_t closureAllocatedSize(const Closure * c) noexcept
{
    return sizeof(Closure) + sizeof(Value) * c->nUpvalues;
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

// The `hasWithsSlot` byte is a FLAGS bitfield.  Bit 0 (THUNK_WITHS_SLOT) marks a
// reserved capturedWiths slot at tail[nUpvalues].
//
// (Bit 1 was THUNK_ENV_SHARED — env-sharing, where a thunk's upvalues lived in a
// shared tenured Env referenced via tail[0] instead of inline in the tail.  It
// was fed only by the env-tuple interning that was default-disabled and shared
// ~nothing on real workloads; RETIRED 2026-08 with the Closure::upvalEnv field.
// Upvalues now always live inline in tail[0..nUpvalues).  Bit 2 was
// THUNK_ENV_CAPTURE — the env-pointer-capture experiment KILLed at Gate C
// 2026-07-04.  Both are reusable after a schema bump — 19 already gates the disk
// cache.)
enum : uint8_t {
    THUNK_WITHS_SLOT = 1,
};
[[gnu::always_inline]] inline bool thunkHasWithsSlot(const Thunk * t) noexcept
{ return (t->hasWithsSlot & THUNK_WITHS_SLOT) != 0; }

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
    // The withs slot sits at tail[nUpvalues], past the inline upvalues.
    // (env-sharing, which relocated it to tail[1], retired 2026-08.)
    const std::size_t idx = t->nUpvalues;
    return *reinterpret_cast<ListVec * const *>(&t->tail[idx]);
}
[[gnu::always_inline]] inline void thunkSetCapturedWiths(Thunk * t, ListVec * w) noexcept
{
    const std::size_t idx = t->nUpvalues;
    *reinterpret_cast<ListVec **>(&t->tail[idx]) = w;
}

// ---------------------------------------------------------------------------
// WS5-B2 (D2b) — flat, self-relative view types embedded in the POD
// LambdaDescriptor so the whole `lambdas[]` block can be BORROWED in place
// from the AOT mmap (Shared_Clean across processes) instead of being
// deserialized into per-process std::string/std::vector heap.
//
// SELF-RELATIVE INVARIANT: `rel` is a byte offset from the ADDRESS OF THE VIEW
// MEMBER ITSELF to its data, which lives later in the SAME contiguous lambda
// block ([descriptors][formals][chars]).  So `data = (const char*)this + rel`.
// Because the offset is self-relative, it stays valid when the ENTIRE block is
// copied as one unit (the owned path) or borrowed from the mmap — but it is
// INVALID if a single LambdaDescriptor / view is copied in ISOLATION.  The VM
// only ever accesses descriptors by reference into the block (LambdaTable's
// operator[]/begin/end return `const LambdaDescriptor&`), never by value, so
// the invariant holds.  These types are trivially copyable (just two ints),
// which keeps LambdaDescriptor a POD the block can reinterpret_cast to.
// ---------------------------------------------------------------------------

/// Self-relative NUL-terminated string embedded in a POD descriptor.
/// `rel==0 && len==0` ⇒ empty.  All reads are branch-free.
struct FlatStr
{
    int32_t  rel = 0;    ///< byte offset from &this to the char data
    uint32_t len = 0;    ///< length in bytes (NUL not counted)
    const char * c_str() const noexcept
    { return len ? reinterpret_cast<const char *>(this) + rel : ""; }
    const char * data() const noexcept { return c_str(); }
    std::size_t  size()  const noexcept { return len; }
    std::size_t  capacity() const noexcept { return len; }  // accounting
    bool         empty() const noexcept { return len == 0; }
    operator std::string_view() const noexcept
    { return std::string_view(len ? reinterpret_cast<const char *>(this) + rel : "", len); }
    std::string  str() const { return std::string(std::string_view(*this)); }
    std::size_t  find(const char * s) const { return std::string_view(*this).find(s); }
    bool operator==(std::string_view o) const noexcept
    { return std::string_view(*this) == o; }
    bool operator!=(std::string_view o) const noexcept
    { return !(std::string_view(*this) == o); }
};

/// Self-relative fixed-stride Formal array embedded in a POD descriptor.
/// Read-only view; mutation happens only on the LambdaBuild staging form.
template<typename FormalT>
struct FlatArray
{
    int32_t  rel = 0;    ///< byte offset from &this to the first element
    uint32_t count = 0;
    const FormalT * data() const noexcept
    { return reinterpret_cast<const FormalT *>(
             reinterpret_cast<const char *>(this) + rel); }
    std::size_t size()     const noexcept { return count; }
    std::size_t capacity() const noexcept { return count; }
    bool        empty()    const noexcept { return count == 0; }
    const FormalT & operator[](std::size_t i) const noexcept { return data()[i]; }
    const FormalT * begin() const noexcept { return data(); }
    const FormalT * end()   const noexcept { return data() + count; }
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
    /// WS5-B2 (D2b): the formals live in the shared lambda block, borrowed in
    /// place from the AOT mmap.  `FlatArray` is a read-only, self-relative view
    /// (offset into the block); mutation happens only on the `LambdaBuild`
    /// staging form used by emit + owned deserialize.
    FlatArray<Formal> formals;
    /// WC-17.1 diagnostic name (lambda or rec-attrset attr name).
    /// Mirrors ir::Function::name; populated by compile().  Used by
    /// V3_DBG_OPCYCLE / disassembler dumps to map LambdaDescriptor
    /// pointers back to the original AST scope.  WS5-B2: FlatStr view into
    /// the shared block (was std::string).
    FlatStr name;
    /// #669 follow-up: contextual binding name (set when this lambda
    /// originated from a `let foo = ...` / `{ foo = ...; }` binding).
    /// Empty for anonymous lambdas — `name` may still have an arg-name
    /// fallback for diagnostics, but `contextualName` only carries the
    /// real binding name.  Used by `printNixValueRich` to match TW's
    /// `«lambda <name>? @ pos»` exactly (TW omits the `<name>` slot
    /// unless `ExprLambda::name` was set by the parser via setName).
    /// WS5-B2: FlatStr view into the shared block (was std::string).
    FlatStr contextualName;
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

    // WS5-B2 (D2b, 2026-07-16): the `void * astLambda` field (the original
    // `nix::ExprLambda *` for the retired v3ToTreeWalker formals bridge) was
    // REMOVED.  It has no readers left (the TW bridge was retired in the
    // TW_VALUE_ERADICATION work — the only remaining mention is a stale comment
    // in primops.cc), and a per-process host pointer cannot live in a
    // read-only, cross-process-shared descriptor block.  ir::Function still
    // carries its own `astLambda`; emit simply no longer copies it here.

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

static_assert(std::is_trivially_copyable_v<LambdaDescriptor>,
    "LambdaDescriptor must stay a POD so the lambda block can be borrowed "
    "(reinterpret_cast) in place from the AOT mmap — WS5-B2 (D2b).");
static_assert(alignof(LambdaDescriptor) <= 8,
    "lambda block is 8-aligned in the blob; descriptor alignment must fit.");

/// WS5-B2 (D2b) — heap-owning STAGING form of a LambdaDescriptor.  Used ONLY
/// by the two BUILD paths — emit (compile) and the owned deserialize (SQLite
/// disk cache) — to accumulate a descriptor's variable-length parts before
/// `LambdaTable::finalize()` packs them into the flat, self-relative block.
/// Mirrors LambdaDescriptor's fields but keeps `name`/`contextualName` as
/// std::string and `formals` as std::vector so they can be built/mutated
/// (remapped, re-sorted) freely.  Never lives past finalize().
struct LambdaBuild
{
    uint32_t codeOffset = 0;
    uint32_t prologueOffset = 0;
    uint16_t nUpvalues = 0;
    uint16_t nLocals = 0;
    uint8_t  arity = 0;
    uint8_t  hasFormals = 0;
    uint8_t  ellipsis = 0;
    uint16_t nWithTargets = 0;
    std::vector<LambdaDescriptor::Formal> formals;
    std::string name;
    std::string contextualName;
    uint32_t posHandle = 0;
    uint32_t selectorSym = 0;
    bool identityLambda = false;
    bool secondArgIdentityLambda = false;
    bool isFormalWrapper = false;
    bool isOrDefault = false;
    bool isInheritWrapper = false;
    LambdaDescriptor::Intrinsic intrinsicKind = LambdaDescriptor::Intrinsic::None;
    int8_t intrinsicVar0 = -1;
    int8_t intrinsicVar1 = -1;
    int8_t intrinsicVar2 = -1;
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
