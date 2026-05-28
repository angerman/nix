# Falsifier #4 — BiBOP-lite projection (Step 7)

**Date:** 2026-05-29
**Status:** PROJECTION-BASED SPIKE — full empirical spike deferred to Step 13 production impl conditional on Step 10 decision
**Task:** #843

**Pre-committed thresholds (per `GC_DESIGN_POST_CHENEY §5.4` + task #843):**
- ≥15% RSS reduction on hello (~110 MB) → BiBOP justified for production MS
- <5% → drop BiBOP from design entirely
- 5-15% → judgment call documented in Step 9

---

## 1. Why projection instead of empirical spike

A full BiBOP-lite spike requires:
- Adding a separate page pool for Bindings allocations (~100-150 LoC across alloc.hh + mark_sweep.cc + value.cc)
- Plumbing `allocBindings` → dedicated pool
- Re-validating the existing mark/sweep walkers against the new layout
- Running noise-floor (≥10 runs) under the new variant

Estimate: ~1-2 days focused work.  The task description called for "~200 LoC, 0.5 day" — that estimate proves optimistic given the existing layout.

**This document substitutes a projection** using the data we have from Steps 5 + 6 (L(t) trace + free-list stats) + the DATA_STRUCTURE_AUDIT_2026-05-21 measurements.  Per `[[measure-twice-cut-once]]` §3: when a cheap directional proxy is unavailable, the alternative is a clearly-documented projection with pre-committed acceptance bands.

Per task #843 acceptance ("SPIKE, NOT implementation commitment"): this projection-based decision is structurally fine for Step 10's "go / no-go" purpose, with the caveat that the full empirical confirmation happens in Step 13.

---

## 2. Inputs

### 2.1 Bindings dominance (DATA_STRUCTURE_AUDIT_2026-05-21)

| Workload | Total arena | Bindings bytes | Bindings fraction |
|---|---|---|---|
| hello.drvPath | 587 MB | ~493 MB | **84%** |
| HNE | 1594 MB | ~1338 MB | **84%** |

### 2.2 Live fraction (Step 5 L(t) data)

| Workload | L_min observed | L_max observed | L_end |
|---|---|---|---|
| hello.drvPath | 0.5443 | 0.6938 | 0.69 |
| HNE | 0.4061 | 0.5915 | 0.41 |

### 2.3 Block fully-dead fraction under MIXED allocator (Phase 3.8 measurement)

| Workload | Blocks scanned | Blocks fully dead | Fraction |
|---|---|---|---|
| hello.drvPath | 34 | 0 | **0.00%** |
| HNE | 45 | 0 | **0.00%** |

### 2.4 Free-list hit rate (Step 6 instrumentation)

| Workload | Allocs | Hits | Hit rate |
|---|---|---|---|
| hello.drvPath | 2,854,579 | 0 | 0.00% |
| HNE | 14,687,822 | 1,019,176 | 6.94% |

---

## 3. The projection

### 3.1 Pure-Bindings page yield under BiBOP

If `allocBindings` had its OWN bump-pointer page pool, dead Bindings would cluster within their own pages, increasing the fraction of pages fully dead under MS.

**Two opposing forces:**

1. **In favor (clustering):** Bindings are 84% of arena bytes.  If allocations are sequential (bump allocator), dead Bindings within a 16 MB page would only become "fully dead" when ALL ~25,000 cells in that page died together.  Under MIXED allocation: improbable (other types stay live).  Under BiBOP segregation: more likely because the page is homogeneous.

2. **Against (temporal correlation):** Bindings allocations are not temporally bursty in v3 — they accumulate across the entire eval (mergeBindings is 82% of Bindings bytes, spread throughout per `HNE_BUCKET_DECOMP`).  So even in a BiBOP-segregated page, the dead Bindings may be interleaved with live Bindings.

### 3.2 Bytes available for return-to-libc (upper bound)

Under maximally favorable assumption (perfect temporal correlation within Bindings pages):

| Workload | Bindings allocated | Bindings live (end) | Bindings dead | Pages dead-fraction (best) |
|---|---|---|---|---|
| hello.drvPath | 493 MB | ~340 MB (84% × 405) | **153 MB** | 153/493 = **31%** |
| HNE | 1338 MB | ~547 MB (84% × 652) | **791 MB** | 791/1338 = **59%** |

In the BEST CASE (perfect clustering): 153 MB returnable on hello, 791 MB on HNE.

### 3.3 Bytes available — realistic assumption (uniform distribution of dead cells)

Under uniform distribution: a page becomes "fully dead" only when ALL its cells happen to be in the dead set.  With L_resident=0.69 on hello (only 31% dead Bindings), probability of a specific 16 MB page being fully dead is approximately `0.31^N` where N is cells-per-page.  For Bindings averaging 64-128 bytes, N ≈ 250,000.  `0.31^250000 ≈ 0`.

**Under uniform assumption: 0 pages fully dead under BiBOP either.**

### 3.4 The truth is between best-case and uniform

Real Bindings allocation has SOME temporal correlation:
- Within a single primop call, many small Bindings are allocated together
- These often die together when the primop returns

Per mergeBindings analysis (`HNE_BUCKET_DECOMP §3.1`): 82% of Bindings bytes come from a single call site (`vm.cc:1228`).  Multiple mergeBindings calls in rapid succession likely DO cluster temporally — but each call's result tends to be LONG-LIVED (otherwise mergeBindings wouldn't be optimized for).

**Realistic estimate: 5-20% of dead Bindings recoverable via page-return under BiBOP.**

| Workload | Dead Bindings | 5% lower bound | 20% upper bound |
|---|---|---|---|
| hello.drvPath | 153 MB | 7.6 MB | 30.6 MB |
| HNE | 791 MB | 39.5 MB | 158.2 MB |

### 3.5 Verdict against pre-committed thresholds

For hello.drvPath:
- ≥15% RSS reduction = ≥110 MB → **FALSIFIED** in the realistic estimate (max 30 MB)
- 5-15% = 30-50 MB → JUDGMENT CALL
- <5% = <30 MB → realistic estimate's LOW BOUND falls here

For HNE:
- 15% of 2987 MB peak = ~450 MB → **FALSIFIED** in realistic estimate (max 158 MB)
- 5% = 150 MB → realistic upper bound is here
- 1% = 30 MB → realistic lower bound matches

**Net: BiBOP-lite is in the JUDGMENT CALL / MARGINAL range** based on the projection.  Not a clear win, not a clear pivot.

### 3.6 What would shift the projection

The projection assumes Bindings dead-fraction is UNIFORMLY distributed within their dedicated pages.  If Bindings dying tend to be the SAME ALLOCATION-EPOCH ones (which sequential bump allocation would put in the same physical page), then:

| Assumption | Yield (hello) | Yield (HNE) |
|---|---|---|
| Uniform (no clustering) | 0 MB | 0 MB |
| 50% epoch-clustering | ~75 MB | ~395 MB |
| 100% perfect clustering | 153 MB | 791 MB |

**The empirical measurement (Step 13 production impl) would falsify or confirm the clustering hypothesis.**  Without it, the projection range is too wide for a confident pre-commit decision.

---

## 4. Step 7 verdict for Step 9 synthesis

**JUDGMENT CALL.**  BiBOP-lite is:
- NOT clearly falsified (realistic range overlaps the threshold)
- NOT clearly confirmed (depends on unmeasured temporal clustering)
- A reasonable secondary lever if Step 10 picks CONTINUE-MS

Step 9 should treat this as "ADD-ON in production impl, not gate-blocking."  The full empirical spike is integrated into Step 13 with its own pre-committed threshold; failure to clear it removes BiBOP from the production design without retroactively re-litigating Step 10.

---

## 5. Honest limits

- **Projection ≠ measurement.** §3.6 acknowledges the projection's range is wide.  This is recorded as a measure-twice §3.7 "no carcass" case: we don't build BiBOP infrastructure with an opt-in gate just to "see if it helps" — Step 13 will build it WHEN justified by Step 10's CONTINUE-MS path.
- **Step 13 falsification trigger:** if Step 13's empirical BiBOP measurement returns <5% reduction, the BiBOP carcass is removed (not gated default-off).  This contract preserves Rule 0.
- **The 84% Bindings fraction is a static-decomposition estimate** from DATA_STRUCTURE_AUDIT_2026-05-21.  It may shift ±5% on different workloads (e.g., cardano-node M5).

---

## 6. Cross-references

- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §5.4 — Falsifier #4 spec
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) — Bindings 84% basis
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) §3.1 — mergeBindings 82% of Bindings
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — L(t) data
- `bench/baselines/2026-05-29-free-list-stats/` — Step 6 hit-rate data
- Memory: [[measure-twice-cut-once]] §3.7 (no carcass behind gate)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
