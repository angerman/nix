#pragma once
/// @file
/// Arena-backed persistent HAMT operations — Change 2 (#149).
///
/// Operates on `HamtNode*` (tenured arena cells, allocHamtNode).  Ports the
/// champ.hh algorithm (proven by smoke tests: persistent insert/merge, structural
/// sharing, canonical order) to the arena, calling `hamtNodePostConstructBarrier`
/// after every node construction so the moving scavenger (walkHamtNode / GK_HAMT)
/// finds each leaf slot's nursery Value payload.
///
/// GC-safety: tenured arena alloc does NOT trigger a nursery scavenge (scavenge is
/// nursery-triggered), so the build (alloc → fill slots → barrier) is atomic w.r.t.
/// the moving GC — identical to allocEnv's fill-then-barrier pattern.  Tenured
/// HamtNodes never move; only their leaf Value payloads are forwarded.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
/// Input Output Group.  SPDX-License-Identifier: Apache-2.0

#include "v3/alloc.hh"
#include "v3/barrier.hh"

namespace nix::v3::ahamt {

inline constexpr unsigned kBits = 5;
inline constexpr unsigned kMask = (1u << kBits) - 1;

inline unsigned idxFor(uint32_t bitmap, unsigned pos) noexcept
{
    return (unsigned)__builtin_popcount(bitmap & ((1u << pos) - 1));
}

/// Lookup `key`; returns its leaf slot (with key/pos/val) or nullptr.
inline const HamtNode::Slot * lookup(const HamtNode * n, uint32_t key) noexcept
{
    unsigned shift = 0;
    while (n) {
        unsigned pos = (key >> shift) & kMask;
        if (!(n->bitmap & (1u << pos))) return nullptr;
        const HamtNode::Slot & s = n->slots[idxFor(n->bitmap, pos)];
        if (!s.isBranch()) return s.key == key ? &s : nullptr;
        n = s.child;
        shift += kBits;
    }
    return nullptr;
}

/// Copy a node (path-copy step); caller mutates one slot then runs the barrier.
inline HamtNode * copyOf(const HamtNode * src) noexcept
{
    HamtNode * out = Alloc::allocHamtNode(src->nSlots);
    out->bitmap = src->bitmap;
    for (uint16_t i = 0; i < src->nSlots; ++i) out->slots[i] = src->slots[i];
    return out;
}

/// Subnode holding two distinct leaves (recurse until their chunks differ).
inline HamtNode * splitPair(const HamtNode::Slot & a, const HamtNode::Slot & b,
                            unsigned shift) noexcept
{
    unsigned pa = (a.key >> shift) & kMask, pb = (b.key >> shift) & kMask;
    if (pa == pb) {
        HamtNode * n = Alloc::allocHamtNode(1);
        n->bitmap = (1u << pa);
        n->slots[0].child = splitPair(a, b, shift + kBits);
        hamtNodePostConstructBarrier(n);
        return n;
    }
    HamtNode * n = Alloc::allocHamtNode(2);
    n->bitmap = (1u << pa) | (1u << pb);
    if (pa < pb) { n->slots[0] = a; n->slots[1] = b; }
    else         { n->slots[0] = b; n->slots[1] = a; }
    hamtNodePostConstructBarrier(n);
    return n;
}

inline HamtNode * insertAt(const HamtNode * n, uint32_t key, uint32_t pos,
                           const Value & val, unsigned shift, bool & grew) noexcept
{
    unsigned p = (key >> shift) & kMask;
    if (!(n->bitmap & (1u << p))) {                 // empty position → new leaf
        HamtNode * out = Alloc::allocHamtNode(uint16_t(n->nSlots + 1));
        out->bitmap = n->bitmap | (1u << p);
        unsigned at = idxFor(out->bitmap, p);
        for (unsigned i = 0; i < at; ++i) out->slots[i] = n->slots[i];
        out->slots[at] = HamtNode::Slot{key, pos, val, nullptr};
        for (unsigned i = at; i < n->nSlots; ++i) out->slots[i + 1] = n->slots[i];
        grew = true;
        hamtNodePostConstructBarrier(out);
        return out;
    }
    unsigned at = idxFor(n->bitmap, p);
    const HamtNode::Slot & s = n->slots[at];
    if (s.isBranch()) {                             // recurse into subnode
        HamtNode * out = copyOf(n);
        out->slots[at].child = insertAt(s.child, key, pos, val, shift + kBits, grew);
        hamtNodePostConstructBarrier(out);
        return out;
    }
    if (s.key == key) {                             // overwrite existing leaf
        HamtNode * out = copyOf(n);
        out->slots[at].pos = pos;
        out->slots[at].val = val;
        grew = false;
        hamtNodePostConstructBarrier(out);
        return out;
    }
    HamtNode * out = copyOf(n);                     // split two distinct leaves
    HamtNode::Slot leaf2{key, pos, val, nullptr};
    out->slots[at].child = splitPair(s, leaf2, shift + kBits);
    out->slots[at].key = 0;
    out->slots[at].pos = 0;
    out->slots[at].val = Value{};                   // now a branch (val unused)
    grew = true;
    hamtNodePostConstructBarrier(out);
    return out;
}

inline HamtNode * emptyNode() noexcept
{
    HamtNode * n = Alloc::allocHamtNode(0);
    hamtNodePostConstructBarrier(n);
    return n;
}

/// Persistent insert: returns a NEW root; `root` unchanged.  `grew` = a new key.
inline HamtNode * insert(const HamtNode * root, uint32_t key, uint32_t pos,
                         const Value & val, bool & grew) noexcept
{
    grew = false;
    if (!root) root = emptyNode();
    return insertAt(root, key, pos, val, 0, grew);
}

template <class F>
void forEach(const HamtNode * n, F && f)
{
    if (!n) return;
    for (uint16_t i = 0; i < n->nSlots; ++i) {
        if (n->slots[i].isBranch()) forEach(n->slots[i].child, std::forward<F>(f));
        else f(n->slots[i]);
    }
}

} // namespace nix::v3::ahamt
