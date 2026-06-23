#pragma once
/// @file
/// Persistent HAMT for v3 attrsets — Change 2 (#149, ARCH_BEAT_TW_PROGRAM;
/// design: lode/C2_HAMT_DESIGN_2026-06-23.md).
///
/// THIS IS THE ALGORITHM-VALIDATION INCREMENT (migration plan step 1): a
/// standalone, header-only persistent HAMT keyed by uint32 (the SymbolId) with
/// reference-counted nodes.  It validates the RISKY core — persistent
/// path-copying insert, right-biased merge (Nix `//` semantics: the right
/// operand shadows the left), lookup, size, and structural SHARING (insert/merge
/// return a new HAMT without mutating the old, sharing all untouched subtrees).
///
/// LATER #149 increments (NOT here) replace the shared_ptr nodes with v3-arena
/// cells + Phase-D write barriers + a scavenger walker (per CLAUDE.md constraint
/// #0), then swap it behind `NIX_V3_HAMT_BINDINGS` for the Sorted/Chain Bindings
/// and migrate the ~30 consumers.  Keeping the algorithm separate lets it be
/// unit-tested in isolation (no GC, no eval) before the integration risk.
///
/// 32-bit keys + 5-bit chunks ⇒ ≤ 7 levels; two distinct keys always differ in
/// some chunk, so no collision-node handling is needed (recurse until they
/// split).  Canonical: the same key SET yields the same shape regardless of
/// insertion order (required for the lockstep consumers — see the design's
/// audit item #1).
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace nix::v3::champ {

inline unsigned popcount32(uint32_t x) noexcept { return (unsigned)__builtin_popcount(x); }

/// Persistent HAMT keyed by uint32, value type V (the production V is the v3
/// `Value` — 8 B; the unit test uses uint64_t).
template <class V>
class Hamt
{
    static constexpr unsigned kBits = 5;
    static constexpr unsigned kMask = (1u << kBits) - 1;

    struct Node;
    using NodeRef = std::shared_ptr<const Node>;

    /// A slot is EITHER a leaf (`child == nullptr`, holds key/val) or a branch
    /// (`child != nullptr`, points to a subnode).
    struct Slot {
        uint32_t key = 0;
        V        val{};
        NodeRef  child;
        bool isBranch() const noexcept { return (bool)child; }
    };

    /// `bitmap` marks which of the 32 positions are occupied; `slots` is the
    /// popcount-packed array (slot for position p is at index popcount(bitmap &
    /// ((1<<p)-1))).
    struct Node {
        uint32_t          bitmap = 0;
        std::vector<Slot> slots;
    };

    NodeRef  root_;
    uint32_t size_ = 0;

    static unsigned idxFor(uint32_t bitmap, unsigned pos) noexcept
    {
        return popcount32(bitmap & ((1u << pos) - 1));
    }

    static const V * lookupIn(const Node * n, uint32_t key, unsigned shift)
    {
        for (;;) {
            unsigned pos = (key >> shift) & kMask;
            if (!(n->bitmap & (1u << pos))) return nullptr;
            const Slot & s = n->slots[idxFor(n->bitmap, pos)];
            if (!s.isBranch()) return s.key == key ? &s.val : nullptr;
            n = s.child.get();
            shift += kBits;
        }
    }

    /// Returns the new subtree for `n` with (key,val) inserted; sets `grew` if a
    /// new key was added (vs an overwrite).  Path-copying: untouched subtrees are
    /// shared by reference.
    static NodeRef insertIn(const Node * n, uint32_t key, const V & val,
                            unsigned shift, bool & grew)
    {
        unsigned pos = (key >> shift) & kMask;
        auto out = std::make_shared<Node>(*n);   // copy this node (path-copy)
        if (!(n->bitmap & (1u << pos))) {         // empty position → new leaf
            out->bitmap |= (1u << pos);
            out->slots.insert(out->slots.begin() + idxFor(out->bitmap, pos),
                              Slot{key, val, nullptr});
            grew = true;
            return out;
        }
        unsigned i = idxFor(n->bitmap, pos);
        const Slot & s = n->slots[i];
        if (s.isBranch()) {                       // recurse into subnode
            out->slots[i].child = insertIn(s.child.get(), key, val, shift + kBits, grew);
            return out;
        }
        if (s.key == key) {                       // overwrite existing leaf
            out->slots[i].val = val;
            grew = false;
            return out;
        }
        // Two distinct leaves at this position → split into a subnode holding
        // both (recurse until their chunks differ).
        out->slots[i].child = splitPair(s.key, s.val, key, val, shift + kBits);
        out->slots[i].key = 0; out->slots[i].val = V{};
        grew = true;
        return out;
    }

    /// Build a subnode containing two distinct keys, starting at `shift`.
    static NodeRef splitPair(uint32_t k1, const V & v1, uint32_t k2, const V & v2,
                             unsigned shift)
    {
        unsigned p1 = (k1 >> shift) & kMask, p2 = (k2 >> shift) & kMask;
        auto n = std::make_shared<Node>();
        if (p1 == p2) {                           // still collide at this level
            n->bitmap = (1u << p1);
            n->slots.push_back(Slot{0, V{}, splitPair(k1, v1, k2, v2, shift + kBits)});
        } else {
            n->bitmap = (1u << p1) | (1u << p2);
            // packed order: lower position first
            if (p1 < p2) { n->slots.push_back(Slot{k1, v1, nullptr});
                           n->slots.push_back(Slot{k2, v2, nullptr}); }
            else         { n->slots.push_back(Slot{k2, v2, nullptr});
                           n->slots.push_back(Slot{k1, v1, nullptr}); }
        }
        return n;
    }

    static void forEachIn(const Node * n, const std::function<void(uint32_t, const V &)> & fn)
    {
        for (const Slot & s : n->slots) {
            if (s.isBranch()) forEachIn(s.child.get(), fn);
            else fn(s.key, s.val);
        }
    }

public:
    Hamt() : root_(std::make_shared<Node>()), size_(0) {}

    uint32_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    const V * lookup(uint32_t key) const { return lookupIn(root_.get(), key, 0); }

    /// Persistent insert: returns a NEW Hamt; `*this` is unchanged.
    Hamt insert(uint32_t key, const V & val) const
    {
        bool grew = false;
        Hamt r;
        r.root_ = insertIn(root_.get(), key, val, 0, grew);
        r.size_ = size_ + (grew ? 1 : 0);
        return r;
    }

    /// Right-biased merge = Nix `a // b`: `b`'s value wins on key conflict.
    /// `this` = a, `other` = b.  Persistent; shares untouched subtrees.
    Hamt update(const Hamt & b) const
    {
        Hamt r = *this;
        b.forEach([&](uint32_t k, const V & v) { r = r.insert(k, v); });
        return r;
    }

    void forEach(const std::function<void(uint32_t, const V &)> & fn) const
    {
        forEachIn(root_.get(), fn);
    }

    /// Add every Node reachable from this version's root to `seen` (deduped by
    /// pointer).  Used by tests to PROVE structural sharing: after a persistent
    /// insert/merge, the new version's tree shares all untouched subtrees with
    /// the old one, so walking both into one set adds only the copied path —
    /// `seen` grows by ~the trie depth, not by a full second tree.
    void collectNodes(std::unordered_set<const void *> & seen) const
    {
        collectIn(root_.get(), seen);
    }

private:
    static void collectIn(const Node * n, std::unordered_set<const void *> & seen)
    {
        if (!seen.insert(n).second) return;        // already visited (shared)
        for (const Slot & s : n->slots)
            if (s.isBranch()) collectIn(s.child.get(), seen);
    }
};

} // namespace nix::v3::champ
