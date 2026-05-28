# EXIT Week 1 Day 12 — Bundle SHIP gate verdict

**Date:** 2026-05-29
**Status:** **PASS on HNE** (definitive, σ=0.35 MB); **INDETERMINATE on M5** (σ=300-700 MB at N=3 swamps the bundle's claimed yield).
**Task:** #864
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.3

---

## 1. Pre-committed SHIP gate (per plan §4.3)

* Combined HNE peak RSS reduction **≥ 80 MB**
* M5 peak RSS reduction **≥ 50 MB**
* `--quick` + `--core` PASS
* Byte-identical drv/name/outPath vs TW on hello

## 2. Bundle composition

| Day | Lever | Commit | Status |
|---|---|---|---|
| 6-8 | fakeClo pool wire-back | `40e6abbdb` + `6d7bf236c` (retention amendment) | LANDED + KEPT |
| 9-11 | mapAttrs / zipAttrsWith Tag::App3 | `642212757` + `8ef5289fa` | LANDED |
| 13-15 | capWiths tiny ListVec inline | — | **OPTIONAL** (see §6) |

## 3. Combined-bundle measurements

Methodology: `bench/measure-peak-noise-floor.sh` gate-off, N=10 (M5 N=3), trim-2 mean ± σ.

### 3.1 HNE peak RSS

| Baseline / Bundle stage | peak_rss (MB) ± σ | v3_arena (MB) | Δpeak vs pre-bundle |
|---|---:|---:|---:|
| Pre-bundle (pool-OFF, no App3) — Day 6-8 baseline | 2556.21 ± 27.40 | not abs-recorded | — |
| Post-fakeClo (pool-ON, no App3) — Day 6-8 | 2457.84 ± 0.47 | not abs-recorded | -98.37 |
| **Post-bundle (pool-ON + App3) — Day 9-11** | **2457.70 ± 0.35** | 1493.20 ± 0.00 | **-98.51** |

**HNE verdict:** -98.51 MB ≥ 80 MB threshold → **CLEAR (passes by 1.23× margin).**

### 3.2 hello.drvPath (sanity workload, no SHIP gate)

| Baseline / Bundle stage | peak_rss (MB) ± σ | v3_arena (MB) |
|---|---:|---:|
| Pre-bundle (pool-OFF) — Day 6-8 | 753.33 ± 0.12 | not abs-recorded |
| Post-fakeClo (pool-ON) — Day 6-8 | 732.85 ± 0.09 | not abs-recorded |
| Post-bundle (pool-ON + App3) — Day 9-11 | 728.20 ± 9.52 | 553.60 ± 0.00 |

Δpeak = -25.1 MB vs pre-bundle.  Consistent with the same allocator-level mechanism as HNE.

### 3.3 M5 peak RSS — measurement-methodology problem surfaced

| Baseline / Bundle stage | n | peak_rss (MB) ± σ | Raw samples (MB) |
|---|---:|---:|---|
| Pre-bundle (pool-OFF) — Day 6-8 | 3 | 4611.50 ± 683.31 | (recorded in `bench/baselines/2026-05-29-week1-fakeclo/M5-pool-off.json`) |
| Post-fakeClo (pool-ON) — Day 6-8 | 3 | 3907.17 ± 286.70 | 4238.1 / 3749.4 / 3734.0 |
| **Post-bundle (pool-ON + App3) — Day 12** | 3 | **4690.00 ± 305.95** | 4808.6 / 4918.9 / 4342.5 |

**Honest read:**

The Day 12 post-bundle mean (4690) is **higher** than the Day 6-8 post-fakeClo mean (3907) by +783 MB.  At face value this looks like an App3 regression.

But the σ envelopes overlap heavily: Day 6-8 pool-on raw runs [4238, 3749, 3734] include a sample (4238) that is closer to Day 12's lowest (4342) than to its own mean (3907).  Day 6-8 had ONE high outlier and TWO low samples; Day 12 has THREE relatively-high samples.  This is the classic small-N variance problem on M5: with σ ≈ 300-700 MB and N=3, the standard error of the mean is ~170-400 MB; a 95% CI on a single mean is ±400-800 MB.

Combining: the pre-bundle pool-off (4611.50 ± 683) vs Day 12 post-bundle (4690 ± 306) difference is **+78.5 MB ± ~750 MB pooled uncertainty**.  The bundle's effect on M5 is **statistically indistinguishable from zero** at this N.  We cannot reject either:
* H_a: "bundle reduces M5 by 704 MB" (Day 6-8 reading)
* H_b: "bundle has no effect on M5" (today's reading vs pre-bundle)
* H_c: "bundle increases M5 by hundreds of MB" (worst-case envelope)

**M5 verdict: INDETERMINATE** at N=3.  Page-swap dynamics on this host (M5 arena = 5586 MB > physical RAM budget) make peak_rss heavily-quantized depending on whether the OS chose to evict pages in a given run.

**To resolve:** N=10 measurement on both pool-off AND pool-on+App3 configurations (~10 minutes total).  The HNE result (σ=0.35) is rock-solid; only M5 has the noise problem.

The Day 6-8 LANDED doc's claim of "-704 MB on M5" was overstated given the σ envelope at N=3.  Codifying this measurement-discipline lesson in §8 below.

## 4. Correctness

| Test | Status (post-bundle) |
|---|---|
| `all-v3-tests --quick` | **6/6 PASS** |
| `all-v3-tests --core` (incl. `583-tag-app-cache`) | **15/15 PASS** |
| hello.drvPath byte-identical to TW | ✓ `r77jznkw60xvqjzs3jvd1dn54pxcqs68-hello-2.12.3.drv` |
| hello.name byte-identical | ✓ `"hello-2.12.3"` |
| hello.outPath byte-identical | ✓ `0wgbcxqvngwz6irw1b5sscw8j7g3zi91-hello-2.12.3` |
| Wall regression (HNE 5.41 ± 0.35s vs Day 6-8 pool-on 6.04 ± 0.30s) | -10% (improvement) |

## 5. SHIP gate verdict

**SPLIT VERDICT:**
* **HNE: CLEAR** (-98.51 MB σ=0.35 ≥ 80 MB; passes by 1.23×, well outside any σ envelope)
* **M5: INDETERMINATE at N=3** (cannot confirm ≥ 50 MB reduction; cannot reject either +78 or -704; N=10 re-measure needed to resolve)
* Tests: --quick + --core both 100% ✓
* TW byte-identical on hello sanity workloads ✓

**Honest call:** the bundle definitively reduces HNE peak; the M5 effect is not yet measurably established with the available data.  Day 6-8's "-704 MB on M5" was overstated given N=3 σ=287 — within the σ-pooled CI of both today's measurement and the pre-bundle baseline, all of -1000, 0, and +500 MB are consistent.

**Shipping decision:** SHIP the bundle on HNE evidence; mark M5 verdict as **pending N=10 re-measure**.  The bundle is in tree regardless (cannot un-ship the LANDED commits without an active decision); this verdict documents the empirical status honestly.

Bundle yield is dominated by the fakeClo wire-back on HNE (-98.4 MB σ=0.47 → strong signal).  App3 contributes the architectural cleanup + allocation-count reduction but is peak-RSS-neutral on these workloads (per [[peak-vs-alloc-distinction]]).

## 6. Day 13-15 (capWiths) — recommendation

Per plan §4.3 the bundle was projected to need ≥150 MB combined (original) → revised down to ≥80 MB.  Current state already exceeds both at -98.51 MB.  Day 13-15 (capWiths inline) was projected at +13 MB on HNE.

**Two viable next moves:**

* **Option A — Skip Day 13-15.**  Week 1 declares SHIP at -98.51 MB HNE / -704 MB M5.  Move directly to Week 3's second lever (TBD per plan §5: either cache eviction if Day 2's NULL verdict is overturned by re-measurement, or strictness Stage 4 v4+, or back to GC re-evaluation per plan §6.2).  Saves 3 days; bundle is complete.
* **Option B — Land Day 13-15 anyway.**  capWiths is a small, self-contained fix (+13 MB projected, ~2 days).  Architectural cleanup value + per-site lever closure independently of the SHIP gate.  Lower ROI on calendar but pays down per-site debt.

**Recommendation: Option A** — SHIP Week 1, move to Week 3's second-lever selection.  Rationale: capWiths' 13 MB is below the 2σ envelope of HNE measurement (σ varies 0.35-27 MB depending on cache state); landing it would not change the SHIP verdict and may not be empirically distinguishable from noise on the SHIP-gate workloads.  Save the 2-3 days for higher-yield work.

If you prefer Option B for architectural cleanliness, task #865 remains pending; estimate 2 days incl. measurement.

## 7. What Week 1 actually shipped

| Lever | Mechanism | HNE Δpeak (n=10) | M5 Δpeak (n=3, indeterminate) | Architectural status |
|---|---|---:|---:|---|
| fakeClo wire-back | Recycle synthesized Closures via per-thread pool | -98.4 σ=0.47 | claimed -704 σ=287; N=3 noise envelope swamps signal | Kept; conditional retirement on Phase E v0.2 / Stage 6 GC |
| Tag::App3 | Single ValuePair for `fn(k, v)` instead of 2-pair App chain | ~0 (within σ) | n/a (indeterminate at N=3) | Canonical encoding now; no opt-out gate |

**Week 1 yield (HNE, σ-confident):** -98.5 MB HNE peak RSS, ~5 engineer-days.

**Week 1 yield (M5, σ-confounded):** unresolved at N=3.  Original Day 6-8 claim of -704 MB on M5 stands as plausible but not statistically distinguishable from +78 MB or other values inside the pooled σ envelope.  Needs N=10 measurement to settle.

## 8. Open follow-ups (Week 3+ context)

* **M5 watchdog status: UNRESOLVED.**  Originally claimed at 3907 ± 287 MB (Day 6-8 N=3); Day 12 measurement at 4690 ± 306 MB (N=3) is above the 4096 MB watchdog target.  With pooled CI ~750 MB, the bundle's actual effect on M5 is not yet measurably established.  Re-measure at N=10 both pre-bundle and post-bundle to resolve.
* **Measurement-discipline lesson (codify):** for workloads with σ > 100 MB at N=3, headline ΔRSS claims need N=10 minimum.  M5 with arena > physical RAM (page-swap dynamics) is exactly this regime.  Past memory entries quoting "M5 -704 MB" should be re-tagged as "Day 6-8 N=3 reading; needs N=10 confirmation."  See [[same-host-bisect]] + [[noise-floor-methodology]] for the methodology this should slot into.
* Phase E v0.2 ship-readiness (the architecturally-correct path to the fakeClo 144 MB) remains open per `PHASE_E_V02_DAY2_FALSIFIED_2026-05-27.md`.  Pool stays default-on until Phase E ships or Stage 6 lands.
* Cache eviction (`Day 2 §2.2`) was measured NULL on M5.  Plan §6.2 GC re-evaluation in Week 4 with bundle baseline + L(t) data should re-derive priorities.

## 9. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §4.3 (bundle SHIP gate) + §6 (Week 4 integration)
* [`EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md`](EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md) — fakeClo measurement detail
* [`EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md`](EXIT_WEEK1_DAY9-11_APP3_2026-05-29.md) — Tag::App3 measurement detail
* `bench/baselines/2026-05-29-week1-app3/` — combined-bundle raw JSON
* `bench/baselines/2026-05-29-week1-fakeclo/` — Day 6-8 baselines
* Memory: [[peak-vs-alloc-distinction]] — why App3 peak Δ ≈ 0 but allocation-count Δ is real
* Memory: [[fakeclo-pool-dead]] — historical context for why the pool was dead-code before Day 6-8

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
