/// @file
/// v3 Value singletons.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/value.hh"
#include "v3/alloc.hh"
#include "v3/barrier.hh"
#include "v3/ir.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nix::v3 {

// SCOPING (broader-A, 2026-06-14): gated per-caller materialize-volume
// attribution helpers (see Bindings::materialize).  Default-off; zero cost
// when V3_DBG_MAT_SITES is unset.
namespace {
struct MatSiteStats {
    std::unordered_map<void *, std::pair<uint64_t, uint64_t>> sites;  // addr → (copies, bytes)
    bool atexitRegistered = false;
};
inline MatSiteStats & matSiteStats() { static MatSiteStats s; return s; }
inline bool matSitesEnabled() {
    static const bool e = std::getenv("V3_DBG_MAT_SITES") != nullptr;
    return e;
}
} // namespace

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
// M-1 (CODEBASE_REVIEW_2026-06-11): the per-chain materialize memo, hoisted to
// file scope so the major-GC safepoint can clear it (see
// Bindings::clearMaterializeMemo).  The map is NOT walked as GC roots, so under
// default-ON major GC: (a) a flat copy reachable only via the memo would be
// swept → a later memo HIT returns a dangling pointer; (b) a freed+reused chain
// address aliases a key → the wrong Bindings is returned for an unrelated
// chain.  Clearing at every collection makes both impossible (the memo
// repopulates on the next materialise; the only cost is at most K copies per
// GC epoch instead of K per eval).
static thread_local std::unordered_map<const Bindings *, const Bindings *>
    s_matMemo;

void Bindings::clearMaterializeMemo()
{
    s_matMemo.clear();
}

Value Bindings::makeMapAttrsNameValue(SymbolId nameId) noexcept
{
    const auto & symTab = ir::globalSymbolTable();
    std::string fallback;
    std::string_view name =
        nameId < symTab.size()
            ? std::string_view(symTab[nameId])
            : std::string_view(fallback = std::to_string(nameId));
    char * nameBuf = Alloc::allocChars(name.size() + 1);
    std::memcpy(nameBuf, name.data(), name.size());
    nameBuf[name.size()] = '\0';
    Value nameStr;
    nameStr.mkString(nameBuf);
    return nameStr;
}

void Bindings::realizeMapAttrsEntry(Entry * e) noexcept
{
    if (!e || !isMapAttrs()) return;
    // Initial cache state: primMapAttrs stored the original source value here
    // and tagged the pos word.  Do not key on Value shape: the mapped result may
    // legitimately force to any tag later.
    if ((e->pos & kMapAttrsUnrealizedPosBit) == 0) return;
    Value nameStr = makeMapAttrsNameValue(e->name);
    Value src = mapAttrsEntrySource(e);

    ValuePair * pp = Alloc::allocPair();
    pp->left = aux;
    pp->right = nameStr;
    pp->third = src;
    pairPostConstructBarrier(pp);

    Value app3;
    app3.mkPair(Tag::App3, pp);
    e->pos &= kPosMask;
    uint32_t idx = static_cast<uint32_t>(e - entries);
    bindingsSetValue(this, idx, app3);
}

Value Bindings::mapAttrsEntrySource(Entry * e) noexcept
{
    if (!e) {
        Value v;
        v.mkUninitialized();
        return v;
    }
    Value src = e->value;
    if (!isMapAttrs() || !parent || !parent->isMapAttrs())
        return src;

    // MapAttrs composition: primMapAttrs can now build
    // `mapAttrs f (mapAttrs g src)` without realizing every `g` entry.
    // On demand, first turn the corresponding parent entry into its lazy
    // mapped value, then pass that value as the source argument to `f`.
    Bindings * p = const_cast<Bindings *>(parent);
    if (Entry * pe = p->lookupLocalEntry(e->name)) {
        p->realizeMapAttrsEntry(pe);
        src = pe->value;
    }
    return src;
}

const Bindings * Bindings::materialize() const
{
    if (kind == uint8_t(Kind::Sorted)) return this;

    // Memoize: a chain iterated K times would otherwise allocate K full
    // materialised copies (measured +195 MB on hello.drvPath — the chain
    // SAVED 301 MB of merge copies but the un-memoised materialise re-added
    // ~496 MB).  Cache the materialised Sorted result per-chain so each
    // chain materialises at most once per GC epoch (s_matMemo is cleared at
    // the major-GC safepoint — see clearMaterializeMemo / M-1 above).
    if (auto it = s_matMemo.find(this); it != s_matMemo.end())
        return it->second;

    if (kind == uint8_t(Kind::MapAttrs)) {
        Bindings * out = Alloc::allocBindings(size);
        auto * self = const_cast<Bindings *>(this);
        for (uint32_t i = 0; i < size; ++i) {
            self->realizeMapAttrsEntry(&self->entries[i]);
            bindingsSetEntry(out, i, self->entries[i]);
        }
        bindingsPostConstructBarrier(out);
        s_matMemo.emplace(this, out);
        return out;
    }

    // Walk the chain leaf-first (overlay-first); collect (level, entry)
    // pairs into a flat vector; sort by (name, level); keep the first
    // occurrence of each name (which is overlay-winning because lower
    // level == closer to leaf == overlay).
    struct LevelEntry { uint16_t level; Entry e; };
    uint32_t cap = 0;
    for (const Bindings * b = this; b; b = b->isChain() ? b->parent : nullptr)
        cap += b->size;
    std::vector<LevelEntry> all;
    all.reserve(cap);
    uint16_t lvl = 0;
    for (const Bindings * b = this; b; b = b->isChain() ? b->parent : nullptr, ++lvl) {
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

    // SCOPING (broader-A, 2026-06-14, gated V3_DBG_MAT_SITES): per-CALLER
    // materialize-volume attribution.  This allocation is the flat copy made on
    // an s_matMemo MISS (the chain's first materialisation); bucket it by the
    // caller return address + bytes and dump the top consumers atexit (dladdr
    // symbolised).  Finds which consumer owns the dominant materialize volume —
    // the target for extending lookup-without-materialize beyond SELECT (L1).
    if (__builtin_expect(matSitesEnabled(), 0)) {
        void * ra = __builtin_return_address(0);
        auto & st = matSiteStats();
        auto & e = st.sites[ra];
        e.first += 1;
        e.second += uint64_t(uniq.size()) * sizeof(Entry) + sizeof(Bindings);
        if (!st.atexitRegistered) {
            st.atexitRegistered = true;
            std::atexit([] {
                auto & s = matSiteStats();
                std::vector<std::pair<void *, std::pair<uint64_t, uint64_t>>> v(
                    s.sites.begin(), s.sites.end());
                std::sort(v.begin(), v.end(), [](auto & a, auto & b) {
                    return a.second.second > b.second.second;  // by bytes desc
                });
                uint64_t totB = 0, totC = 0;
                for (auto & p : v) { totB += p.second.second; totC += p.second.first; }
                std::fprintf(stderr,
                    "v3 materialize-sites: total %.1f MB over %llu flat copies "
                    "(%zu distinct callers):\n",
                    totB / 1e6, (unsigned long long)totC, v.size());
                for (size_t i = 0; i < v.size() && i < 18; ++i) {
                    Dl_info di;
                    const char * sym = "?"; long off = 0;
                    if (dladdr(v[i].first, &di) && di.dli_sname) {
                        sym = di.dli_sname;
                        off = (char *)v[i].first - (char *)di.dli_saddr;
                    }
                    std::fprintf(stderr,
                        "  %8.1f MB  %8llu copies  %s+%ld\n",
                        v[i].second.second / 1e6,
                        (unsigned long long)v[i].second.first, sym, off);
                }
            });
        }
    }

    // Phase D post-construct barrier: if any entry holds a nursery
    // payload, dirty-list the result so the next scavenge walks it.
    // No-op when phaseDActive() is false (default-on but cheap when
    // NIX_V3_NURSERY isn't set; the function early-returns inside).
    bindingsPostConstructBarrier(out);

    s_matMemo.emplace(this, out);
    return out;
}

} // namespace nix::v3
