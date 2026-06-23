# Change 2 (#148) — persistent HAMT attrsets: design — 2026-06-23

The program's #1 remaining lever (Changes 1 + 3 measured modest/done).  Attacks the
TWO biggest nixpkgs buckets at once: `countDistinct` 16–17% CPU + Bindings 142 MB
arena RSS (firefox; mergeBindings was 584 MB / 82.9% of arena Bindings on HNE).
PERF_STRATEGY Stage 11.  Highest-value, highest-effort (~4–6 wk).

## Why now / what's wrong today

`//` (OP_ATTRS_UPDATE) is the nixpkgs hotspot (overrides, overlays, recursive
merges).  Today's representations:
- **Sorted** flat array of `Entry{SymbolId, PosIdx32, Value}` (24 B/entry), binary
  search by SymbolId.  `a // b` copies → O(n) alloc per merge.
- **Chain** (`ChainBindings`, gated `NIX_V3_CHAIN_BINDINGS`, never default): stores
  `(parent, overlay-delta)` to AVOID the copy — but `countDistinct` then WALKS the
  parent chain to dedup keys → O(depth × keys).  Deep nixpkgs override chains make
  the walk expensive; it missed its ≥200 MB SHIP bar.

So today we pay EITHER O(n) copy (Sorted) OR O(depth) walk (Chain).  A persistent
HAMT pays neither: O(changed) merge with structural sharing + O(log₃₂ n) lookup.

## The enabling fact (byte-id): iteration order is materialized lazily

`attrNames` RE-SORTS lexicographically at query time (primops.cc:792); Bindings'
INTERNAL order is by SymbolId, not string.  So **the attrset structure need not
iterate in string order** — every order-sensitive consumer (attrNames, attrValues,
toJSON, drv hashing) already sorts by name at the boundary.  A HAMT keyed by
SymbolId therefore only needs: (a) lookup by SymbolId, (b) enumerate all pairs;
string order stays a query-time sort.  This removes the hardest HAMT constraint.

## Structure — CHAMP keyed by SymbolId

**CHAMP** (Compressed Hash-Array Mapped Prefix-tree, Steindorfer & Vinju 2015) keyed
by `SymbolId` (the hash is the SymbolId itself, or a mix of it):
- **Canonical**: the same key-SET yields the SAME trie shape regardless of insertion
  order ⇒ two equal attrsets enumerate in the SAME order.  This is REQUIRED for the
  lockstep consumers (valueEqual, comparison) that today rely on SymbolId-sorted
  order being identical for equal key-sets — CHAMP preserves that invariant (audit
  item #1).
- **Structural sharing**: `a // b` shares all of `a`'s subtrees that `b` doesn't
  touch; only the path to changed keys is copied (O(changed), not O(n)).
- **Compact**: CHAMP separates data-map / node-map bitmaps so inline entries pack
  tightly; small attrsets (the common case) stay near a flat array's footprint.

## Operations

| op | today | HAMT |
|---|---|---|
| lookup (attrSelect) | binary search O(log n) on Sorted, or chain-walk on Chain | trie walk O(log₃₂ n) + the existing AttrSelectIC |
| `//` merge | O(n) copy (Sorted) / O(depth) walk (Chain) | **O(changed)** + structural sharing |
| countDistinct | walks chain | **gone** — node carries a maintained size |
| enumerate | linear | trie traversal (canonical order) |
| attrNames/attrValues | sort SymbolId-array by string | enumerate → sort by string (unchanged) |

## RSS + CPU win

- **RSS**: deep override layers SHARE their common base instead of copying/chaining.
  On nixpkgs one base attrset is overlaid dozens of times; sharing the base is the
  big win (Bindings 142 MB firefox; the 584 MB/82.9% mergeBindings on HNE).
- **CPU**: `countDistinct` (16–17%) DISAPPEARS (size is maintained, not walked);
  `//` is O(changed) not O(n).

## Risks + audit items (measure-first, byte-id-critical)

1. **Lockstep order** — audit every consumer that walks two Bindings in tandem
   (valueEqual, `==`, structural compare) to confirm CHAMP's canonical-for-equal-
   keys order suffices (it should; same keys → same shape).  HIGHEST risk.
2. **attrNames/toJSON/drv-hash byte-id** — must stay sorted-by-name; the query-time
   sort is unchanged, so this holds IF enumeration yields the full correct key set.
   Validate via full nixpkgs soak (diverge=0).
3. **Small-attrset footprint/lookup** — CHAMP node overhead may exceed a flat array
   for tiny attrsets (most attrsets are small).  Keep a small-N inline representation
   (CHAMP's inline data-map does this) + measure attrSelect hot-path vs binary search
   (the AttrSelectIC absorbs most repeat lookups, so trie-walk cost is on cold
   lookups only).
4. **Moving-GC integration** — CHAMP nodes are arena cells with child pointers →
   need Phase-D write barriers + scavenger walkers + the auditor (the PhD-6 class).
   Persistent sharing ⇒ multiply-referenced nodes (the moving GC must not double-
   copy; env-sharing already proved this pattern).  Per CLAUDE.md constraint #0.
5. **PosIdx** — `Entry.pos` (unsafeGetAttrPos) must survive in the HAMT node payload.
6. **Disk cache** — Bindings are NOT serialized (CUs hold bytecode, not Values), so
   NO on-disk format change (per the ChainBindings note).  No schema bump.

## Migration plan (the #149–#151 tasks)

- **#149**: implement the CHAMP behind `NIX_V3_HAMT_BINDINGS` (retire ChainBindings);
  migrate every consumer (lookup, mergeBindings, print, ~30 primops using
  countDistinct/forEach) to the HAMT API.  Keep Sorted as the fallback until soak.
- **#150**: `//` structural-sharing merge; delete the countDistinct chain-walk.
- **#151**: validate byte-id INCLUDING sorted-iteration (full nixpkgs soak
  diverge=0) + full --brute 22/22; measure countDistinct CPU down + Bindings RSS
  down + attrSelect hot path not regressed (darwin-4).

## Pre-committed SHIP gates

- Correctness: byte-id (full nixpkgs soak, diverge=0) + --brute 22/22 under moving-
  GC stress.
- Perf (darwin-4): `countDistinct` category drops AND Bindings arena RSS drops, with
  attrSelect hot-path within noise.  KILL if Bindings RSS doesn't drop ≥ the
  ChainBindings bar (≥200 MB on a heavy workload) OR attrSelect regresses > noise.

## Decision

This is the single highest-value lever left (both axes, biggest buckets).  It is
also the biggest build (CHAMP + ~30 consumer migrations + GC integration + the
lockstep audit).  Sequence it as the next major effort; #149 begins the build
behind the gate.  Note: ChainBindings already proved the "avoid the copy" half;
the HAMT adds the "avoid the walk + canonical sharing" half that ChainBindings
lacked.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
