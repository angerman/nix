/// @file
/// v3 Value singletons.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/alloc.hh"
#include "v3/barrier.hh"

#include <algorithm>
#include <deque>
#include <vector>

namespace nix::v3 {

// Static empty containers — used as the payload of vEmptyAttrs /
// vEmptyList so callers that dereference `payload.bindings` /
// `payload.list` see a real `size = 0` object instead of dereferencing
// null.  Important now that we hand out the singletons from
// OP_ATTRS_INIT 0 / OP_LIST_INIT 0.  The trailing FAM `entries`/`elems`
// arrays are zero-sized so no extra bytes are needed.
namespace {
Bindings sEmptyBindings = []{
    // #823 / A1a Phase A: post-rename, the `_pad` slot is now `kind`
    // (Sorted = 0 default) + `_pad8[3]`; default constructor handles
    // both via in-class initialisers.  Only `size` (no default) must
    // be set explicitly.  The new `parent` field defaults to nullptr.
    Bindings b;
    b.size = 0;
    return b;
}();
ListVec  sEmptyList     = []{ ListVec l; l.size = 0; l._pad = 0; return l; }();
} // anonymous namespace

// #703: expose the empty-Bindings sentinel to `Alloc::allocBindings(0)`.
// Defined out-of-line here (rather than as an inline in alloc.hh) so
// the sentinel address is stable across translation units — every
// caller observes the same pointer.
Bindings * Alloc::emptyBindingsSentinel() noexcept
{
    return &sEmptyBindings;
}

#ifdef V3_VALUE_8B
// Bootstrap singletons under the tagged 8B layout — constructed via the NaN-box
// codec directly (can't use mkBool/mkNull: they reference vTrue/vNull circularly).
Value Value::vTrue       = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::Bool),      1); return v; }();
Value Value::vFalse      = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::Bool),      0); return v; }();
Value Value::vNull       = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::Null),      0); return v; }();
Value Value::vBlackhole  = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::Blackhole), 0); return v; }();
Value Value::vEmptyList  = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::List),  reinterpret_cast<uintptr_t>(&sEmptyList));     return v; }();
Value Value::vEmptyAttrs = []() { Value v; v.w = v8nan::box(v8nan::codeOf(Tag::Attrs), reinterpret_cast<uintptr_t>(&sEmptyBindings)); return v; }();

// Overflow-int cells: a Nix int outside ±2^47 can't be inlined in the 48-bit
// NaN-box payload, so mkInt() boxes it here.  Backed by a thread_local deque —
// stable addresses (deque never relocates), owned for the thread's lifetime so
// the cell is always live (no arena/GC entanglement; tagIsPointer(Int)==false so
// the precise walker would not keep an arena cell alive anyway).  Overflow ints
// are rare (most counts/sizes/timestamps fit 48-bit); L2b may move these to the
// arena with a leaf-keepalive if they ever prove common on a real eval.
namespace v8nan {
const void * boxInt64(int64_t n)
{
    static thread_local std::deque<int64_t> s_cells;
    s_cells.push_back(n);
    return &s_cells.back();
}
int64_t unboxInt64(const void * cell) noexcept
{
    return *reinterpret_cast<const int64_t *>(cell);
}
} // namespace v8nan
#else
Value Value::vTrue       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 1; return v; }();
Value Value::vFalse      = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Bool);      v.payload.i = 0; return v; }();
Value Value::vNull       = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Null);      v.payload.raw = nullptr; return v; }();
Value Value::vBlackhole  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Blackhole); v.payload.raw = nullptr; return v; }();
Value Value::vEmptyList  = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::List);      v.payload.list = &sEmptyList; return v; }();
Value Value::vEmptyAttrs = []() { Value v; v.tag_payload = static_cast<uint64_t>(Tag::Attrs);     v.payload.bindings = &sEmptyBindings; return v; }();
#endif

// #825 / A1a Phase B (2026-05-26) — `Bindings::materialize` out-of-line
// definition.  See the corresponding stub comment in `alloc.hh` for
// why this lives here (circular include via `barrier.hh`).
//
// Walks the chain leaf-first, deduplicates by name (overlay wins),
// sorts, and emits a freshly-allocated Sorted Bindings.  Crucially,
// calls `bindingsPostConstructBarrier(out)` on the result to honour
// Phase D's inter-generational tracking: the raw `entries[i] =
// uniq[i]` writes below DO NOT fire the per-entry write barrier
// (they're inline struct copies into newly-allocated memory), so a
// post-construction sweep is required.  Without this barrier call,
// any chain whose entries reference nursery payloads would leak
// into a materialised Bindings that the scavenger never walks (the
// container has no dirty-list entry; Phase D Step 7 trusts the
// dirty list and skips `walkBindings`), producing the audit failure
// signature seen in the deferred Phase C SPIKE.
const Bindings * Bindings::materialize() const
{
    if (kind == uint8_t(Kind::Sorted)) return this;

    // Memoize: a chain iterated K times would otherwise allocate K full
    // materialised copies (measured +195 MB on hello.drvPath — the chain
    // SAVED 301 MB of merge copies but the un-memoised materialise re-added
    // ~496 MB).  Cache the materialised Sorted result per-chain so each
    // chain materialises at most once.  Thread-local side table (no struct
    // change); pointers are arena-lived within a single eval (chains don't
    // survive VMState teardown, so cross-eval staleness can't be observed).
    static thread_local std::unordered_map<const Bindings *, const Bindings *>
        s_matMemo;
    if (auto it = s_matMemo.find(this); it != s_matMemo.end())
        return it->second;

    // Walk the chain leaf-first (overlay-first); collect (level, entry)
    // pairs into a flat vector; sort by (name, level); keep the first
    // occurrence of each name (which is overlay-winning because lower
    // level == closer to leaf == overlay).
    struct LevelEntry { uint16_t level; Entry e; };
    uint32_t cap = 0;
    for (const Bindings * b = this; b; b = b->parent) cap += b->size;
    std::vector<LevelEntry> all;
    all.reserve(cap);
    uint16_t lvl = 0;
    for (const Bindings * b = this; b; b = b->parent, ++lvl) {
        for (uint32_t i = 0; i < b->size; ++i) {
            all.push_back({lvl, b->entries[i]});
        }
    }
    // Stable-sort by (name, level): same-name group together with
    // overlay-first within group.
    std::sort(all.begin(), all.end(),
        [](const LevelEntry & x, const LevelEntry & y) {
            if (x.e.name != y.e.name) return x.e.name < y.e.name;
            return x.level < y.level;
        });

    // Dedup: keep first occurrence per name.
    std::vector<Entry> uniq;
    uniq.reserve(all.size());
    for (size_t i = 0; i < all.size();) {
        uniq.push_back(all[i].e);
        SymbolId n = all[i].e.name;
        while (i < all.size() && all[i].e.name == n) ++i;
    }

    Bindings * out = Alloc::allocBindings(uint32_t(uniq.size()));
    for (size_t i = 0; i < uniq.size(); ++i) out->entries[i] = uniq[i];

    // Phase D post-construct barrier: if any entry holds a nursery
    // payload, dirty-list the result so the next scavenge walks it.
    // No-op when phaseDActive() is false (default-on but cheap when
    // NIX_V3_NURSERY isn't set; the function early-returns inside).
    bindingsPostConstructBarrier(out);

    s_matMemo.emplace(this, out);
    return out;
}

} // namespace nix::v3
