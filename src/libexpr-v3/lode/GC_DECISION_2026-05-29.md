# GC binding decision — Step 10 of post-Phase-3.8 plan

**Date:** 2026-05-29
**Status:** BINDING DECISION — supersedes the flat-MS recommendation in `GC_DESIGN_POST_CHENEY_2026-05-28 §4`
**Task:** #846

This document is the FIRST IRREVERSIBLE COMMITMENT in the post-Phase-3.8 plan.  Everything before this is measurement.  Everything after must conform.

---

## 1. The decision

**PIVOT-IMMIX.**

v3's Stage 6 production GC will be a mark-region (Immix-pattern) collector, not flat mark-sweep.

The Phase 0-3.8 flat-MS code stays in tree as scaffolding (mark phase, root walker, conservative C-stack scan, whole-block-free) but the per-exact-size free-list allocator (alloc.hh:1125) retires when the Immix allocator lands.

---

## 2. Why PIVOT-IMMIX (the four-falsifier consensus)

Per [`STAGE_6_FALSIFIERS_RESULT_2026-05-29.md`](STAGE_6_FALSIFIERS_RESULT_2026-05-29.md):

| Falsifier | Verdict | Implication |
|---|---|---|
| **F1 (Immix line-occupancy)** | PASS @ 46.5-50.5% (≥30% threshold) | Immix line-reclaim viable |
| **F2 (flat MS sweep cost)** | FAIL @ 40-54% wall (≤5% threshold) | flat MS sweep dominates wall |
| **F3 (flat MS post-GC peak)** | FAIL: -22 MB regression on hello, ~0 on HNE | flat MS doesn't deliver peak |
| **F4 (BiBOP-lite)** | JUDGMENT @ 5-20% projection | Phase 13+ add-on candidate |

Per the §5.5 flowchart, F1≥30% terminates at "Implement Immix path."  F2/F3 reinforce by showing the alternative (flat MS) doesn't ship.

---

## 3. The replacement steps (rewriting #847–#852 per Step 10 contract)

Tasks #847-#852 were drafted assuming flat-MS CONTINUE-MS rescue.  Under PIVOT-IMMIX they're replaced as follows.  Task #846 stays committed to this rewrite (the original tasks remain as historical record; new tasks supersede their content).

### New Step 11′ — Immix per-block line-mark bitmap

Add to each arena block a SECOND bitmap (alongside the existing cell-start bitmap):
- **Line-mark bitmap**: 1 bit per 128 B line.  16 MB block / 128 B = 131,072 lines = 16 KB per block.
- Updated by mark phase: each marked cell sets the bits for all lines it touches.
- Used by allocator: bump-allocate within reclaimed (zero-bit) lines.

Acceptance: `live_trace.cc::reportLinesAtSize(128)` still reports ≥30% fully dead under Immix configuration (i.e., the allocator's line-claim algorithm doesn't pessimize line-density).

### New Step 12′ — Immix allocator path

Replace `Arena::alloc`'s free-list path with line-region allocation:
1. For each block, maintain a list of "free spans" (consecutive zero-mark lines).
2. Allocator pops a free span sufficient for `bytes`; bump-allocates within it.
3. Falls through to fresh-block alloc when no span fits.
4. After Mark+Sweep, free spans are recomputed from the line-mark bitmap.

Acceptance: `NIX_V3_FREE_LIST_STATS` hit rate ≥70% on HNE.

### New Step 13′ — Adaptive threshold + recycle policy

Adopt the adaptive threshold from current code (next-threshold = `max(initial, 2 × postGCArenaBytes)`).  Add Immix-specific recycle policy: blocks with ≥X% lines dead are RECYCLED (allocator may reuse), blocks with <X% are SKIPPED (treated as full).  Tune X.

Acceptance: arena's peak resident bytes drop ≥30% vs gate-OFF on HNE.

### New Step 14′ — Phase 4 SHIP gate re-measurement under Immix

Use `bench/measure-peak-noise-floor.sh` (Step 1) to re-baseline.  Pre-committed `§9` thresholds carry over:
- hello.drvPath peak_rss reduction ≥200 MB → SHIP
- HNE peak_rss reduction ≥500 MB → SHIP
- hello wall regression ≤10%
- HNE wall regression ≤15%
- All byte-identical to TW oracle.

If any FAIL: write `IMMIX_FALSIFIED_2026-XX-XX.md`, revert behind opt-in, escalate to Step 10 re-litigation (likely SCOPE-PIVOT-CACHE).

### New Step 15′ — Phase 5 stress validation

Same as original Step 15 (NIX_V3_MAJOR_GC + STRESS=1000 across --quick + --core + brute audit), but the underlying GC is Immix.

### New Step 16′ — nixpkgs flake matrix validation

10-package byte-equality check.  Same content as original Step 16.

### New Step 17′ — Default-ON flip

If Steps 14′-16′ all clear: flip `NIX_V3_MAJOR_GC=1` to default-ON.  Same content as original Step 17.

---

## 4. What does NOT change

* **Steps 1-8 outputs stand.**  Noise-floor baselines, L(t) data, free-list stats, BiBOP projection, Immix line-occupancy probe — all valid for Immix-path measurement comparison.
* **Step 18 (string dedup spike)** — orthogonal, independent of GC family.  Still runs.
* **Step 19 (M5 measurement)** — orthogonal.  Will measure post-Immix.
* **Step 20 (ROADMAP + CLAUDE.md + MEMORY.md refresh)** — content updates to reflect Immix verdict; structure unchanged.

---

## 5. Honest limits + risk register

### 5.1 Effort risk

Literature estimate (Whippet, JikesRVM Immix): ~4 KLoC, 4-6 weeks.  Per `GC_DESIGN §10` honest-limits, v3 may shift this ±30% (so 5-8 weeks).  Plan accordingly.

### 5.2 Allocator-path slowdown

Immix's line-region allocator is a few % slower per allocation than bump-pointer (per literature).  For v3's ~3M allocs/eval, that's a measurable wall hit.  Step 14′ wall-regression threshold (≤10% hello / ≤15% HNE) accommodates this.

### 5.3 Concurrent allocator-mark interaction

If Immix marks during eval (not just at safepoints), care is needed for write barriers.  v3 currently has Phase D write barriers from the nursery work — those may need extension.  Risk: 1 week extra if needed.

### 5.4 Block-level fragmentation under Immix

Per `GC_DESIGN §3.2`: Immix's opportunistic defragmentation (medium-difficulty feature) addresses this.  v3's Stage 6 Immix may ship WITHOUT defragmentation initially, with a Phase 6.5 "add defragmentation if fragmentation > X%" gate.

### 5.5 Falsifier #4 (BiBOP) deferred

BiBOP-lite is documented as Phase 13+ add-on.  Step 10 doesn't require BiBOP to ship.  If post-Immix measurement still shows ≥110 MB recoverable via Bindings-page-segregation, BiBOP becomes a follow-on.

### 5.6 M5 watchdog

Per `GC_DESIGN §7`: even perfect arena GC leaves the 990 MB elsewhere bucket on HNE.  M5 likely won't clear 4 GB on Immix alone.  Cache eviction (Phase 4b) remains the orthogonal lever — Step 19 quantifies the remaining gap.

---

## 6. Carcass cleanup (post-Step-10, pre-Step-11′)

Per `[[measure-twice-cut-once]]` §3.7 (no carcass behind gate):

Once Step 11′ lands, the following retires:
* `Arena::freeListBins_` (alloc.hh:1125) — replaced by per-block line-bitmap
* `Arena::freeListAdd` / `freeListTryPop` — replaced by Immix span allocator
* `NIX_V3_FREE_LIST_STATS` env gate (Step 6 infrastructure) — replaced by Immix-specific stats
* Per-exact-size sweep classification in `mark_sweep.cc:686` — replaced by line-bitmap update

Estimated retire: 150-200 LoC.

Keep:
* Mark phase + RootVisitor
* `BitmapMarker` (cell-start + new line-mark) 
* Bridge root registry
* Conservative C-stack scan
* Whole-block-free (Phase 3.8) — now fires more often under Immix when blocks are 100% line-dead
* Adaptive threshold

---

## 7. Decision-blocking contract (Rule 0)

This commit kills the hypothesis "flat MS is the right GC for v3."

A future commit attempting to re-introduce flat MS as default must:
1. Cite NEW empirical data that contradicts F1/F2/F3 (the falsifiers).
2. Provide a pre-committed SHIP threshold that Immix is failing to clear AND flat MS would clear.
3. Have a SAME-HOST-BISECT verification (per `[[same-host-bisect]]`).

No path is permanently closed; the burden of proof is on re-opening.

---

## 8. CLAUDE.md strategic-table updates (handled in Step 20)

Once Step 17′ ships:
- Update CLAUDE.md "READ FIRST for GC" pointer from `GC_DESIGN_POST_CHENEY` to `GC_DECISION_2026-05-29` + Immix design doc (to be written in Step 11′).
- Mark `GC_DESIGN_POST_CHENEY §4` (the flat MS section) as SUPERSEDED.
- Memory: add `[[gc-decision-2026-05-29]]` index entry.

---

## 9. Cross-references

- [`STAGE_6_FALSIFIERS_RESULT_2026-05-29.md`](STAGE_6_FALSIFIERS_RESULT_2026-05-29.md) — synthesis input
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §6.7 (Plan B Immix outline) — the path now taken
- [`IMMIX_LINE_OCCUPANCY_2026-05-29.md`](IMMIX_LINE_OCCUPANCY_2026-05-29.md) — the falsifier that triggered the pivot
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — flat MS SHIP-gate falsification
- [`BIBOP_LITE_PROJECTION_2026-05-29.md`](BIBOP_LITE_PROJECTION_2026-05-29.md) — F4 (deferred to Phase 13+)
- Memory: [[measure-twice-cut-once]] §3 (pre-commit), §3.7 (no carcass), §3.8 (3-pivot rule — this is pivot #1 of the Immix-vs-flat-MS lineage; not yet near falsification family)
- Memory: [[falsification-rule]] — Rule 0 contract above

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
