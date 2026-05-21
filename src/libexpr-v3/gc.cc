// Cheney scavenge implementation for the v3 nursery.  Background and
// design: `lode/CHENEY_NURSERY_DESIGN.md`.  Phase C entry point.
//
// Strategy
// --------
// Side-table forwarding (`unordered_map<oldPtr, newPtr>`) — chosen
// over header bit-stealing for the first version because it doesn't
// require layout changes to `Thunk` / `Closure` / `ListVec`.  The
// per-scavenge map is rebuilt from empty each time, so the cost is
// proportional to the live nursery size, not the historic alloc
// count.
//
// Worklist drain — `forwardClosure` / `forwardThunk` / `forwardList`
// COPY the live nursery object to tenured, record (old, new) in the
// forward map, and queue the new tenured pointer in a graylist;
// they do NOT recurse into the children's pointers.  A single
// `drain()` loop walks the graylist iteratively, mutating each
// queued object's children in place.  This keeps the C-stack flat
// regardless of graph depth.
//
// Tenured walk — for tenured `Closure` / `Thunk` / `ListVec` /
// `Bindings` / `ValuePair` we encounter while walking, we ALSO
// queue them (gated by a `walked` set) so any tenured-to-nursery
// references they hold get rewritten.  In Phase C v1 we lack a
// remembered-set / write-barrier, so this is the conservative way
// to find every live nursery pointer.  Phase D will replace this
// with cell tracking + a write barrier so we don't re-scan all
// reached tenured objects per scavenge.
//
// Bindings / ValuePair stay tenured by design.  If a Bindings or
// ValuePair pointer is observed to be inside the nursery here
// (which would mean an allocator bug), `std::abort` fires — moving
// either type would invalidate `Tag::Slot` / `Thunk::cell`
// pointers that may exist anywhere in the live graph.
//
// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
// Input Output Group.
// SPDX-License-Identifier: Apache-2.0

#include "v3/gc.hh"
#include "v3/nursery.hh"
#include "v3/alloc.hh"
#include "v3/bytecode.hh"  // #705: AttrSelectIC roots
#include "v3/closure.hh"
#include "v3/primop.hh"  // #705: walkV3BridgeRoots
#include "v3/bytecode_primops.hh"  // #705: walkBytecodePrimopRoots, walkBuiltinsRoot
#include "v3/print.hh"  // Round 1 #7: walkDeepForceRoots
#include "v3/barrier.hh"  // Phase D: dirty-list + standalone cells
#include "v3/value.hh"
#include "v3/vm.hh"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nix::v3 {

namespace {

enum GrayKind : uint8_t {
    GK_CLOSURE  = 0,
    GK_THUNK    = 1,
    GK_LIST     = 2,
    GK_BINDINGS = 3,
    GK_PAIR     = 4,
};

struct Gray { void * ptr; uint8_t kind; };

/// Persistent per-thread scratch buffers.  Reused across scavenge
/// calls (cleared at the start of each one, capacity retained).
/// Cuts per-scavenge malloc/free traffic from O(reachable) buckets +
/// O(reachable) hashes per pass to amortised zero — the buffers grow
/// to the high-water-mark of the eval and stay there.  Material
/// under aggressive scavenge (small nursery, frequent reclaims).
struct ScavengeBuffers
{
    std::unordered_map<void *, void *> forward;
    std::unordered_set<void *>         walked;
    std::vector<Gray>                  graylist;

    void clear()
    {
        forward.clear();
        walked.clear();
        graylist.clear();
    }
};

ScavengeBuffers & threadScavengeBuffers() noexcept
{
    thread_local ScavengeBuffers b;
    return b;
}

/// Per-scavenge state — references into the thread-local
/// `ScavengeBuffers` above so we don't allocate fresh containers
/// on every call.  Lifetime is bounded by `scavengeNursery`.
struct Scavenger
{
    Nursery & n;
    VMState & vm;
    /// nursery oldPtr -> tenured newPtr (lookup before copy)
    std::unordered_map<void *, void *> & forward;
    /// objects already queued for walk (deduplication for both
    /// freshly-copied tenured AND originally-tenured paths)
    std::unordered_set<void *> & walked;
    /// queued objects to walk in `drain()`
    std::vector<Gray> & graylist;
    /// #705 (2026-05-21): CUs whose attrSelectCache has been
    /// walked.  Populated from every walkClosure / walkThunk so any
    /// CU transitively reachable from a root is covered.
    std::unordered_set<const CompilationUnit *> walkedCUs;
    /// 2026-05-21 (Phase 1.7 R1 refinement): [start, end) byte
    /// ranges of every TENURED object walked this scavenge.  Used
    /// by `postScavengeBruteScan` to filter "false positive" hits
    /// in dead-but-arena-resident objects (Boehm conservatively
    /// pins the whole arena as a root, so dead tenured Closures /
    /// Bindings keep their nursery pointers in memory).  An hit
    /// outside any range here is dead memory and harmless;
    /// only hits INSIDE one of these ranges indicate a true
    /// missed-root bug (reachable tenured pointer not forwarded).
    ///
    /// Populated by each walk* method on entry (when the object
    /// originated in tenured arena — nursery copies have already
    /// been replaced by their tenured forward at this point).
    /// Sorted-and-searched after drain() completes.
    std::vector<std::pair<uintptr_t, uintptr_t>> liveTenuredRanges;
    /// Record a tenured [start, end) byte range for the BRUTE
    /// reachability filter.  Called by walk* methods.  No-op if
    /// the pointer is in the nursery (means a nursery copy that
    /// hasn't been forwarded yet — caller bug, but BRUTE doesn't
    /// care about nursery bytes either way).
    void recordLiveTenured(const void * p, size_t bytes)
    {
        if (!p || n.contains(p)) return;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(p);
        liveTenuredRanges.emplace_back(lo, lo + bytes);
    }

    // -- pointer forwarders (no recursion; just copy + queue) ----

    Closure  * fwdClosure (Closure  * c);
    Thunk    * fwdThunk   (Thunk    * t);
    ListVec  * fwdList    (ListVec  * l);
    Bindings * fwdBindings(Bindings * b);
    ValuePair * fwdPair   (ValuePair * p);

    // -- value visitor: dispatches to the right forwarder --------

    void visitValue(Value & v);

    // -- per-type field walkers (called from drain) --------------

    void walkClosure (Closure  * c);
    void walkThunk   (Thunk    * t);
    void walkList    (ListVec  * l);
    void walkBindings(Bindings * b);
    void walkPair    (ValuePair * p);

    // -- top-level driver ---------------------------------------

    void drain();
    void run();
};

Closure * Scavenger::fwdClosure(Closure * c)
{
    if (!c) return nullptr;
    if (n.contains(c)) {
        auto it = forward.find(c);
        if (it != forward.end()) return static_cast<Closure *>(it->second);
        const size_t bytes = sizeof(Closure) + sizeof(Value) * c->nUpvalues;
        void * dst = threadArena().alloc(bytes);
        std::memcpy(dst, c, bytes);
        forward.emplace(c, dst);
        graylist.push_back({dst, GK_CLOSURE});
        return static_cast<Closure *>(dst);
    }
    if (walked.insert(c).second) graylist.push_back({c, GK_CLOSURE});
    return c;
}

/// True iff `tg` denotes a Value whose `payload` carries no v3-heap
/// pointer that the walker would need to forward.  Used as a fast-
/// path predicate by `fwdThunk` (and analogous walks) to skip queuing
/// already-WHNF Thunks whose evaluated payload is a leaf scalar.
[[gnu::always_inline]] static inline bool isLeafTag(Tag tg) noexcept
{
    switch (tg) {
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::String:
    case Tag::Path:
    case Tag::PrimOp:
    case Tag::Blackhole:
    case Tag::External:
    case Tag::Uninitialized:
        return true;
    case Tag::Closure:
    case Tag::Thunk:
    case Tag::Attrs:
    case Tag::List:
    case Tag::App:
    case Tag::PrimOpApp:
    case Tag::Slot:
        return false;
    }
    return false;
}

Thunk * Scavenger::fwdThunk(Thunk * t)
{
    if (!t) return nullptr;
    if (n.contains(t)) {
        auto it = forward.find(t);
        if (it != forward.end()) return static_cast<Thunk *>(it->second);
        // #705 / N1 (2026-05-21): Blackhole MUST copy the full
        // Suspended-layout (header + tail[nUpvalues]).  The union
        // variant remains `suspended` while the body is executing,
        // and `clearBlackMarksOnException` can revert Blackhole →
        // Suspended on exception unwind — at which point OP_FORCE
        // re-reads `tail[i]` and `suspended.capturedWiths` to
        // rebuild the fakeClo.  If we copy only the header, the
        // tenured copy's tail[i] is uninitialized garbage; the
        // post-revert force builds a fakeClo with stale upvalues.
        //
        // Suspended / Native / Blackhole all carry a FAM tail of
        // nUpvalues Values.  Evaluated / Bridge use only the header.
        size_t bytes;
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Native:
        case ThunkState::Blackhole:
            bytes = sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
            break;
        case ThunkState::Evaluated:
        case ThunkState::Bridge:
            bytes = sizeof(Thunk);
            break;
        }
        void * dst = threadArena().alloc(bytes);
        std::memcpy(dst, t, bytes);
        forward.emplace(t, dst);
        graylist.push_back({dst, GK_THUNK});
        return static_cast<Thunk *>(dst);
    }
    // Tenured Thunk fast paths — skip queuing entirely when the
    // thunk has no v3-heap payload to walk.  Material on workloads
    // that build many tenured thunks then evaluate them to leaf
    // scalars (e.g. genList of integers force-iterated by foldl'):
    // pre-fix, every such thunk was hashed into `walked` and
    // queued + walked + dispatched-on-state, even though its only
    // ref-bearing fields contained Tag::Int.  Hash insert + queue +
    // drain dominated the per-scavenge cost.
    //
    // Bridge: bridgeSrc is a `nix::Value *` (TW heap), never v3
    // nursery — no work for that field.  BUT a Bridge thunk MAY
    // carry a `cell` (`STG-14b option (a)` cell-update protocol;
    // see vm.cc Bridge handler comment) whose contents may hold a
    // nursery payload.  #705 (2026-05-20): if the cell is set,
    // queue the thunk so walkThunk walks the cell.
    //
    // Blackhole: state's payload is irrelevant (body mid-exec),
    // but the cell is preserved across blackhole → evaluated
    // (CFF_THUNK_RETURN propagates), so the same caveat applies.
    //
    // Evaluated with leaf tag: `evaluated` payload has no
    // forwardable pointer.  Cell included in the gate.
    switch (t->state) {
    case ThunkState::Bridge:
    case ThunkState::Blackhole:
        if (!t->cell) return t;  // truly nothing to walk
        break;                   // fall through to queue if cell set
    case ThunkState::Evaluated:
        if (isLeafTag(t->evaluated.tag()) && !t->cell) return t;
        break;
    case ThunkState::Suspended:
    case ThunkState::Native:
        break;
    }
    if (walked.insert(t).second) graylist.push_back({t, GK_THUNK});
    return t;
}

ListVec * Scavenger::fwdList(ListVec * l)
{
    if (!l) return nullptr;
    if (n.contains(l)) {
        auto it = forward.find(l);
        if (it != forward.end()) return static_cast<ListVec *>(it->second);
        const size_t bytes = sizeof(ListVec) + sizeof(Value) * l->size;
        void * dst = threadArena().alloc(bytes);
        std::memcpy(dst, l, bytes);
        forward.emplace(l, dst);
        graylist.push_back({dst, GK_LIST});
        return static_cast<ListVec *>(dst);
    }
    if (walked.insert(l).second) graylist.push_back({l, GK_LIST});
    return l;
}

Bindings * Scavenger::fwdBindings(Bindings * b)
{
    if (!b) return nullptr;
    // Bindings are tenured-only (alloc.hh allocBindings).  If we
    // see a nursery Bindings here it means an allocator regressed;
    // moving Bindings would orphan any Tag::Slot / Thunk::cell
    // that points into entries[].
    if (n.contains(b)) std::abort();
    if (walked.insert(b).second) graylist.push_back({b, GK_BINDINGS});
    return b;
}

ValuePair * Scavenger::fwdPair(ValuePair * p)
{
    if (!p) return nullptr;
    if (n.contains(p)) std::abort();  // pairs are tenured (allocPair)
    // Fast path — left, right, AND evaluated all carry no v3-heap
    // pointer.  `evaluated` (added 2026-05-18 for App memoisation)
    // must be checked too: a forced Tag::App writes its WHNF result
    // there, and a nursery payload in `evaluated` is a real root.
    // #705 (2026-05-21): the missing `evaluated` check was the
    // primary cause of hello.drvPath SIGSEGV under scavenge.
    if (isLeafTag(p->left.tag()) && isLeafTag(p->right.tag())
        && isLeafTag(p->evaluated.tag())) return p;
    if (walked.insert(p).second) graylist.push_back({p, GK_PAIR});
    return p;
}

void Scavenger::visitValue(Value & v)
{
    // -Werror=switch-enum requires explicit enumeration of every
    // tag.  Leaf tags (Int / Float / Bool / Null / String / Path /
    // PrimOp / Blackhole / External / Uninitialized) hold no v3-
    // heap pointer to forward; they share an empty `break` body.
    // `String` / `Path` reference arena-allocated `const char *`
    // payloads which are tenured by definition.
    switch (v.tag()) {
    case Tag::Closure:
        v.payload.closure = fwdClosure(v.payload.closure);
        break;
    case Tag::Thunk:
        v.payload.thunk = fwdThunk(v.payload.thunk);
        break;
    case Tag::Attrs:
        v.payload.bindings = fwdBindings(v.payload.bindings);
        break;
    case Tag::List:
        v.payload.list = fwdList(v.payload.list);
        break;
    case Tag::App:
    case Tag::PrimOpApp:
        v.payload.pair = fwdPair(v.payload.pair);
        break;
    case Tag::Slot: {
        // Cells (Value *) are tenured; the slot pointer never moves.
        // The Value AT the cell may carry a nursery payload, so we
        // walk through.  We dedup via the same `walked` set so
        // multiple slots aliasing the same cell don't double-walk.
        Value * cell = v.payload.slot;
        if (cell && walked.insert(cell).second) {
            visitValue(*cell);
        }
        break;
    }
    case Tag::Uninitialized:
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::String:
    case Tag::Path:
    case Tag::PrimOp:
    case Tag::Blackhole:
    case Tag::External:
        break;
    }
}

// #705 R9 (audit Round 2 N3): register every CU reached transitively
// so its `attrSelectCache` IC entries are walked too.  Without this,
// an OP_ATTRS_SELECT_IC hit on an IC entry whose Bindings is only
// reachable via the cache slot (not via any other vm root) leaves
// the Bindings's entries unforwarded → next hit returns a stale
// Tag::Thunk payload.
//
// Drains immediately so any nursery Bindings the IC points at gets
// queued for walkBindings (and its entries' nursery payloads
// forwarded) inside the same scavenge pass.
void Scavenger::walkClosure(Closure * c)
{
    // BRUTE-refinement (Phase 1.7 R1): record this object's tenured
    // byte range so postScavengeBruteScan can filter hits to live
    // (reachable-from-roots) objects only.
    recordLiveTenured(c, sizeof(Closure) + sizeof(Value) * c->nUpvalues);
    if (c->cu && walkedCUs.insert(c->cu).second) {
        for (const auto & ic : c->cu->attrSelectCache) {
            for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w) {
                if (Bindings * b = const_cast<Bindings *>(ic.entries[w].bindings))
                    fwdBindings(b);
            }
        }
    }
    if (c->capturedWiths) c->capturedWiths = fwdList(c->capturedWiths);
    for (uint16_t i = 0; i < c->nUpvalues; ++i) {
        visitValue(c->upvalues[i]);
    }
}

void Scavenger::walkThunk(Thunk * t)
{
    // BRUTE-refinement: Thunk size depends on state (matches fwdThunk's
    // copy-size logic).
    {
        size_t bytes;
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Native:
        case ThunkState::Blackhole:
            bytes = sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
            break;
        case ThunkState::Evaluated:
        case ThunkState::Bridge:
            bytes = sizeof(Thunk);
            break;
        }
        recordLiveTenured(t, bytes);
    }
    // The cell (write-back target for OP_RETURN) is tenured; walk
    // its current Value so any nursery payload it holds is found.
    if (t->cell && walked.insert(t->cell).second) {
        visitValue(*t->cell);
    }
    // Round 1 #5 (defensive): shapeCell is only populated when
    // NIX_V3_CELL_EVERYWHERE=1 — currently default-off — but if
    // anything flips that gate the cell holds Tag::Thunk(self) at
    // first then in-progress Bindings; both must be forwarded.
    // No-op when shapeCell is null (the default).
    if (t->shapeCell && walked.insert(t->shapeCell).second) {
        visitValue(*t->shapeCell);
    }
    switch (t->state) {
    case ThunkState::Suspended:
        if (t->suspended.cu && walkedCUs.insert(t->suspended.cu).second) {
            for (const auto & ic : t->suspended.cu->attrSelectCache) {
                for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w) {
                    if (Bindings * b = const_cast<Bindings *>(ic.entries[w].bindings))
                        fwdBindings(b);
                }
            }
        }
        if (t->suspended.capturedWiths)
            t->suspended.capturedWiths = fwdList(t->suspended.capturedWiths);
        for (uint16_t i = 0; i < t->nUpvalues; ++i) {
            visitValue(t->tail[i]);
        }
        break;
    case ThunkState::Evaluated:
        visitValue(t->evaluated);
        break;
    case ThunkState::Native:
        // tail[] holds primop arguments; nUpvalues stores the
        // arity for native thunks.
        for (uint16_t i = 0; i < t->nUpvalues; ++i) {
            visitValue(t->tail[i]);
        }
        break;
    case ThunkState::Bridge:
        // bridgeSrc is a `nix::Value *` from the tree-walker heap;
        // not a v3 nursery pointer, so nothing to forward here.
        break;
    case ThunkState::Blackhole:
        // Mirror Suspended: walk CU's AttrSelectIC (R9) too.
        if (t->suspended.cu && walkedCUs.insert(t->suspended.cu).second) {
            for (const auto & ic : t->suspended.cu->attrSelectCache) {
                for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w) {
                    if (Bindings * b = const_cast<Bindings *>(ic.entries[w].bindings))
                        fwdBindings(b);
                }
            }
        }
        // #705 / N1 (2026-05-21 Round 2 GC audit): walk the
        // Suspended-layout fields even while the thunk is Blackhole.
        // The union variant is still `suspended` (state is just a
        // marker that the body is currently executing); tail[i] hold
        // the captured upvalues and suspended.capturedWiths the
        // outer with-chain.
        //
        // Why this matters: `clearBlackMarksOnException` reverts
        // Blackhole → Suspended on exception unwind (vm.cc ~9400).
        // After the revert, OP_FORCE re-reads `t->tail[i]` and
        // `t->suspended.capturedWiths` to rebuild the fakeClo
        // (vm.cc ~6027 / 11089).  If a scavenge fired while the
        // state was Blackhole, those fields hold stale nursery
        // pointers → next force builds a fakeClo with stale upvalues
        // → OP_TAIL_CALL / OP_GET_UPVALUE crash with desc=null
        // (the "stale callee" / OOR signature we saw on
        // hello.outPath and hello.drvPath).
        //
        // Trigger conditions (all common at workload scale):
        //   - exception during a thunk body (assert / throw / addErrorContext)
        //   - outer dispatch at exitDepth==0
        //   - scavenge fires during the body
        //
        // Fix: identical to the Suspended case.
        if (t->suspended.capturedWiths)
            t->suspended.capturedWiths = fwdList(t->suspended.capturedWiths);
        for (uint16_t i = 0; i < t->nUpvalues; ++i) {
            visitValue(t->tail[i]);
        }
        break;
    }
}

void Scavenger::walkList(ListVec * l)
{
    recordLiveTenured(l, sizeof(ListVec) + sizeof(Value) * l->size);
    for (uint32_t i = 0; i < l->size; ++i) {
        visitValue(l->elems[i]);
    }
}

void Scavenger::walkBindings(Bindings * b)
{
    recordLiveTenured(b, sizeof(Bindings) + sizeof(Bindings::Entry) * b->size);
    for (uint32_t i = 0; i < b->size; ++i) {
        visitValue(b->entries[i].value);
    }
}

void Scavenger::walkPair(ValuePair * p)
{
    recordLiveTenured(p, sizeof(ValuePair));
    visitValue(p->left);
    visitValue(p->right);
    // #705 (2026-05-21): `evaluated` field added 2026-05-18 (commit
    // d3e41c13d) for App-result memoization.  Holds the WHNF result
    // of a previously-forced Tag::App — a nursery payload here
    // (Closure/Thunk/Bindings/List) was the missing root that made
    // hello.drvPath SIGSEGV under scavenge.  When `evaluated` is
    // Tag::Uninitialized, visitValue is a no-op.
    visitValue(p->evaluated);
}

void Scavenger::drain()
{
    while (!graylist.empty()) {
        Gray g = graylist.back();
        graylist.pop_back();
        switch (g.kind) {
        case GK_CLOSURE:  walkClosure (static_cast<Closure  *>(g.ptr)); break;
        case GK_THUNK:    walkThunk   (static_cast<Thunk    *>(g.ptr)); break;
        case GK_LIST:     walkList    (static_cast<ListVec  *>(g.ptr)); break;
        case GK_BINDINGS: walkBindings(static_cast<Bindings *>(g.ptr)); break;
        case GK_PAIR:     walkPair    (static_cast<ValuePair *>(g.ptr)); break;
        }
    }
}

void Scavenger::run()
{
    // -- Stage 1: roots -----------------------------------------

    // valueStack and withStack hold Value payloads at the top
    // edge of the live VM state.  Every reachable runtime object
    // is rooted from one of these (or via a frame's closure /
    // thunk pointer below).
    for (Value & v : vm.valueStack) visitValue(v);
    for (Value & v : vm.withStack)  visitValue(v);

    // #705 (2026-05-21): walk the OTHER active VMStates first
    // (under nested runFunctionWithUpvalues / runFunction).  Their
    // frames hold nursery closure/thunk pointers that the per-vm
    // walk below wouldn't reach.  The nursery is shared across
    // VMStates on the thread, so a scavenge fired from any vm must
    // forward roots in ALL active vms.  Dedup happens via
    // `walked.insert(...)` inside fwdXxx.
    static const bool s_dbgVms =
        std::getenv("V3_DBG_NURSERY") != nullptr;
    std::unordered_set<VMState *> walkedVms{&vm};
    for (VMState * other : activeVMStack()) {
        if (!other || !walkedVms.insert(other).second) continue;
        if (__builtin_expect(s_dbgVms, 0)) {
            std::fprintf(stderr,
                "  walking secondary vm=%p frames=%zu valueStack=%zu\n",
                (void*)other, other->frames.size(), other->valueStack.size());
        }
        for (Value & v : other->valueStack) visitValue(v);
        for (Value & v : other->withStack)  visitValue(v);
        for (CallFrame & f : other->frames) {
            if (f.closure)
                f.closure = fwdClosure(const_cast<Closure *>(f.closure));
            if (f.thunk)
                f.thunk = fwdThunk(f.thunk);
            if (f.forceWriteTarget) visitValue(*f.forceWriteTarget);
        }
    }

    // Frames carry the call-chain's closure / thunk pointers.
    // CallFrame::closure is `const Closure *` so we cast away
    // const for the forward; the const is a documentation hint
    // about who's allowed to mutate the closure body, not a
    // GC-safety constraint.
    for (CallFrame & f : vm.frames) {
        if (f.closure) {
            f.closure = fwdClosure(const_cast<Closure *>(f.closure));
        }
        if (f.thunk) {
            f.thunk = fwdThunk(f.thunk);
        }
        // #705 (2026-05-21): forceWriteTarget is a Value*-pointer
        // to a cell that an in-progress force will write its WHNF
        // result into.  The cell pointer itself is tenured (always
        // via Alloc::allocValue() or a Bindings entry slot), but
        // its CURRENT content may carry a nursery payload that
        // the audit's normal walks won't visit if the cell isn't
        // otherwise reachable from valueStack/withStack/frames.
        //
        // Why this is a missed root: between OP_FORCE setting up
        // writeback (flag bit + target pointer) and OP_RETURN /
        // applyForceWriteback firing, the cell sits unreferenced
        // by any visible Value EXCEPT through this frame field.
        // If scavenge fires in that window, the cell's contents
        // dangle.
        //
        // The fix: visit the cell's content as if it were on the
        // value stack.  Safe even when the flag bit is clear —
        // walking *cell is a no-op for tag::Uninitialized / leaf.
        if (f.forceWriteTarget) {
            // GC_AUDIT_ROUND_2 Round 1 #6 diagnostic: warn (under
            // V3_DBG_NURSERY_FWT=1) if the writeback pointer itself
            // sits inside the nursery.  The known case is
            // OP_CALL_PRIMOP's deepForceList pre-pass storing
            // `&list->elems[i]` for nursery-resident lists.  We
            // walk the value the pointer references but do NOT
            // update the pointer; the writeback after this
            // scavenge will land in dead nursery bytes.  Per audit:
            // silent memoization loss only, not a SIGSEGV — the
            // next OP_CALL_PRIMOP scan re-derives WHNF on the
            // forwarded copy.
            static const bool s_dbgFwt =
                std::getenv("V3_DBG_NURSERY_FWT") != nullptr;
            if (__builtin_expect(s_dbgFwt, 0)
                && n.contains(f.forceWriteTarget)) {
                std::fprintf(stderr,
                    "[v3-gc] forceWriteTarget=%p inside nursery; "
                    "writeback after scavenge will be lost "
                    "(latent — see Round 1 #6)\n",
                    (void *)f.forceWriteTarget);
            }
            visitValue(*f.forceWriteTarget);
        }
    }

    // #705 (2026-05-20): bridge-table roots.  The TW->v3 bridge
    // tables (v3BridgeClosures / v3BridgeAttrs / v3BridgeLists)
    // hold v3 Values keyed by handle; each Value's payload may
    // point at a nursery-allocated Closure / Bindings / ListVec.
    // Without forwarding these, hello.drvPath SIGSEGVs on the
    // first scavenge — TW-side bridge primops dereference stale
    // pointers post-memset.
    std::function<void(Value &)> rootVisit =
        [this](Value & v) { visitValue(v); };
    walkV3BridgeRoots(rootVisit);

    // #705 (2026-05-21): per-CompilationUnit AttrSelectIC roots.
    // The IC caches `(Bindings*, slot)` pairs for OP_ATTRS_SELECT —
    // when the same call site re-fires with a previously-seen Bindings
    // pointer, it skips the binary search and reads
    // `bindings->entries[slot].value` directly.
    //
    // Critical missed-root: a Bindings cached here can be reachable
    // ONLY via this cache (no live valueStack/frame reference at
    // scavenge time).  Scavenge wouldn't walk its entries → entries
    // with nursery payloads dangle → next OP_ATTRS_SELECT IC hit
    // returns a stale Tag::Thunk → forceValue crashes on
    // `t->suspended.desc`.
    //
    // Identified 2026-05-21 by V3_DBG_NURSERY_BRUTE which found
    // 502K stale nursery pointers in tenured arena memory after a
    // "clean" deep audit — only path that could keep them reachable
    // without showing in the graph walk.
    //
    // Walk each CU referenced by any frame, dedupe via a local set.
    {
        std::unordered_set<const CompilationUnit *> walkedCUs;
        auto walkOneCU = [&](const CompilationUnit * cu) {
            if (!cu) return;
            if (!walkedCUs.insert(cu).second) return;
            for (const auto & ic : cu->attrSelectCache) {
                for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w) {
                    if (Bindings * b = const_cast<Bindings *>(ic.entries[w].bindings))
                        fwdBindings(b);
                }
            }
        };
        for (CallFrame & f : vm.frames) walkOneCU(f.cu);
        // Also walk via closures/thunks on the stack — they carry CU
        // refs that may not be in any active frame.
        for (Value & v : vm.valueStack) {
            if (v.tag() == Tag::Closure && v.payload.closure)
                walkOneCU(v.payload.closure->cu);
            else if (v.tag() == Tag::Thunk && v.payload.thunk
                     && (v.payload.thunk->state == ThunkState::Suspended
                         || v.payload.thunk->state == ThunkState::Blackhole))
                walkOneCU(v.payload.thunk->suspended.cu);
        }
        for (Value & v : vm.withStack) {
            if (v.tag() == Tag::Closure && v.payload.closure)
                walkOneCU(v.payload.closure->cu);
            else if (v.tag() == Tag::Thunk && v.payload.thunk
                     && (v.payload.thunk->state == ThunkState::Suspended
                         || v.payload.thunk->state == ThunkState::Blackhole))
                walkOneCU(v.payload.thunk->suspended.cu);
        }
    }

    // #705 (2026-05-21): bytecode-primop replacement roots.  Each
    // Value in `primopReplacementMap` may carry a nursery Closure
    // (compiled by `installBytecodePrimop` via `runRootExpr`).  The
    // map is consulted by every OP_LIT_PRIMOP / OP_CALL_PRIMOP
    // dispatch; if the cached closure dangles, the next dispatch
    // reads from freed nursery memory → forceValue chase finds a
    // memset Thunk pointer and SIGSEGVs at `desc->nLocals`.  This
    // walk closes the missed-root identified on hello.drvPath under
    // NIX_V3_NURSERY_SCAVENGE=1.
    walkBytecodePrimopRoots(rootVisit);

    // #705 (2026-05-21): static `vBuiltins` Value root.  The
    // bytecode-primop install path patches `vBuiltins.payload.bindings`
    // entries in place to point at the freshly-compiled bytecode
    // closures (see bytecode_primops.cc "Install path 3" — patches
    // `b->entries[i].value = installed.rr.value`).  Those entries
    // can carry nursery Closures.  Walk so they're forwarded.
    walkBuiltinsRoot(rootVisit);

    // #705 (2026-05-21): import-cache results.  Each entry holds a
    // Value whose payload may carry nursery Closure/Bindings — a
    // repeat builtins.import after scavenge would otherwise return
    // a stale pointer.
    walkImportCacheRoots(rootVisit);

    // #705 (2026-05-21): cached call-flake closure.  Set once at
    // first getFlake; closure may be nursery-allocated.
    walkCallFlakeRoot(rootVisit);

    // GC_AUDIT_ROUND_2 Round 1 #7 (2026-05-21): deep-force roots.
    // `print.cc::forceDeep`, `printNixValueRich(out, vm, ...)`, and
    // `toJsonValue(vm, ...)` walk Values whose `payload.list` /
    // `payload.bindings` C-locals live across recursive `forceValue`
    // calls.  When invoked at `vm.frames.empty()` (post-eval print /
    // JSON dump from the CLI), the inner forceValue enters
    // dispatchLoop at exitDepth==0 → scavenge enabled → the C-locals
    // dangle if the container is forwarded.  The fix: each of those
    // call sites pushes its current Value onto a thread-local
    // index-addressable root stack here, accesses the container
    // through the stack slot (not the C-local), and pops on exit.
    // The scavenger walks the slots so their payload pointers
    // forward correctly across nested scavenges.
    walkDeepForceRoots(rootVisit);

    // #558 Phase 3.3: partialBindingsRegistry retired (no longer
    // referenced by vm.cc).  No scavenge work needed.

    // Phase D (Stage 3, 2026-05-21): drain the inter-gen dirty-list.
    //
    // Each entry is a tenured Bindings / ValuePair / Thunk whose
    // contents were mutated to point at a nursery payload since the
    // last scavenge.  The natural-root walk above won't necessarily
    // reach these (e.g. a Bindings on the heap that's only
    // referenced from another tenured container, where the only edge
    // is through a Tag::Slot from a tenured cell that wasn't
    // otherwise reachable from valueStack/withStack/frames).  The
    // dirty-list is the remembered-set that closes the gap.
    //
    // walked-set dedup handles duplicate entries (a single container
    // pushed multiple times for multiple writes) for free.
    //
    // See `lode/NURSERY_PHASE_D_DECISION_2026-05-21.md` §2.3 for
    // design rationale + §4 Step 6 for the implementation contract.
    {
        auto & dirty = dirtyContainers();
        for (const DirtyEntry & e : dirty) {
            switch (e.kind) {
            case DirtyKind::Bindings: {
                auto * b = static_cast<Bindings *>(e.ptr);
                if (walked.insert(b).second) {
                    graylist.push_back({b, GK_BINDINGS});
                }
                break;
            }
            case DirtyKind::Pair: {
                auto * p = static_cast<ValuePair *>(e.ptr);
                if (walked.insert(p).second) {
                    graylist.push_back({p, GK_PAIR});
                }
                break;
            }
            case DirtyKind::Thunk: {
                auto * t = static_cast<Thunk *>(e.ptr);
                if (walked.insert(t).second) {
                    graylist.push_back({t, GK_THUNK});
                }
                break;
            }
            }
        }
        // Clear retaining capacity — typical steady-state list size
        // is ~thousands of entries between scavenges; keeping the
        // backing storage avoids per-scavenge realloc churn.
        dirty.clear();

        // Standalone cells: the cell pointers themselves are
        // tenured (`Alloc::allocValue`), but their CONTENTS may
        // hold a nursery payload.  Walk each cell as a root.
        auto & cells = standaloneCellRoots();
        for (Value * cell : cells) {
            visitValue(*cell);
        }
        cells.clear();
    }

    // -- Stage 2: walk graylist ---------------------------------

    drain();

    // -- Stage 3: reset bump pointer ----------------------------
    // forward / walked / graylist live in `threadScavengeBuffers()`
    // and will be cleared by the next call to `scavengeNursery`.
    // Leaving them populated until then is harmless and saves the
    // hash-table hashing-pass that `clear()` does when called now.

    n.resetBumpAfterScavenge();
}

} // namespace

// #705 post-scavenge audit (gated via V3_DBG_NURSERY_AUDIT=1).
// Walks DEEP from the scavenger's roots and asserts no nursery
// pointer remains anywhere reachable.  Localizes a missed-root.
namespace {

struct Auditor {
    const Nursery & n;
    std::unordered_set<const void *> visited;
    // #705 R6 (audit Round 2): mirror scavenger's walkedCUs so the
    // audit also walks AttrSelectIC entries transitively.
    std::unordered_set<const CompilationUnit *> walkedCUs;
    bool ok = true;

    void check(const void * p, const char * what, const char * site)
    {
        if (n.contains(p)) {
            std::fprintf(stderr,
                "v3 SCAVENGE AUDIT: nursery %s %p reachable via %s\n",
                what, p, site);
            ok = false;
        }
    }

    void visitValue(const Value & v, const char * site);
    // Forward decl: visitBindings defined further down in the struct.

    void walkCUAttrSelectCache(const CompilationUnit * cu)
    {
        if (!cu || !walkedCUs.insert(cu).second) return;
        for (const auto & ic : cu->attrSelectCache) {
            for (int w = 0; w < CompilationUnit::AttrSelectIC::kWays; ++w) {
                if (const Bindings * b = ic.entries[w].bindings)
                    visitBindings(b, "CU.attrSelectCache");
            }
        }
    }

    void visitClosure(const Closure * c, const char * site)
    {
        if (!c) return;
        check(c, "Closure", site);
        if (!visited.insert(c).second) return;
        walkCUAttrSelectCache(c->cu);
        if (c->capturedWiths) check(c->capturedWiths, "Closure.capturedWiths", site);
        if (c->capturedWiths) {
            for (uint32_t i = 0; i < c->capturedWiths->size; ++i)
                visitValue(c->capturedWiths->elems[i], "Closure.capturedWiths.elem");
        }
        for (uint16_t i = 0; i < c->nUpvalues; ++i)
            visitValue(c->upvalues[i], "Closure.upvalues[]");
    }

    void visitThunk(const Thunk * t, const char * site)
    {
        if (!t) return;
        check(t, "Thunk", site);
        if (!visited.insert(t).second) return;
        if (t->cell) {
            // cell is tenured Value*; its content may transitively
            // reach nursery.  Recurse into the cell value.
            visitValue(*t->cell, "Thunk.cell");
        }
        // Round 1 #5: shapeCell (NIX_V3_CELL_EVERYWHERE) — same as
        // cell, walk through the contents to catch nursery payloads.
        if (t->shapeCell) {
            visitValue(*t->shapeCell, "Thunk.shapeCell");
        }
        switch (t->state) {
        case ThunkState::Suspended:
            // #705 R9: walk this CU's IC.
            walkCUAttrSelectCache(t->suspended.cu);
            if (t->suspended.capturedWiths)
                check(t->suspended.capturedWiths, "Thunk.suspended.capturedWiths", site);
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i], "Thunk.suspended.tail[]");
            break;
        case ThunkState::Native:
            // N7 (audit Round 2): Suspended and Native have DIFFERENT
            // union variants.  Native's variant is { const PrimOp * fn },
            // no capturedWiths / cu / desc.  Reading those fields here
            // is out-of-bounds.  Just walk the tail[] which holds the
            // primop's accumulated args (still valid).
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i], "Thunk.Native.tail[]");
            break;
        case ThunkState::Evaluated:
            visitValue(t->evaluated, "Thunk.evaluated");
            break;
        case ThunkState::Bridge:
            break;
        case ThunkState::Blackhole:
            // #705 / N1: mirror scavenger's Blackhole walk — tail and
            // suspended.capturedWiths are live because
            // clearBlackMarksOnException can revert Blackhole →
            // Suspended on exception unwind.  See gc.cc walkThunk.
            walkCUAttrSelectCache(t->suspended.cu);
            if (t->suspended.capturedWiths)
                check(t->suspended.capturedWiths,
                      "Thunk.Blackhole.suspended.capturedWiths", site);
            for (uint16_t i = 0; i < t->nUpvalues; ++i)
                visitValue(t->tail[i], "Thunk.Blackhole.tail[]");
            break;
        }
    }

    void visitBindings(const Bindings * b, const char * site)
    {
        if (!b) return;
        check(b, "Bindings", site);
        if (!visited.insert(b).second) return;
        for (uint32_t i = 0; i < b->size; ++i)
            visitValue(b->entries[i].value, "Bindings.entries[].value");
    }

    void visitList(const ListVec * l, const char * site)
    {
        if (!l) return;
        check(l, "ListVec", site);
        if (!visited.insert(l).second) return;
        for (uint32_t i = 0; i < l->size; ++i)
            visitValue(l->elems[i], "ListVec.elems[]");
    }

    void visitPair(const ValuePair * p, const char * site)
    {
        if (!p) return;
        check(p, "ValuePair", site);
        if (!visited.insert(p).second) return;
        visitValue(p->left,      "ValuePair.left");
        visitValue(p->right,     "ValuePair.right");
        visitValue(p->evaluated, "ValuePair.evaluated");
    }
};

void Auditor::visitValue(const Value & v, const char * site)
{
    switch (v.tag()) {
    case Tag::Closure:  visitClosure(v.payload.closure,   site); break;
    case Tag::Thunk:    visitThunk  (v.payload.thunk,     site); break;
    case Tag::Attrs:    visitBindings(v.payload.bindings, site); break;
    case Tag::List:     visitList   (v.payload.list,      site); break;
    case Tag::App:
    case Tag::PrimOpApp: visitPair  (v.payload.pair,      site); break;
    case Tag::Slot:
        if (v.payload.slot) visitValue(*v.payload.slot, "Slot.cell");
        break;
    case Tag::Uninitialized:
    case Tag::Int:
    case Tag::Float:
    case Tag::Bool:
    case Tag::Null:
    case Tag::String:
    case Tag::Path:
    case Tag::PrimOp:
    case Tag::Blackhole:
    case Tag::External:
        break;
    }
}

void postScavengeAudit(const Nursery & n, const VMState & vm)
{
    Auditor a{n, {}, {}, true};
    // #705 R6 (2026-05-21 audit round 2 #8): mirror EVERY root the
    // scavenger walks, so a clean audit verdict is actually a
    // statement of "no nursery pointer reachable from any walked
    // root."  Pre-R6 the auditor walked only valueStack/withStack/
    // frames, missing bridge tables / primopReplacementMap /
    // vBuiltins / importCache / callFlake / AttrSelectIC /
    // forceWriteTarget / active-VMStack — any of those holding a
    // stale pointer would produce a false-positive clean verdict.

    // 1. Per-vm roots (current + every other active VMState on the
    //    thread — same set the scavenger walks via activeVMStack).
    auto walkVm = [&](const char * label, const VMState * vmp) {
        if (!vmp) return;
        for (size_t i = 0; i < vmp->valueStack.size(); ++i)
            a.visitValue(vmp->valueStack[i], label);
        for (size_t i = 0; i < vmp->withStack.size(); ++i)
            a.visitValue(vmp->withStack[i], label);
        for (size_t i = 0; i < vmp->frames.size(); ++i) {
            const CallFrame & f = vmp->frames[i];
            if (f.closure) a.visitClosure(f.closure, "frame.closure");
            if (f.thunk)   a.visitThunk  (f.thunk,   "frame.thunk");
            // Round 1 #6: forceWriteTarget points at a tenured Value
            // cell; the contents may carry nursery payloads.
            if (f.forceWriteTarget)
                a.visitValue(*f.forceWriteTarget, "frame.forceWriteTarget");
        }
    };
    walkVm("currentVm", &vm);
    std::unordered_set<const VMState *> seenVms{&vm};
    for (VMState * other : activeVMStack()) {
        if (!other || !seenVms.insert(other).second) continue;
        walkVm("otherVm", other);
    }

    // 2. Bridge tables — v3BridgeClosures / Attrs / Lists (primops.cc)
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "v3BridgeRoots"); };
        walkV3BridgeRoots(visit);
    }

    // 3. Bytecode-primop replacement map (bytecode_primops.cc).
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "primopReplacementMap"); };
        walkBytecodePrimopRoots(visit);
    }

    // 4. vBuiltins singleton.
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "vBuiltins"); };
        walkBuiltinsRoot(visit);
    }

    // 5. import-cache results.
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "importCache"); };
        walkImportCacheRoots(visit);
    }

    // 6. call-flake closure.
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "callFlakeRoot"); };
        walkCallFlakeRoot(visit);
    }

    // 6b. deep-force roots (Round 1 #7).  See gc.cc:run() comment.
    {
        std::function<void(Value &)> visit =
            [&](Value & v) { a.visitValue(v, "deepForceRoots"); };
        walkDeepForceRoots(visit);
    }

    // 6c. Phase D inter-gen dirty list (2026-05-21).  At AUDIT
    // time the scavenger has already drained the list; this is a
    // diagnostic-parity walk that catches missed-drain regressions.
    // The list will normally be empty by the time auditor runs.
    {
        for (const DirtyEntry & e : dirtyContainers()) {
            switch (e.kind) {
            case DirtyKind::Bindings:
                if (a.visited.insert(e.ptr).second)
                    a.visitBindings(static_cast<Bindings *>(e.ptr), "dirty.Bindings");
                break;
            case DirtyKind::Pair:
                if (a.visited.insert(e.ptr).second)
                    a.visitPair(static_cast<ValuePair *>(e.ptr), "dirty.Pair");
                break;
            case DirtyKind::Thunk:
                if (a.visited.insert(e.ptr).second)
                    a.visitThunk(static_cast<Thunk *>(e.ptr), "dirty.Thunk");
                break;
            }
        }
        for (Value * cell : standaloneCellRoots()) {
            a.visitValue(*cell, "dirty.cell");
        }
    }

    // 7. AttrSelectIC entries via reached Closures / Thunks.
    //    Already handled implicitly: visitClosure / visitThunk above
    //    queue the IC entries' Bindings via the walkedCUs/visited
    //    deduplication.  No extra step needed here — but if R6 is
    //    ever reorganized, add explicit IC walks per CU.

    if (a.ok) {
        std::fprintf(stderr,
            "v3 SCAVENGE AUDIT: clean (deep walk found no nursery pointers)\n");
    } else {
        std::fprintf(stderr,
            "v3 SCAVENGE AUDIT: visited=%zu objects; pointers above are stale\n",
            a.visited.size());
    }
    std::fflush(stderr);
}

// #705 (2026-05-21): brute-force tenured-arena scan.
//
// Gated V3_DBG_NURSERY_BRUTE=1 (separate from AUDIT because it's
// expensive — O(arena_size) per scavenge).  Walks every 8-byte
// aligned word in every tenured arena block and checks whether the
// word is a pointer into the nursery range.  Hits reveal exactly
// which arena offset holds a stale nursery pointer that the deep
// reachable-graph audit missed.
//
// Phase 1.7 R1 refinement (2026-05-21): filter to LIVE objects only.
// Without filtering, BRUTE flags every tenured byte that happens to
// hold a pointer-shaped value inside the nursery range, including
// dead-but-arena-resident objects (Boehm pins the whole arena as a
// root → dead Closures / Bindings stay in memory with their stale
// nursery pointers).  Those hits are harmless noise — no one
// dereferences a dead object.  The real signal is hits inside
// objects the scavenger considers REACHABLE; those represent true
// missed-root bugs where a live object holds a pointer the
// scavenger failed to forward.
//
// Implementation: the Scavenger records [start, end) byte ranges
// for every tenured object it walked into `liveTenuredRanges`;
// they're sorted by start address after drain() and passed here.
// We classify each hit as "live" (inside one of the ranges) or
// "dead" (outside) and report counts separately.  A live hit is
// the Phase 1.7 stop-the-world signal.
//
// `liveRanges` MUST be sorted by start ascending; the caller is
// responsible.  Binary search via std::upper_bound for O(log N)
// per word.
void postScavengeBruteScan(
    const Nursery & n,
    const std::vector<std::pair<uintptr_t, uintptr_t>> & liveRanges)
{
    Arena & arena = threadArena();
    auto blocks = arena.blockRanges();
    // Predicate: is address p inside some live range?  Binary search
    // for the largest range whose start <= p, then check end > p.
    auto inLive = [&](uintptr_t p) -> bool {
        // upper_bound gives the first range with start > p.
        auto it = std::upper_bound(
            liveRanges.begin(), liveRanges.end(),
            std::make_pair(p, uintptr_t{0}),
            [](const auto & a, const auto & b) { return a.first < b.first; });
        if (it == liveRanges.begin()) return false;
        --it;
        return p < it->second;  // it->first <= p < it->second
    };

    size_t hitsLive = 0;
    size_t hitsDead = 0;
    size_t cap = 16;  // dump first N LIVE hits (dead hits are noise; just count)
    for (auto & blk : blocks) {
        // Walk 8-byte aligned words.
        const uintptr_t step = 8;
        uintptr_t lo = reinterpret_cast<uintptr_t>(blk.begin);
        uintptr_t hi = reinterpret_cast<uintptr_t>(blk.end);
        lo = (lo + step - 1) & ~(step - 1);  // align up
        for (uintptr_t p = lo; p + step <= hi; p += step) {
            uintptr_t w = *reinterpret_cast<const uintptr_t *>(p);
            if (w == 0) continue;
            if (!n.contains(reinterpret_cast<const void *>(w))) continue;
            if (inLive(p)) {
                if (hitsLive < cap) {
                    std::fprintf(stderr,
                        "v3 SCAVENGE BRUTE: arena word @ %p holds "
                        "nursery pointer %p\n",
                        (void*)p, (void*)w);
                }
                ++hitsLive;
            } else {
                ++hitsDead;
            }
        }
    }
    // Continue to report the combined "tenured words" count — but
    // ONLY when hitsLive > 0 (live hits are the actionable signal).
    // Dead-only hits get a separate one-line summary so users know
    // the brute scan ran and how much arena bloat is present.
    size_t hits = hitsLive;
    std::fprintf(stderr,
        "v3 SCAVENGE BRUTE: %zu tenured words point into nursery "
        "(first %zu dumped above)\n", hits, std::min(hits, cap));
    if (hitsDead > 0) {
        // Dead hits = arena bloat (Boehm pins arena → dead tenured
        // objects retain stale nursery pointers).  Informational
        // only; future work: precise per-object arena root
        // registration (Stage 3 Phase D adjacent).
        std::fprintf(stderr,
            "v3 SCAVENGE BRUTE: %zu tenured words inside DEAD "
            "(unreachable-from-v3) tenured objects — arena-bloat, "
            "not a missed root\n", hitsDead);
    }
    std::fflush(stderr);
}

} // namespace

void scavengeNursery(Nursery & n, VMState & vm) noexcept
{
    static const bool s_dbg = std::getenv("V3_DBG_NURSERY") != nullptr;
    static const bool s_audit = std::getenv("V3_DBG_NURSERY_AUDIT") != nullptr;
    Nursery::Stats pre{};
    if (s_dbg) pre = n.stats();
    ScavengeBuffers & buf = threadScavengeBuffers();
    buf.clear();
    Scavenger sc{n, vm, buf.forward, buf.walked, buf.graylist};
    sc.run();
    if (__builtin_expect(s_audit, 0)) postScavengeAudit(n, vm);
    static const bool s_brute = std::getenv("V3_DBG_NURSERY_BRUTE") != nullptr;
    if (__builtin_expect(s_brute, 0)) {
        // Sort the live-tenured-range list by start address so
        // postScavengeBruteScan's binary-search lookup is well-formed.
        std::sort(sc.liveTenuredRanges.begin(), sc.liveTenuredRanges.end());
        postScavengeBruteScan(n, sc.liveTenuredRanges);
    }
    // V3_DBG_NURSERY=1 — print one line per scavenge with the
    // forward-map size + tenured-walk size so we can verify the
    // pass actually moved live data and how much it had to
    // process.  Cached env-var lookup so the loop hot path stays
    // free of getenv calls.
    if (s_dbg) [[unlikely]] {
        Nursery::Stats post = n.stats();
        std::fprintf(stderr,
            "[v3 nursery] scavenge#%llu  forwarded=%zu  walked=%zu  "
            "used-pre=%zuB/%zuB\n",
            (unsigned long long)post.scavengeCount,
            sc.forward.size(), sc.walked.size(),
            pre.used, pre.sizeBytes);
    }
}

bool Nursery::maybeScavenge(VMState & vm) noexcept
{
    if (!scavengeEnabled || !shouldScavenge()) return false;
    scavengeNursery(*this, vm);
    return true;
}

bool Nursery::forceScavenge(VMState & vm) noexcept
{
    // STRESS bypasses the `scavengeEnabled` gate (the whole point
    // is to force scavenges even when production users haven't
    // opted in to NIX_V3_NURSERY_SCAVENGE=1).  Still requires the
    // nursery itself to be `enabled` — without it there's no
    // backing buffer to scavenge.
    if (!enabled || !base) return false;
    scavengeNursery(*this, vm);
    return true;
}

} // namespace nix::v3
