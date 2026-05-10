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
#include "v3/closure.hh"
#include "v3/value.hh"
#include "v3/vm.hh"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix::v3 {

// vm.cc owns the registry; we re-declare the accessor here so we
// can rewrite forwarded `Thunk *` keys and walk `Bindings *` values.
std::unordered_map<Thunk *, Bindings *> & partialBindingsRegistry();

namespace {

enum GrayKind : uint8_t {
    GK_CLOSURE  = 0,
    GK_THUNK    = 1,
    GK_LIST     = 2,
    GK_BINDINGS = 3,
    GK_PAIR     = 4,
};

struct Gray { void * ptr; uint8_t kind; };

/// Per-scavenge state.  Created on the stack inside
/// `scavengeNursery`; lifetime is bounded by that call.
struct Scavenger
{
    Nursery & n;
    VMState & vm;
    /// nursery oldPtr -> tenured newPtr (lookup before copy)
    std::unordered_map<void *, void *> forward;
    /// objects already queued for walk (deduplication for both
    /// freshly-copied tenured AND originally-tenured paths)
    std::unordered_set<void *> walked;
    /// queued objects to walk in `drain()`
    std::vector<Gray> graylist;

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

Thunk * Scavenger::fwdThunk(Thunk * t)
{
    if (!t) return nullptr;
    if (n.contains(t)) {
        auto it = forward.find(t);
        if (it != forward.end()) return static_cast<Thunk *>(it->second);
        // Suspended / Native carry a FAM tail of `nUpvalues` Values;
        // Evaluated / Bridge / Blackhole occupy only the header.
        size_t bytes;
        switch (t->state) {
        case ThunkState::Suspended:
        case ThunkState::Native:
            bytes = sizeof(Thunk) + sizeof(Value) * t->nUpvalues;
            break;
        case ThunkState::Evaluated:
        case ThunkState::Blackhole:
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

void Scavenger::walkClosure(Closure * c)
{
    if (c->capturedWiths) c->capturedWiths = fwdList(c->capturedWiths);
    for (uint16_t i = 0; i < c->nUpvalues; ++i) {
        visitValue(c->upvalues[i]);
    }
}

void Scavenger::walkThunk(Thunk * t)
{
    // The cell (write-back target for OP_RETURN) is tenured; walk
    // its current Value so any nursery payload it holds is found.
    if (t->cell && walked.insert(t->cell).second) {
        visitValue(*t->cell);
    }
    switch (t->state) {
    case ThunkState::Suspended:
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
        // Currently-being-forced; payload is irrelevant until the
        // body completes (state will flip to Evaluated then).
        break;
    }
}

void Scavenger::walkList(ListVec * l)
{
    for (uint32_t i = 0; i < l->size; ++i) {
        visitValue(l->elems[i]);
    }
}

void Scavenger::walkBindings(Bindings * b)
{
    for (uint32_t i = 0; i < b->size; ++i) {
        visitValue(b->entries[i].value);
    }
}

void Scavenger::walkPair(ValuePair * p)
{
    visitValue(p->left);
    visitValue(p->right);
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
    }

    // partialBindings registry (vm.cc): Thunk * keys may have been
    // copied; Bindings * values need to be walked (they're tenured
    // but might hold nursery refs in entries[]).  We rebuild the
    // map in one pass to fix forwarded keys.
    {
        auto & reg = partialBindingsRegistry();
        if (!reg.empty()) {
            std::unordered_map<Thunk *, Bindings *> rebuilt;
            rebuilt.reserve(reg.size());
            for (auto & [k, v] : reg) {
                Thunk * nk = fwdThunk(k);
                Bindings * nv = fwdBindings(v);
                rebuilt[nk] = nv;
            }
            reg = std::move(rebuilt);
        }
    }

    // -- Stage 2: walk graylist ---------------------------------

    drain();

    // -- Stage 3: reset bump pointer, drop forward map ---------

    forward.clear();
    walked.clear();
    n.resetBumpAfterScavenge();
}

} // namespace

void scavengeNursery(Nursery & n, VMState & vm) noexcept
{
    Scavenger sc{n, vm, {}, {}, {}};
    sc.run();
}

bool Nursery::maybeScavenge(VMState & vm) noexcept
{
    if (!scavengeEnabled || !shouldScavenge()) return false;
    scavengeNursery(*this, vm);
    return true;
}

} // namespace nix::v3
