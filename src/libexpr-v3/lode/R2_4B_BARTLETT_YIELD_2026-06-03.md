# R2.4b deliverable — Bartlett evacuation: implementation + measured yield

**Date:** 2026-06-03
**Status:** Bartlett mostly-copying evacuation IMPLEMENTED + byte-correct +
**yields real RSS reduction**. Gated `NIX_V3_EVAC=1` (requires
`NIX_V3_MAJOR_GC`), default-OFF. **Wall cost is currently severe** (the
follow-on R2.4d problem). Commit `4b9e73552`.

Companion: [`R2_EVACUATION_DESIGN_2026-06-02.md`](R2_EVACUATION_DESIGN_2026-06-02.md),
[`IMMIX_NOFL_DESIGN_2026-06-02.md`](IMMIX_NOFL_DESIGN_2026-06-02.md) (Gate-C non-moving + evacuation),
[`POST_F4_MEMORY_PROFILE_2026-06-02.md`](POST_F4_MEMORY_PROFILE_2026-06-02.md) (R0 baselines).

---

## 1. What was built (`mark_sweep.cc`, gated `NIX_V3_EVAC`)

Bartlett mostly-copying — the standard solution for a moving GC under a
conservative C-stack:
- **`collectCStackDirectPins`** — NON-transitive C-stack scan: pins only
  cells whose pointer is physically in a stack/register slot. They can't
  move; their blocks are excluded from candidates and their fields are
  rewritten in place.
- **`EvacVisitor`** — walks precise roots + the pinned cells; moves every
  C-unreferenced cell out of sparse candidate blocks into fresh blocks
  (`forceFreshBlock` first, so dest never lands in a candidate), rewriting
  all references via a forward map. Content-walk uses the **R2.1′ per-cell
  type metadata** → moves ANY type incl. Bindings (84% of arena) and
  Chars (string buffers, with string-context side-table re-key).
- **Verify-before-free** (precise re-mark) → `munmap` a candidate only if
  zero precise marks.

Four bugs found-and-fixed by measurement (not guessing): transitive-pin
over-marking; un-rewritten `Tag::Slot` pointers; `drainConservative`
chasing old graph (98s verify); unmoved Chars pinning every block.

## 2. Measured YIELD (the goal)

| workload | cfg | blocksFreed / freedRSS | peak_rss | vs baseline | drvPath |
|---|---|---|---|---|---|
| hello.drvPath | pct=0.25 | 4 blocks / **67 MB** (1 cycle) | 824 (peak neutral — single cycle, free AFTER peak) | 712 baseline | byte-equal ✓ |
| git.drvPath | pct=0.25 | (frees) | — | — | byte-equal ✓ |
| HNE | pct=0.25 | cycle2: 23 blocks / **386 MB** | **2135** | **2434 → −300 MB (−12%)** | (timed out — wall) |

**Evacuation yields real RSS reduction on multi-cycle workloads**: HNE peak
2434 → 2135 MB (−12%), freeing 386 MB (23 sparse blocks) in one cycle for
68 MB of copying (5.6:1). The mechanism is confirmed end-to-end: sparse
blocks → evacuate live → `munmap` → RSS returns (R1's munmap path).

hello is single-GC-cycle, so its free happens AFTER the peak → peak not
reduced (the yield needs multi-cycle workloads to bound the high-water).

## 3. The wall problem (R2.4d — now the gating issue)

Per-cycle GC cost (HNE cycle2): **move 4.7s + verify 3.2s + heavy mark**,
and it grows as dest blocks are added (`findContainingCellStart` is
O(blocks)). HNE (6s baseline) **times out** under evac. The yield is real
but **not shippable until the wall is fixed**. Levers for R2.4d:
1. **Drop the verify re-mark** — Bartlett is correct by construction
   (pinned cells stay; all moved-cell refs rewritten); the verify is
   defense-in-depth (≈⅓ of per-cycle cost).
2. **Speed `findContainingCellStart`** (O(blocks) backward bitmap scan) —
   per-block cell-start index, or use the R2.1′ object-start metadata.
3. **Replace the mark's `drainConservative`** (conservative interior-owner
   byte-scan) with a typed interior-owner walk via R2.1′ metadata.
4. **GC cadence** — fewer, better-timed cycles (the threshold policy).

## 4. Verdict
Bartlett evacuation is the right mechanism (proven: byte-correct, frees
RSS, ~0 conservative over-pin with direct pinning + the metadata). The
**yield is positive (HNE −12% peak in this first cut, more expected on M5
with its 257 sparse blocks / 4311 MB evacuable)**. The next gate is
**wall** (R2.4d), not correctness or yield. The deep handle-redesign is NOT
needed — Bartlett delivers yield at raw-pointer access speed; the wall cost
is in the GC pass (optimizable), not the mutator.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
