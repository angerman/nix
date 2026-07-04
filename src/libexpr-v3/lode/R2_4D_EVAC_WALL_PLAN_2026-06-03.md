# R2.4d — evacuation wall-reduction plan (the SHIP-gate blocker)

**Date:** 2026-06-03
**Status:** PLAN — the evacuation mechanism is proven (byte-correct, frees
real RSS); the ONLY thing between HEAD and the `M5 < 4096` SHIP gate is
per-cycle GC **wall**. This doc names the cost, ranks the levers (each with
a code anchor), and pre-commits the thresholds. Also corrects an
over-sold claim about the `mergeBindings` lever (§3).
**Author:** session synthesis (code-grounded at `33654b071`)

Companion docs:
- [`R2_4B_BARTLETT_YIELD_2026-06-03.md`](R2_4B_BARTLETT_YIELD_2026-06-03.md) — the proven mover + the −12% HNE yield + the wall problem this doc solves
- [`R2_EVACUATION_DESIGN_2026-06-02.md`](R2_EVACUATION_DESIGN_2026-06-02.md) — the mover design + opportunity histogram (§6 M5 257-block / 4311 MB)
- [`POST_F4_GC_GOAL_2026-06-02.md`](POST_F4_GC_GOAL_2026-06-02.md) — the parent goal + SHIP gate
- [`POST_F4_MEMORY_PROFILE_2026-06-02.md`](POST_F4_MEMORY_PROFILE_2026-06-02.md) — R0 numbers (M5 4686 ± 102; E ≈ 590 MB)
- [`IMMIX_NOFL_DESIGN_2026-06-02.md`](IMMIX_NOFL_DESIGN_2026-06-02.md) — why evacuation is load-bearing (macOS `munmap`-only)

---

## 0. The one-line state

Evacuation **works**: HNE peak 2434 → 2151 (−12%, `33654b071`), byte-equal,
~0% conservative over-pin. **Yield is real; the gate is now wall, not
correctness.** Per-cycle cost on HNE cycle2 ≈ **move 4.7s + verify 3.2s +
heavy mark**, and it *grows with block count* — so M5 (424 blocks) times
out. Close the wall and the SHIP gate (`M5 < 4096`) falls.

---

## 1. Where the time goes (code-grounded)

The driver already instruments three costs (`mark_sweep.cc:1319-1326`):
`move` / `verify` / `munmap`, plus the upstream mark phase.

| Phase | Code | Cost driver |
|---|---|---|
| **mark** (`drainConservative`) | `mark_sweep.cc:409-443` | conservative **8-byte-word scan of every cell's bytes** to chase interior `Tag::Slot`/`cell` pointers; calls `findContainingCellStart` + `findNextCellStartOrBlockEnd` per root |
| **move** (`EvacVisitor` + drain) | `mark_sweep.cc:934-1080`, `:1277-1280` | per moved cell: copy + interior-owner lookup via `findContainingCellStart` |
| **verify** (re-mark) | `mark_sweep.cc:1295-1306` | a **full second `walkAllV3Roots`** to confirm candidate blocks are empty before `munmap` |
| **munmap** | `mark_sweep.cc:1308-1313` | cheap; the R1 page-release path |

The cost amplifier sitting under both **mark** and **move**:
`findContainingCellStart` (`alloc.hh:1581`) and
`findNextCellStartOrBlockEnd` (`alloc.hh:1630`) both **linear-scan
`active_.blocks`** to find the containing block. On M5's 424 blocks this is
the "grows as dest blocks are added" cost the R2.4b doc flagged — it is
O(blocks) per lookup and is invoked per-conservative-root (with a per-word
inner loop) and throughout the move's interior rewrite.

---

## 2. The levers (ranked by ROI)

### Lever 1 — `findContainingCellStart`/`findNextCellStartOrBlockEnd` O(blocks) → O(log blocks). **DO FIRST.**
- **What:** maintain a sorted `(blockStart, blockIndex)` side-vector and
  `std::upper_bound` it. (mmap addresses are non-monotonic, so the existing
  `active_.blocks` order can't be binary-searched directly — a parallel
  sorted index is needed; rebuild/patch it on block add/free.)
- **Why first:** it discounts *every* phase that does a cell-start lookup —
  mark's `drainConservative`, the move's interior-owner rewrite, AND the
  verify. Its benefit grows exactly where M5 hurts (block count). Lowest
  risk (pure lookup acceleration, no semantic change), highest reach.
- **Effort:** ~½ day. **Falsifier:** HNE cycle `move`+`mark` ms must drop;
  if flat, the lookup wasn't the cost — re-profile before Lever 3.

### Lever 2 — shrink / drop the verify re-mark (≈⅓ of per-cycle cost)
`mark_sweep.cc:1295-1306` re-marks the whole heap just to prove candidate
emptiness. Bartlett is correct by construction (direct C-pins excluded
pre-evac; all moved-cell refs rewritten). Two graded options:
- **Conservative (recommended first):** keep verify but **only mark into
  candidate ranges** — early-out the instant a candidate gets a mark,
  instead of a whole-heap walk. Most of the 3.2s marks cells you don't
  care about.
- **Aggressive:** drop verify entirely; rely on `run-brute-audit.sh` +
  `NIX_V3_GC_STRESS=N` as the safety net. Higher risk; keep gated.
- **Effort:** ~½ day. **Pre-commit:** take conservative first — it captures
  most of the win without surrendering the safe-leak-not-dangle property.

### Lever 3 — typed interior-owner walk instead of the conservative byte-scan
The "heavy mark" is `drainConservative` (`:409`) byte-scanning every cell.
R2.1′ per-cell type metadata (`8cfd32066`) now lets a cell of known type be
walked at its **exact pointer fields** — removing both the false-positive
marks and the per-word cost. Bigger change (~1-2 days); the structural fix
for mark cost. **Sequence after** Levers 1-2 re-measure (they may already
clear the budget).

### Lever 4 — GC cadence (this is what moves *peak*, not just wall)
**Critical subtlety from the yield data:** single-cycle workloads (hello)
show **0 peak reduction** because the free lands *after* the high-water.
Peak drops only on multi-cycle workloads where a cycle fires *near peak* and
bounds the high-water. So `NIX_V3_MAJOR_GC_THRESHOLD_MB` is not just a wall
knob — on M5 it determines whether evac yields **any** peak reduction. M5's
late cycle offers 4311 MB evacuable (257 sparse blocks) vs the 590 MB
needed, so a single well-timed near-peak cycle clears the watchdog with
huge headroom. Tune the trigger to fire while the arena is near peak.

**Sequence:** Lever 1 (½d, helps everything) → Lever 2 conservative (½d) →
**re-measure HNE wall** → Lever 3 only if mark still dominates → Lever 4
cadence against the M5<4096 gate. Levers 1-2 alone may bring HNE from
timeout into budget.

---

## 3. CORRECTION — `mergeBindings` is NOT the lever it was billed as

An earlier framing called a "two-pass `mergeBindings`" the highest-leverage
single change. **That is wrong — the two-pass already landed (#747).**
Code at `vm.cc:1230-1262` already does Pass-1 exact-count + Pass-2
precise-size alloc, which removed the ~387 MB duplicate-key slack on
hello.drvPath; the empty-operand short-circuits (#748, `vm.cc:1163`) are in
too. **The cheap win is spent.**

What remains is the **structural** cost: `a // b` allocates `kExact`
entries and **copies all of `a`** every time, so an overlay chain is O(n²).
The fix (Chain / persistent-overlay) was **falsified four times**
(#826 Phase C v1-v4, documented at `vm.cc:1166-1228` — all hit
`attribute 'buildPythonApplication' missing`). Per measure-twice, three
failed pivots on one premise = falsification. Revival needs the documented
prereqs (Nix-level `{} // overlay` collapse repro + 208-site `entries[]`
audit) — a multi-session task with a real architectural blocker, **not** a
quick win.

**The reorder that matters:** the Nofl-Immix GC **subsumes** the
`mergeBindings` churn. Those merged intermediates are exactly the dead
garbage in the freeable numbers (HNE 1.43 GB, M5 6.22 GB freeable @ end).
Once evac returns their pages, the churn stops counting toward peak. So
allocation-reduction now matters mainly for **wall** and **live working
set**, not the watchdog target — which means **finishing the GC is the
higher-leverage path**, and Chain revival drops down the list.

---

## 4. Pre-committed SHIP gate + thresholds (measure-twice)

**PRIMARY (inherited from `POST_F4_GC_GOAL`):** M5 peak RSS < 4096 MB under
`NIX_V3_EVAC=1` + tuned cadence, ≥3 runs, pooled-σ envelope.

**R2.4d-specific wall gates (the new bar this doc adds):**
- **HNE completes under `NIX_V3_EVAC=1`** within ≤ 1.15× its gate-OFF wall
  (currently it times out; the bar is "no longer times out, ≤15%
  regression"), ≥10 hyperfine runs.
- **Per-cycle cost:** HNE cycle2 `move`+`verify`+`mark` ms drops ≥ 50% vs
  the `33654b071` baseline after Levers 1-2.
- **Yield preserved:** HNE peak stays ≤ 2151 (the proven −12%) — wall
  reduction must NOT regress the yield.
- **Correctness unchanged:** hello + HNE + M5 drvPath byte-equal;
  `--quick` 6/6 + `--core` 19/19; `run-brute-audit.sh` clean under
  `NIX_V3_GC_STRESS=1000`.

**Falsification exits (Rule 0):**
- Lever 1 lands and the `move`+`mark` ms are flat → the block-lookup wasn't
  the cost; re-profile and route to Lever 3 with the new data (do not stack
  speculative levers).
- Levers 1-3 land and HNE still can't complete in budget → the cost is the
  precise mark walk itself, not the conservative tail; the lever becomes
  mark-incrementality / generational sticky-bit (R3), not more evac tuning.
- Cadence tuned and M5 still ≥ 4096 with yield confirmed → re-measure E and
  freeable per Gate B/D; the residual is cache (HNE `elsewhere` route) or
  genuinely-live, NOT a different GC family on the same profile.

---

## 5. First concrete step

**Lever 1** — the sorted block index. Self-contained, testable in isolation
(the existing `move`/`verify`/`mark` timing split shows the delta directly),
gated behind `NIX_V3_EVAC` / `NIX_V3_MAJOR_GC`. Measure HNE cycle2
before/after; the per-cycle-cost gate in §4 is the accept/reject.

---

## 6. Cross-references

- [[r2-4b-bartlett-yield-2026-06-03]] — the proven mover + the wall problem
- [[r2-evacuation-design-2026-06-02]] — mover design + M5 opportunity histogram
- [[post-f4-gc-goal-2026-06-02]] — parent goal + SHIP gate
- [[immix-nofl-design-2026-06-02]] — macOS munmap-only → evacuation mandatory
- [[measure-twice-cut-once]] — the §4 pre-commit + the three-pivots rule (§3)
- [[falsification-rule]] — the §4 exits
- [[memory-first-class]] — RSS-primary framing
- Code: `alloc.hh:1581`/`:1630` (Lever 1); `mark_sweep.cc:1295-1306` (Lever 2),
  `:409-443` (Lever 3), `:1319-1326` (timing); `vm.cc:1230-1262` (#747 two-pass,
  done), `:1166-1228` (Chain falsified ×4); `NIX_V3_MAJOR_GC_THRESHOLD_MB` (Lever 4)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
