# Stage 6 falsifier synthesis — pre-Step-10 decision input

**Date:** 2026-05-29
**Status:** SYNTHESIS — integrates Falsifiers F1-F4 + the Phase 4 PRELIM verdict into a single decision-quality table
**Task:** #845

Per the `GC_DESIGN_POST_CHENEY_2026-05-28 §5.5` decision flowchart, this doc consolidates the four pre-commit falsifiers + the noise-floor SHIP-gate measurement so Step 10 has a single anchored input.

---

## 1. Falsifier matrix

| Falsifier | Source | Threshold | Measured | Verdict |
|---|---|---|---|---|
| **F1: Immix line-occupancy** | [`IMMIX_LINE_OCCUPANCY_2026-05-29.md`](IMMIX_LINE_OCCUPANCY_2026-05-29.md) | ≥30% lines fully dead | hello: 46.5% (128 B); HNE: 50.5% (128 B) | ✅ **PASS** (Immix justified) |
| **F2: Flat MS sweep cost** | `live_trace.cc` reportSweepCost | ≤5% per cycle | hello: 40% per cycle; HNE: 54% per cycle | ❌ **FAIL** (flat MS too slow) |
| **F3: Post-GC peak RSS** | [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) | ≥200 MB hello / ≥500 MB HNE | hello: −22 MB regression; HNE: ~0 MB | ❌ **FAIL** (flat MS no peak win) |
| **F4: BiBOP-lite** | [`BIBOP_LITE_PROJECTION_2026-05-29.md`](BIBOP_LITE_PROJECTION_2026-05-29.md) | ≥15% RSS reduction | projection 5-20% (judgment-call) | 🟡 **JUDGMENT** (Phase 13+ add-on) |

**Net: 1 PASS for Immix, 2 FAIL for flat MS, 1 JUDGMENT for BiBOP.**

---

## 2. The flowchart walk

Per `GC_DESIGN §5.5`:

```
Run Falsifier #1 (Immix line-occupancy)
├── ≥30% lines dead → Implement Immix path (~4 KLoC, 4-6 wk)
└── <30%      → Continue: Run F2/F3/F4
```

F1 = ≥30% → **flowchart terminates at "Implement Immix path"** without reading F2/F3/F4.

But the F2/F3/F4 measurements REINFORCE this verdict:
- F2 says flat MS sweep wall cost is unacceptable (8-11× over threshold).
- F3 says flat MS peak doesn't reduce despite sweep work.
- F4 says BiBOP wouldn't rescue flat MS — only Immix's line-granularity reclamation can capture the 265-796 MB recoverable bytes that F1 identifies.

---

## 3. Bytes-recoverable projection under Immix

| Workload | Arena | Line-dead bytes (128 B granularity) | Potential peak reduction |
|---|---|---|---|
| hello.drvPath | 570.4 MB | 265.2 MB | ~265 MB if bump-realloc returns mostly-dead blocks |
| HNE | 1577.1 MB | 796.2 MB | ~796 MB if bump-realloc returns mostly-dead blocks |

§9 SHIP gates:
- hello ≥ 200 MB → **POTENTIAL +65 MB margin** under Immix
- HNE ≥ 500 MB → **POTENTIAL +296 MB margin** under Immix

Caveat: bump-realloc doesn't directly return libc memory — Immix reclaims LINES within blocks for new allocations.  Whole-block return requires a block to be 100% dead.  Per Step 8 measurement:
- hello: 13 of 34 blocks all-dead at 64 B → ~208 MB returnable to libc
- HNE: 0 blocks all-dead at 64 B but 20 mostly-dead at 64 B

So Immix's wins on HNE are PRIMARILY through line-reclaim (bytes get reused for new allocations, preventing arena growth) rather than block-return.  On hello, both mechanisms contribute.

**Whether peak RSS actually drops by 265 MB or 796 MB depends on Immix's allocation pattern** — Step 14 (re-measurement after Immix lands) is the binding verification.

---

## 4. What survives, what retires

Per `GC_DESIGN §4.6` — table of carry-over from current code:

| Component | Reuse in Immix |
|---|---|
| `walkAllV3Roots` + `RootVisitor` (Phase 1 mark) | ✅ identical, no changes |
| `tagIsPointer` + Stage 1 static_asserts | ✅ identical |
| `bridge_root_registry` (external-tag) | ✅ identical |
| `BitmapMarker` (per-cell mark bits in alloc.hh) | ✅ identical |
| `MarkVisitor` worklist drain | ✅ identical |
| Phase 3 conservative C-stack scan | ✅ identical |
| Singleton-closure registry (Phase 3.7) | ✅ identical |
| Whole-block-free (Phase 3.8) | ✅ identical |
| `freeListBins_` per-exact-size allocator | ❌ **RETIRE** — Immix uses per-block line-region allocator |
| `freeListAdd` / `freeListTryPop` | ❌ **RETIRE** — replaced by Immix line-bitmap |
| Cell-start bitmap (sweep granularity) | 🔄 **KEEP but augment** — needed for sweep; Immix adds per-LINE bitmap on top |

Approximate carcass to retire: ~150-200 LoC (free-list + size-class machinery).
Approximate Immix code to add: ~3-4 KLoC (per-block line-mark bitmap + Immix allocator + bump-realloc + recycle policy).

---

## 5. Validation criteria for Step 10's binding decision

Step 10 must commit to one of:
- A. CONTINUE-MS (flat MS + Steps 11/12/13 rescue attempts)
- B. CONTINUE-MS+BiBOP (flat MS + BiBOP-lite production impl)
- C. PIVOT-IMMIX (this synthesis's recommendation)
- D. SCOPE-PIVOT-CACHE (defer GC, attack ImportCache first)

**This synthesis recommends C (PIVOT-IMMIX).** Rationale:
- C is the only option where F1, F2, F3 ALL point in the same direction.
- A and B don't address F2 (sweep cost) or F3 (peak doesn't drop).
- D is independent of GC family; can run in parallel.

Step 10's binding role: write the choice + commit subsequent task descriptions (#847-#856) to reflect that choice.

---

## 6. Honest limits

- **F1 measurement is single-sample per workload.**  Per Step 2 noise floor, arena bytes σ=0; confident in the qualitative finding.  Per-percentile noise may be ±2% — well within the 11-23 point margin over threshold.
- **F2 sweep-cost projection uses mark wall time as a proxy** (the existing `reportSweepCost` extrapolates from `walkAllV3Roots`'s wall).  Real sweep cost may be ±50% of this.  Even with that uncertainty, the threshold (5% per cycle) is exceeded by 8-11× — robust falsification.
- **F4 is a projection, not a measurement.**  Empirical confirmation deferred to Step 13 IF Step 10 picks a path that includes BiBOP.  PIVOT-IMMIX defers BiBOP to Phase 13+ regardless.
- **M5 not measured for any falsifier.**  Step 19 will measure M5 post-Step-17.  If M5 behaves materially differently from HNE on the falsifiers (unlikely given both are flake-eval-shaped), the synthesis may need an addendum.
- **The 4-6 week Immix effort is literature-derived (Whippet, JikesRVM Immix).**  Actual v3 effort may be 30% higher (per `GC_DESIGN §10` honest-limits).

---

## 7. Cross-references

- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §5 (falsifier spec) + §6.7 (Plan B Immix outline)
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) (Step 3)
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) (Step 5)
- [`BIBOP_LITE_PROJECTION_2026-05-29.md`](BIBOP_LITE_PROJECTION_2026-05-29.md) (Step 7)
- [`IMMIX_LINE_OCCUPANCY_2026-05-29.md`](IMMIX_LINE_OCCUPANCY_2026-05-29.md) (Step 8)
- Memory: [[measure-twice-cut-once]] §3 + §3.7 + §3.8 (no carcass; 3-pivot rule)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
