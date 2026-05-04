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

#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    struct Entry { SymbolId name; Value value; };

    uint32_t size;
    uint32_t _pad;
    Entry    entries[]; // FAM, sorted ascending by name

    /// Binary search.  Returns nullptr if not found.
    const Value * lookup(SymbolId name) const noexcept
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

    /// Non-const overload — returns a writable pointer for callers that
    /// want to memoize lazy entries (e.g., resolving Tag::App in
    /// OP_ATTRS_SELECT_DYN and writing the WHNF result back into the
    /// slot).  Phase 13.3 mapAttrs memoization.
    Value * lookup(SymbolId name) noexcept
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

    bool has(SymbolId name) const noexcept { return lookup(name) != nullptr; }
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
    /// 1 MB blocks: large enough that a single block holds many
    /// thousands of typical allocations, small enough that a
    /// long-running eval doesn't hold onto huge unused tails.
    /// 16 MB block size keeps Boehm's GC_add_roots root-region count
    /// low for nixpkgs-scale evaluations: each 1MB block was registered
    /// as its own root region, so a 4GB arena → 4k+ regions, exceeding
    /// Boehm's MAX_ROOTS limit (`Too many root sets`).  16MB blocks
    /// drop the count 16x, putting full nixpkgs evals back under the
    /// Boehm cap.  Cost: a 16MB minimum allocation per thread on first
    /// arena use, vs. 1MB before — accepted because the arena is the
    /// hot allocation path.
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

private:
    char *  cur        = nullptr;
    char *  end        = nullptr;
    /// Owning blocks; never freed in normal operation (they live
    /// for the lifetime of the thread).
    std::vector<char *> blocks;
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

struct Alloc
{
    static Value * allocValue() noexcept
    {
        return static_cast<Value *>(threadArena().alloc(sizeof(Value)));
    }

    static Closure * allocClosure(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Closure) + sizeof(Value) * nUpvalues;
        auto * c = static_cast<Closure *>(threadArena().alloc(bytes));
        c->nUpvalues = nUpvalues;
        c->_pad = 0;
        c->capturedWiths = nullptr;
        c->cu = nullptr;
        return c;
    }

    /// Allocate a Suspended thunk with `nUpvalues` captured upvalues
    /// stored in the FAM tail.
    static Thunk * allocThunkSuspended(uint16_t nUpvalues) noexcept
    {
        const size_t bytes = sizeof(Thunk) + sizeof(Value) * nUpvalues;
        auto * t = static_cast<Thunk *>(threadArena().alloc(bytes));
        t->state = ThunkState::Suspended;
        t->nUpvalues = nUpvalues;
        t->forces = 0;
        t->suspended.capturedWiths = nullptr;
        t->suspended.cu = nullptr;
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
        auto * t = static_cast<Thunk *>(threadArena().alloc(bytes));
        t->state = ThunkState::Bridge;
        t->nUpvalues = 0;
        t->forces = 0;
        t->bridgeSrc = src;
        return t;
    }

    static Env * allocEnv(uint16_t nValues) noexcept
    {
        const size_t bytes = sizeof(Env) + sizeof(Value) * nValues;
        auto * e = static_cast<Env *>(threadArena().alloc(bytes));
        e->parent = nullptr;
        e->isWithEnv = false;
        e->nValues = nValues;
        return e;
    }

    static ListVec * allocList(uint32_t n) noexcept
    {
        const size_t bytes = sizeof(ListVec) + sizeof(Value) * n;
        auto * l = static_cast<ListVec *>(threadArena().alloc(bytes));
        l->size = n;
        return l;
    }

    /// REVIEW CRIT-3: ValuePair allocation routed through the arena
    /// instead of std::malloc.  Each ValuePair holds Value payloads
    /// with Boehm-managed pointers (Closure / Thunk / Bindings); the
    /// prior std::malloc'd storage was invisible to Boehm so the
    /// inner payloads could be reclaimed under load.  Arena-allocated
    /// pairs sit inside a GC_add_roots-registered region (alloc.hh:225).
    static ValuePair * allocPair() noexcept
    {
        return static_cast<ValuePair *>(threadArena().alloc(sizeof(ValuePair)));
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
        return static_cast<char *>(threadArena().alloc(n));
    }

    static Bindings * allocBindings(uint32_t n) noexcept
    {
        const size_t bytes = sizeof(Bindings) + sizeof(Bindings::Entry) * n;
        auto * b = static_cast<Bindings *>(threadArena().alloc(bytes));
        b->size = n;
        // Track size distribution for VM-2 sizing decisions.  Cheap
        // (one branch + one increment) — runs once per attrset.
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
        return b;
    }
};

// ---------------------------------------------------------------------------
// Per-attr position side-table.
//
// Tree-walker stores a PosIdx alongside every Bindings::Entry; v3 keeps
// the Entry slim (24 bytes) and uses this side-table instead.  The key
// `(bindings, name)` is uniquely defined: each Bindings sees exactly one
// PosIdx per name.  The side-table is populated by OP_ATTRS_INIT[_DYN] /
// OP_ATTRS_REC_INIT and looked up by `builtins.unsafeGetAttrPos`.
// Lifetime tracking is best-effort: we never explicitly free entries
// since Bindings live for the duration of the eval anyway.
// ---------------------------------------------------------------------------

struct PosKey
{
    const Bindings * bindings;
    SymbolId         name;
    bool operator==(const PosKey & o) const noexcept
    { return bindings == o.bindings && name == o.name; }
};

struct PosKeyHash
{
    size_t operator()(const PosKey & k) const noexcept
    {
        return std::hash<const Bindings *>{}(k.bindings) ^
               (std::hash<SymbolId>{}(k.name) << 1);
    }
};

} // namespace nix::v3

#include <unordered_map>

namespace nix::v3 {

inline std::unordered_map<PosKey, uint32_t, PosKeyHash> & attrPosTable()
{
    static std::unordered_map<PosKey, uint32_t, PosKeyHash> tbl;
    return tbl;
}

inline void recordAttrPos(const Bindings * b, SymbolId name, uint32_t pos)
{
    if (pos == 0) return;
    attrPosTable()[{b, name}] = pos;
}

inline uint32_t lookupAttrPos(const Bindings * b, SymbolId name)
{
    auto & tbl = attrPosTable();
    auto it = tbl.find({b, name});
    return it == tbl.end() ? 0 : it->second;
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

/// Push a snapshot into the pool and return its 1-based handle (0 = none).
inline uint32_t recordPosSnapshot(PosSnapshot s)
{
    auto & p = posSnapshotPool();
    p.push_back(std::move(s));
    return static_cast<uint32_t>(p.size() - 1);
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

} // namespace nix::v3
