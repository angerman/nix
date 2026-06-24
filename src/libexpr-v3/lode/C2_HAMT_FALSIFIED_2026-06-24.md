# C2 (Change 2 — HAMT Bindings): FALSIFIED as an RSS/CPU lever — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

## Verdict (Rule 0)

**Hypothesis killed:** "Replacing v3's flat/Chain `Bindings` with a persistent
HAMT is an RSS+CPU win (O(changed) `//` sharing + O(1) `countDistinct`, beating
the Chain's parent-walk)."

**Result:** the HAMT — fully implemented, byte-identical on 20 diverse packages +
21/22 of the full `--brute` battery — measures a **structural 4.7–9.2× RSS
regression and a 1.3× CPU regression** on darwin-4. It loses to the *existing*
`ChainBindings` on both axes. The lever does not ship.

This falsifies Change 2 of the six proposed fundamental architectural changes
(`ARCH_BEAT_TW_PROGRAM_2026-06-23.md`).

## The measurement (darwin-4, cache-off, median-of-5)

| workload | default-v3 | HAMT (materialize-select) | HAMT (HAMT-native select) |
|---|---|---|---|
| firefox.drvPath CPU | 2.69 s | 3.76 s (1.40×) | 3.54 s (1.32×) |
| firefox.drvPath RSS | 673 MB | 6235 MB (**9.28×**) | 6178 MB (**9.18×**) |
| git.drvPath CPU | 1.44 s | 1.98 s (1.38×) | 1.88 s (1.31×) |
| git.drvPath RSS | 405 MB | 1942 MB (**4.80×**) | 1916 MB (**4.67×**) |

Byte-identical throughout (hello/git/firefox.drvPath; 20-pkg gate-on-vs-default
sweep 20/20; gate-on `--brute` 21/22 — lone fail a benign smoke optimization-count
assertion). Correctness was never the problem.

## Root cause (controlled experiment, not speculation)

The first suspicion was the `materialize()` shortcut: every consumer (`OP_ATTRS_
SELECT`, `valueEqual`, `attrValues`, …) on a `Kind::Hamt` allocated + memoized a
full **Sorted copy** of the attrset, and the per-epoch memo retains every distinct
attrset accessed. `OP_ATTRS_SELECT` is the highest-frequency consumer, so it was
rewritten **HAMT-native** (direct `hamtLookupSlot`, no copy, no writeback into the
shared node — the safe chain "parent-hit" semantics).

**Removing the dominant consumer-side allocation left RSS essentially unchanged:
9.28× → 9.18× (firefox), 4.80× → 4.67× (git).** A controlled experiment: remove the
suspected cause, the effect persists ⇒ the cause is elsewhere.

The only thing the HAMT path *uniquely* adds versus the default is **HamtNode
allocation**. HamtNodes are TENURED (never moved, like the shared `Env`), and the
v3 arena **never releases pages to the OS** (proven this session: #134 import-cache,
#136 page-release, mid-eval-reuse — all KILLed on the same arena-pin). A persistent
HAMT `//` *path-copies* O(log₃₂ n) interior nodes **per inserted key** — and nixpkgs
performs an enormous number of `//` merges. Every node ever allocated stays
resident. The 6 GB is the monotonic accumulation of path-copy interior nodes.

## Why it can't win: Chain already captures the advantages, more cheaply

The HAMT was proposed to fix two Chain weaknesses. Neither is real:

1. **`//` sharing.** `ChainBindings` already shares: `a // b` allocates ONE overlay
   `Bindings` (b's entries) + a parent pointer to `a` — **O(|b|), zero interior-node
   copies**. The HAMT path-copies **O(|b|·log n)** interior nodes, each ~40 B
   (key+pos+24 B Value+child). The HAMT allocates *strictly more* per merge.

2. **`countDistinct` chain-walk.** Already memoized (the `_pad8` slot, see
   `PROFILE_AT_SCALE_2026-06-21` L1) → O(1) after first. The HAMT's O(1) size is no
   improvement.

The HAMT's only theoretical edge — O(log n) lookup vs O(depth) chain-walk — does not
materialize: CPU *regressed* 1.3× (node-pointer chasing + tenured-alloc overhead +
GC-walk of the node forest dominate any lookup-depth saving; real-world chains are
shallow). 

## What was built (correct; reusable)

- `champ.hh` — standalone shared_ptr persistent HAMT (algorithm validation).
- `arena_hamt.hh` — arena `HamtNode*` persistent insert/merge/lookup/forEach.
- `alloc.hh` — `CellType::HamtNode`, `Bindings::Kind::Hamt` (root in unused `aux`
  Value slot; leaf `Slot{key,pos,val}` layout-aliases `Entry{name,pos,value}`,
  static_assert'd), inline `hamtLookupSlot`.
- Moving-GC integration (`gc.cc`/`mark_sweep.cc`/`barrier.hh`): `GK_HAMT`,
  `walkHamtNode`, `DirtyKind::HamtNode`, `hamtNodePostConstructBarrier`, auditor
  `visitHamtNode` — **a clean second instance of the env-sharing (`GK_ENV`) shared-
  tenured-cell pattern.** This GC machinery is the reusable artifact; it validated
  the pattern a second time and is the template for any future shared-tenured cell.
- write-path `//` merge + ~12 consumer migrations; gate `NIX_V3_HAMT_BINDINGS`
  (default OFF).

## Disposition

Per Rule 0 the falsified path's gate should not persist as "both coexist." The code
is correct + validated and the GC integration is a reusable reference; the data
structure itself is a measured regression with no path to a win (Chain dominates by
construction). Recommend RETIRE: either delete the HAMT backend + gate, or keep it
as an explicitly-marked falsified reference. (User decision — large correct
subsystem.)

## Lesson

A persistent/path-copying data structure is the *wrong* tool under an arena that
never frees: path-copy garbage accumulates monotonically with no reclamation. The
incumbent `ChainBindings` (single-overlay + parent pointer) was already the
memory-optimal `//` representation for this allocator. "More principled data
structure" ≠ "less memory" when the allocator can't reclaim the principled
structure's churn. Measure the *realized* footprint against the incumbent before
adopting — exactly the FP-2/FP-3 "allocated-churn ≠ live" trap, one level up.
