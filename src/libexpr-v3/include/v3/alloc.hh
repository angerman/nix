#pragma once
/// @file
/// v3 allocator: per-EvalState arena + simple refcount-free heap.
///
/// For the bring-up phase we use plain malloc/free under a thin wrapper.
/// The arena/refcount story is the architectural Phase A item — not yet
/// implemented; the wrapper exists so call-sites are stable when we
/// switch.
///
/// Heap-allocated runtime objects (besides Value):
///   - Closure  : LambdaDescriptor* + FAM upvalues
///   - Thunk    : state + descriptor + FAM upvalues / args
///   - Env      : parent + FAM values (let/with scopes)
///   - ListVec  : size + FAM Value elements
///   - Bindings : size + FAM (SymbolId, Value) pairs (sorted)
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/closure.hh"
#include "v3/nursery.hh"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <new>

// WC-13: optionally register arena blocks as Boehm GC roots so any
// raw `nix::Value *` (or other GC-managed pointer) stored inside a
// Bridge thunk's bridgeSrc field keeps the underlying object alive.
// Without this, the v3 arena is invisible to Boehm's mark phase and
// values pointed to only from there are reclaimed mid-evaluation.
//
// Pulled in only when NIX_USE_BOEHMGC is defined; otherwise the
// arena uses plain malloc and there's no GC to integrate with.
#include "nix/expr/config.hh"
#if NIX_USE_BOEHMGC
#  include <gc/gc.h>
#endif

// --------------------------------------------------------------------------
// #824 / A2 (2026-05-26) — V3_RELEASE compile flag.
//
// `-DV3_RELEASE=1` (set by meson option `v3_release`) strips always-on
// instrumentation from the v3 hot paths.  The two macros below are the
// uniform escape hatch used wherever an unconditional counter or
// diagnostic-side-table update fires from a hot path:
//
//   V3_STATS_BUMP(field, n)  — semantically `allocStats().field += (n)`,
//                              elided to `((void)0)` under V3_RELEASE.
//   V3_STATS_INC(field)      — semantically `++allocStats().field`,
//                              elided to `((void)0)` under V3_RELEASE.
//   V3_STATS_BLOCK { ... }   — multi-statement diagnostic side-effect
//                              that should disappear entirely under
//                              V3_RELEASE.  The braces produce a block;
//                              under V3_RELEASE the macro expands to
//                              `if (false)` which the compiler trivially
//                              DCE's.  Use for the 10-branch attrset-
//                              size cascade and the bindingsAllocSite
//                              callback in `allocBindings`.
//
// Env-gated diagnostic counters (those guarded by `getenv("NIX_VM_*")`
// or `V3_DBG_*`) do NOT use these macros — their cost-when-off is
// already a single cached-bool branch, and they remain compileable.
// See NEXT_STEPS_2026-05-25.md §1.0 A2 + §8.5 AR1 for the rationale.
//
// Readers of the counters (`run.cc` NIX_VM_STATS dump, `limits.cc`
// resource-error message) tolerate zeroed counters and produce
// honest-zero output under V3_RELEASE.
#ifdef V3_RELEASE
#  define V3_STATS_BUMP(field, n) ((void)(n))
#  define V3_STATS_INC(field)     ((void)0)
#  define V3_STATS_BLOCK          if (false)
#else
#  define V3_STATS_BUMP(field, n) (allocStats().field += (n))
#  define V3_STATS_INC(field)     (++allocStats().field)
#  define V3_STATS_BLOCK          if (true)
#endif

namespace nix::v3 {

using SymbolId = uint32_t;
constexpr SymbolId kInvalidSymbol = 0;

struct EvalState;

// ---------------------------------------------------------------------------
// ListVec — flat array of Values with a length prefix.
// ---------------------------------------------------------------------------

struct ListVec
{
    uint32_t size;
    uint32_t _pad;
    Value    elems[]; // FAM
};

// ---------------------------------------------------------------------------
// Bindings — sorted (SymbolId, Value) pairs with binary search.
//
// For the bring-up phase this is the only Bindings shape.  The v3 design doc
// envisages Empty/Single/Small/Sorted polymorphism, but a single Sorted form
// is correct and lets us defer the polymorphism work until the perf gap
// motivates it.
// ---------------------------------------------------------------------------

/// 32-bit AST position handle.  Mirrors `nix::PosIdx`'s underlying
/// representation — we re-export it as a plain uint32_t to avoid
/// pulling the libexpr header into the v3 inner core.  0 means "no
/// position info known".
using PosIdx32 = uint32_t;
constexpr PosIdx32 kNoPos = 0;

struct Bindings
{
    /// 2026-05-21 #752: PosIdx32 fits in what used to be Entry's
    /// implicit padding slot (between the 4-byte SymbolId at offset
    /// 0 and the 8-byte-aligned Value at offset 8).  sizeof(Entry)
    /// is unchanged at 24 B; the side-table-style attrPosTable
    /// that previously held ~14 M (Bindings*,SymbolId)->PosIdx32
    /// mappings on hello.drvPath (~ 719 MB of "elsewhere" RSS per
    /// #751 attribution) is no longer required for entries we
    /// allocate ourselves — `entry.pos` IS the position.  Default
    /// 0 means "no position info."
    struct Entry { SymbolId name; PosIdx32 pos; Value value; };

    /// #823 / A1a Phase A (2026-05-26) — ChainBindings discriminator.
    ///
    /// Background.  HNE memory attribution (lode/HNE_MEMORY_
    /// ATTRIBUTION_2026-05-26.md) found `mergeBindings` =
    /// 584 MB / 82.9 % of v3-arena Bindings on the canonical haskell.
    /// nix-example workload, concentrated 98.3 % at site 1
    /// (OP_ATTRS_UPDATE_TAIL).  A pointer-keyed memo cache (A1b)
    /// hit 0.0 % — falsified per #822.  The only remaining lever is
    /// to AVOID materialising the merge: store
    /// `(parent_ptr, overlay_delta)` instead of copying the
    /// parent's entries on every merge.
    ///
    /// Layout.  When `kind == Sorted` (today's representation),
    /// `parent` is `nullptr`, `size` is the entry count, and
    /// `entries[]` is the full sorted attrset.  When `kind == Chain`,
    /// `parent` points to another Bindings (Sorted or Chain), `size`
    /// is the overlay-entry count, and `entries[]` is the sorted
    /// overlay (shadowing whatever names parent has at those keys).
    /// Chain bindings are NEVER stored on disk and NEVER serialised —
    /// they are pure runtime representation; `serialize.cc` materialises
    /// before emit if it ever sees a Chain (which it shouldn't, since
    /// CUs hold bytecode not Bindings).
    ///
    /// Header grew from 8 B → 16 B.  On hello.drvPath this is
    /// ~22 K Bindings × 8 B = +176 KB; on HNE ~1 M Bindings × 8 B =
    /// +8 MB.  Both negligible against the 200 MB - 1 GB Chain
    /// recovery target.
    ///
    /// This file (Phase A) defines the discriminator and chain-aware
    /// lookup ONLY.  All consumers still ALWAYS construct Sorted
    /// (mergeBindings unchanged; primops unchanged).  Phase B will
    /// add a chain-aware `forEach` helper and convert iteration
    /// sites.  Phase C enables Chain creation in mergeBindings under
    /// `NIX_V3_CHAIN_BINDINGS=1`.  Phase D promotes to default after
    /// the falsifier (≥ 200 MB recovered on HNE) is met.
    enum class Kind : uint8_t { Sorted = 0, Chain = 1 };

    uint8_t  kind = uint8_t(Kind::Sorted);   // offset 0
    uint8_t  _pad8[3] = {};                  // offset 1..3
    uint32_t size;                           // offset 4 — Sorted: count of entries[]
                                             //         — Chain:  count of overlay entries[]
    const Bindings * parent = nullptr;       // offset 8 — nullptr for Sorted
    Entry    entries[];                      // offset 16 — FAM, sorted ascending by name
                                             //          (overlay-only for Chain)

    bool isChain() const noexcept { return kind == uint8_t(Kind::Chain); }

    /// Binary search the entries array of `this` (does NOT walk parent).
    /// Internal helper used by `lookup` to factor the chain walk.
    const Value * lookupLocal(SymbolId name) const noexcept
    {
        uint32_t lo = 0, hi = size;
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            SymbolId midName = entries[mid].name;
            if (midName == name) return &entries[mid].value;
            if (midName < name) lo = mid + 1;
            else                hi = mid;
        }
        return nullptr;
    }

    /// Chain-aware binary-search lookup.  Walks overlay then parent.
    /// Returns nullptr if not found anywhere in the chain.  For
    /// Sorted Bindings (parent == nullptr), this is equivalent to
    /// the pre-#823 single-segment binary search.
    const Value * lookup(SymbolId name) const noexcept
    {
        for (const Bindings * b = this; b; b = b->parent) {
            const Value * v = b->lookupLocal(name);
            if (v) return v;
        }
        return nullptr;
    }

    /// Non-const overload — returns a writable pointer for callers that
    /// want to memoize lazy entries (e.g., resolving Tag::App in
    /// OP_ATTRS_SELECT_DYN and writing the WHNF result back into the
    /// slot).  Phase 13.3 mapAttrs memoization.
    ///
    /// Chain caveat: writes go to the OVERLAY entry if the name is
    /// present in overlay; otherwise we fall through to the parent.
    /// Returning a pointer into a shared parent's entry would let a
    /// caller mutate state visible to other chains rooted at the same
    /// parent — that's the same hazard the Phase 13.3 memo path
    /// already accepts on Sorted bindings (the merged result is
    /// shared, and writeback is idempotent).  The non-const overload
    /// preserves that contract.
    Value * lookup(SymbolId name) noexcept
    {
        for (Bindings * b = this; b; b = const_cast<Bindings *>(b->parent)) {
            uint32_t lo = 0, hi = b->size;
            while (lo < hi) {
                uint32_t mid = (lo + hi) >> 1;
                SymbolId midName = b->entries[mid].name;
                if (midName == name) return &b->entries[mid].value;
                if (midName < name) lo = mid + 1;
                else                hi = mid;
            }
        }
        return nullptr;
    }

    bool has(SymbolId name) const noexcept { return lookup(name) != nullptr; }

    // -----------------------------------------------------------------
    // #825 / A1a Phase B (2026-05-26) — chain-aware iteration helpers.
    //
    // These helpers let callers iterate a Bindings without caring
    // whether it's Sorted (today's representation) or Chain (Phase C
    // opt-in via NIX_V3_CHAIN_BINDINGS=1).  The two patterns:
    //
    //   * `forEach(func)` — calls `func(entry)` once per distinct name
    //     in the chain, in ascending name order.  Overlay shadows
    //     parent.  Today's implementation always materialises for
    //     Chain (correct but loses the memory benefit).  Phase C may
    //     refine to streaming merge for chain-depth == 1.
    //
    //   * `materialize()` — returns `this` if already Sorted; for
    //     Chain, walks the chain, dedups names (overlay wins), sorts,
    //     and returns a freshly allocated Sorted Bindings.  Callers
    //     that need indexed `entries[]` access (e.g. mutating writes,
    //     binary-search-by-index, or hot iteration over a known-Sorted
    //     result) call materialize() and then proceed unchanged.
    //
    //   * `isSorted()` / `chainDepth()` — diagnostics.
    //
    // Read-only iteration sites should prefer `forEach`.  Sites that
    // must mutate `entries[]` (write-back paths, OP_ATTRS_UPDATE
    // construction) should call `materialize()` first.
    bool isSorted() const noexcept { return kind == uint8_t(Kind::Sorted); }

    uint32_t chainDepth() const noexcept {
        uint32_t d = 0;
        for (const Bindings * b = this; b; b = b->parent) ++d;
        return d;
    }

    /// Number of unique names visible by walking the chain (overlay
    /// shadows parent).  For Sorted this is exactly `size`.  For Chain
    /// it is the total count after dedup.  O(N log N) on Chain because
    /// we sort + dedup; O(1) on Sorted.
    uint32_t totalSize() const noexcept;

    /// Materialise this Bindings.  Sorted: returns `this` unchanged.
    /// Chain: allocates a fresh Sorted Bindings containing every
    /// distinct (name, value) pair from the chain (overlay wins).
    /// Out-of-line; defined further down in this header so it can
    /// call `Alloc::allocBindings`.
    const Bindings * materialize() const;

    /// Walk the chain calling `func(const Entry &)` once per distinct
    /// name in ascending order.  Overlay shadows parent.  Today's
    /// implementation materialises first when `isChain()`; Phase C
    /// may refine to streaming merge for chain-depth == 1.
    template <typename F>
    void forEach(F && func) const {
        if (kind == uint8_t(Kind::Sorted)) {
            for (uint32_t i = 0; i < size; ++i) func(entries[i]);
            return;
        }
        const Bindings * m = materialize();
        for (uint32_t i = 0; i < m->size; ++i) func(m->entries[i]);
    }
};

// ---------------------------------------------------------------------------
// Allocation counters (defined before Alloc so allocBindings can record
// the size histogram inline).
// ---------------------------------------------------------------------------

struct AllocStats
{
    uint64_t valuesAllocated   = 0;
    uint64_t closuresAllocated = 0;
    uint64_t thunksAllocated   = 0;
    uint64_t envsAllocated     = 0;
    uint64_t listsAllocated    = 0;
    uint64_t attrsetsAllocated = 0;
    uint64_t pairsAllocated    = 0;

    /// #538 dispatch profiling: total bytecode instructions executed
    /// across all VMState instances in the process.  Bumped by
    /// `dispatchLoop` per opcode iteration when NIX_VM_STATS=1 enables
    /// the per-instruction counter.  Reported at atexit alongside
    /// alloc counters; lets us divide v3.run wall time by the
    /// instruction count to get nanoseconds-per-op (the dispatch
    /// loop's amortised cost).
    uint64_t bytecodeInstructions = 0;

    /// 2026-05-18 profiling: per-opcode dispatch counter.  Indexed by
    /// the `Op` enum value (uint8_t, 0..255).  Bumped at the same
    /// dispatch site as `bytecodeInstructions` but ONLY when
    /// NIX_VM_OPCOUNTS=1 — the per-op increment is one extra memory
    /// write per dispatch (a few percent overhead on tight loops).
    /// Dumped at process exit as a top-N table sorted by count.
    ///
    /// Workflow:
    ///   NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 v3-eval --file ... --strict
    /// Reports the top 20 hot opcodes; tells us which dispatch
    /// branches dominate (e.g. OP_FORCE vs OP_GET_LOCAL vs OP_CALL),
    /// driving where to focus VM-level optimisation work.
    uint64_t opcodeCounts[256] = {};

    /// #782 (2026-05-23) bigram (prev_op, current_op) counts.  Gated
    /// by NIX_VM_OPCOUNTS=1 with NIX_VM_BIGRAMS=1 to add the second
    /// counter increment (~1 ns extra dispatch when both are on).
    /// 256×256 × 8 B = 524 KB of zero-init memory; acceptable for
    /// diagnostic-only.  Reports top-N bigrams to identify
    /// super-instruction candidates before committing to #780
    /// register-VM rewrite.
    uint64_t bigramCounts[256][256] = {};

    /// #786 (2026-05-23) per-opcode cycle accumulator.  Gated by
    /// NIX_VM_OPCYCLES=1.  Records total CPU time spent dispatched
    /// in each opcode's case body, divided by its count, to give
    /// per-op cost in ns.  Used to verify or contradict the
    /// pre-implementation per-op-ns estimates that drove the #780
    /// and #783 estimate-based falsifiers (see PERF_AUDIT_2026-05-23
    /// review).  Per-dispatch overhead: 1 mach_absolute_time call
    /// = ~10 ns; tolerable under the gate, distorts but doesn't
    /// invalidate relative comparisons across opcodes.
    uint64_t opcycleNs[256] = {};

    /// #787 (2026-05-23) per-phase decomposition of OP_RETURN —
    /// 51 % of eval wall on hello.drvPath per #786 OPCYCLES.
    /// Gated by NIX_V3_DBG_RETURN_BREAKDOWN=1.  Three buckets:
    ///   prePopNs: from case entry (after pop retVal) through
    ///             frame-field capture + valueStack/withStack resize
    ///             + frames.pop_back.
    ///   thunkEvalNs: CFF_THUNK_RETURN branch — Evaluated chase,
    ///             self-cycle detection, cell update,
    ///             Phase D barrier propagation.
    ///   postEvalNs: from end of thunk branch (or skip if non-thunk)
    ///             through push retVal + tail-call cleanup + break.
    /// Plus thunkReturns / callReturns counters to derive per-phase
    /// averages for each return kind.
    uint64_t opReturnPrePopNs    = 0;
    uint64_t opReturnThunkEvalNs = 0;
    uint64_t opReturnPostEvalNs  = 0;
    uint64_t opReturnThunkCalls  = 0;
    uint64_t opReturnCallCalls   = 0;

    /// #783-measure (2026-05-23) refined bigram subcounters.  The
    /// bigramCounts above are pair-of-opcode counts; for fusion
    /// design we need to know what FRACTION are same-operand.
    /// Currently tracking only the top-1 bigram (SET_LOCAL ->
    /// GET_LOCAL) since that's the fusion candidate; if we widen
    /// later, add more counters.
    uint64_t bigramSetGetSameSlot = 0;

    /// Bindings allocation histogram by size.  Buckets:
    /// [0]=0, [1]=1, [2]=2, [3]=3-4, [4]=5-8, [5]=9-16, [6]=17-32,
    /// [7]=33-64, [8]=65-128, [9]=129+.  Used to size-tune the
    /// VM-2 polymorphic Bindings (Empty/Single/Small/Sorted) plan.
    uint64_t attrsetSizeBuckets[10] = {0,0,0,0,0,0,0,0,0,0};

    /// Phase 13 instrumentation: total Suspended → Blackhole
    /// transitions across the whole process.  Each thunk should
    /// transition at most once per lifetime, so this should be
    /// roughly equal to thunksAllocated under correct memoization;
    /// a 300x slowdown with 300x more transitions tells us we're
    /// allocating new thunks for what should be shared bindings.
    uint64_t thunksForced = 0;
    /// Bridge thunks (cross-evaluator value imports) — counted
    /// separately because they can legitimately be force-resolved
    /// once each per Bridge thunk allocated.
    uint64_t bridgeThunksForced = 0;

    /// #424: how many OP_CALL invocations took the selector-lambda
    /// fast path (frame-elision project of `arg.<sym>`).  Reported
    /// by V3_DUMP_LAMBDAS / NIX_VM_STATS so we can confirm the
    /// emit-time peephole is firing on real workloads.
    uint64_t selectorLambdaCalls = 0;

    /// #495: how many OP_CALL invocations dispatched to the v3-native
    /// `lib.fix` intrinsic (instead of running its bytecode body).
    /// Mirrors selectorLambdaCalls -- confirms that lower.cc's
    /// recogniseIntrinsic is firing AND the runtime dispatch is
    /// taking the fast path on real workloads.
    uint64_t intrinsicFixCalls = 0;

    /// STG-13c (#509/#512): native-dispatch counters for the inner
    /// `extends` / `composeExtensions` lambdas.  Each call replaces
    /// the bytecode body of `final: let prev = f final; in prev //
    /// overlay final prev` (or the 4-arg compose body) with a v3-side
    /// computation that calls f/overlay (or f/g) directly + merges the
    /// resulting attrsets via mergeBindings.  Eliminates the OP_CALL
    /// frames that today bridge to TW for the chain's leaf rattrs.
    uint64_t intrinsicExtendsCalls = 0;
    uint64_t intrinsicComposeCalls = 0;

    /// #821 (2026-05-26) per-caller attribution for `mergeBindings`.
    /// On HNE `.hello.drvPath` the function alone accounts for 584 MB
    /// of Bindings allocation (82.9 % of arena Bindings).  There are 9
    /// in-VM call sites; this array buckets bytes + calls by site so
    /// the per-site optimisation (ChainBindings / persistent overlay /
    /// caller-specific short-circuit) can target the dominant caller
    /// rather than re-architecting mergeBindings wholesale.
    ///
    /// Site IDs (see vm.cc enum MergeBindingsSite — exhaustive):
    ///   0  vm.cc:8333  OP_ATTRS_UPDATE      (`a // b`)
    ///   1  vm.cc:8405  OP_ATTRS_UPDATE_TAIL
    ///   2  vm.cc:4490  OP_CALL ExtendsBody:  prev // overlay
    ///   3  vm.cc:4538  OP_CALL ExtendsBody:  prev // overlay (2nd path)
    ///   4  vm.cc:4551  OP_CALL ComposeBody:  fApplied // overlay
    ///   5  vm.cc:12288 OP_TAIL_CALL ExtendsBody: prev // overlay
    ///   6  vm.cc:12318 OP_TAIL_CALL ExtendsBody: prev // overlay (2nd)
    ///   7  vm.cc:12329 OP_TAIL_CALL ComposeBody: fApplied // overlay
    ///   8  primops.cc primIntersectAttrs's two-pass merge
    /// Dumped under NIX_VM_STATS=1 alongside the per-alloc-site
    /// breakdown when totalCalls > 0.
    static constexpr uint8_t kMergeBindingsSiteSlots = 16;
    uint64_t mergeBindingsCallsBySite[kMergeBindingsSiteSlots] = {};
    uint64_t mergeBindingsBytesBySite[kMergeBindingsSiteSlots] = {};

    /// #821 — input size (nb) histogram for the dominant call site.
    /// To target a ChainBindings / persistent-overlay rewrite at the
    /// 98 %-dominant OP_ATTRS_UPDATE_TAIL (site 1 on HNE), we need to
    /// know whether the overlay (`b`) is small enough that
    /// `parent + overlay_delta` is cheaper than the current
    /// `parent ∪ overlay` materialisation.  Buckets:
    ///   [0]=1   [1]=2  [2]=3-4   [3]=5-8   [4]=9-16
    ///   [5]=17-32 [6]=33-64 [7]=65-128 [8]=129-256 [9]=257+
    uint64_t mergeBindingsNbHist[10] = {};
    /// Same buckets for parent (`a`) — together they tell us the
    /// (parent, overlay) size pair distribution.
    uint64_t mergeBindingsNaHist[10] = {};

    /// #702 / 2026-05-20: BYTES per allocation category.  Existing
    /// counts above were partly bumped by primop call sites
    /// (listsAllocated, attrsetsAllocated) and missed Alloc::*
    /// invocations from vm.cc dispatch, so they undercount.  These
    /// byte counters are bumped *inside* the Alloc::* functions
    /// (which are the chokepoint for every v3 allocation), so they
    /// are authoritative.
    ///
    /// Use case: hello.drvPath 4 GB RSS came from "somewhere outside
    /// Boehm" — these counters let us split the arena bytes by
    /// category and identify which subsystem owns the growth.
    ///
    /// Reported by NIX_VM_STATS=1 in run.cc.
    ///
    /// Retirement criterion: when Stage 3 (nursery default-on) lands
    /// and per-allocator telemetry moves into the nursery's own
    /// stats() API, these become redundant.  Until then they're the
    /// only honest byte counter v3 has.
    uint64_t bytesValues   = 0;
    uint64_t bytesClosures = 0;
    uint64_t bytesThunks   = 0;
    uint64_t bytesEnvs     = 0;
    uint64_t bytesLists    = 0;
    uint64_t bytesBindings = 0;
    uint64_t bytesPairs    = 0;
    uint64_t bytesChars    = 0;

    /// #736 (2026-05-21) IFD-probe per-kind counters.  Bumped from
    /// OP_IFD_PROBE dispatch (see vm.cc and IFD_DEEP_DIVE §5 / S5).
    /// Indexed by IfdProbeKind values 1..(kIfdProbeKindCount-1);
    /// slot 0 is unused (kIfdNone sentinel).
    ///
    /// Read by run.cc / v3-eval.cc NIX_VM_STATS summary.  When all
    /// entries are zero, the workload triggered no IFD-class primops
    /// — the production-default expectation.
    uint64_t ifdProbeCount[16] = {};

    /// #741 Phase 4 measurement spike (2026-05-23): per-kind counter
    /// for IFD-class primop calls whose path argument has NON-EMPTY
    /// NixStringContext.  This is the discriminator between literal-
    /// path imports (~all of hello.drvPath's 951 import calls — they
    /// import nixpkgs library files with empty context) and the
    /// derivation-output-path imports that haskell.nix /
    /// callCabalProjectToNix actually trigger.
    ///
    /// The "withCtx" count is a STRICT UPPER BOUND on real IFD events:
    ///   * Context types: Opaque (no build needed) / DrvDeep (build
    ///     dep closure) / Built (build a specific output).  Only the
    ///     latter two trigger builds; Opaque just references already-
    ///     realised paths.
    ///   * `realisePath` only fires builds for un-realised outputs;
    ///     if the output is already on disk, no build.
    ///
    /// Bumped from `primImport` / `primReadFile` / `primPathExists`
    /// when the path argument has `!lookupStringContextEntries(...)->
    /// empty()`.  If a workload's `withCtx_*` counts are zero, that
    /// workload has NO IFD events and Phase 4 cache wouldn't apply.
    /// If non-zero, those primop calls are Phase 4 cache candidates.
    uint64_t ifdProbeWithCtx[16] = {};

    /// #795 (2026-05-24): per-call-site counter for v3ToTreeWalker
    /// (the v3→TW bridge entry).  Each call site in primops.cc is
    /// assigned a numeric ID below.  When NIX_VM_STATS, the dump
    /// reports per-site counts so we can localize where v3 most
    /// often crosses to TW (and therefore where the V3-NATIVE
    /// elimination effort should focus).
    ///
    /// Site IDs (keep in sync with v3ToTreeWalker call-site comments):
    ///   0  primReadDir / primReadFile (string-with-ctx via realisePath)
    ///   1  primReadDir (attrset arg)
    ///   2  primImport (string-with-ctx)
    ///   3  primImport (attrset arg)
    ///   4  primPathExists (string-with-ctx) — line 2981 site
    ///   5  primReadFile (string-with-ctx) — line 3459 site
    ///   6  primDerivationStrict TW fallback
    ///   7  primV3CallBridge1 / primV3ForceAttr / primV3ForceListElem
    ///   8  primPath / fetch* / fetchFinalTree (FFI leaves)
    ///   9  v3ToTreeWalker eager bridge (small list/attrset structural)
    ///  10  primTrace (diagnostic)
    ///  11  primV3ForceAttr re-bridge inner (line 4491)
    ///  12  primV3ForceListElem re-bridge inner (line 4651)
    ///  13  reserved
    ///  14  reserved
    ///  15  other / unattributed
    uint64_t v3ToTwBySite[16] = {};
};

inline AllocStats & allocStats()
{
    static AllocStats stats;
    return stats;
}

// ---------------------------------------------------------------------------
// VM-3: bump-pointer arena allocator.
//
// All v3 runtime allocations (Bindings, Closure, Thunk, Env, ListVec,
// boxed Value) live for the entire process — `std::free` is never
// called on them — so per-allocation `malloc` is wasted work.  An
// 8 MB bump-pointer block, refilled on exhaustion, replaces it:
//   - amortised cost per allocation: 1 add + 1 compare + 1 store
//     (vs `malloc`'s lock + free-list walk + size class branch)
//   - tighter spatial locality: consecutive allocations end up
//     adjacent in memory
//   - oversized requests (> 1 MB) fall through to `malloc` so we
//     don't waste a fresh block on a single huge object
//
// Alignment: every allocation is 16-byte aligned (matches the
// largest field used inside the v3 runtime — `Value` is 16 B).
//
// Lifetime: the arena is per-thread (v3 is single-threaded) and
// blocks are released only at thread/process exit; we deliberately
// do NOT free individual objects.  This matches the existing
// `malloc`-and-leak strategy.
// ---------------------------------------------------------------------------

class Arena
{
public:
    /// 16 MB blocks: each block holds many thousands of typical
    /// allocations and a long-running eval doesn't accumulate too
    /// many block tails.  The 16 MB choice (audit §2.7 correction
    /// 2026-05-21: this comment block previously stated "1 MB"
    /// reflecting an older value — the constant has been 16 MB for
    /// a while) is driven by Boehm's `MAX_ROOTS` limit: each block is
    /// registered as its own root region via `GC_add_roots`, so a
    /// nixpkgs-scale eval (multi-GB arena) at 1 MB blocks produced
    /// thousands of root regions and exceeded Boehm 8.2.8's MAX_ROOTS
    /// default of 2048 (`Too many root sets`).  16 MB blocks drop
    /// the region count 16× and put full evals back under the cap.
    /// Cost: a 16 MB minimum first allocation per thread vs. 1 MB
    /// before — accepted because the arena is the hot path.
    static constexpr size_t kBlockSize = 16 * (1 << 20);
    /// Direct-`malloc` cutoff.  Anything bigger gets its own
    /// allocation rather than pinning down the rest of a fresh
    /// block.
    static constexpr size_t kHugeCutoff = kBlockSize / 4;

    void * alloc(size_t bytes) noexcept
    {
        // 16-byte align the request.
        bytes = (bytes + 15) & ~size_t{15};
        if (bytes > kHugeCutoff) {
            // Oversized: dedicated allocation outside the regular
            // block churn.  REVIEW CRIT-4 critic: register the block
            // with Boehm so any Value pointers stored inside it are
            // visible to the GC.  Pre-fix used std::malloc which left
            // the storage invisible; payloads inside (Closure*,
            // Bindings*, ...) were reachable only via the conservative
            // C-stack scan.  std::calloc zero-fills so stale bit
            // patterns don't pin objects.
            void * blk = std::calloc(1, bytes);
#if NIX_USE_BOEHMGC
            if (blk) GC_add_roots(blk, static_cast<char *>(blk) + bytes);
#endif
            // N11/R10 (audit Round 2): track huge allocations so
            // V3_DBG_NURSERY_BRUTE's scan covers them.  Without this,
            // a stale-pointer hit inside a huge Bindings (e.g. one
            // with >170K entries at nixpkgs scale) is invisible to
            // BRUTE — false-clean diagnostic.
            if (blk) {
                hugeBlocks.push_back({static_cast<char *>(blk),
                                       static_cast<char *>(blk) + bytes});
                totalBytes += bytes;
            }
            return blk;
        }
        if (cur + bytes > end) refill();
        void * p = cur;
        cur += bytes;
        return p;
    }

    /// Total bytes pinned by all blocks the arena has ever
    /// allocated.  Cheap to read; useful for the alloc-stats dump.
    size_t bytesAllocated() const noexcept { return totalBytes; }

    /// #705 diagnostic accessor: iterate the arena's blocks for
    /// brute-force scanning.  Returns (block_start, block_end_used).
    /// `cur` is the bump pointer in the active block — we only scan
    /// up to `cur` for that block, and the full block size for the
    /// older blocks.
    /// N11/R10 (audit Round 2): also returns the huge-allocation
    /// ranges so callers walk every byte the arena owns, not just
    /// the regular block churn.
    struct BlockRange { const char * begin; const char * end; };
    std::vector<BlockRange> blockRanges() const
    {
        std::vector<BlockRange> r;
        r.reserve(blocks.size() + hugeBlocks.size());
        for (size_t i = 0; i < blocks.size(); ++i) {
            const char * b = blocks[i];
            const char * e = (b == (cur ? blocks.back() : nullptr) && i + 1 == blocks.size())
                ? cur : b + kBlockSize;
            // Defensive: if cur is null (no allocations yet), use full block.
            if (!cur && i + 1 == blocks.size()) e = b + kBlockSize;
            r.push_back({b, e});
        }
        // Huge allocations: each is fully used (allocator does the
        // entire calloc'd region as one object), so begin..end is
        // the whole block.
        for (const auto & h : hugeBlocks) {
            r.push_back({h.begin, h.end});
        }
        return r;
    }

private:
    char *  cur        = nullptr;
    char *  end        = nullptr;
    /// Owning blocks; never freed in normal operation (they live
    /// for the lifetime of the thread).
    std::vector<char *> blocks;
    /// N11/R10 (audit Round 2): track oversized allocations
    /// (kHugeCutoff < bytes) so V3_DBG_NURSERY_BRUTE can scan them
    /// for stale nursery pointers.  Without this list, allocations
    /// > 4 MB (kHugeCutoff = kBlockSize / 4) bypass `blocks[]` and
    /// `blockRanges()` returns an incomplete view.
    struct HugeBlock { char * begin; char * end; };
    std::vector<HugeBlock> hugeBlocks;
    size_t  totalBytes = 0;

    void refill() noexcept
    {
        // Phase-13 review HIGH-6 fix: zero-fill the block before
        // GC_add_roots.  Boehm scans every word in the registered
        // region; OS-recycled garbage often contains pointer-shaped
        // bit patterns that pin Boehm-managed objects until process
        // exit (phantom retention scaling with arena lifetime).
        // calloc gives us a zero page directly from the kernel —
        // cheaper than malloc + memset for fresh allocations.
        char * blk = static_cast<char *>(std::calloc(1, kBlockSize));
        blocks.push_back(blk);
        cur = blk;
        end = blk + kBlockSize;
        totalBytes += kBlockSize;
#if NIX_USE_BOEHMGC
        // WC-13: tell Boehm to scan this block for pointers to GC
        // memory.  Bridge thunks store raw `nix::Value *`; without
        // this they become invisible to the collector and the values
        // they point to may be reclaimed mid-evaluation.
        // GC_add_roots is idempotent over overlapping regions and
        // safe to call concurrently — the underlying mutex is held
        // for a short string of pointer arithmetic.
        GC_add_roots(blk, blk + kBlockSize);
#endif
    }
};

inline Arena & threadArena() noexcept
{
    thread_local Arena a;
    return a;
}

// ---------------------------------------------------------------------------
// Allocator surface
// ---------------------------------------------------------------------------

// Forward declaration — defined further down, after the BindingsOrigin
// table.  Allocators in `Alloc` call this to record the caller's source
// file:line when NIX_V3_DBG_BINDINGS_ORIGIN=1.
struct Bindings;
void bindingsAllocSiteRecord(const Bindings * b, const char * file, uint32_t line) noexcept;

// T1.3 (2026-05-27) — forward declaration for the per-Thunk attribution
// table.  Called from `Alloc::allocThunkSuspended` when
// NIX_V3_THUNKS_ATTR=1.  Templated from the BINDINGS_ATTR pattern.
struct Thunk;
void thunkAllocSiteRecord(const Thunk * t, const char * file,
                          uint32_t line, uint16_t nUp) noexcept;

// T1.3 (2026-05-27) — forward declaration for the per-Closure
// attribution table.  Called from `Alloc::allocClosure` +
// `allocClosureTenured` when NIX_V3_CLOSURES_ATTR=1.  Closures are
// dispersed across ~7 vm.cc sites (unlike Thunks; see
// T1_3_THUNKS_ATTR_2026-05-27.md), so per-site rollup is genuinely
// informative.
struct Closure;
void closureAllocSiteRecord(const Closure * c, const char * file,
                             uint32_t line, uint16_t nUp) noexcept;

// T1.3 (2026-05-27) — forward decls for Pair + ListVec attribution.
// Both have many allocation sites (>20 each across primops.cc +
// vm.cc), so per-site rollup is informative.
struct ValuePair;
void pairAllocSiteRecord(const ValuePair * p, const char * file,
                          uint32_t line) noexcept;

struct ListVec;
void listAllocSiteRecord(const ListVec * l, const char * file,
                          uint32_t line, uint32_t size) noexcept;

// #768a (2026-05-22): namespace-scope env-var cache for allocator-
// path debug gates whose call sites appear BEFORE the main detail::
// block further down in this header (the gates referenced by
// `recordBindingsOrigin` / `cellOwnTrack` / `cellTraceWrite` etc.
// live in the later block).  The cellEverywhere gate is consulted
// from `Alloc::allocThunkSuspended` — on every thunk allocation,
// ~661 K times on hello.drvPath.
namespace detail {
inline const bool g_cellEverywhere =
    std::getenv("NIX_V3_CELL_EVERYWHERE") != nullptr;
}

struct Alloc
{
    /// #548c (2026-05-10) Cheney nursery routing.  When the
    /// nursery is enabled (NIX_V3_NURSERY=1), short-lived
    /// allocations (Thunk / Closure / ListVec) try the nursery
    /// first and fall back to the tenured arena on overflow.
    /// Phase A: fall-back-only (no scavenge yet).
    /// Phase C: scavenge implemented in gc.cc — copies live
    /// nursery objects to tenured, rewrites pointers in roots and
    /// any walked tenured objects, then resets the nursery's bump
    /// pointer.  Phase E will flip default-on.  See
    /// `lode/CHENEY_NURSERY_DESIGN.md`.
    ///
    /// Cells (allocValue), pairs (allocPair), AND Bindings stay
    /// tenured by design — Bindings entries are pointed at by
    /// long-lived Tag::Slot captures and `Thunk::cell` write-back
    /// pointers; moving a Bindings would invalidate those.  Phase
    /// D will revisit if Bindings turns out to dominate nursery
    /// pressure.
    [[gnu::always_inline]]
    static void * nurseryOrArena(size_t bytes) noexcept
    {
        if (void * p = threadNursery().tryAlloc(bytes)) return p;
        return threadArena().alloc(bytes);
    }

    static Value * allocValue() noexcept
    {
        V3_STATS_BUMP(bytesValues, sizeof(Value));
        return static_cast<Value *>(threadArena().alloc(sizeof(Value)));
    }

    /// `file` / `line` default to the caller's site via `__builtin_FILE`
    /// + `__builtin_LINE`.  Used by T1.3 per-Closure attribution
    /// (NIX_V3_CLOSURES_ATTR=1).  Zero cost when the gate is off.
    /// Closures have ~7 distinct allocation sites on hot paths
    /// (OP_MAKE_CLOSURE general + singleton + multiple fakeClo paths
    /// in OP_FORCE / OP_TAIL_CALL primop wrappers), so per-site
    /// attribution is genuinely informative — unlike Thunks (single
    /// dominant site at OP_MAKE_THUNK per T1_3_THUNKS_ATTR_2026-05-27).
    static Closure * allocClosure(uint16_t nUpvalues,
                                   const char * file = __builtin_FILE(),
                                   uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
        V3_STATS_BUMP(bytesClosures, bytes);
        auto * c = static_cast<Closure *>(nurseryOrArena(bytes));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->capturedWiths = nullptr;
        c->cu = nullptr;
        closureAllocSiteRecord(c, file, line, nUpvalues);
        return c;
    }

    /// #705 (2026-05-20): tenured-only Closure allocator.
    ///
    /// Use this when the returned pointer will be stored in a
    /// long-lived tenured location that the scavenger DOES NOT walk.
    /// Putting such a pointer through `nurseryOrArena()` would be
    /// unsound: the scavenger would (correctly) reclaim the nursery
    /// memory, but the tenured holder would still hold the stale
    /// pointer.  Next deref → SIGSEGV.
    ///
    /// Concrete known case: `LambdaDescriptor::cachedSingletonClosure`
    /// is mutated in-place by `OP_MAKE_CLOSURE` to memoize a
    /// nUp==0 / nWiths==0 lambda's Closure.  The LambdaDescriptor
    /// lives in `cu->lambdas` (tenured) and is NOT a scavenge root.
    /// Routing the underlying Closure to the nursery caused SIGSEGV
    /// on hello.drvPath at the first scavenge.
    ///
    /// Audit: any future caller adding a tenured cache for
    /// Closure* / Thunk* / ListVec* MUST use a tenured-only
    /// allocator and add itself to this list:
    ///   - LambdaDescriptor::cachedSingletonClosure  (this fix)
    ///
    /// Safety: identical layout to `allocClosure`; only the alloc
    /// backend differs.  No nursery slack lost (the singleton path
    /// is rare).
    /// T1.3: same file/line attribution as `allocClosure`.
    static Closure * allocClosureTenured(uint16_t nUpvalues,
                                          const char * file = __builtin_FILE(),
                                          uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
        V3_STATS_BUMP(bytesClosures, bytes);
        auto * c = static_cast<Closure *>(threadArena().alloc(bytes));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->capturedWiths = nullptr;
        c->cu = nullptr;
        closureAllocSiteRecord(c, file, line, nUpvalues);
        return c;
    }

    /// Allocate a Suspended thunk with `nUpvalues` captured upvalues
    /// stored in the FAM tail.
    ///
    /// `file` / `line` default to the caller's site via `__builtin_FILE`
    /// + `__builtin_LINE`.  Used by T1.3 per-Thunk attribution
    /// (NIX_V3_THUNKS_ATTR=1).  Zero cost when the gate is off.
    static Thunk * allocThunkSuspended(uint16_t nUpvalues,
                                        const char * file = __builtin_FILE(),
                                        uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(Thunk) + sizeof(Value) * nUpvalues;
        V3_STATS_BUMP(bytesThunks, bytes);
        auto * t = static_cast<Thunk *>(nurseryOrArena(bytes));
        t->state = ThunkState::Suspended;
        t->nUpvalues = nUpvalues;
        t->forces = 0;
        t->cell = nullptr;
        t->cellContainer = nullptr;  // Phase D write-barrier metadata
        t->shapeCell = nullptr;
        // #558 Phase 1.5: pre-allocate shapeCell so the body can
        // publish in-progress state via *shapeCell, and forceValue
        // Black can read it.  Gated NIX_V3_CELL_EVERYWHERE=1.
        //
        // #768a (2026-05-22): use the namespace-scope `inline const
        // bool` defined below so the per-thunk-alloc fast path skips
        // the magic-static guard-byte load.  allocThunkSuspended is
        // hit ~661 K times on hello.drvPath; even the marginal load
        // cost shows up in the i-cache.
        if (__builtin_expect(detail::g_cellEverywhere, 0)) {
            Value * sc = allocValue();
            // Sentinel: Tag::Thunk(t) — "this thunk has not yet
            // published in-progress state."  Readers compare against
            // (Tag::Thunk && ptr == t) to detect the sentinel.
            sc->tag_payload = static_cast<uint64_t>(Tag::Thunk);
            sc->payload.thunk = t;
            t->shapeCell = sc;
        }
        t->suspended.capturedWiths = nullptr;
        t->suspended.cu = nullptr;
        // T1.3: record allocation origin under NIX_V3_THUNKS_ATTR=1.
        // Forward declaration: thunkAllocSiteRecord is defined further
        // down in this header (needs <unordered_map>); the call site
        // here resolves to the inline noexcept body at compile time.
        thunkAllocSiteRecord(t, file, line, nUpvalues);
        return t;
    }

    /// WC-10: Allocate a Bridge thunk that, when OP_FORCE'd, calls
    /// back into tree-walker for the given nix::Value*.  Used by
    /// the rec-attrset materialisation: each entry of the
    /// synthesised Bindings* is one of these thunks, so only
    /// entries the v3 thunk body actually accesses pay the bridge
    /// cost.  `src` is a `nix::Value *` (cast to void* here so
    /// alloc.hh stays decoupled from nix:: types).
    static Thunk * allocBridgeThunk(void * src) noexcept
    {
        // No upvalues / no FAM tail.
        const size_t bytes = sizeof(Thunk);
        V3_STATS_BUMP(bytesThunks, bytes);
        auto * t = static_cast<Thunk *>(threadArena().alloc(bytes));
        t->state = ThunkState::Bridge;
        t->nUpvalues = 0;
        t->forces = 0;
        t->cell = nullptr;
        t->cellContainer = nullptr;  // Phase D write-barrier metadata
        // Bridge thunks don't have a v3-side body; no shapeCell needed.
        t->shapeCell = nullptr;
        t->bridgeSrc = src;
        return t;
    }

    static Env * allocEnv(uint16_t nValues) noexcept
    {
        const size_t bytes = sizeof(Env) + sizeof(Value) * nValues;
        V3_STATS_BUMP(bytesEnvs, bytes);
        auto * e = static_cast<Env *>(threadArena().alloc(bytes));
        e->parent = nullptr;
        e->isWithEnv = false;
        e->nValues = nValues;
        return e;
    }

    /// T1.3 (2026-05-27): file/line attribution via `__builtin_FILE` /
    /// `__builtin_LINE` default args.  NIX_V3_LISTS_ATTR=1 enables
    /// dump; zero cost when off.
    static ListVec * allocList(uint32_t n,
                                const char * file = __builtin_FILE(),
                                uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(ListVec) + sizeof(Value) * n;
        V3_STATS_BUMP(bytesLists, bytes);
        auto * l = static_cast<ListVec *>(nurseryOrArena(bytes));
        l->size = n;
        listAllocSiteRecord(l, file, line, n);
        return l;
    }

    /// REVIEW CRIT-3: ValuePair allocation routed through the arena
    /// instead of std::malloc.  Each ValuePair holds Value payloads
    /// with Boehm-managed pointers (Closure / Thunk / Bindings); the
    /// prior std::malloc'd storage was invisible to Boehm so the
    /// inner payloads could be reclaimed under load.  Arena-allocated
    /// pairs sit inside a GC_add_roots-registered region (alloc.hh:225).
    ///
    /// T1.3 (2026-05-27): file/line attribution.  NIX_V3_PAIRS_ATTR=1
    /// enables dump.  Pairs are 125 MB on HNE — third-largest non-
    /// Bindings bucket.
    static ValuePair * allocPair(const char * file = __builtin_FILE(),
                                  uint32_t     line = __builtin_LINE()) noexcept
    {
        V3_STATS_INC(pairsAllocated);
        V3_STATS_BUMP(bytesPairs, sizeof(ValuePair));
        auto * p = static_cast<ValuePair *>(
            threadArena().alloc(sizeof(ValuePair)));
        pairAllocSiteRecord(p, file, line);
        return p;
    }

    /// REVIEW CRIT-4: long-lived character buffer allocation routed
    /// through the arena instead of std::malloc.  Used for Tag::String
    /// / Tag::Path payloads built by primops and by string ops in the
    /// VM dispatch loop.  These buffers don't contain GC pointers
    /// directly, but std::malloc'd C strings leak (we never call
    /// std::free) and pollute heap profiling.  Arena allocation gives
    /// process-lifetime ownership identical to the pre-fix behaviour
    /// (no free), with allocation amortised to a single bump and the
    /// memory in a region Boehm scans for accidental Value pointers.
    ///
    /// Caller is responsible for null-terminating if a C string is
    /// expected (the caller already does buf[n] = '\0' in every
    /// existing call site -- this helper just replaces the std::malloc).
    static char * allocChars(size_t n) noexcept
    {
        V3_STATS_BUMP(bytesChars, n);
        return static_cast<char *>(threadArena().alloc(n));
    }

    // Phase A1 (RCA 2026-05-11): record the C++ source location of every
    // allocBindings call when NIX_V3_DBG_BINDINGS_ORIGIN=1.  Uses
    // __builtin_FILE / __builtin_LINE so the actual caller file:line is
    // captured without changing every call site.  Zero perf cost when
    // the env-var is off — both __builtin_FILE and __builtin_LINE are
    // compile-time constants embedded directly in the call.
    //
    // We don't store the full file:line at every binding (would explode
    // the side-table), but we DO use the file+line as a hash to a small
    // pool of "alloc-site" labels.  Callers that want a semantic label
    // (e.g. "primMapAttrs") still get explicit recordBindingsOrigin()
    // calls; this hook is the default-on fallback that makes EVERY
    // Bindings allocation tagged with where it came from.
    /// Empty-Bindings sentinel.  Returned by `allocBindings(0)` to
    /// avoid per-empty-attrset arena allocation (97 K allocs on
    /// hello.drvPath = 0.78 MB, plus per-alloc bookkeeping cost).
    /// Defined alongside `Value::vEmptyAttrs`' inner payload in
    /// `value.cc`'s anonymous namespace, but addressable via this
    /// extern so call sites read it directly.  Read-only after init.
    ///
    /// Safety: callers must never write to `entries[]` of an
    /// empty-Bindings.  All existing call sites either guard on
    /// `size > 0` before writing or use `lookup()` which returns
    /// nullptr immediately for an empty Bindings (so they don't
    /// touch entries[]).  Audited 2026-05-20.
    static Bindings * emptyBindingsSentinel() noexcept;

    // (Phase C `allocChainBindings` helper intentionally NOT
    //  added — Phase C is FALSIFIED across three pivots per
    //  vm.cc:1167-1206 in-code memo.  Adding a constructor here
    //  without callers is an "optimization carcass" anti-pattern
    //  per [[measure-twice-cut-once]] §3.7.  The Phase C revival
    //  prerequisites in the vm.cc comment are multi-session work;
    //  not in scope for this turn.)

    static Bindings * allocBindings(uint32_t n,
                                     const char * file = __builtin_FILE(),
                                     uint32_t     line = __builtin_LINE()) noexcept
    {
        // #703 (2026-05-20): route empty Bindings to a static
        // sentinel.  Track the histogram-bucket count for the dump
        // (so the size-0 stat still increments), record an
        // "alloc-site" if the env-var is on, then return the
        // shared sentinel — no arena allocation.
        if (n == 0) {
            V3_STATS_INC(attrsetSizeBuckets[0]);
            // Don't bump bytesBindings — the shared sentinel doesn't
            // grow the arena.  Don't record per-Bindings origin
            // either (the sentinel is reused, so a per-pointer
            // record would be a write race / stale label).
            return emptyBindingsSentinel();
        }
        const size_t bytes = sizeof(Bindings) + sizeof(Bindings::Entry) * n;
        V3_STATS_BUMP(bytesBindings, bytes);
        // Tenured by design (Phase C v1): Bindings entries[] hold
        // long-lived Tag::Slot targets and `Thunk::cell` write-back
        // pointers that must stay pointer-stable across nursery
        // scavenges.  Phase D may revisit if Bindings turns out to
        // dominate nursery pressure (then we'd need a remembered
        // set / cell registry).
        auto * b = static_cast<Bindings *>(threadArena().alloc(bytes));
        b->size = n;
        // Track size distribution for VM-2 sizing decisions.  Cheap
        // (one branch + one increment) — runs once per attrset.
        //
        // #824 / A2: the 10-branch cascade + the bindingsAllocSiteRecord
        // call below are both diagnostic-only.  Under V3_RELEASE the
        // entire side-effect block is DCE'd by the compiler via the
        // `if (false) { ... }` pattern in V3_STATS_BLOCK.
        V3_STATS_BLOCK {
            auto & buckets = allocStats().attrsetSizeBuckets;
            if      (n == 0)        buckets[0]++;
            else if (n == 1)        buckets[1]++;
            else if (n == 2)        buckets[2]++;
            else if (n <= 4)        buckets[3]++;
            else if (n <= 8)        buckets[4]++;
            else if (n <= 16)       buckets[5]++;
            else if (n <= 32)       buckets[6]++;
            else if (n <= 64)       buckets[7]++;
            else if (n <= 128)      buckets[8]++;
            else                    buckets[9]++;
            // Phase A1 default-recording (RCA 2026-05-11): tag every
            // Bindings allocation with its C++ caller file:line when
            // NIX_V3_DBG_BINDINGS_ORIGIN=1.  Routed through a forward-
            // declared free helper that's defined further down (it needs
            // <unordered_map> and the BindingsOrigin types, which appear
            // later in this header).  Zero cost when the env-var is off
            // (early-return inside), but the call+return itself isn't
            // free; eliding it under V3_RELEASE removes the call as well.
            bindingsAllocSiteRecord(b, file, line);
        }
        return b;
    }

    // -----------------------------------------------------------------
    // #558 Phase 4: fakeClo recycling pool.
    //
    // Each Suspended thunk force in vm.cc:OP_FORCE allocates a "fake"
    // Closure to carry the thunk's upvalues + capturedWiths + cu through
    // the body's frame.  Under THUNK_ALL on full nixpkgs, this fires
    // hundreds of millions of times — Boehm allocation + zeroing
    // dominates the per-force budget.  Recycling the fakeClo at
    // OP_RETURN reuses already-warm cache lines and skips the alloc
    // entirely.
    //
    // Buckets are indexed by nUpvalues (0..15); each bucket holds up
    // to kPoolPerBucket pointers.  Closures with nUp >= 16 are not
    // pooled (rare; would also blow up bucket count); they fall back
    // to plain allocClosure.
    //
    // SAFETY: pooled closures are always arena-backed (threadArena,
    // never nursery), so the pointer stays valid across scavenges.
    // We zero out the upvalues on recycle so the closure doesn't
    // pin stale GC references between uses.
    //
    // Gate: NIX_V3_NO_CLOSURE_POOL=1 reverts to plain allocClosure on
    // every force.
    static constexpr uint16_t kPoolMaxBuckets  = 16;
    static constexpr size_t   kPoolPerBucket   = 128;

    /// Sentinel value stamped into `Closure::_pad` by allocFakeClo and
    /// checked at recycleFakeClo.  Without this, a "real" closure
    /// produced by OP_MAKE_CLOSURE can end up in a CFF_THUNK_RETURN
    /// frame's `closure` field (e.g., via OP_TAIL_CALL replacement) and
    /// be incorrectly pooled, where a later allocFakeClo pops it and
    /// overwrites its desc — silently corrupting cell-stored
    /// Tag::Closure entries that still reference that pointer.
    /// 0xFA5E ("FASE", chosen for distinctness from 0/sentinel padding).
    static constexpr uint16_t kFakeCloMagic = 0xFA5E;

    /// Pop a recycled Closure of the requested size, or nullptr if no
    /// matching entry is pooled.  The returned closure has unspecified
    /// upvalues — the caller MUST overwrite all `nUpvalues` slots
    /// before any forceValue / dispatch sees it.
    static Closure * tryPopFakeClo(uint16_t nUpvalues) noexcept;

    /// Allocate a fakeClo, preferring the pool.  Always returns a
    /// closure with `c->nUpvalues == nUpvalues`; caller fills in the
    /// remaining fields (desc, cu, capturedWiths, upvalues).
    static Closure * allocFakeClo(uint16_t nUpvalues) noexcept;

    /// Return a fakeClo to the pool.  Caller must guarantee the
    /// closure is no longer referenced by any frame / Value /
    /// transitively.  Safe to call with nullptr or a closure that
    /// can't be pooled (nUp >= kPoolMaxBuckets or bucket full) —
    /// these become no-ops.
    static void recycleFakeClo(Closure * c) noexcept;
};

/// Thread-local closure pool storage.  Declared as a free function
/// (analogous to threadArena()) so the singleton is one per OS thread.
struct ClosurePool
{
    // Each bucket is a small fixed array used as a free-stack.  We
    // avoid std::vector here to keep the per-force fast path purely
    // pointer arithmetic — no heap allocations for the pool itself.
    Closure * slots[Alloc::kPoolMaxBuckets][Alloc::kPoolPerBucket] = {};
    uint16_t  count[Alloc::kPoolMaxBuckets] = {};
};

inline ClosurePool & threadClosurePool() noexcept
{
    thread_local ClosurePool pool;
    return pool;
}

inline Closure * Alloc::tryPopFakeClo(uint16_t nUpvalues) noexcept
{
    if (__builtin_expect(nUpvalues >= kPoolMaxBuckets, 0)) return nullptr;
    auto & pool = threadClosurePool();
    uint16_t n = pool.count[nUpvalues];
    if (n == 0) return nullptr;
    Closure * c = pool.slots[nUpvalues][n - 1];
    pool.count[nUpvalues] = n - 1;
    {
        static const bool s_dbg =
            std::getenv("V3_DBG_POP_FAKECLO") != nullptr;
        if (__builtin_expect(s_dbg, 0)) {
            std::fprintf(stderr,
                "v3 POP fakeClo=%p (prev desc=%p codeOff=%u nUp=%u)\n",
                (void *)c, (void *)c->desc,
                c->desc ? c->desc->codeOffset : 0,
                (unsigned)nUpvalues);
        }
    }
    return c;
}

inline Closure * Alloc::allocFakeClo(uint16_t nUpvalues) noexcept
{
    if (Closure * c = tryPopFakeClo(nUpvalues)) {
        // Pool hit — closure was previously stamped with the fakeClo
        // magic at allocFakeClo time, and the magic survived through
        // recycleFakeClo (which doesn't touch _pad).  Caller is about
        // to overwrite desc/cu/capturedWiths/upvalues; magic stays.
        return c;
    }
    // Pool miss: always arena (never nursery) so subsequent
    // recycle's pointer stability survives Cheney scavenges.
    const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
    V3_STATS_BUMP(bytesClosures, bytes);
    auto * c = static_cast<Closure *>(threadArena().alloc(bytes));
    c->nUpvalues = nUpvalues;
    c->_pad = kFakeCloMagic;   // Mark as fakeClo for safe pooling.
    c->capturedWiths = nullptr;
    c->cu = nullptr;
    return c;
}

inline void Alloc::recycleFakeClo(Closure * c) noexcept
{
    if (!c) return;
    // Phase A5 FIX (RCA 2026-05-11): only recycle when the closure
    // carries the fakeClo magic in _pad.  Real closures produced by
    // OP_MAKE_CLOSURE have _pad=0; recycling them would let allocFakeClo
    // return their pointer for a different thunk's force frame, where
    // `fakeClo->desc = newDesc` would silently corrupt cell-stored
    // Tag::Closure entries that still reference the address.
    if (c->_pad != kFakeCloMagic) {
        static const bool s_dbg =
            std::getenv("V3_DBG_RECYCLE_REJECT") != nullptr;
        if (__builtin_expect(s_dbg, 0)) {
            std::fprintf(stderr,
                "v3 RECYCLE REJECT (not a fakeClo): closure=%p _pad=0x%x "
                "desc=%p codeOff=%u nUp=%u\n",
                (void *)c, (unsigned)c->_pad,
                (void *)c->desc,
                c->desc ? c->desc->codeOffset : 0,
                (unsigned)c->nUpvalues);
        }
        return;
    }
    // Phase A5 RCA: diagnostic — log every recycle when env-var set.
    // This catches whether a closure with a "real" body (e.g., the
    // OP_MAKE_CLOSURE-allocated darwinArch closure at codeOff=2863)
    // ends up in the pool, which would let allocFakeClo overwrite its
    // desc and corrupt cell-resident closures.
    {
        static const bool s_dbg =
            std::getenv("V3_DBG_RECYCLE_FAKECLO") != nullptr;
        if (__builtin_expect(s_dbg, 0)) {
            std::fprintf(stderr,
                "v3 RECYCLE fakeClo=%p desc=%p codeOff=%u nUp=%u\n",
                (void *)c, (void *)c->desc,
                c->desc ? c->desc->codeOffset : 0,
                (unsigned)c->nUpvalues);
        }
    }
    const uint16_t nUp = c->nUpvalues;
    if (nUp >= kPoolMaxBuckets) return;
    auto & pool = threadClosurePool();
    uint16_t n = pool.count[nUp];
    if (n >= kPoolPerBucket) return;
    // Zero upvalues to avoid pinning stale GC references between uses.
    // N9 (audit Round 2): also zero capturedWiths.  desc / cu are
    // ALWAYS overwritten by the next user; capturedWiths was assumed
    // to be (caller writes it before any opcode runs that reads it),
    // but defensively zeroing here means a scavenge that sees a
    // pool-resident closure won't try to forward a stale ListVec*.
    // upvalues[] is FAM and Value payloads can contain Boehm pointers
    // — clearing avoids accidental retention through the pool itself
    // (which sits in arena memory GC_add_roots'd).
    for (uint16_t i = 0; i < nUp; ++i) c->upvalues[i] = Value{};
    c->capturedWiths = nullptr;
    pool.slots[nUp][n] = c;
    pool.count[nUp] = n + 1;
}

// ---------------------------------------------------------------------------
// Per-attr position.
//
// Tree-walker stores a PosIdx alongside every Bindings::Entry; v3
// matches that exactly by inlining a PosIdx32 into the Entry's pad
// slot (see Bindings::Entry above).  The prior side-table
// (Bindings*,SymbolId) -> PosIdx32 was retired in #752 after the
// #751 elsewhere-probe measured 14 M entries / ~719 MB on
// hello.drvPath.  Lookup is now O(log N) binary search reading
// `entries[mid].pos` (see lookupAttrPos below).
// ---------------------------------------------------------------------------

} // namespace nix::v3

#include <unordered_map>

namespace nix::v3 {

// ---------------------------------------------------------------------------
// #825 / A1a Phase B — out-of-line Bindings::materialize / totalSize.
//
// These need Alloc::allocBindings (defined above) and <algorithm>+<vector>
// (already pulled in via the header preamble), so we define them here
// rather than inline in the Bindings struct.  Both are O(1) on Sorted
// (the fast path), O(N log N) on Chain.  Hot iteration on Chain via
// forEach pays one materialize() per call; Phase C may refine to a
// streaming merge for chain-depth == 1.
// ---------------------------------------------------------------------------

inline uint32_t Bindings::totalSize() const noexcept
{
    if (kind == uint8_t(Kind::Sorted)) return size;
    // Chain: count distinct names by walking + dedup.  We accept the
    // O(N) walk because totalSize is rarely called outside diagnostic
    // dumps; primops that need the count (attrNames, length) call
    // forEach + count or materialise themselves.
    std::vector<SymbolId> names;
    uint32_t cap = 0;
    for (const Bindings * b = this; b; b = b->parent) cap += b->size;
    names.reserve(cap);
    for (const Bindings * b = this; b; b = b->parent) {
        for (uint32_t i = 0; i < b->size; ++i) names.push_back(b->entries[i].name);
    }
    std::sort(names.begin(), names.end());
    uint32_t distinct = 0;
    for (size_t i = 0; i < names.size();) {
        ++distinct;
        SymbolId n = names[i];
        while (i < names.size() && names[i] == n) ++i;
    }
    return distinct;
}

// `Bindings::materialize()` is defined out-of-line in `value.cc`.  The
// reason: the body needs `bindingsPostConstructBarrier(out)` from
// `barrier.hh` to mark the freshly-allocated result as dirty when any
// of its entries holds a nursery payload (the entries are copies from
// the chain, so they may hold pointers that the Phase D write
// barriers won't fire on at materialise time — the writes go through
// raw `entries[i] = uniq[i]` for speed, then we audit-scan once at
// the end).  But `barrier.hh` includes `alloc.hh`, so including
// `barrier.hh` here would be circular.  Moving the definition into
// `value.cc` (which is allowed to include both) breaks the cycle.

/// Read the per-attr position for entry `name` in Bindings `b`.
/// Returns 0 ("no position") when not found.  Reads directly from
/// `entry.pos` after binary-searching for the entry — the side-
/// table-style attrPosTable that this function used to consult was
/// retired in #752 once every recordAttrPos call site was converted
/// to write `b->entries[i].pos = ps` directly by index.
///
/// #825 / A1a Phase B caveat: this binary search assumes Sorted
/// representation.  For Chain Bindings, callers should materialise
/// first (or use `Bindings::lookup()` which is chain-aware, then
/// route through `entries[idx].pos` only on the materialised result).
inline uint32_t lookupAttrPos(const Bindings * b, SymbolId name)
{
    if (!b || b->size == 0) return 0;
    uint32_t lo = 0, hi = b->size;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        SymbolId midName = b->entries[mid].name;
        if (midName == name) return b->entries[mid].pos;
        if (midName < name) lo = mid + 1;
        else                hi = mid;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Bindings origin side-table (Phase A1, RCA 2026-05-11).
//
// Diagnostic-only side-table that maps `Bindings*` → "where was this attrset
// allocated".  Lets the cross-evaluator divergence harness answer the
// "where did THIS specific size-1 {family} attrset come from?" question
// that the STR_CONCAT failure surfaces.
//
// Gate: `NIX_V3_DBG_BINDINGS_ORIGIN=1`.  When unset, both record and lookup
// are a single cached-bool branch — zero perf cost on the hot path.
//
// The origin info has two parts:
//   - `posHandle` — index into `posSnapshotPool` (file:line:col).  Set when
//     the OP_ATTRS_INIT call site has a known source position; 0 otherwise.
//   - `source` — a string literal naming the alloc kind ("OP_ATTRS_INIT",
//     "OP_ATTRS_REC_INIT", "primMapAttrs", "treeWalkerToV3", etc.).  Always
//     non-null at record time; lookup returns nullptr for unrecorded ptrs.
//
// Lifetime: same as the per-attr position table — Bindings live process-
// long; entries are not freed.  Bounded by the count of allocated Bindings
// (~hundreds of thousands on full nixpkgs; ~MB of map storage).
// ---------------------------------------------------------------------------

struct BindingsOrigin
{
    uint32_t     posHandle;
    const char * source;
    /// 2026-05-21 diagnostic for #747: capture the `n` passed to
    /// allocBindings so dumpBindingsAttribution can compare
    /// alloc-time bytes vs dump-time bytes (b->size after any
    /// post-alloc mutation) per origin.  Bumped from
    /// `bindingsAllocSiteRecord` / `recordBindingsOrigin`.
    uint32_t     allocN;
};

// #768 (2026-05-22): namespace-scope `inline const bool` instead of
// function-local `static const bool` so the compiler can hoist the
// load and skip the magic-static guard byte the language requires
// for function-local statics.  Same pattern as #767a phaseDActive.
// `bindingsAllocSiteRecord` (called from EVERY `Alloc::allocBindings`,
// ~200 K calls/hello.drvPath) reads this on entry.
namespace detail {
// 2026-05-21 #746 spike: recording also auto-enables when the
// attribution rollup is requested, so users can ask for the dump
// with a single env var (NIX_V3_BINDINGS_ATTR=1) instead of two.
// Retirement criterion: when Bindings-attribution data has been
// captured and the next Bindings lever decision has landed, this
// OR'd second gate (and dumpBindingsAttribution) come out.
inline const bool g_bindingsOriginEnabled =
    std::getenv("NIX_V3_DBG_BINDINGS_ORIGIN") != nullptr
 || std::getenv("NIX_V3_BINDINGS_ATTR") != nullptr;

/// 2026-05-21 #746 spike gate: when set, dumpBindingsAttribution()
/// rolls up the bindingsOriginTable at run-exit and prints the
/// top-N construction sites by total bytes.
inline const bool g_bindingsAttrDumpEnabled =
    std::getenv("NIX_V3_BINDINGS_ATTR") != nullptr;
}

[[gnu::always_inline]] inline bool bindingsOriginEnabled() noexcept
{
    return detail::g_bindingsOriginEnabled;
}

[[gnu::always_inline]] inline bool bindingsAttrDumpEnabled() noexcept
{
    return detail::g_bindingsAttrDumpEnabled;
}

inline std::unordered_map<const Bindings *, BindingsOrigin> & bindingsOriginTable()
{
    static std::unordered_map<const Bindings *, BindingsOrigin> tbl;
    return tbl;
}

inline void recordBindingsOrigin(const Bindings * b, uint32_t pos, const char * src) noexcept
{
    if (!b || !bindingsOriginEnabled()) return;
    // Preserve allocN if already recorded (the implicit
    // bindingsAllocSiteRecord captures it first; explicit semantic
    // labels via recordBindingsOrigin should NOT clobber it).
    auto & tbl = bindingsOriginTable();
    auto it = tbl.find(b);
    uint32_t prevAllocN = (it != tbl.end()) ? it->second.allocN : (b ? b->size : 0u);
    tbl[b] = {pos, src, prevAllocN};
}

inline const BindingsOrigin * lookupBindingsOrigin(const Bindings * b)
{
    if (!b) return nullptr;
    auto & tbl = bindingsOriginTable();
    auto it = tbl.find(b);
    return it == tbl.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// T1.3 per-Thunk attribution table (2026-05-27).  Templated from #746
// BINDINGS_ATTR.  Tracks Thunk allocation origins so dump-time rollup
// can identify which call sites are responsible for the per-workload
// Thunk bytes (320 MB on HNE per HNE_BUCKET_DECOMP_2026-05-27).
//
// Gate: NIX_V3_THUNKS_ATTR=1 enables both recording AND end-of-run
// dump.  Default: off; zero cost when not enabled (one cached-bool
// read per allocThunkSuspended).
//
// Retirement criterion: when Thunk-attribution data has informed a
// concrete fix (analogous to #748/#750/#752 for Bindings), the gate
// + dump function can be retired.  Until then, this is the canonical
// per-Thunk-site instrumentation.
// ---------------------------------------------------------------------------

struct ThunkOrigin
{
    const char * file;
    uint32_t     line;
    uint32_t     nUpvalues;  // captured for per-site nUpvalues distribution
};

namespace detail {
inline const bool g_thunksAttrEnabled =
    std::getenv("NIX_V3_THUNKS_ATTR") != nullptr;
}

[[gnu::always_inline]] inline bool thunksAttrEnabled() noexcept
{
    return detail::g_thunksAttrEnabled;
}

inline std::unordered_map<const Thunk *, ThunkOrigin> & thunkOriginTable()
{
    static std::unordered_map<const Thunk *, ThunkOrigin> tbl;
    return tbl;
}

inline void thunkAllocSiteRecord(const Thunk * t, const char * file,
                                  uint32_t line, uint16_t nUp) noexcept
{
    if (!t || !thunksAttrEnabled()) return;
    auto & tbl = thunkOriginTable();
    tbl[t] = {file, line, nUp};
}

// ---------------------------------------------------------------------------
// T1.3 per-Closure attribution (2026-05-27).  Same shape as Thunks
// but Closures are dispersed across ~7 distinct vm.cc sites
// (OP_MAKE_CLOSURE general + singleton + various fakeClo paths
// in OP_FORCE / OP_TAIL_CALL bodies + intrinsic dispatch).  Per-site
// rollup distinguishes "user lambda creation" from "VM-internal
// fakeClo wrapping" — the latter is purely overhead.
// ---------------------------------------------------------------------------

struct ClosureOrigin
{
    const char * file;
    uint32_t     line;
    uint32_t     nUpvalues;
};

namespace detail {
inline const bool g_closuresAttrEnabled =
    std::getenv("NIX_V3_CLOSURES_ATTR") != nullptr;
}

[[gnu::always_inline]] inline bool closuresAttrEnabled() noexcept
{
    return detail::g_closuresAttrEnabled;
}

inline std::unordered_map<const Closure *, ClosureOrigin> & closureOriginTable()
{
    static std::unordered_map<const Closure *, ClosureOrigin> tbl;
    return tbl;
}

inline void closureAllocSiteRecord(const Closure * c, const char * file,
                                    uint32_t line, uint16_t nUp) noexcept
{
    if (!c || !closuresAttrEnabled()) return;
    auto & tbl = closureOriginTable();
    tbl[c] = {file, line, nUp};
}

// ---------------------------------------------------------------------------
// T1.3 per-ValuePair attribution (2026-05-27).  Same shape as Closures.
// Pairs are 125 MB on HNE; allocated by 4-5+ distinct sites including
// Tag::App memoization, primMap intermediate, primFilter, etc.
// ---------------------------------------------------------------------------

struct PairOrigin
{
    const char * file;
    uint32_t     line;
};

namespace detail {
inline const bool g_pairsAttrEnabled =
    std::getenv("NIX_V3_PAIRS_ATTR") != nullptr;
}

[[gnu::always_inline]] inline bool pairsAttrEnabled() noexcept
{
    return detail::g_pairsAttrEnabled;
}

inline std::unordered_map<const ValuePair *, PairOrigin> & pairOriginTable()
{
    static std::unordered_map<const ValuePair *, PairOrigin> tbl;
    return tbl;
}

inline void pairAllocSiteRecord(const ValuePair * p, const char * file,
                                 uint32_t line) noexcept
{
    if (!p || !pairsAttrEnabled()) return;
    auto & tbl = pairOriginTable();
    tbl[p] = {file, line};
}

// ---------------------------------------------------------------------------
// T1.3 per-ListVec attribution (2026-05-27).  Same shape; tracks `size`
// per allocation since lists have variable FAM tail length and the
// per-site size distribution matters.
// ---------------------------------------------------------------------------

struct ListOrigin
{
    const char * file;
    uint32_t     line;
    uint32_t     size;
};

namespace detail {
inline const bool g_listsAttrEnabled =
    std::getenv("NIX_V3_LISTS_ATTR") != nullptr;
}

[[gnu::always_inline]] inline bool listsAttrEnabled() noexcept
{
    return detail::g_listsAttrEnabled;
}

inline std::unordered_map<const ListVec *, ListOrigin> & listOriginTable()
{
    static std::unordered_map<const ListVec *, ListOrigin> tbl;
    return tbl;
}

inline void listAllocSiteRecord(const ListVec * l, const char * file,
                                 uint32_t line, uint32_t size) noexcept
{
    if (!l || !listsAttrEnabled()) return;
    auto & tbl = listOriginTable();
    tbl[l] = {file, line, size};
}

// ---------------------------------------------------------------------------
// Cell-ownership invariant tracker (RCA 2026-05-11, Phase A4a).
//
// v3 thunks carry an optional `cell : Value*` field that's used as a
// heap-stable update target.  When the thunk's body completes, the
// CFF_THUNK_RETURN handler writes the body's retVal to `*cell` and
// clears `t->cell = nullptr`.  This is v3's approximation of STG's
// `Ind` (indirection) closure.
//
// STG invariant: each (cell-bearing) thunk owns its cell — no two
// distinct thunks shall have the same `cell` pointer.  This invariant
// is implicit in the code: each S2/S3 setter site (cf. CELL_INVARIANTS.md)
// guards with `t->cell == nullptr` to prevent double-setting the SAME
// thunk, but does NOT protect against two DIFFERENT thunks pointing at
// the SAME storage.
//
// This tracker maintains a side-table `cellOwner: Value* → Thunk*` and
// fires on every cell-set / cell-write.  When a setter targets storage
// already owned by ANOTHER thunk, we log the invariant violation
// (NIX_V3_DBG_CELL_OWN=1 — log-only by default; NIX_V3_ASSERT_CELL_OWN=1
// to abort instead).
//
// Zero hot-path cost when the env-var is off.  When enabled, each cell
// op pays one hash-map lookup + one write.
// ---------------------------------------------------------------------------

struct Thunk;  // forward decl — defined in closure.hh

// #768: namespace-scope `inline const bool` — same rationale as
// bindingsOriginEnabled above.  `cellOwnTrack` / `checkSlot`
// (cell-set / cell-write callers) hit these on entry; promoting
// removes the magic-static guard byte from the per-call path.
namespace detail {
inline const bool g_cellOwnEnabled =
    std::getenv("NIX_V3_DBG_CELL_OWN") != nullptr;
inline const bool g_cellOwnAssertEnabled =
    std::getenv("NIX_V3_ASSERT_CELL_OWN") != nullptr;
}

[[gnu::always_inline]] inline bool cellOwnEnabled() noexcept
{
    return detail::g_cellOwnEnabled;
}

[[gnu::always_inline]] inline bool cellOwnAssertEnabled() noexcept
{
    return detail::g_cellOwnAssertEnabled;
}

inline std::unordered_map<const Value *, const Thunk *> & cellOwnerTable()
{
    static std::unordered_map<const Value *, const Thunk *> tbl;
    return tbl;
}

inline std::atomic<uint64_t> & cellOwnViolationCount()
{
    static std::atomic<uint64_t> count{0};
    return count;
}

/// Record a setter: `t->cell = storage` is about to happen.  If
/// `storage` already has a DIFFERENT owner, log the violation.
/// `source` is a string literal naming the setter site (e.g.
/// "OP_ATTRS_REC_SET", "OP_THUNK_SET_LOCAL_THROUGH_CELL").
void cellOwnRecordSet(const Value * storage, const Thunk * t,
                       const char * source) noexcept;

/// Record a writer: `*cell = ...; t->cell = nullptr` is about to
/// happen.  Verify that `storage` is indeed owned by `t`; clear the
/// ownership entry.
void cellOwnRecordWrite(const Value * storage, const Thunk * t,
                         const char * source) noexcept;

/// Cell-ownership tracker — see CELL_INVARIANTS.md (Phase A4a).
///
/// We use INLINE definitions to avoid a separate cell_invariants.cc.
/// Both `cellOwnRecordSet` and `cellOwnRecordWrite` are gated by the
/// cached env-var; the fast path is one branch + return.
inline void cellOwnRecordSet(const Value * storage, const Thunk * t,
                              const char * source) noexcept
{
    if (!storage || !t || !cellOwnEnabled()) return;
    auto & tbl = cellOwnerTable();
    auto it = tbl.find(storage);
    if (it != tbl.end() && it->second != t) {
        // Invariant I-CELL-1 violation: two different thunks point at
        // the same cell storage.  Log the offender + the original
        // owner so the divergence can be traced.
        ++cellOwnViolationCount();
        std::fprintf(stderr,
            "v3 CELL OWNERSHIP VIOLATION: storage=%p — was owned by "
            "thunk=%p, now being claimed by thunk=%p (setter=%s)\n",
            (const void *)storage,
            (const void *)it->second,
            (const void *)t,
            source ? source : "<?>");
        if (cellOwnAssertEnabled()) {
            std::fprintf(stderr,
                "v3 CELL OWNERSHIP: aborting (NIX_V3_ASSERT_CELL_OWN=1)\n");
            std::abort();
        }
    }
    tbl[storage] = t;
}

// Trace every cell write when NIX_V3_DBG_CELL_TRACE=1 — orthogonal to
// the I-CELL-1 ownership check.  Logs (storage, t, value-tag,
// for-attrs-the-key-set) for each `*cell = v` that fires.  Used to
// localize WHAT value lands at a given cell.
// #768: namespace-scope `inline const bool` — same rationale as
// the other alloc.hh debug gates.  Read at the entry of
// `cellTraceWrite`, which `cellSet` / cellSet variants call on
// every cell write.
namespace detail {
inline const bool g_cellTraceEnabled =
    std::getenv("NIX_V3_DBG_CELL_TRACE") != nullptr;
}

[[gnu::always_inline]] inline bool cellTraceEnabled() noexcept
{
    return detail::g_cellTraceEnabled;
}

// Minimal cell-write trace: prints (storage, thunk, tag, attrs-size,
// bindings-origin source@line if recorded).  Doesn't reach into the
// ir:: namespace (alloc.hh sits below ir.hh in include order); callers
// that want symbol-table annotation should call this AND then their
// own context-aware dump.
inline void cellTraceWrite(const Value * storage, const Thunk * t,
                            const Value & writtenValue,
                            const char * source) noexcept
{
    if (!storage || !cellTraceEnabled()) return;
    Tag tg = writtenValue.tag();
    std::fprintf(stderr,
        "v3 CELL WRITE storage=%p thunk=%p value-tag=%u source=%s",
        (const void *)storage, (const void *)t, (unsigned)tg,
        source ? source : "<?>");
    if (writtenValue.tag() == Tag::Attrs && writtenValue.payload.bindings) {
        auto * b = writtenValue.payload.bindings;
        std::fprintf(stderr, " attrs ptr=%p size=%u",
            (const void *)b, (unsigned)b->size);
        if (const BindingsOrigin * o = lookupBindingsOrigin(b)) {
            std::fprintf(stderr, " value-origin=%s",
                o->source ? o->source : "?");
        }
    } else if (writtenValue.tag() == Tag::String && writtenValue.payload.str) {
        std::fprintf(stderr, " str=\"%.40s\"", writtenValue.payload.str);
    } else if (writtenValue.tag() == Tag::Closure && writtenValue.payload.closure) {
        // Log closure pointer + desc codeOff so we can correlate with
        // later fakeClo allocations.  Phase A5 RCA: if the pooled
        // fakeClo pool returns a closure whose pointer matches a
        // cell-resident closure, the next OP_FORCE will overwrite
        // c->desc with the new thunk's desc — silently mutating the
        // cell-stored closure to the WRONG body.
        auto * c = writtenValue.payload.closure;
        std::fprintf(stderr, " closure-ptr=%p desc=%p",
            (const void *)c, (const void *)c->desc);
        if (c->desc) {
            std::fprintf(stderr, " codeOff=%u nUp=%u",
                c->desc->codeOffset, (unsigned)c->nUpvalues);
        }
    } else if (writtenValue.tag() == Tag::Thunk && writtenValue.payload.thunk) {
        std::fprintf(stderr, " thunk-ptr=%p state=%d",
            (const void *)writtenValue.payload.thunk,
            (int)writtenValue.payload.thunk->state);
    }
    std::fprintf(stderr, "\n");
}

inline void cellOwnRecordWrite(const Value * storage, const Thunk * t,
                                const char * source) noexcept
{
    if (!storage || !t || !cellOwnEnabled()) return;
    auto & tbl = cellOwnerTable();
    auto it = tbl.find(storage);
    if (it == tbl.end()) {
        // Writing to a cell with no recorded owner — could mean the
        // setter site is not instrumented, or a different process
        // already cleared the entry.  Log but don't assert.
        std::fprintf(stderr,
            "v3 CELL WRITE on UNOWNED storage=%p thunk=%p (writer=%s)\n",
            (const void *)storage, (const void *)t,
            source ? source : "<?>");
        return;
    }
    if (it->second != t) {
        // Invariant I-CELL-1 violation observed at write time: the
        // thunk that's writing isn't the recorded owner.  This
        // catches the case where the setter wasn't instrumented but
        // ownership conflict still happened.
        ++cellOwnViolationCount();
        std::fprintf(stderr,
            "v3 CELL WRITE OWNERSHIP MISMATCH: storage=%p owned by "
            "thunk=%p, write attempted by thunk=%p (writer=%s)\n",
            (const void *)storage,
            (const void *)it->second,
            (const void *)t,
            source ? source : "<?>");
        if (cellOwnAssertEnabled()) {
            std::fprintf(stderr,
                "v3 CELL OWNERSHIP: aborting (NIX_V3_ASSERT_CELL_OWN=1)\n");
            std::abort();
        }
    }
    tbl.erase(it);
}

// Default-record: tag a freshly-allocated Bindings with its caller's
// C++ source location.  Forward-declared near the top of this header
// so Alloc::allocBindings can call it.  No-op when
// NIX_V3_DBG_BINDINGS_ORIGIN is unset; interns label strings into a
// thread-local pool so the BindingsOrigin->source pointer is stable.
inline void bindingsAllocSiteRecord(const Bindings * b,
                                     const char * file,
                                     uint32_t line) noexcept
{
    if (!b || !bindingsOriginEnabled()) return;
    // Don't overwrite an explicit recordBindingsOrigin() call that
    // happened just before allocBindings returned — the explicit
    // semantic label wins.  (Per the recordBindingsOrigin contract:
    // last write wins.  In practice the explicit recorder is called
    // AFTER allocBindings, so this branch is a no-op for the explicit
    // case.  But guarding here means we don't fight ourselves.)
    if (lookupBindingsOrigin(b)) return;
    static thread_local std::unordered_map<uint64_t, const char *> labels;
    uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(file)) * 1024
                 + line;
    auto it = labels.find(key);
    const char * lbl;
    if (it != labels.end()) {
        lbl = it->second;
    } else {
        char buf[256];
        std::snprintf(buf, sizeof buf, "alloc@%s:%u",
            file ? file : "<?>", line);
        size_t len = std::strlen(buf) + 1;
        char * out = static_cast<char *>(std::malloc(len));
        std::memcpy(out, buf, len);
        lbl = out;
        labels[key] = lbl;
    }
    // recordBindingsOrigin preserves allocN if it was previously set;
    // since this is the FIRST record for `b`, capture the current
    // b->size (== n at alloc time, before any post-alloc mutation).
    bindingsOriginTable()[b] = {0, lbl, b->size};
}

// ---------------------------------------------------------------------------
// 2026-05-21 #746 spike — per-origin Bindings allocation rollup.
//
// Phase 1 of the post-Stage-4-v4.2 plan.  The dominant v3-arena
// consumer on hello.drvPath is Bindings (84% / 956 MB).  Until we
// know WHERE those Bindings come from we cannot pick between (a)
// persistent-map overlay sharing, (b) construction-site inlining,
// or (c) Bindings-shape polymorphism as the next lever.
//
// Mechanism: walk the existing bindingsOriginTable (which already
// captures __builtin_FILE/__builtin_LINE for every non-empty
// allocBindings call), aggregate by `source` label string,
// compute per-origin total bytes + per-size-bucket breakdown,
// sort by total bytes descending, print the top N.
//
// Gated by NIX_V3_BINDINGS_ATTR=1.  Because bindingsOriginEnabled()
// also fires on this env var, setting it alone is sufficient for
// both recording and dumping.
//
// Cost: when off, zero.  When on: each allocBindings pays one
// unordered_map insertion + one cached string-intern; the dump
// itself walks ~millions of entries once at exit.
//
// Retirement criterion: when the next Bindings lever decision has
// landed (and the associated Rule 0 falsifier-or-confirmer commit
// has measured the actual size impact), the spike and its env-var
// gate come out of the tree.
// ---------------------------------------------------------------------------

struct BindingsAttrRollupEntry
{
    const char * source;
    uint64_t     allocCount;
    uint64_t     totalBytes;       // dump-time: 8 + 24 * b->size summed
    uint64_t     totalAllocBytes;  // alloc-time: 8 + 24 * allocN summed
    uint64_t     sizeBuckets[10];  // matches attrsetSizeBuckets layout
};

inline void dumpBindingsAttribution(std::FILE * out, size_t topN = 20) noexcept
{
    if (!bindingsAttrDumpEnabled()) return;
    auto & tbl = bindingsOriginTable();
    if (tbl.empty()) {
        if (bindingsOriginEnabled()) {
            std::fprintf(out,
                "v3-direct bindings-attr: empty (recording is on, "
                "no Bindings allocated yet at this dump)\n");
        } else {
            std::fprintf(out,
                "v3-direct bindings-attr: empty (recording is OFF — "
                "NIX_V3_BINDINGS_ATTR / NIX_V3_DBG_BINDINGS_ORIGIN "
                "must be set BEFORE the recorded run)\n");
        }
        return;
    }
    // Aggregate by source label.  We use the label POINTER as the
    // map key (not the string contents): bindingsAllocSiteRecord's
    // intern pool ensures identical labels share a pointer, and
    // explicit recordBindingsOrigin call-sites pass C string
    // literals (also pointer-equal across calls).
    std::unordered_map<const char *, BindingsAttrRollupEntry> agg;
    agg.reserve(1024);
    uint64_t skippedNullSource = 0;
    for (const auto & kv : tbl) {
        const Bindings * b = kv.first;
        const BindingsOrigin & o = kv.second;
        if (!b) continue;
        if (!o.source) { ++skippedNullSource; continue; }
        auto & r = agg[o.source];
        r.source = o.source;
        ++r.allocCount;
        const uint64_t bytes = sizeof(Bindings)
                             + sizeof(Bindings::Entry) * b->size;
        r.totalBytes += bytes;
        const uint64_t allocBytes = sizeof(Bindings)
                                  + sizeof(Bindings::Entry) * o.allocN;
        r.totalAllocBytes += allocBytes;
        const uint32_t n = b->size;
        int bk;
        if      (n == 0)        bk = 0;
        else if (n == 1)        bk = 1;
        else if (n == 2)        bk = 2;
        else if (n <= 4)        bk = 3;
        else if (n <= 8)        bk = 4;
        else if (n <= 16)       bk = 5;
        else if (n <= 32)       bk = 6;
        else if (n <= 64)       bk = 7;
        else if (n <= 128)      bk = 8;
        else                    bk = 9;
        r.sizeBuckets[bk]++;
    }
    // Sort by totalBytes descending — the biggest arena consumers
    // first.  Stable across runs because we sort by bytes (a
    // deterministic function of the workload), not pointer identity.
    std::vector<BindingsAttrRollupEntry> sorted;
    sorted.reserve(agg.size());
    for (auto & kv : agg) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const BindingsAttrRollupEntry & a,
           const BindingsAttrRollupEntry & b) {
            // Sort by ALLOC bytes (what each origin actually drew
            // from the arena) — this is what reduces RSS, not the
            // dump-time live-entry count.
            if (a.totalAllocBytes != b.totalAllocBytes)
                return a.totalAllocBytes > b.totalAllocBytes;
            if (a.allocCount != b.allocCount)
                return a.allocCount > b.allocCount;
            return std::less<const char *>{}(a.source, b.source);
        });

    uint64_t grandBytes = 0, grandAllocs = 0, grandAllocBytes = 0;
    for (const auto & r : sorted) {
        grandBytes      += r.totalBytes;
        grandAllocBytes += r.totalAllocBytes;
        grandAllocs     += r.allocCount;
    }
    std::fprintf(out,
        "v3-direct bindings-attr: %zu distinct origins, "
        "%llu total allocs, dump=%.1f MB alloc=%.1f MB slack=%.1f MB "
        "(top %zu by alloc-bytes; skipped null-source=%llu):\n",
        sorted.size(),
        (unsigned long long)grandAllocs,
        grandBytes      / 1e6,
        grandAllocBytes / 1e6,
        (grandAllocBytes - grandBytes) / 1e6,
        std::min(topN, sorted.size()),
        (unsigned long long)skippedNullSource);
    // Header — left-aligned label up to 70 cols; tabular counts.
    std::fprintf(out,
        "  %-70s %10s %10s %10s %7s   %s\n",
        "origin", "allocs", "dump_MB", "alloc_MB", "slack%",
        "[0/1/2/3-4/5-8/9-16/17-32/33-64/65-128/129+]");
    const size_t lim = std::min(topN, sorted.size());
    for (size_t i = 0; i < lim; ++i) {
        const auto & r = sorted[i];
        // Truncate to 70 chars to keep output table-shaped.
        const char * src = r.source ? r.source : "<unknown>";
        const size_t slen = std::strlen(src);
        char label[72];
        if (slen <= 70) {
            std::snprintf(label, sizeof label, "%s", src);
        } else {
            // Keep the tail (file:line is usually at the end of
            // alloc-site labels) — that's the part we want to read.
            std::snprintf(label, sizeof label, "...%s", src + (slen - 67));
        }
        const double slackPct = r.totalAllocBytes > 0
            ? double(r.totalAllocBytes - r.totalBytes) * 100.0
              / double(r.totalAllocBytes)
            : 0.0;
        std::fprintf(out,
            "  %-70s %10llu %9.1fMB %9.1fMB %6.1f%%   "
            "[%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu]\n",
            label,
            (unsigned long long)r.allocCount,
            r.totalBytes      / 1e6,
            r.totalAllocBytes / 1e6,
            slackPct,
            (unsigned long long)r.sizeBuckets[0],
            (unsigned long long)r.sizeBuckets[1],
            (unsigned long long)r.sizeBuckets[2],
            (unsigned long long)r.sizeBuckets[3],
            (unsigned long long)r.sizeBuckets[4],
            (unsigned long long)r.sizeBuckets[5],
            (unsigned long long)r.sizeBuckets[6],
            (unsigned long long)r.sizeBuckets[7],
            (unsigned long long)r.sizeBuckets[8],
            (unsigned long long)r.sizeBuckets[9]);
    }
    // Trailing summary: how much of the total is captured by the
    // top-N?  Useful for "is this a long-tail or head-heavy?"
    // decisions when picking the lever.
    if (lim < sorted.size()) {
        uint64_t topAllocBytes = 0, topAllocs = 0;
        for (size_t i = 0; i < lim; ++i) {
            topAllocBytes += sorted[i].totalAllocBytes;
            topAllocs     += sorted[i].allocCount;
        }
        const double topPct = grandAllocBytes > 0
            ? double(topAllocBytes) * 100.0 / double(grandAllocBytes)
            : 0.0;
        std::fprintf(out,
            "  (top-%zu covers %.1f%% of alloc-bytes / %llu of %llu allocs; "
            "%zu more origins in the tail)\n",
            lim, topPct,
            (unsigned long long)topAllocs,
            (unsigned long long)grandAllocs,
            sorted.size() - lim);
    }
}

// Resolved AST source position: file path string, line, and column.
// The lowerer fills this snapshot pool from the EvalState's PosTable
// during compilation; runtime stores 1-based indices into this pool
// in the per-attr position side-table.  Pool slot 0 is reserved as
// "no position" so a 0 handle uniformly means "unknown".
struct PosSnapshot
{
    std::string file;
    uint32_t    line;
    uint32_t    column;
};

inline std::vector<PosSnapshot> & posSnapshotPool()
{
    static std::vector<PosSnapshot> pool = { PosSnapshot{} }; // index 0 = none
    return pool;
}

/// Dedup index for `recordPosSnapshot`.  Maps `{file, line, column}`
/// to the existing pool handle so repeated registrations of the same
/// source position return the SAME PosIdx — required for cross-
/// process determinism (Schema 14, R1 trigger fix 2026-05-26):
/// `serialize::deserializeCU` calls `recordPosSnapshot` for every
/// posTable entry, and the freshly-compiled bytecode in a later
/// `lower → optimise → compile` cycle does the same for the SAME
/// source.  Without dedup the two pathways assign different PosIdx
/// values; `V3_DBG_DESERIALIZE_VERIFY` would observe drift even
/// after the per-CU pos remap lands.
///
/// Key = `(file, line, column)`.  Custom hash uses XOR-combined
/// hashes of the three components; file is the largest contributor.
struct PosSnapshotKey {
    std::string file;
    uint32_t    line;
    uint32_t    column;
    bool operator==(const PosSnapshotKey & o) const noexcept {
        return line == o.line && column == o.column && file == o.file;
    }
};
struct PosSnapshotKeyHash {
    size_t operator()(const PosSnapshotKey & k) const noexcept {
        size_t h = std::hash<std::string>{}(k.file);
        h ^= std::hash<uint32_t>{}(k.line) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.column) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
inline std::unordered_map<PosSnapshotKey, uint32_t, PosSnapshotKeyHash> &
posSnapshotIndex()
{
    static std::unordered_map<PosSnapshotKey, uint32_t, PosSnapshotKeyHash> idx;
    return idx;
}

/// Push a snapshot into the pool and return its 1-based handle (0 = none).
/// Dedup-aware (#R1 trigger fix, 2026-05-26): repeated calls with the
/// same `(file, line, column)` return the SAME PosIdx.  Required for
/// the Schema 14 deserialise→remap→fresh-compile pipeline to converge.
///
/// HNE refresh follow-up (2026-05-26 evening): `NIX_V3_NO_POS_DEDUP=1`
/// disables the dedup hash lookup — caller always gets a fresh PosIdx.
/// This BREAKS Schema 14's R1-trigger convergence (cold-vs-warm
/// V3_DBG_DESERIALIZE_VERIFY would explode) but is safe for normal eval
/// (PosIdx is only consumed by diagnostic-position lookup).  Used to
/// isolate the dedup hash overhead's contribution to the HNE warm wall
/// regression measured at +1700 ms post-Schema-14.
inline uint32_t recordPosSnapshot(PosSnapshot s)
{
    static const bool s_noDedup =
        std::getenv("NIX_V3_NO_POS_DEDUP") != nullptr;
    if (s_noDedup) {
        auto & p = posSnapshotPool();
        p.push_back(std::move(s));
        return static_cast<uint32_t>(p.size() - 1);
    }
    PosSnapshotKey k{s.file, s.line, s.column};
    auto & idx = posSnapshotIndex();
    auto it = idx.find(k);
    if (it != idx.end()) return it->second;
    auto & p = posSnapshotPool();
    p.push_back(std::move(s));
    uint32_t h = static_cast<uint32_t>(p.size() - 1);
    idx.emplace(std::move(k), h);
    return h;
}

inline const PosSnapshot * resolvePosSnapshot(uint32_t handle)
{
    if (handle == 0) return nullptr;
    auto & p = posSnapshotPool();
    if (handle >= p.size()) return nullptr;
    return &p[handle];
}

// ---------------------------------------------------------------------------
// String-context side-table.
//
// v3 Tag::String values are plain `const char *` payloads — context
// info is kept in a separate map keyed by that pointer.  Entries use
// the tree-walker-style encoding: `<path>` (Opaque), `=<drvPath>`
// (DrvDeep), `!<output>!<drvPath>` (Built).
//
// Both the VM (when path-coercion produces a store-path string) and
// the primops (`getContext`, `appendContext`, `unsafeDiscard*`) read
// and write this table.  Putting it here in alloc.hh keeps vm.cc
// independent of the nix:: NixStringContext type.
// ---------------------------------------------------------------------------

inline std::unordered_map<const char *, std::vector<std::string>> & stringContextSideTable()
{
    static std::unordered_map<const char *, std::vector<std::string>> tbl;
    return tbl;
}

inline void setStringContextEntries(const char * buf, std::vector<std::string> entries)
{
    if (entries.empty()) return;
    stringContextSideTable()[buf] = std::move(entries);
}

inline const std::vector<std::string> * lookupStringContextEntries(const char * buf)
{
    if (!buf) return nullptr;
    auto & tbl = stringContextSideTable();
    auto it = tbl.find(buf);
    return it == tbl.end() ? nullptr : &it->second;
}

/// REVIEW §2.6: drop a single side-table entry.  Useful when a string
/// is overwritten in-place at the same arena address (rare but
/// possible in primop fast-paths that reuse a buffer).  The default
/// behaviour of setStringContextEntries replaces, so this helper is
/// only needed when we want to clear context WITHOUT setting new.
inline void dropStringContextEntries(const char * buf)
{
    if (!buf) return;
    stringContextSideTable().erase(buf);
}

/// REVIEW §2.6: drop EVERY entry whose key was allocated in the
/// caller's arena window (a reset hook for long-running daemons).
/// Without this, the side-table grows linearly across evals; arena
/// pointer reuse silently injects unrelated context.  The current
/// v3-eval CLI is single-eval so it never triggers this; daemons
/// should call between top-level evals.
inline void clearStringContextSideTable()
{
    stringContextSideTable().clear();
}

// ---------------------------------------------------------------------------
// T1.3 — Thunk attribution dump (2026-05-27).
//
// Templated from `dumpBindingsAttribution`.  Rolls up per-file:line
// allocation sites + reports top-N by total bytes.  Each Thunk's bytes
// are sizeof(Thunk) + nUpvalues * sizeof(Value).
//
// Gate: NIX_V3_THUNKS_ATTR=1 enables recording AND dump.  Caller
// should fire `dumpThunksAttribution(stderr)` at end-of-run after
// the main NIX_VM_STATS dump.
//
// Origin keying: by (file *, line) pair — file pointers from
// `__builtin_FILE` are deduplicated by the compiler / linker so
// pointer equality is a valid key.
// ---------------------------------------------------------------------------

struct ThunkAttrRollup {
    const char * file;
    uint32_t     line;
    uint64_t     allocCount;
    uint64_t     totalBytes;
    uint32_t     nUpvaluesSum;  // sum across all instances; lets us
                                 // report avg nUpvalues per site
    uint32_t     nUpvaluesMax;
};

inline void dumpThunksAttribution(std::FILE * out, size_t topN = 20) noexcept
{
    if (!thunksAttrEnabled()) return;
    auto & tbl = thunkOriginTable();
    if (tbl.empty()) {
        std::fprintf(out,
            "v3-direct thunks-attr: empty (NIX_V3_THUNKS_ATTR=1 "
            "set but no Thunks allocated yet at this dump)\n");
        return;
    }
    // Aggregate by (file*, line).  File pointers from
    // __builtin_FILE are stable string literals so pointer-equal.
    // Compose a uint64 key from (file_ptr_bits >> 4 << 32) | line.
    // file_ptr fits in 48 bits typically; collisions unlikely at
    // the dump granularity.  Simpler: use string-format key.
    struct Key { const char * file; uint32_t line; };
    struct KeyHash {
        size_t operator()(const Key & k) const noexcept
        {
            return reinterpret_cast<size_t>(k.file) * 1000003u
                 + size_t(k.line);
        }
    };
    struct KeyEq {
        bool operator()(const Key & a, const Key & b) const noexcept
        {
            return a.file == b.file && a.line == b.line;
        }
    };
    std::unordered_map<Key, ThunkAttrRollup, KeyHash, KeyEq> agg;
    agg.reserve(256);
    for (const auto & kv : tbl) {
        const Thunk * t = kv.first;
        const ThunkOrigin & o = kv.second;
        if (!t || !o.file) continue;
        Key k{o.file, o.line};
        auto & r = agg[k];
        r.file = o.file;
        r.line = o.line;
        ++r.allocCount;
        const uint64_t bytes = sizeof(Thunk)
                             + sizeof(Value) * uint64_t(o.nUpvalues);
        r.totalBytes += bytes;
        r.nUpvaluesSum += o.nUpvalues;
        if (o.nUpvalues > r.nUpvaluesMax) r.nUpvaluesMax = o.nUpvalues;
    }
    std::vector<ThunkAttrRollup> sorted;
    sorted.reserve(agg.size());
    for (auto & kv : agg) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const ThunkAttrRollup & a, const ThunkAttrRollup & b) {
            if (a.totalBytes != b.totalBytes)
                return a.totalBytes > b.totalBytes;
            if (a.allocCount != b.allocCount)
                return a.allocCount > b.allocCount;
            return std::less<const char *>{}(a.file, b.file);
        });
    uint64_t grandBytes = 0, grandAllocs = 0;
    for (const auto & r : sorted) {
        grandBytes  += r.totalBytes;
        grandAllocs += r.allocCount;
    }
    std::fprintf(out,
        "v3-direct thunks-attr: %zu distinct origins, %llu total allocs, "
        "%.1f MB tracked  (top %zu by alloc-bytes):\n",
        sorted.size(),
        (unsigned long long)grandAllocs,
        double(grandBytes) / (1024.0 * 1024.0),
        std::min(sorted.size(), topN));
    std::fprintf(out,
        "  %-60s %10s %10s %8s %8s\n",
        "origin (file:line)", "allocs", "MB", "avg-nUp", "max-nUp");
    size_t n = std::min(sorted.size(), topN);
    for (size_t i = 0; i < n; ++i) {
        const auto & r = sorted[i];
        const double mb = double(r.totalBytes) / (1024.0 * 1024.0);
        const double avg = r.allocCount > 0
            ? double(r.nUpvaluesSum) / double(r.allocCount)
            : 0.0;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s:%u",
            r.file ? r.file : "<null>", r.line);
        std::fprintf(out,
            "  %-60s %10llu  %8.2f  %7.2f  %7u\n",
            buf,
            (unsigned long long)r.allocCount,
            mb, avg, r.nUpvaluesMax);
    }
}

// T1.3 — Closure attribution dump.  Mirrors `dumpThunksAttribution`.
// Closures are dispersed across ~7 vm.cc sites — per-site rollup
// distinguishes "user lambda creation" from "VM-internal fakeClo
// wrapping" (the latter is overhead with potential elision targets).
inline void dumpClosuresAttribution(std::FILE * out, size_t topN = 20) noexcept
{
    if (!closuresAttrEnabled()) return;
    auto & tbl = closureOriginTable();
    if (tbl.empty()) {
        std::fprintf(out,
            "v3-direct closures-attr: empty (NIX_V3_CLOSURES_ATTR=1 "
            "set but no Closures allocated yet at this dump)\n");
        return;
    }
    struct Key { const char * file; uint32_t line; };
    struct KeyHash {
        size_t operator()(const Key & k) const noexcept
        {
            return reinterpret_cast<size_t>(k.file) * 1000003u
                 + size_t(k.line);
        }
    };
    struct KeyEq {
        bool operator()(const Key & a, const Key & b) const noexcept
        {
            return a.file == b.file && a.line == b.line;
        }
    };
    struct Rollup {
        const char * file = nullptr;
        uint32_t     line = 0;
        uint64_t     allocCount = 0;
        uint64_t     totalBytes = 0;
        uint32_t     nUpvaluesSum = 0;
        uint32_t     nUpvaluesMax = 0;
    };
    std::unordered_map<Key, Rollup, KeyHash, KeyEq> agg;
    agg.reserve(64);
    for (const auto & kv : tbl) {
        const Closure * c = kv.first;
        const ClosureOrigin & o = kv.second;
        if (!c || !o.file) continue;
        Key k{o.file, o.line};
        auto & r = agg[k];
        r.file = o.file;
        r.line = o.line;
        ++r.allocCount;
        const uint64_t bytes = sizeof(Closure)
                             + sizeof(Value) * uint64_t(o.nUpvalues);
        r.totalBytes += bytes;
        r.nUpvaluesSum += o.nUpvalues;
        if (o.nUpvalues > r.nUpvaluesMax) r.nUpvaluesMax = o.nUpvalues;
    }
    std::vector<Rollup> sorted;
    sorted.reserve(agg.size());
    for (auto & kv : agg) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const Rollup & a, const Rollup & b) {
            if (a.totalBytes != b.totalBytes)
                return a.totalBytes > b.totalBytes;
            return a.allocCount > b.allocCount;
        });
    uint64_t grandBytes = 0, grandAllocs = 0;
    for (const auto & r : sorted) {
        grandBytes  += r.totalBytes;
        grandAllocs += r.allocCount;
    }
    std::fprintf(out,
        "v3-direct closures-attr: %zu distinct origins, %llu total allocs, "
        "%.1f MB tracked  (top %zu by alloc-bytes):\n",
        sorted.size(),
        (unsigned long long)grandAllocs,
        double(grandBytes) / (1024.0 * 1024.0),
        std::min(sorted.size(), topN));
    std::fprintf(out,
        "  %-60s %10s %10s %8s %8s\n",
        "origin (file:line)", "allocs", "MB", "avg-nUp", "max-nUp");
    size_t n = std::min(sorted.size(), topN);
    for (size_t i = 0; i < n; ++i) {
        const auto & r = sorted[i];
        const double mb = double(r.totalBytes) / (1024.0 * 1024.0);
        const double avg = r.allocCount > 0
            ? double(r.nUpvaluesSum) / double(r.allocCount)
            : 0.0;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s:%u",
            r.file ? r.file : "<null>", r.line);
        std::fprintf(out,
            "  %-60s %10llu  %8.2f  %7.2f  %7u\n",
            buf,
            (unsigned long long)r.allocCount,
            mb, avg, r.nUpvaluesMax);
    }
}

// T1.3 — ValuePair attribution dump.
inline void dumpPairsAttribution(std::FILE * out, size_t topN = 20) noexcept
{
    if (!pairsAttrEnabled()) return;
    auto & tbl = pairOriginTable();
    if (tbl.empty()) {
        std::fprintf(out,
            "v3-direct pairs-attr: empty (NIX_V3_PAIRS_ATTR=1 "
            "set but no Pairs allocated yet)\n");
        return;
    }
    struct Key { const char * file; uint32_t line; };
    struct KeyHash {
        size_t operator()(const Key & k) const noexcept
        {
            return reinterpret_cast<size_t>(k.file) * 1000003u
                 + size_t(k.line);
        }
    };
    struct KeyEq {
        bool operator()(const Key & a, const Key & b) const noexcept
        {
            return a.file == b.file && a.line == b.line;
        }
    };
    struct Rollup { const char * file = nullptr; uint32_t line = 0;
                    uint64_t allocCount = 0; };
    std::unordered_map<Key, Rollup, KeyHash, KeyEq> agg;
    agg.reserve(64);
    for (const auto & kv : tbl) {
        const ValuePair * p = kv.first;
        const PairOrigin & o = kv.second;
        if (!p || !o.file) continue;
        Key k{o.file, o.line};
        auto & r = agg[k];
        r.file = o.file;
        r.line = o.line;
        ++r.allocCount;
    }
    std::vector<Rollup> sorted;
    sorted.reserve(agg.size());
    for (auto & kv : agg) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const Rollup & a, const Rollup & b) {
            return a.allocCount > b.allocCount;
        });
    uint64_t grandAllocs = 0;
    for (const auto & r : sorted) grandAllocs += r.allocCount;
    const uint64_t bytesPerPair = sizeof(ValuePair);
    std::fprintf(out,
        "v3-direct pairs-attr: %zu distinct origins, %llu total allocs, "
        "%.1f MB tracked (sizeof(ValuePair)=%llu)  (top %zu):\n",
        sorted.size(),
        (unsigned long long)grandAllocs,
        double(grandAllocs * bytesPerPair) / (1024.0 * 1024.0),
        (unsigned long long)bytesPerPair,
        std::min(sorted.size(), topN));
    std::fprintf(out,
        "  %-60s %12s %10s\n",
        "origin (file:line)", "allocs", "MB");
    size_t n = std::min(sorted.size(), topN);
    for (size_t i = 0; i < n; ++i) {
        const auto & r = sorted[i];
        const double mb =
            double(r.allocCount * bytesPerPair) / (1024.0 * 1024.0);
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s:%u",
            r.file ? r.file : "<null>", r.line);
        std::fprintf(out,
            "  %-60s %12llu  %8.2f\n",
            buf, (unsigned long long)r.allocCount, mb);
    }
}

// T1.3 — ListVec attribution dump.  Lists have variable FAM tail
// (size varies per alloc); rollup tracks total bytes per site, not
// just count.
inline void dumpListsAttribution(std::FILE * out, size_t topN = 20) noexcept
{
    if (!listsAttrEnabled()) return;
    auto & tbl = listOriginTable();
    if (tbl.empty()) {
        std::fprintf(out,
            "v3-direct lists-attr: empty (NIX_V3_LISTS_ATTR=1 "
            "set but no Lists allocated yet)\n");
        return;
    }
    struct Key { const char * file; uint32_t line; };
    struct KeyHash {
        size_t operator()(const Key & k) const noexcept
        {
            return reinterpret_cast<size_t>(k.file) * 1000003u
                 + size_t(k.line);
        }
    };
    struct KeyEq {
        bool operator()(const Key & a, const Key & b) const noexcept
        {
            return a.file == b.file && a.line == b.line;
        }
    };
    struct Rollup {
        const char * file = nullptr;
        uint32_t line = 0;
        uint64_t allocCount = 0;
        uint64_t totalBytes = 0;
        uint64_t totalElems = 0;
        uint32_t maxSize = 0;
    };
    std::unordered_map<Key, Rollup, KeyHash, KeyEq> agg;
    agg.reserve(64);
    for (const auto & kv : tbl) {
        const ListVec * l = kv.first;
        const ListOrigin & o = kv.second;
        if (!l || !o.file) continue;
        Key k{o.file, o.line};
        auto & r = agg[k];
        r.file = o.file;
        r.line = o.line;
        ++r.allocCount;
        const uint64_t bytes = sizeof(ListVec)
                             + sizeof(Value) * uint64_t(o.size);
        r.totalBytes += bytes;
        r.totalElems += o.size;
        if (o.size > r.maxSize) r.maxSize = o.size;
    }
    std::vector<Rollup> sorted;
    sorted.reserve(agg.size());
    for (auto & kv : agg) sorted.push_back(kv.second);
    std::sort(sorted.begin(), sorted.end(),
        [](const Rollup & a, const Rollup & b) {
            if (a.totalBytes != b.totalBytes)
                return a.totalBytes > b.totalBytes;
            return a.allocCount > b.allocCount;
        });
    uint64_t grandAllocs = 0, grandBytes = 0;
    for (const auto & r : sorted) {
        grandAllocs += r.allocCount;
        grandBytes  += r.totalBytes;
    }
    std::fprintf(out,
        "v3-direct lists-attr: %zu distinct origins, %llu total allocs, "
        "%.1f MB tracked  (top %zu by bytes):\n",
        sorted.size(),
        (unsigned long long)grandAllocs,
        double(grandBytes) / (1024.0 * 1024.0),
        std::min(sorted.size(), topN));
    std::fprintf(out,
        "  %-60s %12s %10s %8s %8s\n",
        "origin (file:line)", "allocs", "MB", "avg-size", "max-size");
    size_t n = std::min(sorted.size(), topN);
    for (size_t i = 0; i < n; ++i) {
        const auto & r = sorted[i];
        const double mb = double(r.totalBytes) / (1024.0 * 1024.0);
        const double avg = r.allocCount > 0
            ? double(r.totalElems) / double(r.allocCount) : 0.0;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s:%u",
            r.file ? r.file : "<null>", r.line);
        std::fprintf(out,
            "  %-60s %12llu  %8.2f  %7.2f  %7u\n",
            buf, (unsigned long long)r.allocCount, mb, avg, r.maxSize);
    }
}

} // namespace nix::v3
