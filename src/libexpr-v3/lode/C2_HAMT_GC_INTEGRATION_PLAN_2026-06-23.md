# Change 2 (#149 increments 3–4) — arena-HAMT moving-GC integration recipe — 2026-06-23

Increments 1+2 proved the HAMT ALGORITHM standalone (champ.hh: persistent insert/
merge/lookup, structural sharing 3/1057 nodes, sorted byte-id, 50-layer chain).
This is the concrete recipe for the multi-week, UAF-risky core: replacing the
shared_ptr nodes with v3-arena cells under the moving GC, then wiring it into
Bindings.  It de-risks the build by mirroring an ALREADY-PROVEN pattern.

## The key insight — this is the env-sharing pattern, not new GC theory

A persistent HAMT node is a **shared, multiply-referenced cell holding Values +
child pointers** — structurally identical to the shared `Env` that env-interning
(default-on, #145) already runs under the moving GC.  Persistence (a node reachable
from many HAMT versions, a DAG across versions) is handled by the EXISTING
Cheney/forwarding scavenger exactly as any shared cell: first visit copies + installs
a forwarding pointer; later referrers follow it (gc.cc side-table forwarding, line 6).
**So there is no new GC theory — just apply the proven GK_ENV machinery to a new cell
type.**  This is what shrinks the risk from "open-ended moving-GC research" to
"mirror walkEnv for HamtNode."

## File-level recipe (mirror the GK_ENV / walkBindings sites)

1. **Cell type** — `CellType::HamtNode` in `alloc.hh:1164` enum.
2. **Layout + alloc** — `allocHamtNode(nSlots)`: FAM cell `{ uint32 bitmap; uint16
   nSlots; Slot slots[]; }`, `Slot = { uint32 key; Value val; HamtNode* child; }`
   (leaf: child==null; branch: child!=null).  Mirror `allocEnv`/`allocClosure`
   (FAM + cell-type stamp).  champ.hh's algorithm ports 1:1 (the shared_ptr<Node>
   becomes HamtNode*; make_shared<Node>(*n) becomes allocHamtNode + copy).
3. **Phase-D barrier** — `hamtNodePostConstructBarrier(n)`: after a node is
   constructed with slots pointing at nursery cells (child nodes or Values), record
   in `dirtyContainers` (mirror `envPostConstructBarrier`, gc.cc:675).  Call it at
   every node-construction site in insert/splitPair/merge (the path-copy points).
4. **Scavenger walker** — `Scavenger::walkHamtNode(HamtNode*)` in gc.cc (mirror
   `walkBindings` gc.cc:789 + walkEnv): for each slot, scavenge `val` (if pointer)
   and forward+recurse `child`.  Add `GK_HAMT` to the graylist-kind enum (gc.cc:70)
   + the dispatch switch (gc.cc:831).  Shared nodes forward ONCE via the side-table
   (the proven env path).
5. **Mark walker** — `markHamtNode` in mark_sweep.cc (mirror Bindings mark): mark
   `val` + recurse `child`.  Covers the major mark + the mid-eval mark.
6. **Auditor** — include HamtNode in `precise_root.cc`'s root walk (mirror Env/
   Bindings) so `NIX_V3_*_AUDIT` + `--brute` catch any missed root.
7. **Serialize** — none.  Bindings/HAMT are NOT serialized (CUs hold bytecode), so
   NO disk-format change / schema bump (per the ChainBindings note).

## Validation gate (increment 3, before ANY Bindings wiring)

A dedicated smoke test (`testChampArenaGcStress`): build arena-HAMTs, do
inserts/merges that create CROSS-VERSION sharing, run under
`V3_DBG_GC_STRESS=1MB-nursery` + `V3_DBG_NURSERY_AUDIT=1`, force scavenges between
ops, then assert: (a) every lookup correct post-collection; (b) auditor reports 0
missed roots; (c) two live versions sharing a subtree agree after a collection
forwarded it (no torn sharing).  THEN full `--brute` 22/22 under moving-GC stress.
Only after this is green does increment 4 (Bindings wiring) start.

## Increment 4 — wire into Bindings behind NIX_V3_HAMT_BINDINGS

Migrate consumers one at a time, byte-id-validated per step:
1. Bindings gains an optional HAMT backend (gate-selected); Sorted/Chain stay as
   fallback until the soak passes.
2. Route `lookup`/attrSelect → HAMT lookup (+ keep the AttrSelectIC absorbing
   repeat lookups so the trie-walk cost is cold-only).
3. Route `mergeBindings` (`//`, OP_ATTRS_UPDATE) → `HAMT.update` — O(changed)
   structural sharing; **retire the countDistinct chain-walk** (#150).
4. Route `forEach` + the ~30 `countDistinct` call sites (primops.cc) to the HAMT
   API.  `attrNames`/`attrValues` enumerate → sort by name (UNCHANGED — byte-id).
5. PosIdx: carry `Entry.pos` in the HAMT slot payload (unsafeGetAttrPos).

## Risk register (+ mitigation)

- **Missed-root UAF (PhD class)** — mitigated by mirroring the proven env-sharing
  walkers + the mandatory `--brute` audit gate BEFORE wiring.
- **Forwarding consistency for shared nodes** — identical to env interning (proven).
- **attrSelect hot-path (trie vs binary search)** — measure on darwin-4; IC absorbs
  repeats; CHAMP inline-data-map (a later compression of champ.hh's plain nodes) if
  small-attrset lookup/footprint regresses.
- **Small-attrset footprint** — most attrsets are tiny; keep a small-N inline node
  (CHAMP data-map) so the HAMT isn't heavier than the flat array there.

## SHIP gate (#151)

Full nixpkgs byte-id soak (diverge=0) with NIX_V3_HAMT_BINDINGS=1 + `--brute` 22/22
under moving-GC stress; darwin-4: `countDistinct` CPU drops AND Bindings arena RSS
drops (≥ the ChainBindings 200 MB bar on a heavy workload), attrSelect within noise.
Then default-flip + retire the gate.

## Status

This recipe makes increments 3–4 EXECUTABLE + de-risked (proven-pattern mirror +
explicit file map + gate-before-wiring).  It is still multi-week implementation
(the arena/GC walkers + ~30 consumer migrations + the soak), gated at every step.
The algorithm (increments 1+2) is proven; this is the integration blueprint.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
