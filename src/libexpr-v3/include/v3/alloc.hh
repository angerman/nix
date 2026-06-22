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
#include <unordered_map>
#include <vector>
#include <new>
#include <sys/mman.h>   // R2.0 (2026-06-02): mmap/munmap block lifecycle —
                        // the page-release primitive R1 proved is the ONLY
                        // one that returns RSS on macOS (std::free does not).

// WC-13: optionally register arena blocks as Boehm GC roots so any
// raw `nix::Value *` (or other GC-managed pointer) stored inside a
// Bridge thunk's bridgeSrc field keeps the underlying object alive.
// Without this, the v3 arena is invisible to Boehm's mark phase and
// values pointed to only from there are reclaimed mid-evaluation.
//
// NIX_USE_BOEHMGC gates Boehm vs plain-malloc arena (no GC to integrate
// with otherwise).  Routed through the v3-owned gc-config header so this
// file carries no direct TW include (FFI consolidation §2.4 #4).
#include "v3/gc-config.hh"
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
    static constexpr PosIdx32 kMapAttrsUnrealizedPosBit = 0x80000000u;
    static constexpr PosIdx32 kPosMask = ~kMapAttrsUnrealizedPosBit;

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
    enum class Kind : uint8_t { Sorted = 0, Chain = 1, MapAttrs = 2 };

    uint8_t  kind = uint8_t(Kind::Sorted);   // offset 0
    uint8_t  _pad8[3] = {};                  // offset 1..3
    uint32_t size;                           // offset 4 — Sorted: count of entries[]
                                             //         — Chain:  count of overlay entries[]
    const Bindings * parent = nullptr;       // offset 8 — nullptr for Sorted
    Value    aux;                            // offset 16 — MapAttrs: mapping function
    Entry    entries[];                      // offset 24 — FAM, sorted ascending by name
                                             //          (overlay-only for Chain)

    bool isChain() const noexcept { return kind == uint8_t(Kind::Chain); }
    bool isMapAttrs() const noexcept { return kind == uint8_t(Kind::MapAttrs); }

    /// Binary search the entries array of `this` (does NOT walk parent).
    /// Internal helper used by `lookupEntry` to factor the chain walk.
    const Entry * lookupLocalEntry(SymbolId name) const noexcept
    {
        uint32_t lo = 0, hi = size;
        while (lo < hi) {
            uint32_t mid = (lo + hi) >> 1;
            SymbolId midName = entries[mid].name;
            if (midName == name) return &entries[mid];
            if (midName < name) lo = mid + 1;
            else                hi = mid;
        }
        return nullptr;
    }

    Entry * lookupLocalEntry(SymbolId name) noexcept
    {
        return const_cast<Entry *>(
            static_cast<const Bindings *>(this)->lookupLocalEntry(name));
    }

    const Value * lookupLocal(SymbolId name) const noexcept
    {
        if (const Entry * e = lookupLocalEntry(name)) {
            if (isMapAttrs())
                const_cast<Bindings *>(this)->realizeMapAttrsEntry(
                    const_cast<Entry *>(e));
            return &e->value;
        }
        return nullptr;
    }

    Value * lookupLocal(SymbolId name) noexcept
    {
        if (Entry * e = lookupLocalEntry(name)) {
            if (isMapAttrs())
                realizeMapAttrsEntry(e);
            return &e->value;
        }
        return nullptr;
    }

    /// Chain-aware entry lookup.  Same overlay-then-parent precedence as
    /// lookup(), but returns the full Entry so read-only consumers can copy
    /// name/pos/value without materialising the entire chain.
    const Entry * lookupEntry(SymbolId name) const noexcept
    {
        for (const Bindings * b = this; b; b = b->isChain() ? b->parent : nullptr) {
            const Entry * e = b->lookupLocalEntry(name);
            if (e) {
                if (b->isMapAttrs())
                    const_cast<Bindings *>(b)->realizeMapAttrsEntry(
                        const_cast<Entry *>(e));
                return e;
            }
        }
        return nullptr;
    }

    Entry * lookupEntry(SymbolId name) noexcept
    {
        for (Bindings * b = this; b; b = b->isChain() ? const_cast<Bindings *>(b->parent) : nullptr) {
            Entry * e = b->lookupLocalEntry(name);
            if (e) {
                if (b->isMapAttrs())
                    b->realizeMapAttrsEntry(e);
                return e;
            }
        }
        return nullptr;
    }

    /// Chain-aware binary-search lookup.  Walks overlay then parent.
    /// Returns nullptr if not found anywhere in the chain.  For
    /// Sorted Bindings (parent == nullptr), this is equivalent to
    /// the pre-#823 single-segment binary search.
    const Value * lookup(SymbolId name) const noexcept
    {
        if (const Entry * e = lookupEntry(name))
            return &e->value;
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
        if (Entry * e = lookupEntry(name))
            return &e->value;
        return nullptr;
    }

    bool has(SymbolId name) const noexcept {
        for (const Bindings * b = this; b; b = b->isChain() ? b->parent : nullptr) {
            if (b->lookupLocalEntry(name)) return true;
        }
        return false;
    }

    /// MapAttrs lazy-entry realization.  `primMapAttrs` stores each output
    /// entry's captured source value in `entry.value` as the initial cache
    /// state.  On first value demand this synthesizes the attr-name string and
    /// replaces the entry with App3(fn, name, srcValue).
    /// Defined in value.cc because it needs write barriers.
    void realizeMapAttrsEntry(Entry * e) noexcept;
    Value mapAttrsEntrySource(Entry * e) noexcept;
    static Value makeMapAttrsNameValue(SymbolId name) noexcept;

    // -----------------------------------------------------------------
    // #825 / A1a Phase B (2026-05-26) — chain-aware iteration helpers.
    //
    // These helpers let callers iterate a Bindings without caring
    // whether it's Sorted (today's representation) or Chain (Phase C
    // opt-in via NIX_V3_CHAIN_BINDINGS=1).  The two patterns:
    //
    //   * `forEach(func)` — calls `func(entry)` once per distinct name
    //     in the chain, in ascending name order.  Overlay shadows
    //     parent.  It streams via Cursor and does not materialise the
    //     chain unless the safety depth cap is exceeded.
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
        for (const Bindings * b = this; b; b = b->isChain() ? b->parent : nullptr) ++d;
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

    /// M-1 (CODEBASE_REVIEW_2026-06-11): drop the per-chain materialize memo.
    /// The memo is a thread_local map<const Bindings*, const Bindings*> that is
    /// never walked as GC roots; under default-ON major GC its keys (freed +
    /// reused chain addresses) and values (flat copies reachable only via the
    /// memo) become stale → wrong-Bindings / use-after-free.  Called at the
    /// major-GC safepoint, exactly like the attrSelectCache/recSlotCache
    /// invalidation, so the memo never survives a collection.
    static void clearMaterializeMemo();

    // -----------------------------------------------------------------
    // Lever A (MEMORY_REPRESENTATION_2026-06-07 §6) — k-way-merge
    // Cursor.  This is the piece cppnix has (`Bindings::iterator`) and
    // v3 lacked: a way for read-only consumers to iterate a Chain's
    // layers IN PLACE, in sorted-name order with overlay-wins
    // precedence, WITHOUT calling `materialize()` (which copies the
    // whole base on every consume — the 88 %/470 MB firefox cost and
    // the reason ChainBindings measured NEUTRAL, §5).
    //
    // Precedence matches `materialize()`: the leaf (`this`, the most
    // recent overlay) is layer 0 and wins ties; deeper parents are
    // higher layer indices.  Each distinct name is yielded exactly
    // once, ascending by SymbolId, with shadowed copies skipped.
    //
    // No allocation; O(depth) per `next()`.  Chains are depth-bounded
    // by construction in `mergeBindings` (cap == kMaxLayers); the
    // constructor carries a safety net that falls back to a one-shot
    // `materialize()` if a chain ever exceeds the cap, so correctness
    // never depends on the bound.
    class Cursor {
    public:
        static constexpr uint32_t kMaxLayers = 16;

        explicit Cursor(const Bindings * b, bool realizeMapAttrs = true) noexcept
            : realizeMapAttrs_(realizeMapAttrs)
        {
            nLayers_ = 0;
            for (const Bindings * p = b; p; p = p->isChain() ? p->parent : nullptr) {
                if (p->size == 0) continue;            // empty layer — skip
                if (nLayers_ == kMaxLayers) {
                    // Deeper than the inline array can hold: collapse the
                    // WHOLE chain to one materialised layer.  Safety net
                    // only — mergeBindings caps depth at kMaxLayers.
                    const Bindings * m = b->materialize();
                    heads_[0] = m->entries;
                    ends_[0]  = m->entries + m->size;
                    owners_[0] = m;
                    nLayers_  = m->size ? 1 : 0;
                    return;
                }
                heads_[nLayers_] = p->entries;
                ends_[nLayers_]  = p->entries + p->size;
                owners_[nLayers_] = p;
                ++nLayers_;
            }
        }

        /// Return the next winning Entry (ascending name, overlay-wins),
        /// or nullptr when exhausted.  Advances past shadowed copies.
        const Entry * next() noexcept
        {
            constexpr uint32_t kInvalid = 0xFFFFFFFFu;
            uint32_t best = kInvalid;
            SymbolId bestName = 0;
            for (uint32_t l = 0; l < nLayers_; ++l) {
                if (heads_[l] == ends_[l]) continue;
                SymbolId n = heads_[l]->name;
                // strict `<` keeps the LOWEST layer index on ties == the
                // overlay-winning entry (layer 0 is the leaf).
                if (best == kInvalid || n < bestName) { best = l; bestName = n; }
            }
            if (best == kInvalid) return nullptr;
            const Entry * winner = heads_[best];
            const Bindings * owner = owners_[best];
            lastOwner_ = owner;
            for (uint32_t l = 0; l < nLayers_; ++l)
                if (heads_[l] != ends_[l] && heads_[l]->name == bestName)
                    ++heads_[l];
            if (realizeMapAttrs_ && owner && owner->isMapAttrs())
                const_cast<Bindings *>(owner)->realizeMapAttrsEntry(
                    const_cast<Entry *>(winner));
            return winner;
        }

        const Bindings * lastOwner() const noexcept { return lastOwner_; }

    private:
        const Entry * heads_[kMaxLayers];
        const Entry * ends_[kMaxLayers];
        const Bindings * owners_[kMaxLayers];
        const Bindings * lastOwner_ = nullptr;
        uint32_t      nLayers_;
        bool          realizeMapAttrs_;
    };

    /// Count distinct names in the chain.  O(1) on Sorted, O(N·depth)
    /// alloc-free cursor walk on Chain (cheaper than `totalSize()`'s
    /// sort, used where a count is needed to size an output array).
    uint32_t countDistinct() const noexcept
    {
        if (kind == uint8_t(Kind::Sorted)
            || kind == uint8_t(Kind::MapAttrs))
            return size;
        // L1 (PROFILE_AT_SCALE_2026-06-21): `countDistinct` on a Chain is an
        // O(N·depth) Cursor walk and was the #1 named v3 self-time function
        // (~17% firefox / ~16% HNE on-CPU) — OP_UPDATE sizes every `//` with it
        // and shared base chains get re-counted by every consumer's override.
        // MEMOIZE the result in the 3 spare header pad bytes (`_pad8`, a 24-bit
        // little-endian count).  Safe because a chain's NAME SET is immutable
        // after construction — only VALUES are written back (lookup()/OP_RETURN),
        // never names/parent — so the distinct-count is a stable function of the
        // structure.  The cache lives INLINE (not a side-table: that's the M-1
        // stale-key UAF trap) so it moves with the object under the nursery and
        // stays valid (a parent moving forwards the pointer but keeps the names).
        // Sentinel 0 = uncomputed (a chain always has >=1 distinct name).
        // Opt-out: NIX_V3_NO_COUNTDISTINCT_MEMO.
        static const bool memo =
            std::getenv("NIX_V3_NO_COUNTDISTINCT_MEMO") == nullptr;
        if (memo) {
            uint32_t cached = uint32_t(_pad8[0])
                            | (uint32_t(_pad8[1]) << 8)
                            | (uint32_t(_pad8[2]) << 16);
            if (cached != 0) return cached;
        }
        Cursor c(this, false);
        uint32_t n = 0;
        while (c.next()) ++n;
        if (memo && n != 0 && n <= 0xFFFFFFu) {
            Bindings * self = const_cast<Bindings *>(this);
            self->_pad8[0] = uint8_t(n & 0xFF);
            self->_pad8[1] = uint8_t((n >> 8) & 0xFF);
            self->_pad8[2] = uint8_t((n >> 16) & 0xFF);
        }
        return n;
    }

    /// Walk the chain calling `func(const Entry &)` once per distinct
    /// name in ascending order.  Overlay shadows parent.  Streams the
    /// layers via Cursor — NO materialise for chains (Lever A).
    template <typename F>
    void forEach(F && func) const {
        if (kind == uint8_t(Kind::Sorted)) {
            for (uint32_t i = 0; i < size; ++i) func(entries[i]);
            return;
        }
        if (kind == uint8_t(Kind::MapAttrs)) {
            auto * self = const_cast<Bindings *>(this);
            for (uint32_t i = 0; i < size; ++i) {
                self->realizeMapAttrsEntry(&self->entries[i]);
                func(self->entries[i]);
            }
            return;
        }
        Cursor c(this);
        while (const Entry * e = c.next()) func(*e);
    }

    /// Name-only iteration.  Unlike forEach(), this does not realize MapAttrs
    /// lazy value caches, so attrNames/has-style consumers do not allocate
    /// App3 cells just to inspect keys.
    template <typename F>
    void forEachName(F && func) const {
        if (kind == uint8_t(Kind::Sorted)
            || kind == uint8_t(Kind::MapAttrs)) {
            for (uint32_t i = 0; i < size; ++i) func(entries[i].name);
            return;
        }
        Cursor c(this, false);
        while (const Entry * e = c.next()) func(e->name);
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

    /// Step-1 (2026-06-04, BYTECODE_NGRAM_ANALYSIS §7) trigram
    /// (prevPrev, prev, curr) counts.  Gated by NIX_VM_OPCOUNTS=1 with
    /// NIX_VM_TRIGRAMS=1.  A dense [256]³ array would be 256³×8 = 134 MB
    /// of zero-init memory; instead use a SPARSE map keyed by the packed
    /// 24-bit triple (pp<<16)|(p<<8)|c.  The distinct trigrams that
    /// actually occur number in the low thousands, so the map stays
    /// small; per-dispatch cost is one hash probe+increment UNDER THE
    /// GATE ONLY (the default path is untouched).  This is the
    /// execution-weighted confirmation of the STATIC n-gram candidates
    /// (BYTECODE_NGRAM_ANALYSIS §4) — the Step-1 gate before any
    /// super-instruction / register-VM work.
    /// RETIREMENT: delete alongside bigramCounts when the #780
    /// register-VM decision (BYTECODE_NGRAM_ANALYSIS §7 Step 3) is
    /// reached — either the register-VM arc opens (the counter has
    /// served its purpose) or the dispatch lever is falsified (Rule-0
    /// doc written).  Until then it stays diagnostic-only, default-off.
    std::unordered_map<uint32_t, uint64_t> trigramCounts;

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
// FreeListStats — Step 6 of post-Phase-3.8 plan (2026-05-29).
//
// Per `lode/PHASE_4_PRELIM_FALSIFIED_2026-05-29.md` §2.3: HNE arena
// shrinks 1593 → 1510 MB under reuse-on (sweep finds dead cells) but
// peak_rss doesn't reduce.  Hypothesis: per-exact-size bins (line
// ~1125 freeListBins_ unordered_map<size_t, ...>) MISS too often
// against variable-size Bindings allocation, so freed cells aren't
// reused, leaving the arena resident even though logically freeable.
//
// This instrumentation quantifies the hit rate so Step 11 (log-spaced
// size-class bins) has a measurement to justify it vs. a guess.
//
// Gate: NIX_V3_FREE_LIST_STATS=1 — instrument every Arena::alloc
//       call with per-bin hit/miss counts.  Zero cost when gate OFF.
//
// Retirement criterion (per Rule 0 §2): delete when log-spaced
// size-class bins (Step 11) land AND hit-rate ≥70% on HNE is
// confirmed in production.
//
// Pre-committed decision threshold (per measure-twice §3 + the
// Step 6 task description):
//   * hit rate ≥50% on HNE → bins are fine; Step 11 NOT justified
//   * hit rate <20%        → per-exact-size IS the bottleneck;
//                            Step 11 fires
//   * 20-50% → judgment call documented in Step 9 synthesis
// ---------------------------------------------------------------------------

namespace detail {

/// Cached at startup so the hot-path read is a single comparison
/// against a bool constant (well-predicted; zero cost when OFF).
inline const bool g_freeListStatsEnabled =
    std::getenv("NIX_V3_FREE_LIST_STATS") != nullptr;

/// Step 18 (2026-05-29) — per-allocChars-site attribution.
/// Gate: NIX_V3_STRINGS_ATTR=1
/// Retirement (per Rule 0): "Delete when string dedup decision
/// commits in lode/STRING_DEDUP_DECISION_*.md".
inline const bool g_stringsAttrEnabled =
    std::getenv("NIX_V3_STRINGS_ATTR") != nullptr;

/// Step 12′ (Immix, 2026-05-29) — line-region allocator gate.
/// When enabled (and NIX_V3_MAJOR_GC=1), allocations come from
/// `Arena::freeSpans` rebuilt after each major GC.  Supersedes
/// `V3_DBG_FREELIST_REUSE`: when both are set, Immix wins.  When
/// IMMIX_ALLOC=0 and FREELIST_REUSE=1, falls back to per-exact-size
/// free-list bins (legacy path, scheduled for retirement post-
/// Step 12′ validation).
///
/// Retirement criterion (per Rule 0 §2): "Delete the gate (and
/// `freeListBins_` legacy path) when Step 14′ honest re-measurement
/// confirms Immix path meets SHIP gate."
inline const bool g_immixAllocEnabled =
    std::getenv("V3_DBG_IMMIX_ALLOC") != nullptr;

/// P-3 (CODEBASE_REVIEW_2026-06-11): free-list-reuse gate, promoted to file
/// scope.  It used to be a `static const bool` declared INSIDE the per-alloc
/// hot path (alloc()), so every allocation paid a magic-static guard byte-load
/// (the #768 fast-path-cleanup missed this one).  File scope → init once, no
/// per-alloc guard.
inline const bool g_freeListReuseEnabled =
    std::getenv("V3_DBG_FREELIST_REUSE") != nullptr;

/// MIDEVAL_GC_DESIGN_2026-06-22 — non-moving mid-eval tenured mark-sweep gate.
/// When set, a non-moving runMajorMarkSweep fires at exitDepth>0 on arena
/// pressure (vm.cc), reclaiming scattered dead tenured cells into the free-list
/// bins, AND this allocator consumes them (the pop in alloc()).  Default-OFF.
/// The measured fix for v3 peak RSS 1.9× TW: the arena grows monotonically
/// because ALL prior GC is gated to exitDepth==0, which deep nixpkgs eval never
/// reaches (project_v3_vs_tw_rss_rootcause_2026-06-22).
/// Retirement (Rule 0): flip default-on once darwin-4 shows firefox/M5 RSS drops
/// toward TW + --brute clean on a full nixpkgs sweep; or delete if Immix
/// (GC_DECISION_2026-05-29) lands and subsumes it.
inline const bool g_midEvalGcEnabled =
    std::getenv("NIX_V3_MIDEVAL_GC") != nullptr;

/// MIDEVAL_GC_DESIGN_2026-06-22 — free-list REUSE opt-in, SPLIT from the gate
/// above.  `NIX_V3_MIDEVAL_GC=1` alone runs the correct mark + sweep + bin-build
/// (validated byte-id + --brute) but does NOT pop/reuse — so it does not reclaim
/// RSS yet.  Reuse (the pop below) is GATED SEPARATELY behind NIX_V3_MIDEVAL_REUSE
/// because it currently SEGVs: popping a swept cell that is still live (a residual
/// mark-completeness gap) or wrong-sized.  Isolated here so the correct sweep+bin
/// path stays committable while the reuse-safety is debugged (next: a differential
/// mark audit / poison-on-bin + check-on-pop to identify the live-but-binned cell).
inline const bool g_midEvalReuseEnabled =
    std::getenv("NIX_V3_MIDEVAL_REUSE") != nullptr;

/// MIDEVAL_GC_DESIGN_2026-06-22 — reuse-safety diagnostic.  When set, freeListAdd
/// stamps a sentinel into each binned cell and freeListTryPop verifies it survived
/// to pop time; if the mutator overwrote it (the cell was actually LIVE when the
/// sweep classified it dead → a missed root), abort with the cell's type.  Pins
/// the SEGV's cause.
inline const bool g_midEvalPoison =
    std::getenv("NIX_V3_MIDEVAL_POISON") != nullptr;

} // namespace detail

struct FreeListStats
{
    /// Total calls to Arena::alloc (NOT counting huge-block path,
    /// which bypasses the free list).
    uint64_t allocCount = 0;

    /// Total free-list hits — i.e., calls to freeListTryPop that
    /// returned a non-null slot.  Only nonzero when both stats gate
    /// AND V3_DBG_FREELIST_REUSE are enabled.
    uint64_t hitCount = 0;

    /// Per-bin request + hit histogram.  Bin i covers byte range
    /// `[16 << i, 16 << (i+1))`:
    ///   bin 0: [16,    32)      single Value / smallest cells
    ///   bin 1: [32,    64)      Pair, small Closure
    ///   bin 2: [64,    128)     Closure, small Bindings
    ///   bin 3: [128,   256)     Bindings 4-9 entries
    ///   bin 4: [256,   512)     Bindings 10-20 entries
    ///   bin 5: [512,   1024)    Bindings 21-41 entries
    ///   bin 6: [1024,  2048)    Bindings 42-84 entries
    ///   bin 7: [2048,  4096)    Bindings 85-169 entries
    ///   bin 8: [4096,  8192)    Bindings 170-340 entries
    ///   bin 9: [8192,  16384)   Bindings ~340-680 entries
    ///   bin 10: [16384, 32768)  Bindings ~680-1360 entries
    ///   bin 11: [32768, ∞)      everything larger (catchall)
    static constexpr size_t kNumBins = 12;
    uint64_t requestsByBin[kNumBins] = {};
    uint64_t hitsByBin[kNumBins]     = {};
};

inline FreeListStats & freeListStats()
{
    static FreeListStats stats;
    return stats;
}

// ---------------------------------------------------------------------------
// AllocChars site attribution (Step 18, 2026-05-29).
//
// Per-call-site count + bytes for `Alloc::allocChars`.  Mirrors T1.3
// #746 origin tables but for character buffers (Tag::String / Tag::Path
// payloads).  Gate-guarded; zero cost when OFF.
//
// Pre-committed thresholds (per `STRING_DEDUP_AUDIT_2026-05-28.md`):
// total dedup-able-bytes (estimated downstream after duplication scan):
//   <  50 MB → no lever
//    50-100 → marginal
//   100-200 → moderate
//   > 200   → significant
//
// THIS spike provides per-site attribution only.  Duplication-rate
// estimate is a follow-on (would require hashing every allocChars
// content; defer until per-site data justifies).
// ---------------------------------------------------------------------------

struct AllocCharsSite
{
    const char * file = nullptr;  // pointer into program rodata (stable)
    uint32_t     line = 0;
    uint64_t     count = 0;        // # calls from this site
    uint64_t     bytes = 0;        // sum of n across all calls
};

inline std::vector<AllocCharsSite> & allocCharsSites()
{
    static std::vector<AllocCharsSite> sites;
    return sites;
}

inline void recordAllocCharsSite(const char * file,
                                  uint32_t     line,
                                  size_t       n) noexcept
{
    auto & sites = allocCharsSites();
    // Linear search keyed by (file, line).  N call sites in v3 is
    // small (currently ~18 per grep); linear is faster than a hash
    // map at this scale.  When this grows past ~50, swap to unordered_map.
    for (auto & s : sites) {
        if (s.file == file && s.line == line) {
            ++s.count;
            s.bytes += n;
            return;
        }
    }
    sites.push_back({file, line, 1, n});
}

// ---------------------------------------------------------------------------
// ImmixAllocStats — Step 12′ Immix line-region allocator (2026-05-29).
//
// Per `GC_DECISION_2026-05-29 §3 New Step 12′`: hit rate ≥70% on
// HNE is the acceptance gate.  This struct tracks the three event
// classes for that measurement.
//
// Gate: `NIX_V3_FREE_LIST_STATS=1` (reused) reports these alongside
// the per-bin free-list stats (which become 0 under Immix since
// the legacy bin path is bypassed when `V3_DBG_IMMIX_ALLOC=1`).
// ---------------------------------------------------------------------------

struct ImmixAllocStats
{
    /// Total `Arena::alloc()` calls when Immix path is active
    /// (gate on + non-huge byte size).  Equal to the sum of the
    /// three event counters below.
    uint64_t allocs = 0;

    /// Alloc served from the CURRENT span without advancing
    /// (`immixCur_ + bytes <= immixEnd_`).  The fast path.
    uint64_t spanHits = 0;

    /// Alloc that required advancing to one or more next-spans
    /// (current span too small or exhausted).  Slower path but
    /// still serves from reclaimed lines.
    uint64_t spanAdvances = 0;

    /// Alloc that exhausted all freeSpans and fell through to
    /// `refill()` for a fresh block bump.  Represents "could not
    /// reuse" — the inverse of hit rate.
    uint64_t bumpFresh = 0;

    /// Cumulative bytes served from spans (spanHits + spanAdvances
    /// allocs).  Used to compute byte-weighted hit rate.
    uint64_t bytesFromSpans = 0;

    /// Cumulative bytes served from fresh-block bump (bumpFresh).
    uint64_t bytesFromBump  = 0;
};

inline ImmixAllocStats & immixAllocStats()
{
    static ImmixAllocStats stats;
    return stats;
}

// ---------------------------------------------------------------------------
// ImmixRecycleStats — Step 13′ block recycle policy (2026-05-29).
//
// Per `GC_DECISION_2026-05-29 §3 New Step 13′`: blocks with
// dead-line fraction below `NIX_V3_IMMIX_RECYCLE_PCT` are SKIPPED
// from the freeSpans rebuild → allocator concentrates new
// allocations into the recyclable subset.
//
// Reset to zero at the start of each `rebuildFreeSpansFromLineMarks`
// call (cycle-fresh stats; latest GC's recycling activity).
// ---------------------------------------------------------------------------

struct ImmixRecycleStats
{
    /// Blocks with dead-line% >= threshold (allocator targets them).
    uint64_t blocksRecyclable = 0;
    /// Blocks with dead-line% < threshold (allocator skips them).
    uint64_t blocksSkipped = 0;
    /// Sum of dead bytes in recyclable blocks (potential reclaim).
    uint64_t recyclableDeadBytes = 0;
    /// Sum of dead bytes in skipped blocks (NOT reclaimable this cycle).
    uint64_t skippedDeadBytes = 0;
};

/// Per-cycle stats — written by `rebuildFreeSpansFromLineMarks()`,
/// read by `mark_sweep.cc` post-rebuild banner.  Reset at the start
/// of each rebuild (not cumulative across cycles).
inline ImmixRecycleStats & immixRecycleStats() {
    static ImmixRecycleStats stats;
    return stats;
}

/// Map a byte size to its log2-bin.  bin 0 covers [16, 32); each bin
/// is one power of two wider.  Saturates at kNumBins-1 for the catchall.
inline size_t sizeToFreeListBin(size_t bytes) noexcept
{
    if (bytes < 16) return 0;
    size_t v = bytes >> 4;  // bytes / 16, so v ≥ 1
    size_t b = 0;
    while (v > 1 && b + 1 < FreeListStats::kNumBins) {
        v >>= 1;
        ++b;
    }
    return b;
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

namespace detail {
/// Arena deregistration gate.  Read once at first call; thereafter a
/// cached load.  DEFAULT-ON 2026-06-04 (no-Boehm goal): the arena is NOT
/// registered as a Boehm root region — v3 cells are collected by v3's own
/// major GC, not Boehm, and no arena cell holds the SOLE reference to a
/// Boehm object (F4 deleted the bridge apparatus; v3 creates no Tag::
/// External arena cells; TW-side nix::Value stays alive via TW's own Boehm
/// roots).  Empirically safe: hello/HNE/M5 byte-equal under
/// MAJOR_GC + ARENA_NOROOT.  Opt OUT (re-register with Boehm) via
/// NIX_V3_ARENA_ROOT=1.  RETIREMENT: drop the opt-out once a daemon-soak
/// confirms no arena->Boehm sole-reference regresses across a release.
inline bool arenaNorootEnabledImpl() noexcept
{
    static const bool s_v =
        std::getenv("NIX_V3_ARENA_ROOT") == nullptr;
    return s_v;
}
}
[[gnu::always_inline]] inline bool arenaNorootEnabled() noexcept
{
    return detail::arenaNorootEnabledImpl();
}

namespace detail {
/// Stage 6 Phase 2: major-GC gate.  Read once at startup; thereafter a
/// cached `static const bool`.  Allocator's cell-start + cell-type + line
/// bookkeeping is conditional on this.  DEFAULT-ON 2026-06-04 (no-Boehm
/// goal): v3 self-collects its arena via the non-moving major mark-sweep
/// (5 precise global roots + huge-cell typing + conservative C-stack net)
/// — CORRECT + fast on hello/HNE/M5 (M5 mark 11.6 s, byte-equal).  This is
/// the v3-owned replacement for relying on Boehm to bound v3-cell growth.
/// Opt OUT via NIX_V3_NO_MAJOR_GC=1 (reverts to no v3 collection — arena
/// grows unbounded, as before).  Cost when ON: per-alloc cell-start bit +
/// type stamp + per-block bitmap memory; the mark/sweep pause at the
/// NIX_V3_MAJOR_GC_THRESHOLD_MB (256 MB default) boundary.  RETIREMENT:
/// remove the opt-out once default-ON has soaked across a release.
// OPT-OUT RETIRED (2026-06-15): the nursery is now unconditional (the flip
// soaked clean across all of nixpkgs on darwin-4).  The non-generational per-op
// major mark-sweep is therefore FULLY REPLACED by the gen-major Shape A
// safepoint (vm.cc: forceScavenge the nursery EMPTY, then mark the now
// nursery-free tenured set — M-3-safe).  So `g_majorGcEnabled` is now a hard
// `false`: the per-op major trigger AND its per-alloc bookkeeping (cell-start
// bitmaps / type stamps / immix / freelist-reuse, all gated on
// majorGcEnabled()) stay OFF — exactly the value validated in the default flip
// and in the FP-4 gen-major measurements (where it was already false).
//
// CRITICAL: this MUST be `false`, NOT `getenv("NIX_V3_NO_MAJOR_GC")==nullptr`
// (which is `true` by default) — re-enabling the per-op major alongside the
// always-on nursery would reinstate the M-3 use-after-free (the major marker
// skips nursery-resident cells).  The old NIX_V3_NO_MAJOR_GC opt-out is now
// vestigial (per-op major never runs regardless).
inline const bool g_majorGcEnabled = false;
} // namespace detail

/// R2.1′ (2026-06-03): per-cell TYPE metadata for Nofl-style evacuation.
/// v3 cells carry no self-identifying header — a cell's type is otherwise
/// known only from the Tag of the Value pointing at it.  The mover needs
/// the type at an arbitrary cell-start to (a) content-walk it (forward its
/// pointer fields) and (b) resolve interior-pointer owners (Bindings reached
/// via Tag::Slot).  We record it in a gated side-table (parallel to the
/// cell-start bitmap), stamped at allocation by the typed allocators.  A
/// MISSED stamp is SAFE: it leaves the cell `None`, the mover declines to
/// move it, and verify-before-free pins its block (yield loss, never a
/// dangle).  Values fit the existing 16-byte granule; one byte per
/// cell-start suffices.
enum class CellType : uint8_t {
    None     = 0,  ///< not stamped (interior, free, or pre-gate alloc)
    Value    = 1,  ///< standalone Value cell (allocValue)
    Closure  = 2,
    Thunk    = 3,
    Bindings = 4,
    List     = 5,
    Pair     = 6,  ///< ValuePair (App / App3 / PrimOpApp)
    Env      = 7,
    Chars    = 8,  ///< allocChars string/path buffer
};

// M-9 (CODEBASE_REVIEW_2026-06-11): the per-block cellTypes array is
// NIBBLE-PACKED — two adjacent 16-byte granules share one byte (even granule
// in the low nibble, odd granule in the high nibble).  CellType has 9 values
// (0-8), so 4 bits suffice; this halves the array from 1 MB to 512 KB per
// 16 MB block (≈ tens of MB on firefox/HNE-class arenas).  Every read/write
// of cellTypes routes through these three helpers so the packing layout lives
// in exactly ONE place (alloc.hh's stamp sites + mark_sweep's sweepOneBlock).
// Single-threaded VM ⇒ the read-modify-write of a shared byte is race-free.

/// Bytes needed to nibble-pack `granules` granule-types (round up).
inline constexpr size_t cellTypeBytes(size_t granules) noexcept
{
    return (granules + 1) >> 1;
}

/// Unpack the CellType stamped at granule index `bit`.  Returns None if the
/// (packed) byte index is out of range — callers rely on this for the
/// "interior / unstamped" case.
inline CellType cellTypeUnpack(const std::vector<uint8_t> & arr, size_t bit) noexcept
{
    const size_t byte = bit >> 1;
    if (byte >= arr.size()) return CellType::None;
    const unsigned shift = (bit & 1u) << 2;   // 0 for even granule, 4 for odd
    return static_cast<CellType>((arr[byte] >> shift) & 0x0Fu);
}

/// Pack CellType `t` into granule index `bit`, preserving the neighbouring
/// granule's nibble (read-modify-write).  No-op if out of range.
inline void cellTypePack(std::vector<uint8_t> & arr, size_t bit, CellType t) noexcept
{
    const size_t byte = bit >> 1;
    if (byte >= arr.size()) return;
    const unsigned shift = (bit & 1u) << 2;
    arr[byte] = static_cast<uint8_t>(
        (arr[byte] & ~(0x0Fu << shift))
        | ((static_cast<unsigned>(t) & 0x0Fu) << shift));
}

class Arena
{
public:
    /// Cached env-gate accessor (alloc-hot-path friendly).  Reads
    /// the `static const bool` initialised at startup; the call
    /// inlines to a load + branch that the predictor optimises away.
    static bool majorGcEnabled() noexcept { return detail::g_majorGcEnabled; }
    /// MIDEVAL_GC_DESIGN_2026-06-22: the cell-start/type bitmap is needed by BOTH
    /// the legacy major GC AND the non-moving mid-eval sweep (to find + classify
    /// cells for in-block free-list reuse).  Maintain it whenever EITHER is on
    /// (both default-off → zero cost).  NOTE: this gates METADATA ONLY (cell-start
    /// + cell-type bitmaps) — NOT the block alloc/free method.  Under mid-eval
    /// blocks stay calloc'd (majorGcEnabled() still false), so whole-block-free
    /// (munmap) MUST stay off (skipped in runMajorMarkSweep); the mid-eval win is
    /// in-block free-list reuse, which needs no munmap.  Keeping the block method
    /// unchanged avoids the munmap-on-calloc hazard.
    static bool cellMetaEnabled() noexcept {
        return detail::g_majorGcEnabled || detail::g_midEvalGcEnabled;
    }

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

    /// Step 11′ (Immix, 2026-05-29): line size for the per-block
    /// line-mark bitmap.  Canonical Immix-line-size per
    /// `GC_DECISION_2026-05-29.md §3 New Step 11′` + the F1
    /// empirical measurement at `IMMIX_LINE_OCCUPANCY_2026-05-29.md`
    /// (46.5% fully-dead at 128 B on hello, 50.5% on HNE).
    static constexpr size_t kLineBytes = 128;

    /// Lines per regular 16 MB block = 131,072.
    static constexpr size_t kLinesPerBlock = kBlockSize / kLineBytes;

    /// u64 words to cover one block's lines = 2,048 (16 KB per
    /// block).
    static constexpr size_t kLineU64sPerBlock = kLinesPerBlock / 64;

    /// Stage 6 Day 1 refactor (per STAGE_6_IMPLEMENTATION_GUIDE_2026-
    /// 05-27.md §"Day 1"): group block storage into a `Region` so a
    /// the Cheney semispace experiment introduced a backup region;
    /// retired 2026-05-28 with the Cheney scavenger.  Today flat MS
    /// uses only the active region (cells stay in place).
    // R2.4d (2026-06-03): carry the CellType so cellTypeAt can TYPE a
    // huge cell (≥ kHugeCutoff).  Without it huge cells are CellType::None
    // → the mark's interior-owner walk falls back to drainConservative's
    // O(bytes) byte-scan, which on M5's huge (>4 MB, 170k-entry) Bindings
    // reached via interior Tag::Slot pointers cost a 445 s mark cycle
    // (consW = 1.6 billion).  Typed huge cells let the mark typed-walk
    // them (their exact pointer fields) instead.
    struct HugeBlock { char * begin; char * end; CellType type; };

    /// Step 12′ (Immix, 2026-05-29): contiguous zero-mark line
    /// range within a single arena block.  `begin` + `end` are
    /// kLineBytes-aligned byte pointers; alloc bumps within
    /// [begin, end).
    struct FreeSpan { char * begin; char * end; };

    struct Region {
        char *  cur        = nullptr;
        char *  end        = nullptr;
        /// Owning blocks; never freed in current single-region mode.
        /// Future Stage 6 Day 2-3 will free the backup region's
        /// blocks after a major scavenge.
        std::vector<char *> blocks;
        /// Oversized allocations (> kHugeCutoff), tracked separately.
        std::vector<HugeBlock> hugeBlocks;
        size_t  totalBytes = 0;
        /// Stage 6 Phase 2 (2026-05-28): per-block cell-start bitmap.
        /// Parallel to `blocks` (cellStarts[i] is the bitmap for
        /// blocks[i]).  One bit per 16-byte slot of the block; bit
        /// set means "a cell starts here" (set by `alloc()` on every
        /// allocation).
        ///
        /// Used by the flat MS sweep (mark_sweep.cc) to enumerate
        /// cell starts in address order; cell SIZE inferred from the
        /// distance to the next set bit (or block end).
        ///
        /// Maintained only when `NIX_V3_MAJOR_GC=1` (cached at startup
        /// via Arena::s_majorGcEnabled).  Cost when gate OFF: zero
        /// memory, single branch in alloc().
        std::vector<std::vector<uint64_t>> cellStarts;

        /// R2.1′ (2026-06-03) + M-9 (CODEBASE_REVIEW_2026-06-11): per-cell-
        /// start TYPE, parallel to `blocks` (cellTypes[i] is block i's type
        /// array).  NIBBLE-PACKED: HALF a byte per 16-byte granule (two
        /// granules share a byte — see cellTypeBytes/cellTypePack/
        /// cellTypeUnpack), so 512 KB/block instead of 1 MB.  A nonzero nibble
        /// == a cell of that CellType starts at that granule.  Stamped by
        /// `alloc(bytes, type)` when major-GC is on; read by the evacuation
        /// mover to content-walk + move any cell (esp. Bindings, which the
        /// cell-start-only nursery scavenger cannot move).  Gate-OFF: vector
        /// stays empty, zero cost.
        std::vector<std::vector<uint8_t>> cellTypes;

        /// Step 11′ (Immix, 2026-05-29 per GC_DECISION_2026-05-29.md):
        /// per-block 128 B line-mark bitmap.  Parallel to `blocks`
        /// (lineMarks[i] is the line-mark bitmap for blocks[i]).  One
        /// bit per 128 B line of the block; bit set means "at least
        /// one byte of this line is live."
        ///
        /// Cleared at start of each mark phase by
        /// `Arena::clearAllLineMarks()`; populated by
        /// `Arena::markLinesForCell()` (called from each MarkVisitor
        /// walk function with the cell's address + size); read by the
        /// future Immix line-region allocator (Step 12′).
        ///
        /// Per-block size = (kBlockSize / kLineBytes) / 64 u64 words
        ///                = (16 MB / 128 B) / 64
        ///                = 131,072 lines / 64
        ///                = 2,048 u64 words = 16 KB.
        ///
        /// Maintained only when `NIX_V3_MAJOR_GC=1` (cached at startup
        /// via Arena::s_majorGcEnabled).  Cost when gate OFF: zero
        /// memory (vector stays empty), single branch in refill().
        std::vector<std::vector<uint64_t>> lineMarks;

        /// Step 12′ (Immix line-region allocator, 2026-05-29):
        /// per-block list of free spans (contiguous zero-mark line
        /// ranges) rebuilt after each major GC.  Parallel to
        /// `blocks` (freeSpans[i] is the span list for blocks[i]).
        ///
        /// Each `FreeSpan` is a [begin, end) byte range aligned to
        /// kLineBytes (128 B) on both ends.  The allocator bump-
        /// allocates within a span until it's exhausted, then
        /// advances to the next span in the same block (or next
        /// block).  Cells smaller than the line size still consume
        /// only their bytes, not a full line — the line bookkeeping
        /// is page-level for span construction; the allocator
        /// internally fragments by cell granularity.
        ///
        /// Lifetime: rebuilt by `rebuildFreeSpansFromLineMarks()`
        /// at end of each `runMajorMarkSweep`; consumed by
        /// `Arena::alloc()` until next major GC.
        ///
        /// Maintained only when `NIX_V3_MAJOR_GC=1` AND
        /// `V3_DBG_IMMIX_ALLOC=1`.  Zero memory cost otherwise.
        std::vector<std::vector<FreeSpan>> freeSpans;
    };

    void * alloc(size_t bytes, CellType type = CellType::None) noexcept
    {
        // 16-byte align the request.
        bytes = (bytes + 15) & ~size_t{15};
        if (bytes > kHugeCutoff) {
            // Oversized: dedicated allocation outside the regular block
            // churn.  Review #4: under the major-GC gate, mmap (so a dead
            // huge block can be munmap'd → RSS return on macOS, per R1)
            // instead of calloc (std::free of a large calloc returns 0%
            // RSS).  Default no-GC path keeps calloc.  Both zero-fill, so
            // allocators relying on zero-init are unaffected.  The
            // alloc-vs-free primitive is keyed on the same process-wide
            // majorGcEnabled() gate, so freeHugeBlock picks the matching
            // inverse — no per-block flag needed.
            const bool gc = majorGcEnabled();
            void * blk = gc ? static_cast<void *>(mapArenaBlock(bytes))
                            : std::calloc(1, bytes);
            // Review #3: OOM backstop — never push a null huge block.
            if (__builtin_expect(!blk, 0)) {
                std::fprintf(stderr,
                    "v3 fatal: huge arena allocation failed (%zu MB) "
                    "— out of memory\n", bytes >> 20);
                std::abort();
            }
#if NIX_USE_BOEHMGC
            // NIX_V3_ARENA_NOROOT (default-ON 2026-06-04) opts out of
            // registering arena blocks with Boehm.
            if (!arenaNorootEnabled())
                GC_add_roots(static_cast<char *>(blk),
                             static_cast<char *>(blk) + bytes);
#endif
            // R2.4d: stamp the type so cellTypeAt can typed-walk a huge
            // cell (e.g. a >4 MB / 170k-entry Bindings) instead of the
            // O(bytes) conservative byte-scan.
            active_.hugeBlocks.push_back({static_cast<char *>(blk),
                                   static_cast<char *>(blk) + bytes, type});
            active_.totalBytes += bytes;
            return blk;
        }
        // Stage 6 Phase 3: free-list reuse.  Opt-in via
        // V3_DBG_FREELIST_REUSE=1.
        //
        // M-4 (CODEBASE_REVIEW_2026-06-11) comment correction: the major-GC
        // MARK phase now DOES conservatively scan the C-stack
        // (mark_sweep.cc::walkCStackConservative runs at the safepoint), so the
        // old "mark does not scan the C-stack" rationale is stale.  The free-
        // list-reuse hazard is INDEPENDENT of that: reuse happens at ALLOC
        // time (mid-primop), not at a GC safepoint, and the allocator does NOT
        // consult the C-stack — so a cell reused via the free list MAY still be
        // referenced by a primop body's C-local that holds the previous
        // occupant.  HNE crashed with SIGBUS under reuse-on due to this gap;
        // hello.drvPath did not (smaller workload, fewer primop-mid-flight
        // states).
        //
        // Reuse-correct path requires a stricter trigger (only reuse cells that
        // can't be C-stack-live — e.g. freed at the outermost OP_RETURN).
        // Deferred to a follow-up commit; the opt-in gate preserves the
        // mechanism for measurement.
        // Step 6 of post-Phase-3.8 plan: free-list stats per-call
        // tracking.  Zero cost when NIX_V3_FREE_LIST_STATS unset
        // (cached bool comparison; well-predicted false branch).
        // Single reference binding (avoids inline-static ODR concerns).
        if (__builtin_expect(detail::g_freeListStatsEnabled, 0)) {
            auto & fls = freeListStats();
            ++fls.allocCount;
            const size_t bin = sizeToFreeListBin(bytes);
            if (bin < FreeListStats::kNumBins) {
                ++fls.requestsByBin[bin];
            }
        }

        // Step 12′ (Immix, 2026-05-29): line-region allocator path.
        // When gate ON, try the current free span first (fast path:
        // 1 cmp + 1 bump), advancing to next span if exhausted.
        // Bypasses the legacy `freeListBins_` path entirely.
        //
        // Acceptance gate (per task #848): hit rate (allocs served
        // from spans / total allocs) ≥70% on HNE.
        if (__builtin_expect(majorGcEnabled() && detail::g_immixAllocEnabled, 0)) {
            auto & is = immixAllocStats();
            ++is.allocs;
            // Fast path: current span has room.
            if (immixCur_ && immixCur_ + bytes <= immixEnd_) {
                void * p = immixCur_;
                immixCur_ += bytes;
                setCellStartBitInBlock(p, immixCurBlockIdx_);
                setCellTypeInBlock(p, immixCurBlockIdx_, type);  // review #13
                // Phase 3.6 reuse-safety (carried over for Immix):
                // span bytes hold STALE data from previously-live
                // cells.  Allocators (allocBindings/allocClosure/...)
                // only write a few header fields and expect zero-init
                // for the rest (calloc convention).  Without memset
                // here, stale `kind` bytes cause Bindings::lookup to
                // chase a garbage `parent` chain → SIGSEGV.
                std::memset(p, 0, bytes);
                ++is.spanHits;
                is.bytesFromSpans += bytes;
                return p;
            }
            // Slow path: advance through spans until one fits.
            while (immixAdvanceToNextSpan()) {
                if (immixCur_ + bytes <= immixEnd_) {
                    void * p = immixCur_;
                    immixCur_ += bytes;
                    setCellStartBitInBlock(p, immixCurBlockIdx_);
                    setCellTypeInBlock(p, immixCurBlockIdx_, type);  // review #13
                    std::memset(p, 0, bytes);  // Phase 3.6 reuse-safety
                    ++is.spanAdvances;
                    is.bytesFromSpans += bytes;
                    return p;
                }
                // Current span too small for this alloc; immixAdvance
                // continues iterating until exhaustion or fit.
            }
            // All spans exhausted; fall through to fresh-block bump.
            ++is.bumpFresh;
            is.bytesFromBump += bytes;
            // Fall through to bump path below — do NOT return here.
        }

        // P-3: file-scope gate (no per-alloc magic-static guard).
        // Step 12′: legacy freeListBins_ path active ONLY when
        // V3_DBG_IMMIX_ALLOC=0.  When Immix is on, the bin path is
        // bypassed (its hits/misses would be wrong against the Immix
        // line-region state).  See GC_DECISION §6 — this entire
        // section retires when Step 14′ SHIP gate clears.
        //
        // MIDEVAL_GC_DESIGN_2026-06-22: the pop was gated on majorGcEnabled()
        // (the legacy non-moving major GC), hard-false since the nursery shipped
        // (M-3) → reuse was dead code → the tenured arena never reused dead cells
        // → it grew monotonically (v3 RSS 1.9× TW).  The bins are BUILT by the
        // sweep (mark_sweep.cc, gated g_freeListReuseEnabled||g_midEvalGcEnabled);
        // let the allocator CONSUME them under EITHER opt-in.  Default-OFF (both
        // gates) → byte-for-byte the old default path.  Correctness: bins hold
        // cells the precise+conservative non-moving mark proved dead.
        if (__builtin_expect((detail::g_freeListReuseEnabled
                              || (detail::g_midEvalGcEnabled && detail::g_midEvalReuseEnabled))
                             && !detail::g_immixAllocEnabled, 0)) {
            if (void * p = freeListTryPop(bytes)) {
                // Step 6: count the hit.  Bin is the requested size's
                // bin, NOT the popped slot's actual bin (per-exact-size
                // map means they're identical today; with Step 11
                // log-spaced bins they could differ).
                if (__builtin_expect(detail::g_freeListStatsEnabled, 0)) {
                    auto & fls = freeListStats();
                    ++fls.hitCount;
                    const size_t bin = sizeToFreeListBin(bytes);
                    if (bin < FreeListStats::kNumBins) {
                        ++fls.hitsByBin[bin];
                    }
                }
                // free-list pop re-sets the cell-start bit
                // internally; stamp the (new) type too so the reused
                // granule doesn't keep the prior occupant's type (#13).
                setCellTypeFor(p, type);
                // RCA fix (2026-06-23): clear stale interior cell-start bits so the
                // sweep doesn't split this reused multi-granule cell at a leftover
                // bit from a prior occupant (the reuse-SEGV root cause).
                clearInteriorCellStarts(p, bytes);
                return p;
            }
        }
        if (active_.cur + bytes > active_.end) refill();
        void * p = active_.cur;
        active_.cur += bytes;
        // Stage 6 Phase 2: record cell-start bit for sweep.  Gated on
        // majorGcEnabled() (cached at startup); zero cost when gate
        // OFF.  Hot path: branch is well-predicted (single fixed bool
        // per process).
        if (__builtin_expect(cellMetaEnabled(), 0)) {
            const size_t offset = static_cast<size_t>(
                static_cast<char *>(p) - active_.blocks.back());
            const size_t bit  = offset >> 4;           // /16
            const size_t word = bit >> 6;              // /64
            active_.cellStarts.back()[word] |=
                1ULL << (bit & 63);
            // R2.1′ + M-9: stamp the cell type at this granule (bump path —
            // the default under NIX_V3_MAJOR_GC; immix-span/freelist/huge
            // paths leave None, pinning those cells, which is safe).  Nibble-
            // packed write (preserves the neighbouring granule's nibble).
            if (type != CellType::None)
                cellTypePack(active_.cellTypes.back(), bit, type);
            // RCA fix (2026-06-23): clear any stale interior cell-start bits in
            // this cell's span.  Bump regions are USUALLY fresh (bitmap 0), but a
            // reused/recycled block region can carry a leftover bit from a prior
            // finer-grained occupant; a spurious interior bit makes the sweep
            // split this live multi-granule cell (the reuse-SEGV: a string with an
            // interior stale Bindings cell-start, mis-binned + reused → corruption).
            clearInteriorCellStarts(p, bytes);
        }
        return p;
    }

    /// Stage 6 Phase 2: query whether `p` is a recorded cell-start.
    /// Used by mark phase to distinguish slot targets that are
    /// standalone cells (need marking) from slot targets that point
    /// INSIDE a larger container like a Bindings entry (don't mark;
    /// the container's mark covers them).
    ///
    /// Returns false if `p` is null, not in active arena, or no
    /// cell-start bit is set there (in which case `p` is either
    /// interior of a larger cell, or the gate was OFF when the cell
    /// was allocated).
    bool isCellStart(const void * p) const noexcept
    {
        if (!cellMetaEnabled()) return false;
        if (!p) return false;
        const char * cp = static_cast<const char *>(p);
        {
            const long bi = blockIndexContaining(cp);  // Lever 1: O(log blocks)
            if (bi >= 0) {
                const size_t i = static_cast<size_t>(bi);
                const char * blk = active_.blocks[i];
                const size_t offset = static_cast<size_t>(cp - blk);
                if ((offset & 15) != 0) return false;  // not aligned
                const size_t bit  = offset >> 4;
                const size_t word = bit >> 6;
                if (word >= active_.cellStarts[i].size()) return false;
                return (active_.cellStarts[i][word]
                        & (1ULL << (bit & 63))) != 0;
            }
        }
        // Huge blocks: each is one allocation starting at .begin.
        for (const auto & h : active_.hugeBlocks) {
            if (cp == h.begin) return true;
        }
        return false;
    }

    /// Stage 6 Phase 2: accessor for sweep to enumerate cell starts.
    const std::vector<std::vector<uint64_t>> & cellStartBitmaps() const noexcept
        { return active_.cellStarts; }

    /// R2.1′ (2026-06-03): cell type stamped at `p`'s granule, or
    /// CellType::None if `p` isn't a stamped cell-start (interior, free,
    /// huge, or allocated via a non-bump path).  O(active blocks) — used
    /// by the evacuation mover at GC time, not on the alloc hot path.
    CellType cellTypeAt(const void * p) const noexcept
    {
        if (!cellMetaEnabled() || !p) return CellType::None;
        const char * cp = static_cast<const char *>(p);
        const long bi = blockIndexContaining(cp);  // Lever 1: O(log blocks)
        if (bi >= 0) {
            const size_t i = static_cast<size_t>(bi);
            const char * blk = active_.blocks[i];
            const size_t offset = static_cast<size_t>(cp - blk);
            if ((offset & 15) != 0) return CellType::None;
            const size_t bit = offset >> 4;
            if (i >= active_.cellTypes.size())
                return CellType::None;
            // M-9: nibble-packed read (cellTypeUnpack bounds-checks the
            // packed byte index and returns None when out of range).
            return cellTypeUnpack(active_.cellTypes[i], bit);
        }
        // R2.4d: huge cells (≥ kHugeCutoff) live outside the regular
        // block index; the cell-start is the block begin.  Return the
        // stamped type so the mark can typed-walk it (avoids the O(bytes)
        // conservative byte-scan on huge Bindings).
        for (const auto & h : active_.hugeBlocks) {
            if (cp == h.begin) return h.type;
        }
        return CellType::None;
    }

    /// R2.1′: type-array accessor (parallel to cellStartBitmaps), so the
    /// sweep/mover can iterate types block-indexed without the O(blocks)
    /// address lookup.
    const std::vector<std::vector<uint8_t>> & cellTypeArrays() const noexcept
        { return active_.cellTypes; }

    // ============================================================
    // Step 11′ — Immix line-mark bitmap API (2026-05-29).
    //
    // Per `GC_DECISION_2026-05-29.md §3 New Step 11′`: maintain a
    // per-block 128 B line-mark bitmap.  Mark phase populates;
    // future Immix allocator (Step 12′) reads.
    //
    // Zero-cost when major-GC gate OFF.  Gate-ON cost:
    //   * memory: 16 KB per 16 MB block = 0.1% of arena
    //   * mark wall: O(reachable cells) markLinesForCell calls,
    //     each O(blocks-linear-search) + O(lines-in-cell)
    //   * clear at start of mark: O(total lineMark u64s)
    //     ≈ 2048 × n_blocks ≈ 200 KB memset for HNE — ~50 µs.
    // ============================================================

    /// Clear all line-mark bitmaps.  Called at start of each mark
    /// phase by runMajorMarkSweep.  No-op when gate OFF (vector
    /// stays empty) or when no blocks allocated yet.
    void clearAllLineMarks() noexcept
    {
        if (!majorGcEnabled()) return;
        for (auto & bits : active_.lineMarks)
            std::fill(bits.begin(), bits.end(), 0ULL);
    }

    /// Mark every 128 B line that the cell at [`addr`, `addr`+`bytes`)
    /// overlaps.  Called from each MarkVisitor walk* function with
    /// the cell's known size.  No-op when gate OFF, when `addr` is
    /// null, or when `addr` is not in active arena (huge-block /
    /// external).
    ///
    /// `bytes` is the LOGICAL cell size (e.g., sizeof(Closure) +
    /// nUpvalues*sizeof(Value)).  Not 16-byte-aligned by this
    /// function; the line-bit set OR's across whatever range the
    /// cell spans.
    void markLinesForCell(const void * addr, size_t bytes) noexcept
    {
        if (!majorGcEnabled() || !addr || bytes == 0) return;
        const char * cp = static_cast<const char *>(addr);
        const size_t nBlocks = active_.blocks.size();
        if (nBlocks == 0 || nBlocks > active_.lineMarks.size()) return;
        // P-7 (CODEBASE_REVIEW_2026-06-11): O(log blocks) block lookup via the
        // sorted index (was an O(blocks) last-first linear scan PER marked
        // cell during the line-marking pass).  Same containing-block result.
        const long bi = blockIndexContaining(cp);
        if (bi < 0) return;  // huge / external — skip (as before)
        const size_t i = static_cast<size_t>(bi);
        if (i >= active_.lineMarks.size()) return;
        const char * blk = active_.blocks[i];
        const size_t offset = static_cast<size_t>(cp - blk);
        const size_t end = offset + bytes;
        // Clamp end-of-cell to end-of-block (defensive; cells should never
        // straddle block boundaries given allocator refills on overflow).
        const size_t clampedEnd = (end > kBlockSize) ? kBlockSize : end;
        const size_t firstLine = offset / kLineBytes;
        const size_t lastLine  = (clampedEnd - 1) / kLineBytes;
        auto & bits = active_.lineMarks[i];
        for (size_t L = firstLine; L <= lastLine && L < kLinesPerBlock; ++L)
            bits[L >> 6] |= 1ULL << (L & 63);
    }

    /// Query whether the line containing `addr` is marked.  Used by
    /// the Step 12′ Immix allocator (predeclared here; not used in
    /// Step 11′).  Returns false if `addr` is null, not in active
    /// arena, or gate OFF.
    bool isLineMarked(const void * addr) const noexcept
    {
        if (!majorGcEnabled() || !addr) return false;
        const char * cp = static_cast<const char *>(addr);
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            const char * blk = active_.blocks[i];
            if (cp < blk || cp >= blk + kBlockSize) continue;
            if (i >= active_.lineMarks.size()) return false;
            const size_t offset = static_cast<size_t>(cp - blk);
            const size_t line = offset / kLineBytes;
            const size_t word = line / 64;
            if (word >= active_.lineMarks[i].size()) return false;
            return (active_.lineMarks[i][word] >> (line & 63)) & 1ULL;
        }
        return false;
    }

    /// Accessor for diagnostic + future Step 12′ allocator.
    const std::vector<std::vector<uint64_t>> & lineMarkBitmaps() const noexcept
        { return active_.lineMarks; }

    /// Aggregate count of fully-dead (zero) line bits across all
    /// regular blocks' lineMarks bitmaps.  Used by mark_sweep.cc to
    /// report the per-cycle line-occupancy alongside sweep stats.
    /// Returns (totalLines, fullyDeadLines).
    std::pair<size_t, size_t> countLineMarks() const noexcept
    {
        size_t total = 0;
        size_t deadLines = 0;
        for (const auto & bits : active_.lineMarks) {
            total += bits.size() * 64;
            for (uint64_t w : bits)
                deadLines += 64 - __builtin_popcountll(w);
        }
        // Lines beyond the block's allocated range are NOT
        // necessarily zero (we don't track block-used-bytes here).
        // For the purpose of "fully dead lines in tracked region"
        // this is close enough; precise accounting is reportSweepCost
        // / live_trace.cc.
        return {total, deadLines};
    }

    // ============================================================
    // Step 12′ — Immix line-region allocator API (2026-05-29).
    //
    // Per `GC_DECISION_2026-05-29.md §3 New Step 12′`: after each
    // major GC, scan the per-block line-mark bitmap and build a
    // list of contiguous zero-bit (free-line) ranges per block.
    // Allocator bumps within these ranges instead of consuming a
    // fresh block bump pointer.
    //
    // Gate: V3_DBG_IMMIX_ALLOC=1 (must be ON together with
    // NIX_V3_MAJOR_GC=1).  Zero cost when OFF.
    //
    // Retirement (per Rule 0): "Delete `freeListBins_` + the env
    // gate once Step 14′ honest re-measurement confirms Immix path
    // meets SHIP gate."
    // ============================================================

    /// Walk `active_.lineMarks` per-block, identify contiguous runs
    /// of zero bits (= "fully dead lines"), and build `active_.freeSpans`.
    /// Resets `immixCur_/Idx_` state so next alloc starts at the
    /// first span of the first block with non-empty spans.
    ///
    /// Called by `runMajorMarkSweep` AFTER sweep completes.
    /// No-op when major-GC gate OFF.
    ///
    /// Span construction algorithm: word-by-word scan of bits.
    /// * word == 0          → entire 64 lines are dead; extend run
    /// * word == ~0ULL      → all 64 lines live; close any open run
    /// * mixed              → walk bits within word
    void rebuildFreeSpansFromLineMarks() noexcept
    {
        if (!majorGcEnabled()) return;
        const size_t nBlocks = active_.blocks.size();
        active_.freeSpans.clear();
        active_.freeSpans.resize(nBlocks);
        if (nBlocks > active_.lineMarks.size()) return;

        // Step 13′ (Immix recycle policy, 2026-05-29).
        // Per `GC_DECISION_2026-05-29 §3 New Step 13′`: blocks with
        // dead-line fraction below the threshold are SKIPPED — the
        // allocator treats them as "too live to bother."  This
        // concentrates new allocations into a subset of recyclable
        // blocks, increasing the chance that other blocks become
        // fully dead and Phase 3.8 `freeWholeBlock` reclaims them.
        //
        // Gate: NIX_V3_IMMIX_RECYCLE_PCT=N (default 0; range 0-100).
        //   0  = recycle all (Step 12′ behavior, no skip)
        //  30  = skip blocks with <30% dead lines
        //
        // Retirement (per Rule 0): "delete the env when Step 14′
        // SHIP gate is met; recycle policy fixed at the empirically-
        // best X."
        static const long s_recyclePctThreshold = []() -> long {
            const char * v = std::getenv("NIX_V3_IMMIX_RECYCLE_PCT");
            long pct = 0;
            if (v) {
                long parsed = std::strtol(v, nullptr, 10);
                if (parsed >= 0 && parsed <= 100) pct = parsed;
            }
            return pct;
        }();
        immixRecycleStats() = ImmixRecycleStats{};  // reset per-cycle
        // Step 12′ correctness fix (2026-05-29): the ACTIVE block's
        // reserve tail (`active_.cur` .. `active_.end`) holds no cells
        // — those bytes are reserved for the bump allocator's NEXT
        // allocation.  Their lineMarks bits are 0 (no live cell there),
        // so a naive freeSpans rebuild would include them in spans.
        // Immix would then allocate from those bytes WHILE bump also
        // hands them out — same bytes returned twice → corruption.
        //
        // Mark the reserve-tail lines as "live" (set bits) before
        // span construction, so they're excluded from freeSpans.
        // The non-active blocks have no bump pointer (fully populated
        // before refill moved on), so this only applies to the LAST
        // block when active_.cur is valid.
        if (active_.cur && active_.end && !active_.blocks.empty()) {
            const size_t lastIdx = active_.blocks.size() - 1;
            const char * lastBlk = active_.blocks[lastIdx];
            // Verify active_.cur is in the last block.
            if (active_.cur >= lastBlk &&
                active_.cur <  lastBlk + kBlockSize &&
                lastIdx < active_.lineMarks.size())
            {
                const size_t curOffset =
                    static_cast<size_t>(active_.cur - lastBlk);
                // First line at or after cur is reserved.  If cur
                // lands MID-line (line contains both live cells before
                // cur and reserve bytes after), the partially-live
                // case is already covered by the live cell's mark; we
                // only need to RESERVE lines fully past cur.
                const size_t firstReserveLine =
                    (curOffset + kLineBytes - 1) / kLineBytes;
                auto & bits = active_.lineMarks[lastIdx];
                for (size_t L = firstReserveLine;
                     L < kLinesPerBlock && (L >> 6) < bits.size();
                     ++L)
                {
                    bits[L >> 6] |= 1ULL << (L & 63);
                }
            }
        }
        for (size_t bi = 0; bi < nBlocks; ++bi) {
            auto & blockSpans = active_.freeSpans[bi];
            const auto & bits = active_.lineMarks[bi];
            char * blkBase = active_.blocks[bi];
            const size_t nWords = bits.size();
            if (nWords == 0) continue;

            // Step 13′ recycle policy: compute dead-line fraction
            // first; skip block if below threshold.  When skipped,
            // the block's freeSpans stays empty and the allocator
            // bypasses it.
            if (s_recyclePctThreshold > 0) {
                size_t liveBits = 0;
                for (uint64_t w : bits) liveBits += __builtin_popcountll(w);
                const size_t totalBits = nWords * 64;
                const size_t deadBits = totalBits - liveBits;
                const double deadPct = totalBits > 0
                    ? 100.0 * double(deadBits) / double(totalBits) : 0.0;
                if (deadPct < double(s_recyclePctThreshold)) {
                    ++immixRecycleStats().blocksSkipped;
                    immixRecycleStats().skippedDeadBytes +=
                        deadBits * kLineBytes;
                    continue;
                }
                ++immixRecycleStats().blocksRecyclable;
                immixRecycleStats().recyclableDeadBytes +=
                    deadBits * kLineBytes;
            } else {
                ++immixRecycleStats().blocksRecyclable;
            }
            // Walk bit-by-bit at outer level for clarity (the inner
            // word-level fast paths can be added later if perf bad).
            size_t lineIdx = 0;
            while (lineIdx < kLinesPerBlock) {
                // Skip leading live lines.
                while (lineIdx < kLinesPerBlock &&
                       (bits[lineIdx >> 6] >> (lineIdx & 63)) & 1ULL) {
                    ++lineIdx;
                }
                if (lineIdx >= kLinesPerBlock) break;
                const size_t spanStart = lineIdx;
                // Skip dead lines (the span body).
                while (lineIdx < kLinesPerBlock &&
                       !((bits[lineIdx >> 6] >> (lineIdx & 63)) & 1ULL)) {
                    ++lineIdx;
                }
                const size_t spanEnd = lineIdx;
                blockSpans.push_back({
                    blkBase + spanStart * kLineBytes,
                    blkBase + spanEnd   * kLineBytes
                });
            }
        }
        // Reset allocator state to start fresh in span 0 of block 0.
        immixCurBlockIdx_ = 0;
        immixCurSpanIdx_  = 0;
        immixCur_         = nullptr;
        immixEnd_         = nullptr;
        immixAdvanceToNextSpan();
    }

    /// Advance allocator to the next available free span.  Returns
    /// true if a span was found (immixCur_/End_ are now valid for
    /// bumping); false if all spans across all blocks exhausted.
    ///
    /// Empty spans (begin == end) are skipped.
    bool immixAdvanceToNextSpan() noexcept
    {
        while (immixCurBlockIdx_ < active_.freeSpans.size()) {
            auto & blockSpans = active_.freeSpans[immixCurBlockIdx_];
            while (immixCurSpanIdx_ < blockSpans.size()) {
                const FreeSpan & span = blockSpans[immixCurSpanIdx_];
                ++immixCurSpanIdx_;
                if (span.begin < span.end) {
                    immixCur_ = span.begin;
                    immixEnd_ = span.end;
                    return true;
                }
            }
            ++immixCurBlockIdx_;
            immixCurSpanIdx_ = 0;
        }
        immixCur_ = nullptr;
        immixEnd_ = nullptr;
        return false;
    }

    /// Step 12′ helper: set the cell-start bit for an address known
    /// to be in `blocks[blockIdx]`.  Used by the Immix alloc path
    /// where the block index is already known from `immixCurBlockIdx_`,
    /// avoiding the linear-scan in `setCellStartBitFor()`.
    void setCellStartBitInBlock(const void * p, size_t blockIdx) noexcept
    {
        if (!majorGcEnabled() || !p) return;
        if (blockIdx >= active_.cellStarts.size()) return;
        const char * blk = active_.blocks[blockIdx];
        const size_t offset =
            static_cast<size_t>(static_cast<const char *>(p) - blk);
        const size_t bit = offset >> 4;
        const size_t word = bit >> 6;
        auto & bits = active_.cellStarts[blockIdx];
        if (word < bits.size()) {
            bits[word] |= 1ULL << (bit & 63);
        }
    }

    /// Review #13: stamp the CellType for a cell allocated via the immix
    /// span path (parallel to setCellStartBitInBlock).  Stamps
    /// UNCONDITIONALLY — including CellType::None — because a reused
    /// granule still carries the PRIOR occupant's type byte; skipping
    /// None (as the bump path does for fresh None-init cells) would leave
    /// that stale type, so cellTypeAt lies and the typed mark/evac walk
    /// reinterpret_casts the wrong layout (heap corruption / SIGSEGV).
    void setCellTypeInBlock(const void * p, size_t blockIdx, CellType type) noexcept
    {
        if (!cellMetaEnabled() || !p) return;
        if (blockIdx >= active_.cellTypes.size()) return;
        const char * blk = active_.blocks[blockIdx];
        const size_t bit =
            (static_cast<size_t>(static_cast<const char *>(p) - blk)) >> 4;
        // M-9: nibble-packed write — UNCONDITIONAL (incl. None) so a reused
        // granule loses the prior occupant's type (see header above).
        cellTypePack(active_.cellTypes[blockIdx], bit, type);
    }

    /// Accessor for diagnostic + downstream Step 13′ recycle policy.
    const std::vector<std::vector<FreeSpan>> & freeSpansForBlocks() const noexcept
        { return active_.freeSpans; }

    /// Aggregate (total bytes in spans, span count) across all
    /// blocks.  Used by sweep stats banner to verify the rebuilt
    /// spans match the line-mark dead-line count.
    std::pair<size_t, size_t> countFreeSpanBytes() const noexcept
    {
        size_t totalBytes = 0;
        size_t totalSpans = 0;
        for (const auto & blockSpans : active_.freeSpans) {
            totalSpans += blockSpans.size();
            for (const auto & s : blockSpans) {
                totalBytes += static_cast<size_t>(s.end - s.begin);
            }
        }
        return {totalBytes, totalSpans};
    }

    /// Stage 6 Phase 3.5: backward search for the cell-start at or
    /// below `p`.  Used by conservative C-stack scan to locate the
    /// owning cell of an arbitrary arena address.  Returns nullptr
    /// if `p` is not in active arena or no cell-start exists at or
    /// before `p` within its block.
    // ===== Lever 1 (R2.4d): O(log blocks) block lookup =====
    // findContainingCellStart / findNextCellStartOrBlockEnd / isCellStart /
    // cellTypeAt all need the active_.blocks index of the block containing
    // a pointer.  A linear scan is O(blocks) and dominates the evacuation
    // GC's mark+move+verify on M5 (424 blocks).  Maintain a sorted
    // (start,index) side-index; rebuild lazily on the first lookup after
    // any block add/free (refill / freeWholeBlock set the dirty flag).
    // mmap block addresses are non-monotonic, so active_.blocks itself
    // can't be binary-searched — hence the parallel sorted index.
    mutable std::vector<std::pair<const char *, size_t>> sortedBlocks_;
    mutable bool sortedBlocksDirty_ = true;

    void rebuildSortedBlocks() const noexcept
    {
        sortedBlocks_.clear();
        sortedBlocks_.reserve(active_.blocks.size());
        for (size_t i = 0; i < active_.blocks.size(); ++i)
            sortedBlocks_.emplace_back(active_.blocks[i], i);
        std::sort(sortedBlocks_.begin(), sortedBlocks_.end());
        sortedBlocksDirty_ = false;
    }

    /// Index into active_.blocks of the regular block containing `cp`,
    /// or -1.  O(log blocks) after an amortised rebuild.
    long blockIndexContaining(const char * cp) const noexcept
    {
        if (sortedBlocksDirty_) rebuildSortedBlocks();
        auto it = std::upper_bound(
            sortedBlocks_.begin(), sortedBlocks_.end(), cp,
            [](const char * v, const std::pair<const char *, size_t> & e) {
                return v < e.first;
            });
        if (it == sortedBlocks_.begin()) return -1;
        --it;
        if (cp >= it->first && cp < it->first + kBlockSize)
            return static_cast<long>(it->second);
        return -1;
    }

    const char * findContainingCellStart(const void * p) const noexcept
    {
        if (!cellMetaEnabled() || !p) return nullptr;
        const char * cp = static_cast<const char *>(p);
        {
            const long bi = blockIndexContaining(cp);
            if (bi >= 0) {
                const size_t i = static_cast<size_t>(bi);
                const char * blk = active_.blocks[i];
                const size_t offset = static_cast<size_t>(cp - blk);
                size_t bit = offset / 16;
                // Scan backward word-by-word.
                size_t word = bit / 64;
                const auto & bits = active_.cellStarts[i];
                if (word >= bits.size()) return nullptr;
                // Check the current word from bit position downward.
                {
                    const size_t bitInWord = bit % 64;
                    uint64_t mask = (bitInWord == 63)
                        ? uint64_t(-1)
                        : ((uint64_t(1) << (bitInWord + 1)) - 1);
                    uint64_t masked = bits[word] & mask;
                    if (masked) {
                        // Highest set bit at-or-below bitInWord
                        int hi = 63 - __builtin_clzll(masked);
                        size_t foundBit = word * 64 + size_t(hi);
                        return blk + (foundBit * 16);
                    }
                }
                // Scan earlier words.
                if (word == 0) return nullptr;
                for (size_t w = word; w-- > 0;) {
                    if (bits[w] == 0) continue;
                    int hi = 63 - __builtin_clzll(bits[w]);
                    size_t foundBit = w * 64 + size_t(hi);
                    return blk + (foundBit * 16);
                }
                return nullptr;
            }
        }
        // Huge: cell-start is at h.begin.
        for (const auto & h : active_.hugeBlocks) {
            if (cp >= h.begin && cp < h.end) return h.begin;
        }
        return nullptr;
    }

    /// Stage 6 Phase 3.5: forward search for the next cell-start
    /// strictly AFTER `cellStart`.  Returns the next cell-start
    /// address, or the block end if no more cell-starts exist.
    /// Used by conservative cell-walk to determine cell size.
    const char * findNextCellStartOrBlockEnd(const char * cellStart) const noexcept
    {
        if (!cellStart) return nullptr;
        {
            const long bi = blockIndexContaining(cellStart);
            if (bi >= 0) {
                const size_t i = static_cast<size_t>(bi);
                const char * blk = active_.blocks[i];
                const size_t startOffset =
                    static_cast<size_t>(cellStart - blk);
                size_t bit = startOffset / 16 + 1;  // search AFTER
                size_t word = bit / 64;
                const auto & bits = active_.cellStarts[i];
                // Check current word from bitInWord upward.
                if (word < bits.size()) {
                    const size_t bitInWord = bit % 64;
                    uint64_t mask = (bitInWord == 0)
                        ? uint64_t(-1)
                        : ~((uint64_t(1) << bitInWord) - 1);
                    uint64_t masked = bits[word] & mask;
                    if (masked) {
                        int lo = __builtin_ctzll(masked);
                        size_t foundBit = word * 64 + size_t(lo);
                        return blk + (foundBit * 16);
                    }
                }
                // Scan later words.
                for (size_t w = word + 1; w < bits.size(); ++w) {
                    if (bits[w] == 0) continue;
                    int lo = __builtin_ctzll(bits[w]);
                    size_t foundBit = w * 64 + size_t(lo);
                    return blk + (foundBit * 16);
                }
                // Past last cell-start: cell extends to block end
                // (active.cur for current block, kBlockSize for older).
                if (i + 1 == active_.blocks.size() && active_.cur)
                    return active_.cur;
                return blk + kBlockSize;
            }
        }
        // Huge: extends to its end.
        for (const auto & h : active_.hugeBlocks) {
            if (cellStart >= h.begin && cellStart < h.end) return h.end;
        }
        return nullptr;
    }

    /// Stage 6 Phase 3.5: arena bounds for fast filtering during
    /// conservative C-stack scan.  Returns [min, max) covering all
    /// active blocks + huge blocks; caller can range-check candidates
    /// before doing the precise `inActive` walk.
    void activeBounds(uintptr_t & minOut,
                      uintptr_t & maxOut) const noexcept
    {
        uintptr_t mn = UINTPTR_MAX;
        uintptr_t mx = 0;
        for (const char * blk : active_.blocks) {
            uintptr_t b = reinterpret_cast<uintptr_t>(blk);
            if (b < mn) mn = b;
            if (b + kBlockSize > mx) mx = b + kBlockSize;
        }
        for (const auto & h : active_.hugeBlocks) {
            uintptr_t b = reinterpret_cast<uintptr_t>(h.begin);
            uintptr_t e = reinterpret_cast<uintptr_t>(h.end);
            if (b < mn) mn = b;
            if (e > mx) mx = e;
        }
        if (mn == UINTPTR_MAX) { mn = 0; mx = 0; }
        minOut = mn;
        maxOut = mx;
    }

    /// Stage 6 Phase 3: per-exact-size free list, populated by sweep
    /// + popped by `alloc()` slow path.  Keyed by exact byte size
    /// (16-byte-aligned) — sweep records the precise size of each
    /// dead cell (computed from cell-start bitmap gaps); allocator
    /// looks up by request size for exact-fit reuse.
    ///
    /// Bytes added to the free list stay PHYSICALLY in their original
    /// arena block.  Reuse pops a `void *` and the alloc returns it
    /// (re-setting the cell-start bit at that offset).  No block
    /// freeing happens here — the arena's totalBytes counter stays
    /// the same; what changes is that future allocs draw from the
    /// free list instead of bumping cur forward.
    ///
    /// Lifetime: persistent across GC cycles.  Each sweep adds dead
    /// cells; each alloc that hits the free list removes them.
    /// MIDEVAL_GC_DESIGN_2026-06-22 (reuse-SEGV fix, 2026-06-23): drop ALL
    /// free-list entries.  Called at the start of each mid-eval sweep so the bins
    /// are REBUILT fresh from the current cell-start bitmap — never carrying a
    /// stale entry from a prior sweep whose cell-start configuration differed (the
    /// overlapping-entry corruption: a region binned as one big cell in sweep N,
    /// then sub-divided by sweep N+1, leaving sweep N's oversized entry spanning a
    /// now-live neighbour).  Safe: live cells are never in the bins; dead cells
    /// keep their start bits (the sweep no longer clears them) so the next sweep
    /// re-bins them — nothing is lost.
    void clearFreeListBins() noexcept
    {
        freeListBins_.clear();
        freeListEntries_ = 0;
        binnedDbg_.clear();
    }
    void freeListAdd(void * p, size_t bytes) noexcept
    {
        if (__builtin_expect(detail::g_midEvalPoison, 0)) {
            // RCA: detect DOUBLE-BIN (same address added to a bin twice without an
            // intervening pop) + record the size to spot cross-size staleness.
            auto dit = binnedDbg_.find(p);
            if (dit != binnedDbg_.end()) {
                std::fprintf(stderr,
                    "[mideval-rca] DOUBLE-BIN: p=%p already in bin[%zu], re-added to "
                    "bin[%zu] type=%d — stale free-list entry (bins never cleared)\n",
                    p, dit->second, bytes, (int)cellTypeAt(p));
                std::abort();
            }
            binnedDbg_[p] = bytes;
            *reinterpret_cast<uint64_t *>(p) = 0xDEADBEEFCAFEF00DULL;  // sentinel
        }
        freeListBins_[bytes].push_back(p);
        ++freeListEntries_;
    }
    void * freeListTryPop(size_t bytes) noexcept
    {
        auto it = freeListBins_.find(bytes);
        if (it == freeListBins_.end() || it->second.empty()) return nullptr;
        void * p = it->second.back();
        it->second.pop_back();
        --freeListEntries_;
        if (__builtin_expect(detail::g_midEvalPoison, 0)) {
            binnedDbg_.erase(p);
            if (*reinterpret_cast<uint64_t *>(p) != 0xDEADBEEFCAFEF00DULL) {
            // RCA: find the nearest SET cell-start at/below p-16.  If it's a
            // nearby cell (esp. a string) that p falls inside, p is INTERIOR to a
            // live cell (spurious-bit split); if it's p itself / far, p is a
            // standalone MISSED live cell.
            const char * below =
                findContainingCellStart(static_cast<const char *>(p) - 16);
            long dist = below ? (long)((const char *)p - below) : -1;
            std::fprintf(stderr,
                "[mideval-poison] binned-then-mutated: p=%p type=%d size=%zu "
                "word0=0x%016llx | nearest-start-below=%p dist=%ld belowType=%d\n",
                p, (int)cellTypeAt(p), bytes,
                (unsigned long long)*reinterpret_cast<uint64_t *>(p),
                (const void *)below, dist,
                below ? (int)cellTypeAt(below) : -1);
            std::abort();
            }
        }
        // Re-set the cell-start bit at this address (sweep cleared
        // it when adding to free list).  Slow path: linear-scan
        // blocks to find owner.  Only called on free-list pop,
        // which is rare relative to bump-allocations.
        setCellStartBitFor(p);
        // Phase 3 reuse-safety (2026-05-28): zero the cell so it
        // matches the calloc-zero-init guarantee of bump-allocated
        // fresh cells.  Without this, popped cells carry STALE bytes
        // from prior use — allocClosure/allocBindings/etc. only write
        // a few header fields (size/state/nUpvalues), expecting other
        // fields (kind, parent, cell, shapeCell, upvalues[], entries[])
        // to be zero from calloc.  With reuse those would be garbage,
        // causing spurious chain walks (Bindings::Kind::Chain from
        // stale `kind` byte), bogus thunk states, bad slot pointers.
        std::memset(p, 0, bytes);
        return p;
    }
    size_t freeListEntryCount() const noexcept { return freeListEntries_; }

    /// Phase 3.8 (2026-05-28): free a fully-dead arena block back to
    /// libc.  Called by sweep when a block has zero live cells.
    /// Returns the block's bytes that were returned to libc (kBlockSize).
    ///
    /// Removes the block from `active_.blocks`, its corresponding
    /// `cellStarts` bitmap entry, and any pending free-list entries
    /// that point into the block.  Updates `totalBytes` accordingly.
    ///
    /// Pre-condition: caller has verified no live cells nor any mark
    /// bits in the block (no Tag::Slot targets, etc.).  Sweep must
    /// have removed all cell-start bits from this block before
    /// calling.
    ///
    /// O(blocks) due to the linear-scan for the block index + an
    /// O(freeList) pass to filter out entries.  Called once per
    /// freeable block per GC, so amortised cost is low.
    size_t freeWholeBlock(const char * blockStart) noexcept
    {
        // 1. Find block index.
        size_t idx = active_.blocks.size();
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            if (active_.blocks[i] == blockStart) {
                idx = i;
                break;
            }
        }
        if (idx == active_.blocks.size()) return 0;  // not found

        // 2. Remove free-list entries that point into this block.
        for (auto & [sz, vec] : freeListBins_) {
            auto newEnd = std::remove_if(vec.begin(), vec.end(),
                [blockStart](void * p) {
                    const char * cp = static_cast<const char *>(p);
                    return cp >= blockStart
                        && cp < blockStart + kBlockSize;
                });
            const size_t removed = static_cast<size_t>(vec.end() - newEnd);
            vec.erase(newEnd, vec.end());
            freeListEntries_ -= removed;
        }

        // 3. GC_remove_roots + munmap the block bytes.  R2.0: munmap
        //    (NOT std::free) is the only primitive that returns the
        //    bytes to RSS on macOS (R1 spike).  freeWholeBlock only
        //    runs under the major-GC gate, where refill() mmap'd the
        //    block, so munmap is always the correct inverse here.
#if NIX_USE_BOEHMGC
        if (!arenaNorootEnabled())
            GC_remove_roots(const_cast<char *>(blockStart),
                            const_cast<char *>(blockStart) + kBlockSize);
#endif
        unmapArenaBlock(const_cast<char *>(blockStart), kBlockSize);

        // 4. Remove from active_.blocks + parallel cellStarts +
        //    parallel lineMarks (Step 11′ Immix, 2026-05-29).
        active_.blocks.erase(active_.blocks.begin() + idx);
        active_.cellStarts.erase(active_.cellStarts.begin() + idx);
        if (idx < active_.lineMarks.size()) {
            active_.lineMarks.erase(active_.lineMarks.begin() + idx);
        }
        // R2.1′: keep the parallel cellTypes array in sync with blocks.
        if (idx < active_.cellTypes.size()) {
            active_.cellTypes.erase(active_.cellTypes.begin() + idx);
        }
        // Review #14: freeSpans is ALSO parallel to blocks (freeSpans[i] is
        // blocks[i]'s span list).  Erasing the block without erasing here
        // length-skews freeSpans so freeSpans[i] no longer matches
        // blocks[i]; the Immix span allocator (immixAdvanceToNextSpan) would
        // then read spans pointing into this now-munmapped block -> wild
        // write / SIGSEGV before the next rebuildFreeSpansFromLineMarks.
        if (idx < active_.freeSpans.size()) {
            active_.freeSpans.erase(active_.freeSpans.begin() + idx);
        }
        // The Immix cursor may index a block at/after `idx`; invalidate it
        // so a stale (now-shifted) immixCurBlockIdx_ can't bump into the
        // wrong / freed block before the post-GC span rebuild.
        immixCur_ = nullptr;
        immixEnd_ = nullptr;
        sortedBlocksDirty_ = true;  // Lever 1: block set changed (indices shifted)

        // 5. Update totalBytes + cur/end if we freed the current
        //    block.  After freeing, the next alloc will refill (since
        //    cur points to a now-invalid address).
        active_.totalBytes -= kBlockSize;
        if (active_.cur >= blockStart
            && active_.cur < blockStart + kBlockSize)
        {
            active_.cur = nullptr;
            active_.end = nullptr;
        }

        return kBlockSize;
    }

    /// Review #4: snapshot of (begin, byte-size) for every huge block, so
    /// the major GC can check marks + reclaim the dead ones.  Returns a
    /// COPY (freeHugeBlock mutates the live vector).
    std::vector<std::pair<const char *, size_t>> hugeBlockRanges() const
    {
        std::vector<std::pair<const char *, size_t>> r;
        r.reserve(active_.hugeBlocks.size());
        for (const auto & h : active_.hugeBlocks)
            r.emplace_back(h.begin, static_cast<size_t>(h.end - h.begin));
        return r;
    }

    /// Review #4: free a dead huge block.  Uses the inverse of the alloc
    /// primitive keyed on the SAME process-wide majorGcEnabled() gate
    /// (munmap for the mmap'd GC path → RSS return; std::free otherwise),
    /// GC_remove_roots if registered, and drops it from hugeBlocks.
    /// Returns bytes freed (0 if `begin` is not a current huge block).
    size_t freeHugeBlock(const char * begin) noexcept
    {
        for (size_t i = 0; i < active_.hugeBlocks.size(); ++i) {
            if (active_.hugeBlocks[i].begin != begin) continue;
            const size_t sz = static_cast<size_t>(
                active_.hugeBlocks[i].end - active_.hugeBlocks[i].begin);
#if NIX_USE_BOEHMGC
            if (!arenaNorootEnabled())
                GC_remove_roots(const_cast<char *>(begin),
                                const_cast<char *>(begin) + sz);
#endif
            if (majorGcEnabled())
                unmapArenaBlock(const_cast<char *>(begin), sz);
            else
                std::free(const_cast<char *>(begin));
            active_.totalBytes -= sz;
            active_.hugeBlocks.erase(active_.hugeBlocks.begin() + i);
            return sz;
        }
        return 0;
    }

    /// Stage 6 Phase 3: sweep helper — clear a cell-start bit (when
    /// a dead cell is added to the free list, the slot is no longer
    /// a cell-start until reused).
    void clearCellStartBitFor(const void * p) noexcept
    {
        if (!p) return;
        const char * cp = static_cast<const char *>(p);
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            const char * blk = active_.blocks[i];
            if (cp >= blk && cp < blk + kBlockSize) {
                const size_t offset = static_cast<size_t>(cp - blk);
                const size_t bit  = offset >> 4;
                const size_t word = bit >> 6;
                if (word < active_.cellStarts[i].size())
                    active_.cellStarts[i][word] &= ~(1ULL << (bit & 63));
                return;
            }
        }
    }

    /// MIDEVAL_GC_DESIGN_2026-06-22: clear stale cell-start bits in the INTERIOR
    /// granules of a freshly-allocated cell `[p+16, p+bytes)`.  THE reuse-SEGV fix
    /// (RCA 2026-06-23): a multi-granule cell whose region previously held a
    /// smaller cell can carry a stale interior cell-start bit (+ stale type) from
    /// that prior occupant.  The non-moving sweep iterates cell-starts in address
    /// order, so a spurious interior bit splits this live cell: it sees the real
    /// start (marked → live, but its byte-0 mark doesn't cover the interior),
    /// computes the cell as ending at the spurious bit, then treats `[interior,…)`
    /// as a SEPARATE dead cell → bins + reuses it → corrupts THIS live cell (the
    /// "type=Bindings, content=string" poison hit).  Clearing the interior on
    /// alloc guarantees the sweep sees one cell.  Gated (cellMetaEnabled); only
    /// touches this cell's own granules.
    void clearInteriorCellStarts(void * p, size_t bytes) noexcept
    {
        if (!cellMetaEnabled() || !p || bytes <= 16) return;
        bytes = (bytes + 15) & ~size_t{15};               // round to cell granularity
        const char * cp = static_cast<const char *>(p);
        const long bi = blockIndexContaining(cp);
        if (bi < 0) return;                                // huge/external: single cell
        const size_t i = static_cast<size_t>(bi);
        if (i >= active_.cellStarts.size()) return;
        const char * blk = active_.blocks[i];
        const size_t base = static_cast<size_t>(cp - blk);
        auto & bits = active_.cellStarts[i];
        for (size_t off = base + 16; off < base + bytes; off += 16) {
            const size_t bit = off >> 4, word = bit >> 6;
            if (word < bits.size()) bits[word] &= ~(1ULL << (bit & 63));
        }
    }

    /// Total bytes pinned by all blocks the arena has ever
    /// allocated.  Cheap to read; useful for the alloc-stats dump.
    size_t bytesAllocated() const noexcept { return active_.totalBytes; }

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
        r.reserve(active_.blocks.size() + active_.hugeBlocks.size());
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            const char * b = active_.blocks[i];
            const char * e = (b == (active_.cur ? active_.blocks.back() : nullptr) && i + 1 == active_.blocks.size())
                ? active_.cur : b + kBlockSize;
            // Defensive: if cur is null (no allocations yet), use full block.
            if (!active_.cur && i + 1 == active_.blocks.size()) e = b + kBlockSize;
            r.push_back({b, e});
        }
        // Huge allocations: each is fully used (allocator does the
        // entire calloc'd region as one object), so begin..end is
        // the whole block.
        for (const auto & h : active_.hugeBlocks) {
            r.push_back({h.begin, h.end});
        }
        return r;
    }

    /// Stage 6 Day 6: pointer-classification enum.  Returned by
    /// `regionOf(p)` to indicate whether a raw pointer falls in:
    ///   * Active: a live cell in the arena
    ///   * External: not in the arena — could be libc-malloc
    ///     (CompilationUnit), Boehm (TW nix::Value*), nursery, or
    ///     a stack address.  GC skips.
    ///
    /// 2026-05-28: Backup region retired with the Cheney scavenger
    /// (per GC_DESIGN_POST_CHENEY_2026-05-28.md §4.6 + §6.1).  Flat
    /// MS doesn't move cells, so a separate destination region is
    /// unneeded.  Two-state enum (External/Active) kept for ABI
    /// compatibility with callers that switch on RegionKind.
    enum class RegionKind : uint8_t {
        External = 0,
        Active   = 1,
    };

    /// Classify `p` against the arena's region boundaries.  Cheap
    /// per-pointer test: linear walks the blocks vectors (O(N) in
    /// block count, ~N is small — 16 MB blocks, 587 MB arena = ~36
    /// blocks max on the canonical workloads).
    RegionKind regionOf(const void * p) const noexcept
    {
        if (!p) return RegionKind::External;
        const char * cp = static_cast<const char *>(p);
        // P-7 (CODEBASE_REVIEW_2026-06-11): O(log blocks) via the sorted block
        // index, not an O(blocks) linear scan per visited pointer.  The mark
        // phase calls this per edge (tryMark→inActive→regionOf), so the old
        // linear scan was O(edges × blocks) — M5 has 424 regular blocks and
        // mark ran ~11.6 s.  The sortedBlocks_ cache is the same one
        // findContainingCellStart already trusts in production; its dirty flag
        // is set on every block add/free, and mark doesn't allocate, so the
        // (at most one) rebuild amortises across all of mark's lookups.
        if (blockIndexContaining(cp) >= 0) return RegionKind::Active;
        // Huge blocks are few (one cell each); a linear scan is fine.
        for (const auto & h : active_.hugeBlocks) {
            if (cp >= h.begin && cp < h.end) return RegionKind::Active;
        }
        return RegionKind::External;
    }

    /// Convenience check: "is `p` an arena-resident cell?"
    bool inActive(const void * p) const noexcept
    {
        return regionOf(p) == RegionKind::Active;
    }

private:
    /// Stage 6 Day 1: active region — the only region used by
    /// alloc()/refill()/blockRanges() in current single-region mode.
    Region active_;

    /// Stage 6 Phase 3: free-list bins keyed by exact (16-byte-
    /// aligned) byte size.  Populated by sweep; popped by
    /// alloc()'s slow path.  Lifetime: persistent across GC cycles.
    ///
    /// SCHEDULED FOR RETIREMENT after Step 12′ Immix path validates
    /// (see GC_DECISION_2026-05-29 §6 + IMMIX_LINE_MARK_2026-05-29).
    /// Kept for one validation cycle so V3_DBG_IMMIX_ALLOC=0 falls
    /// back gracefully.
    std::unordered_map<size_t, std::vector<void *>> freeListBins_;
    // RCA only (NIX_V3_MIDEVAL_POISON): address → bin-size of currently-binned
    // cells, to detect double-bin / cross-size staleness.  Never touched when the
    // poison gate is off.
    std::unordered_map<void *, size_t> binnedDbg_;
    size_t freeListEntries_ = 0;

    /// Step 12′ (Immix, 2026-05-29) — line-region allocator state.
    /// Updated by `Arena::alloc()` to track the current bump pointer
    /// within the active free span; reset by
    /// `rebuildFreeSpansFromLineMarks()` at end of each major GC.
    ///
    /// All four fields are nullptr/0 when Immix path is unused
    /// (V3_DBG_IMMIX_ALLOC unset OR major-GC gate off OR no GC
    /// fired yet).
    size_t immixCurBlockIdx_ = 0;
    size_t immixCurSpanIdx_  = 0;
    char * immixCur_         = nullptr;
    char * immixEnd_         = nullptr;

    /// Slow-path helper used on free-list pop to re-set the cell-
    /// start bit at `p`'s offset.
    void setCellStartBitFor(const void * p) noexcept
    {
        if (!p) return;
        const char * cp = static_cast<const char *>(p);
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            const char * blk = active_.blocks[i];
            if (cp >= blk && cp < blk + kBlockSize) {
                const size_t offset = static_cast<size_t>(cp - blk);
                const size_t bit  = offset >> 4;
                const size_t word = bit >> 6;
                if (word < active_.cellStarts[i].size())
                    active_.cellStarts[i][word] |= 1ULL << (bit & 63);
                return;
            }
        }
    }

    /// Review #13: stamp the CellType for a cell handed out by the
    /// free-list reuse path (linear-scan sibling of setCellStartBitFor).
    /// Stamps unconditionally (incl. None) to overwrite the prior
    /// occupant's stale type on the reused granule.
    void setCellTypeFor(const void * p, CellType type) noexcept
    {
        if (!cellMetaEnabled() || !p) return;
        const char * cp = static_cast<const char *>(p);
        for (size_t i = 0; i < active_.blocks.size(); ++i) {
            const char * blk = active_.blocks[i];
            if (cp >= blk && cp < blk + kBlockSize) {
                if (i >= active_.cellTypes.size()) return;
                const size_t bit = static_cast<size_t>(cp - blk) >> 4;
                // M-9: nibble-packed write — unconditional (incl. None).
                cellTypePack(active_.cellTypes[i], bit, type);
                return;
            }
        }
    }

    // R2.0 (2026-06-02): block-level page-release primitive.  R1
    // (bench/page-release-spike.cc) proved that on macOS aarch64 ONLY
    // munmap returns freed bytes to RSS — std::free of a calloc'd 16 MB
    // block returns 0% (libmalloc caches large frees in its magazines)
    // and madvise(MADV_DONTNEED/FREE) is a no-op for anonymous RSS.  So
    // under the major-GC gate the arena allocates regular blocks via
    // mmap (page-aligned, zero-filled like calloc) and `freeWholeBlock`
    // releases them via munmap.  Gated on majorGcEnabled() so the
    // default (no-GC) path is byte-for-byte the prior calloc behaviour.
    static char * mapArenaBlock(size_t bytes) noexcept
    {
        void * p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
        return (p == MAP_FAILED) ? nullptr : static_cast<char *>(p);
    }
    static void unmapArenaBlock(void * p, size_t bytes) noexcept
    {
        if (p) ::munmap(p, bytes);
    }

public:
    /// R2.4b: force the next allocation to start a fresh block.  Used by
    /// evacuation so dest copies never bump into a candidate (sparse,
    /// about-to-be-freed) block — they go into a brand-new block that is
    /// not in the candidate set.  Cheap: one refill (wastes the current
    /// block's tail, reclaimed by a later GC).
    void forceFreshBlock() noexcept
    {
        refill();
        // Review #15: refill() resets only the BUMP cursor; the Immix
        // span cursor (immixCur_/immixEnd_) is left pointing into a
        // SURVIVING block's leftover span from the previous GC's rebuild.
        // Since alloc() tries the immix path FIRST, evac dest copies would
        // otherwise land in that surviving block, violating the "dest is
        // always a brand-new non-candidate block" invariant evac relies on.
        // Null the cursor so the next alloc falls through to the fresh
        // bump block; rebuildFreeSpansFromLineMarks repopulates it post-GC.
        immixCur_ = nullptr;
        immixEnd_ = nullptr;
    }

private:
    void refill() noexcept
    {
        // Phase-13 review HIGH-6 fix: zero-fill the block before
        // GC_add_roots.  Boehm scans every word in the registered
        // region; OS-recycled garbage often contains pointer-shaped
        // bit patterns that pin Boehm-managed objects until process
        // exit (phantom retention scaling with arena lifetime).
        // mmap(MAP_ANON) (GC path) and calloc (default path) both hand
        // back zero pages directly from the kernel.
        char * blk = majorGcEnabled()
            ? mapArenaBlock(kBlockSize)
            : static_cast<char *>(std::calloc(1, kBlockSize));
        // Review #3: mapArenaBlock (mmap) / calloc return nullptr on OOM /
        // address-space exhaustion.  refill() is noexcept and MUST NOT set
        // the bump cursor to a null base — the next alloc would bump from
        // ~0 and write through it (SIGSEGV in low memory) and the null
        // would propagate into sortedBlocks_ / cellTypeAt indexing.  Fail
        // loudly + cleanly (the higher-level NIX_V3_MAX_HEAP guard normally
        // trips first; this is the last-resort allocator backstop).
        if (__builtin_expect(!blk, 0)) {
            std::fprintf(stderr,
                "v3 fatal: arena block allocation failed (%zu MB request) "
                "— out of memory / address space exhausted\n",
                size_t(kBlockSize) >> 20);
            std::abort();
        }
        active_.blocks.push_back(blk);
        sortedBlocksDirty_ = true;  // Lever 1: block set changed
        active_.cur = blk;
        active_.end = blk + kBlockSize;
        active_.totalBytes += kBlockSize;
        // Stage 6 Phase 2: extend cell-start bitmap if major-GC gate
        // is on.  Per-block bitmap = (kBlockSize / 16 / 64) uint64s
        // = 16384 words = 128 KB per 16 MB block.
        // Step 11′ (Immix, 2026-05-29): in parallel, extend the
        // line-mark bitmap by kLineU64sPerBlock words (2048 = 16 KB
        // per 16 MB block).  Cleared at start of each mark phase by
        // Arena::clearAllLineMarks().
        if (cellMetaEnabled()) {
            active_.cellStarts.emplace_back(
                kBlockSize / 16 / 64, 0ULL);
            active_.lineMarks.emplace_back(
                kLineU64sPerBlock, 0ULL);
            // R2.1′ + M-9: nibble-packed type array — half a byte per
            // 16-byte granule (512 KB/block, was 1 MB).  See cellTypeBytes /
            // cellTypePack / cellTypeUnpack.
            active_.cellTypes.emplace_back(
                cellTypeBytes(kBlockSize / 16), uint8_t(CellType::None));
        }
#if NIX_USE_BOEHMGC
        // WC-13: tell Boehm to scan this block for pointers to GC
        // memory.  Bridge thunks store raw `nix::Value *`; without
        // this they become invisible to the collector and the values
        // they point to may be reclaimed mid-evaluation.
        // GC_add_roots is idempotent over overlapping regions and
        // safe to call concurrently — the underlying mutex is held
        // for a short string of pointer arithmetic.
        //
        // Arena deregistration gate (per ARENA_DEREGISTRATION_DESIGN
        // _2026-05-27): NIX_V3_ARENA_NOROOT=1 skips this call.
        // Bridge sources are tracked separately via the bridge-root
        // registry; see allocBridgeThunk + bridge_root_registry.hh.
        // Tag::External audit (bench/arena-dereg-audit.sh PASS on
        // hello + firefox + HNE + ackermann) confirms no other
        // Boehm-managed pointers persist in arena cells.
        if (!arenaNorootEnabled())
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

// M-8 (CODEBASE_REVIEW_2026-06-11): the `detail::g_cellEverywhere` env-gate
// cache was REMOVED — its sole consumer (allocThunkSuspended's shapeCell
// pre-allocation) is gone with the Thunk::shapeCell field.

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
    static void * nurseryOrArena(size_t bytes, CellType type = CellType::None) noexcept
    {
        if (void * p = threadNursery().tryAlloc(bytes)) return p;
        return threadArena().alloc(bytes, type);
    }

    static Value * allocValue() noexcept
    {
        V3_STATS_BUMP(bytesValues, sizeof(Value));
        return static_cast<Value *>(
            threadArena().alloc(sizeof(Value), CellType::Value));
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
        auto * c = static_cast<Closure *>(nurseryOrArena(bytes, CellType::Closure));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->capturedWiths = nullptr;
        c->cu = nullptr;
        c->upvalEnv = nullptr;   // env-sharing: inline-FAM path until a gate builds an Env
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
        auto * c = static_cast<Closure *>(
            threadArena().alloc(bytes, CellType::Closure));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->capturedWiths = nullptr;
        c->cu = nullptr;
        c->upvalEnv = nullptr;   // env-sharing: inline-FAM path until a gate builds an Env
        closureAllocSiteRecord(c, file, line, nUpvalues);
        return c;
    }

    /// Allocate a Suspended thunk with `nUpvalues` captured upvalues
    /// stored in the FAM tail.
    ///
    /// `file` / `line` default to the caller's site via `__builtin_FILE`
    /// + `__builtin_LINE`.  Used by T1.3 per-Thunk attribution
    /// (NIX_V3_THUNKS_ATTR=1).  Zero cost when the gate is off.
    // FP-2b: `reserveWithsSlot` adds one trailing 8 B slot at tail[nUpvalues]
    // for the capturedWiths ListVec*.  OP_MAKE_THUNK passes `willHaveWiths`
    // (computed before this call) which EXACTLY predicts capturedWiths!=null, so
    // ~74-97% of thunks pass false and pay nothing (24 B header, no slot).
    static Thunk * allocThunkSuspended(uint16_t nUpvalues,
                                        bool reserveWithsSlot = false,
                                        const char * file = __builtin_FILE(),
                                        uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(Thunk)
            + sizeof(Value) * nUpvalues
            + (reserveWithsSlot ? sizeof(Value) : 0);  // FP-2b withs slot
        V3_STATS_BUMP(bytesThunks, bytes);
        auto * t = static_cast<Thunk *>(nurseryOrArena(bytes, CellType::Thunk));
        t->state = ThunkState::Suspended;
        t->hasWithsSlot = reserveWithsSlot ? 1 : 0;  // FP-2b (was _pad0)
        t->nUpvalues = nUpvalues;
        t->forces = 0;
        t->cell = nullptr;
        // M-8 (CODEBASE_REVIEW_2026-06-11): the shapeCell field + its
        // NIX_V3_CELL_EVERYWHERE pre-allocation block were REMOVED.  The
        // experiment was default-off (shapeCell never non-null in production),
        // so dropping it is byte-identical for prod and reclaims 8 B/thunk.
        // FP-2b: zero the withs slot up front so a GC fired between this alloc
        // and the OP_MAKE_THUNK store sees a null (not garbage) ListVec*.  The
        // upvalue tail[0..nUpvalues) is filled by the maker; the slot sits past
        // it at tail[nUpvalues].
        if (reserveWithsSlot) thunkSetCapturedWiths(t, nullptr);
        // FP-2a (2026-06-14): `suspended.cu` removed (derived via thunkCU from
        // desc->cu, set at OP_MAKE_THUNK).  No per-thunk init needed.
        // T1.3: record allocation origin under NIX_V3_THUNKS_ATTR=1.
        // Forward declaration: thunkAllocSiteRecord is defined further
        // down in this header (needs <unordered_map>); the call site
        // here resolves to the inline noexcept body at compile time.
        thunkAllocSiteRecord(t, file, line, nUpvalues);
        return t;
    }

    /// env-sharing (NIX_V3_ENV_SHARING): Suspended thunk whose upvalues live in a
    /// shared Env (tail[0] holds the Env*, filled by the caller) instead of inline
    /// in the tail.  Tail is a fixed [Env* @ 0] + [withs @ 1 iff reserveWithsSlot]
    /// — independent of the logical nUpvalues.  Header stays 24 B (the ENV_SHARED
    /// flag bit in hasWithsSlot drives thunkScanSize/GC/accessor layout).
    static Thunk * allocThunkSuspendedShared(uint16_t nUpvalues,
                                             bool reserveWithsSlot = false,
                                             const char * file = __builtin_FILE(),
                                             uint32_t     line = __builtin_LINE()) noexcept
    {
        const size_t bytes = sizeof(Thunk)
            + sizeof(Value) * (1 + (reserveWithsSlot ? 1 : 0));
        V3_STATS_BUMP(bytesThunks, bytes);
        auto * t = static_cast<Thunk *>(nurseryOrArena(bytes, CellType::Thunk));
        t->state = ThunkState::Suspended;
        t->hasWithsSlot = static_cast<uint8_t>(
            THUNK_ENV_SHARED | (reserveWithsSlot ? THUNK_WITHS_SLOT : 0));
        t->nUpvalues = nUpvalues;
        t->forces = 0;
        t->cell = nullptr;
        *reinterpret_cast<Env **>(&t->tail[0]) = nullptr;  // Env* slot; caller fills
        if (reserveWithsSlot) thunkSetCapturedWiths(t, nullptr);  // tail[1]
        thunkAllocSiteRecord(t, file, line, nUpvalues);
        return t;
    }

    // (allocBridgeThunk retired; TW_VALUE_ERADICATION F4, 2026-06-02.)

    static Env * allocEnv(uint16_t nValues) noexcept
    {
        const size_t bytes = sizeof(Env) + sizeof(Value) * nValues;
        V3_STATS_INC(envsAllocated);
        V3_STATS_BUMP(bytesEnvs, bytes);
        auto * e = static_cast<Env *>(threadArena().alloc(bytes, CellType::Env));
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
        auto * l = static_cast<ListVec *>(nurseryOrArena(bytes, CellType::List));
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
    ///
    /// M-11 (CODEBASE_REVIEW_2026-06-11) FALSIFIED standalone — do NOT split
    /// into 24 B (App/PrimOpApp: left+right+evaluated) vs 32 B (App3: +third)
    /// size classes here.  `alloc()` rounds every request UP to a 16 B
    /// boundary (alloc.hh: `bytes = (bytes + 15) & ~15`) AND cell-starts are
    /// tracked per-16-B granule, so a 24 B pair occupies a full 32 B cell
    /// (the next cell cannot start before the following 16 B boundary) —
    /// `alloc(24)` and `alloc(32)` both yield 32 B.  The split therefore saves
    /// ZERO bytes until the arena gains 8 B-granular cell-start tracking
    /// (doubling the cellStarts/cellTypes bitmaps + changing kAlign), which is
    /// exactly the coordinated allocator/repr change the review means by
    /// "fold into the next repr change."  The App3-vs-App discriminant the
    /// split would need (a CellType::Pair3 routed through walkPair/walkPair3 +
    /// the gc.cc evac + barrier) is real engineering; spending it for a 0-byte
    /// win is a measure-twice anti-pattern.  Revisit ONLY alongside that
    /// allocator-granularity change.
    static ValuePair * allocPair(const char * file = __builtin_FILE(),
                                  uint32_t     line = __builtin_LINE()) noexcept
    {
        V3_STATS_INC(pairsAllocated);
        V3_STATS_BUMP(bytesPairs, sizeof(ValuePair));
        auto * p = static_cast<ValuePair *>(
            threadArena().alloc(sizeof(ValuePair), CellType::Pair));
        // 2026-05-30: explicitly zero-init `evaluated` and `third`
        // slots.  Many callers (App, PrimOpApp) set only `left` and
        // `right`, relying on the other slots being Tag::Uninitialized.
        // That worked while arena allocs always came from calloc'd
        // fresh blocks; once free-list / recycle reuses cells (Step
        // 13′ / brute-mode scavenge stress), reused cells may contain
        // garbage in unwritten slots.  Garbage in `third` causes
        // walkers/auditors to dereference random pointers → SIGSEGV.
        // Zero-init costs ~2 ns per pair (16 B write to evaluated +
        // 16 B to third).
        p->evaluated.mkUninitialized();  // zeroes tag + payload (16 B)
        p->third.mkUninitialized();
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
    /// Step 18 of post-Phase-3.8 plan (2026-05-29): per-call-site
    /// attribution.  Defaulted file/line via __builtin_FILE/__builtin_LINE
    /// — zero cost when gate OFF (compile-time constants discarded).
    /// Mirrors the T1.3 #746 pattern used for Bindings origin.
    static char * allocChars(size_t n,
                             const char * file = __builtin_FILE(),
                             uint32_t     line = __builtin_LINE()) noexcept
    {
        V3_STATS_BUMP(bytesChars, n);
        // Per-call-site attribution when NIX_V3_STRINGS_ATTR=1.
        if (__builtin_expect(detail::g_stringsAttrEnabled, 0)) {
            recordAllocCharsSite(file, line, n);
        }
        return static_cast<char *>(threadArena().alloc(n, CellType::Chars));
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

    // Phase C revival attempt #4 (2026-05-30): per explicit user
    // direction "Continue Phase C", added `allocChainBindings`
    // helper with caller wired in mergeBindings (gated
    // NIX_V3_CHAIN_BINDINGS=1, default OFF).  Prior 3 attempts
    // falsified per vm.cc:1167-1210 inline ledger.  This attempt
    // adds diagnostic instrumentation (NIX_V3_CHAIN_DBG=1 logs every
    // chain construction + every iteration-site that hits a Chain
    // without materialize) to narrow the failing pattern.
    static Bindings * allocChainBindings(const Bindings * parent,
                                          uint32_t overlaySize,
                                          const char * file = __builtin_FILE(),
                                          uint32_t     line = __builtin_LINE()) noexcept
    {
        V3_STATS_INC(attrsetsAllocated);
        size_t bytes = sizeof(Bindings) + overlaySize * sizeof(Bindings::Entry);
        V3_STATS_BUMP(bytesBindings, bytes);
        auto * b = static_cast<Bindings *>(
            threadArena().alloc(bytes, CellType::Bindings));
        b->kind = uint8_t(Bindings::Kind::Chain);
        b->_pad8[0] = b->_pad8[1] = b->_pad8[2] = 0;
        b->size = overlaySize;
        b->parent = parent;
        b->aux.mkUninitialized();
        // Histogram bucketing same as Sorted.
        V3_STATS_BLOCK {
            auto & buckets = allocStats().attrsetSizeBuckets;
            uint32_t n = overlaySize;
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
            bindingsAllocSiteRecord(b, file, line);
        }
        return b;
    }

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
        auto * b = static_cast<Bindings *>(
            threadArena().alloc(bytes, CellType::Bindings));
        // Phase 3 reuse-safety (2026-05-28): allocBindings used to
        // rely on calloc-zero-init of fresh arena blocks to give us
        // kind=Sorted (=0) + _pad8=0 + parent=nullptr.  With free-list
        // reuse, popped cells have STALE bytes — kind could be
        // Kind::Chain, parent could be a garbage pointer.  lookup()
        // then walks the spurious chain via b->parent and SIGSEGVs.
        //
        // Initialize the header explicitly so the Bindings is in a
        // known-good state regardless of underlying memory's prior
        // history.  Entries[] are still NOT initialized — callers
        // MUST fill all n entries (existing contract).
        b->kind = uint8_t(Bindings::Kind::Sorted);
        b->_pad8[0] = b->_pad8[1] = b->_pad8[2] = 0;
        b->size = n;
        b->parent = nullptr;
        b->aux.mkUninitialized();
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
    // EXIT_GC_SPIRAL Day 6-8 wire-back (2026-05-29): the pool gate
    // `NIX_V3_NO_CLOSURE_POOL=1` opts OUT of pool reuse.  When set,
    // every call falls through to a fresh arena allocation (still
    // arena-backed; preserves the fakeClo magic so recycleFakeClo
    // would still classify it correctly, just never hits the pool).
    //
    // Retirement (amended 2026-05-29 evening, supersedes prior
    // "delete gate when SHIP-gate clears" criterion):
    //   The pool + sentinel infrastructure (kFakeCloMagic / _pad /
    //   NIX_V3_NO_CLOSURE_POOL gate) MAY be retired AFTER the rest
    //   of v3's GC reaches a state where it reclaims the 144 MB
    //   unaided — concretely, when Phase E v0.2 ships default-on at
    //   acceptable wall+RSS, OR Stage 6 production precise GC lands.
    //   Until then the pool stays default-on; the gate stays as an
    //   A/B opt-out.  Cross-ref: lode/EXIT_GC_SPIRAL_PLAN_2026-05-29
    //   §4.3 amendment + lode/ROADMAP_TO_VISION_2026-05-15 deferred
    //   retirement note.
    static const bool s_poolDisabled =
        std::getenv("NIX_V3_NO_CLOSURE_POOL") != nullptr;
    if (__builtin_expect(!s_poolDisabled, 1)) {
        if (Closure * c = tryPopFakeClo(nUpvalues)) {
            // Pool hit — closure was previously stamped with the fakeClo
            // magic at allocFakeClo time, and the magic survived through
            // recycleFakeClo (which doesn't touch _pad).  Caller is about
            // to overwrite desc/cu/capturedWiths/upvalues; magic stays.
            // env-sharing: MUST reset upvalEnv — a recycled fakeClo from a
            // prior env-shared force (upvalEnv != null) would otherwise leak a
            // stale Env into a non-env-shared reuse (closureUpvalue reads it →
            // wrong value/UAF).  Callers that share set it again after.
            c->upvalEnv = nullptr;
            return c;
        }
    }
    // Pool miss (or pool disabled): always arena (never nursery) so
    // subsequent recycle's pointer stability survives Cheney scavenges.
    const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
    V3_STATS_BUMP(bytesClosures, bytes);
    auto * c = static_cast<Closure *>(
        threadArena().alloc(bytes, CellType::Closure));
    c->nUpvalues = nUpvalues;
    c->_pad = kFakeCloMagic;   // Mark as fakeClo for safe pooling.
    c->capturedWiths = nullptr;
    c->cu = nullptr;
    c->upvalEnv = nullptr;   // env-sharing: inline-FAM path (fakeClos never share an Env)
    return c;
}

inline void Alloc::recycleFakeClo(Closure * c) noexcept
{
    if (!c) return;
    // EXIT_GC_SPIRAL Day 6-8 wire-back: if pool disabled, recycling
    // is a no-op — the closure is just left for arena GC to reclaim
    // (or stays resident if no GC fires).  Pool gate same as alloc.
    static const bool s_poolDisabled =
        std::getenv("NIX_V3_NO_CLOSURE_POOL") != nullptr;
    if (__builtin_expect(s_poolDisabled, 0)) return;
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
// rather than inline in the Bindings struct.  totalSize is O(1) on Sorted
// and streams a Cursor on Chain.  materialize is O(1) on Sorted and copies
// a Chain only for callers that require an indexed flat entries[] view.
// ---------------------------------------------------------------------------

inline uint32_t Bindings::totalSize() const noexcept
{
    // Alloc-free distinct-name count via the k-way-merge Cursor
    // (Lever A).  O(1) on Sorted; O(N·depth) on Chain with no
    // intermediate vector/sort.  Delegates to countDistinct() so the
    // two never diverge.
    return countDistinct();
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
/// Chain-aware: walks overlay then parent and returns the winning
/// entry's position without copying the chain.
inline uint32_t lookupAttrPos(const Bindings * b, SymbolId name)
{
    if (!b) return 0;
    if (const Bindings::Entry * e = b->lookupEntry(name))
        return e->pos & Bindings::kPosMask;
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
    if (writtenValue.tag() == Tag::Attrs && writtenValue.asAttrs()) {
        auto * b = writtenValue.asAttrs();
        std::fprintf(stderr, " attrs ptr=%p size=%u",
            (const void *)b, (unsigned)b->size);
        if (const BindingsOrigin * o = lookupBindingsOrigin(b)) {
            std::fprintf(stderr, " value-origin=%s",
                o->source ? o->source : "?");
        }
    } else if (writtenValue.tag() == Tag::String && writtenValue.asString()) {
        std::fprintf(stderr, " str=\"%.40s\"", writtenValue.asString());
    } else if (writtenValue.tag() == Tag::Closure && writtenValue.asClosure()) {
        // Log closure pointer + desc codeOff so we can correlate with
        // later fakeClo allocations.  Phase A5 RCA: if the pooled
        // fakeClo pool returns a closure whose pointer matches a
        // cell-resident closure, the next OP_FORCE will overwrite
        // c->desc with the new thunk's desc — silently mutating the
        // cell-stored closure to the WRONG body.
        auto * c = writtenValue.asClosure();
        std::fprintf(stderr, " closure-ptr=%p desc=%p",
            (const void *)c, (const void *)c->desc);
        if (c->desc) {
            std::fprintf(stderr, " codeOff=%u nUp=%u",
                c->desc->codeOffset, (unsigned)c->nUpvalues);
        }
    } else if (writtenValue.tag() == Tag::Thunk && writtenValue.asThunk()) {
        std::fprintf(stderr, " thunk-ptr=%p state=%d",
            (const void *)writtenValue.asThunk(),
            (int)writtenValue.asThunk()->state);
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

// ---------------------------------------------------------------------------
// T1.3 unified cross-type allocation attribution dump (2026-05-27).
// Aggregates the top allocation sites across Closures + Thunks + Pairs
// + Lists into a single sorted-by-bytes table.  Useful for "where is
// the memory going" overview without scanning four separate dumps.
//
// Gate: `NIX_V3_ALLOC_ATTR=1` is the master switch.  When set, the
// individual NIX_V3_*_ATTR gates above must ALSO be set to populate
// their respective tables.  For convenience, this dump function
// internally reads from whichever tables are populated.
//
// Each table is keyed differently (Closure*, Thunk*, ValuePair*,
// ListVec*) but all converge to per-(file, line) Rollup entries.
// We emit a "Type" column so users can identify which Tag a site
// allocates.
// ---------------------------------------------------------------------------

inline void dumpAllocAttribution(std::FILE * out, size_t topN = 30) noexcept
{
    static const bool s_enabled =
        std::getenv("NIX_V3_ALLOC_ATTR") != nullptr;
    if (!s_enabled) return;

    struct Row {
        const char * type;     // "Closure" / "Thunk" / "Pair" / "List"
        const char * file;
        uint32_t     line;
        uint64_t     allocCount;
        uint64_t     totalBytes;
    };
    std::vector<Row> rows;
    rows.reserve(256);

    // -- Closures --------------------------------------------------
    {
        struct Key { const char * file; uint32_t line; };
        struct KeyHash { size_t operator()(const Key & k) const noexcept {
            return reinterpret_cast<size_t>(k.file) * 1000003u + size_t(k.line); } };
        struct KeyEq { bool operator()(const Key & a, const Key & b) const noexcept {
            return a.file == b.file && a.line == b.line; } };
        std::unordered_map<Key, Row, KeyHash, KeyEq> agg;
        for (const auto & kv : closureOriginTable()) {
            const Closure * c = kv.first;
            const ClosureOrigin & o = kv.second;
            if (!c || !o.file) continue;
            Key k{o.file, o.line};
            auto & r = agg[k];
            r.type = "Closure"; r.file = o.file; r.line = o.line;
            ++r.allocCount;
            r.totalBytes += sizeof(Closure) + sizeof(Value) * uint64_t(o.nUpvalues);
        }
        for (auto & kv : agg) rows.push_back(kv.second);
    }

    // -- Thunks ----------------------------------------------------
    {
        struct Key { const char * file; uint32_t line; };
        struct KeyHash { size_t operator()(const Key & k) const noexcept {
            return reinterpret_cast<size_t>(k.file) * 1000003u + size_t(k.line); } };
        struct KeyEq { bool operator()(const Key & a, const Key & b) const noexcept {
            return a.file == b.file && a.line == b.line; } };
        std::unordered_map<Key, Row, KeyHash, KeyEq> agg;
        for (const auto & kv : thunkOriginTable()) {
            const Thunk * t = kv.first;
            const ThunkOrigin & o = kv.second;
            if (!t || !o.file) continue;
            Key k{o.file, o.line};
            auto & r = agg[k];
            r.type = "Thunk"; r.file = o.file; r.line = o.line;
            ++r.allocCount;
            r.totalBytes += sizeof(Thunk) + sizeof(Value) * uint64_t(o.nUpvalues);
        }
        for (auto & kv : agg) rows.push_back(kv.second);
    }

    // -- Pairs -----------------------------------------------------
    {
        struct Key { const char * file; uint32_t line; };
        struct KeyHash { size_t operator()(const Key & k) const noexcept {
            return reinterpret_cast<size_t>(k.file) * 1000003u + size_t(k.line); } };
        struct KeyEq { bool operator()(const Key & a, const Key & b) const noexcept {
            return a.file == b.file && a.line == b.line; } };
        std::unordered_map<Key, Row, KeyHash, KeyEq> agg;
        for (const auto & kv : pairOriginTable()) {
            const ValuePair * p = kv.first;
            const PairOrigin & o = kv.second;
            if (!p || !o.file) continue;
            Key k{o.file, o.line};
            auto & r = agg[k];
            r.type = "Pair"; r.file = o.file; r.line = o.line;
            ++r.allocCount;
            r.totalBytes += sizeof(ValuePair);
        }
        for (auto & kv : agg) rows.push_back(kv.second);
    }

    // -- Lists -----------------------------------------------------
    {
        struct Key { const char * file; uint32_t line; };
        struct KeyHash { size_t operator()(const Key & k) const noexcept {
            return reinterpret_cast<size_t>(k.file) * 1000003u + size_t(k.line); } };
        struct KeyEq { bool operator()(const Key & a, const Key & b) const noexcept {
            return a.file == b.file && a.line == b.line; } };
        std::unordered_map<Key, Row, KeyHash, KeyEq> agg;
        for (const auto & kv : listOriginTable()) {
            const ListVec * l = kv.first;
            const ListOrigin & o = kv.second;
            if (!l || !o.file) continue;
            Key k{o.file, o.line};
            auto & r = agg[k];
            r.type = "List"; r.file = o.file; r.line = o.line;
            ++r.allocCount;
            r.totalBytes += sizeof(ListVec) + sizeof(Value) * uint64_t(o.size);
        }
        for (auto & kv : agg) rows.push_back(kv.second);
    }

    if (rows.empty()) {
        std::fprintf(out,
            "v3-direct alloc-attr: empty (NIX_V3_ALLOC_ATTR=1 set but no\n"
            "  per-type recording gates — set at least one of\n"
            "  NIX_V3_CLOSURES_ATTR / NIX_V3_THUNKS_ATTR /\n"
            "  NIX_V3_PAIRS_ATTR / NIX_V3_LISTS_ATTR)\n");
        return;
    }

    std::sort(rows.begin(), rows.end(),
        [](const Row & a, const Row & b) {
            if (a.totalBytes != b.totalBytes) return a.totalBytes > b.totalBytes;
            return a.allocCount > b.allocCount;
        });

    uint64_t grandBytes = 0, grandAllocs = 0;
    for (const auto & r : rows) {
        grandBytes  += r.totalBytes;
        grandAllocs += r.allocCount;
    }
    const size_t n = std::min(rows.size(), topN);
    std::fprintf(out,
        "\nv3-direct alloc-attr unified: %zu sites tracked, %llu total allocs, "
        "%.1f MB cross-type (top %zu):\n",
        rows.size(),
        (unsigned long long)grandAllocs,
        double(grandBytes) / (1024.0 * 1024.0),
        n);
    std::fprintf(out,
        "  %-8s %-50s %12s %10s   %5s\n",
        "Type", "Origin (file:line)", "allocs", "MB", "%cumul");
    uint64_t cumBytes = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto & r = rows[i];
        cumBytes += r.totalBytes;
        const double mb = double(r.totalBytes) / (1024.0 * 1024.0);
        const double cumPct = grandBytes > 0
            ? 100.0 * double(cumBytes) / double(grandBytes) : 0.0;
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s:%u",
            r.file ? r.file : "<null>", r.line);
        std::fprintf(out,
            "  %-8s %-50s %12llu  %8.2f  %5.1f%%\n",
            r.type, buf, (unsigned long long)r.allocCount, mb, cumPct);
    }
}

} // namespace nix::v3
