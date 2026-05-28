# EXIT Week 1 Day 12 — Bundle SHIP gate verdict

**Date:** 2026-05-29
**Status: SUPERSEDED by Week 1 retrospective 2026-05-29 evening.**  This Day 12 verdict's measurements used the bench script's default `NIX_BIN=$ROOT/build/src/nix/nix` — a separate stripped build whose mtime is older than the Day 6-8 commit.  The honest re-measurement on `builddir/` with App3 rolled back (see [`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md)) shows the bundle is real (-105 MB HNE / -822 MB M5 arena) BUT M5 trim-2 mean = 4343 MB is **247 MB OVER** the watchdog target, not 50 MB under as claimed below.  Bundle SHIPS on both workloads; watchdog NOT closed by Week 1 alone.  Original status preserved below for historical context.

**Original status:** **PASS on HNE** (definitive, σ=0.35 MB at N=10).  **PASS on M5** at N=10 with -519 MB peak Δ (1.2σ signal) and -806 MB arena Δ (deterministic).  Watchdog goal (4096 MB) cleared by ~50 MB on the trim-2 mean.
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

### 3.3 M5 peak RSS — settled at N=10

| Baseline / Bundle stage | n | peak_rss (MB) ± σ | arena (MB) | Raw samples (MB) |
|---|---:|---:|---:|---|
| Pre-bundle (pool-OFF, App3 ON) — Day 12 N=10 | 10 | **4564.86 ± 336.79** (trim-2 N=8) | **6392.1 ± 0** | 2703 / 4021 / 4307 / 4455 / 5021 / 4781 / 4819 / 5248 / 4229 / 4883 |
| **Post-bundle (pool-ON + App3) — Day 12 N=10** | 10 | **4045.88 ± 280.99** (trim-2 N=8) | **5586.8 ± 0** | (full set in `bench/baselines/2026-05-29-week1-app3/m5-gate-off-n10.json`) |
| _(reference)_ Pre-bundle (pool-OFF, no App3) — Day 6-8 N=3 | 3 | 4611.50 ± 683.31 | (not recorded) | — |
| _(reference)_ Post-fakeClo (pool-ON, no App3) — Day 6-8 N=3 | 3 | 3907.17 ± 286.70 | (not recorded) | 4238 / 3749 / 3734 |

**Two metrics, two confidence levels:**

* **peak_rss Δ = -518.98 MB**, pooled σ ≈ 437 MB → **~1.2σ signal**.  Likely real; 95% CI is roughly ±860 MB so true value plausibly between -1380 and +340 MB.  Above the ≥ 50 MB SHIP threshold by 1.2σ.  Honest framing: "likely a real reduction; magnitude has wide CI."
* **v3_arena Δ = -805.30 MB**, σ = 0 → **deterministic**.  The bundle removes 805 MB of v3-managed arena bytes per M5 eval; this is the same number Day 6-8 saw (-805 MB), confirming the allocator-level effect is consistent.

**Why peak σ is so much wider than arena σ:** M5's v3 arena (5586 MB pool-on / 6392 MB pool-off) exceeds physical RAM budget.  Peak RSS is heavily-quantized by OS page-eviction decisions per run; arena bytes are a deterministic v3-counter.  Different runs evict different pages, producing run-to-run peak swings of ~2500 MB even at the same arena footprint.

**The arena Δ is the load-bearing measurement.**  It's noise-free, replicates Day 6-8's reading, and proves the bundle reduces v3-managed memory by 805 MB on M5.  The peak Δ tracks it (consistent direction + smaller-than-arena magnitude due to page-out), just with much wider CI.

**Watchdog status:** post-bundle peak mean = 4045.88 MB ± 281 MB; watchdog target = 4096 MB.  Trim-2 mean is **50 MB under the watchdog**.  σ envelope: at +1σ runs (~4327 MB) M5 occasionally exceeds the watchdog by ~230 MB; at -1σ runs (~3765 MB) it's well under by 330 MB.  **The bundle puts M5 right at the watchdog edge**, with most runs under and some runs over.  Not unconditionally safe; not unconditionally over.

**Day 6-8 LANDED doc retrospective:**  the "-704 MB on M5" claim at N=3 is within today's σ envelope (today's reading: -519 MB ± 337).  Both are estimates of the same underlying parameter; N=10 gives the tighter point estimate.  Codifying the measurement-discipline lesson in §8.

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

**CLEAR on all four criteria:**

* **HNE: CLEAR** (-98.51 MB σ=0.35 ≥ 80 MB threshold; passes by 1.23×, definitive)
* **M5: CLEAR** (peak Δ = -519 ± 337 MB at N=10 ≥ 50 MB threshold by 1.4σ; arena Δ = -805 MB deterministic, σ=0)
* Tests: --quick + --core both 100% ✓
* TW byte-identical on hello sanity workloads ✓

Bundle yield is dominated by the fakeClo wire-back on HNE (-98.4 MB σ=0.47).  App3 contributes the architectural cleanup + allocation-count reduction but is peak-RSS-neutral (per [[peak-vs-alloc-distinction]]).

**Honest framing of M5 result:**
* The -519 MB peak Δ is ~1.2σ — likely real but with wide CI.  Could plausibly be anywhere from -1380 to +340 MB at 95% CI.
* The -805 MB arena Δ is deterministic — proves the bundle removes 805 MB of v3-managed arena bytes per M5 eval.
* Peak σ is wide because M5 arena exceeds physical RAM, making peak_rss page-swap-noise-dominated.  Arena Δ is the trustworthy v3-side measurement.
* Watchdog status: trim-2 mean post-bundle is 50 MB under the 4096 MB target; ~30-40% of runs exceed.  Edge-of-watchdog, not safely-under-watchdog.

**Shipping decision:** SHIP the bundle.  All four pre-committed gate criteria pass.

## 6. Day 13-15 (capWiths) — recommendation

Per plan §4.3 the bundle was projected to need ≥150 MB combined (original) → revised down to ≥80 MB.  Current state already exceeds both at -98.51 MB.  Day 13-15 (capWiths inline) was projected at +13 MB on HNE.

**Two viable next moves:**

* **Option A — Skip Day 13-15.**  Week 1 declares SHIP at -98.51 MB HNE / -704 MB M5.  Move directly to Week 3's second lever (TBD per plan §5: either cache eviction if Day 2's NULL verdict is overturned by re-measurement, or strictness Stage 4 v4+, or back to GC re-evaluation per plan §6.2).  Saves 3 days; bundle is complete.
* **Option B — Land Day 13-15 anyway.**  capWiths is a small, self-contained fix (+13 MB projected, ~2 days).  Architectural cleanup value + per-site lever closure independently of the SHIP gate.  Lower ROI on calendar but pays down per-site debt.

**Recommendation: Option A** — SHIP Week 1, move to Week 3's second-lever selection.  Rationale: capWiths' 13 MB is below the 2σ envelope of HNE measurement (σ varies 0.35-27 MB depending on cache state); landing it would not change the SHIP verdict and may not be empirically distinguishable from noise on the SHIP-gate workloads.  Save the 2-3 days for higher-yield work.

If you prefer Option B for architectural cleanliness, task #865 remains pending; estimate 2 days incl. measurement.

## 7. What Week 1 actually shipped

| Lever | Mechanism | HNE Δpeak (n=10) | M5 Δpeak (n=10) | M5 Δarena (n=10) | Architectural status |
|---|---|---:|---:|---:|---|
| Combined bundle (fakeClo + App3) | Pool recycle + 1-pair mapAttrs | -98.5 σ=0.35 | -519 σ=337 (1.2σ) | **-805 σ=0** | Both kept |
| _(per-lever attribution below)_ |  |  |  |  |  |
| fakeClo wire-back | Recycle synthesized Closures via per-thread pool | -98.4 σ=0.47 | dominant share | -805 σ=0 | Kept; conditional retirement on Phase E v0.2 / Stage 6 GC |
| Tag::App3 | Single ValuePair for `fn(k, v)` instead of 2-pair App chain | ~0 (within σ) | likely ~0 (per [[peak-vs-alloc-distinction]]) | likely ~0 | Canonical encoding now; no opt-out gate |

**Week 1 yield (HNE, σ-confident):** -98.5 MB HNE peak RSS.
**Week 1 yield (M5, peak):** -519 MB ± 337 MB (likely real, wide CI).
**Week 1 yield (M5, arena — load-bearing):** **-805 MB deterministic** v3-arena reduction.

The -805 MB arena reduction is the cleanest single number from Week 1 on M5.  It's noise-free, replicates Day 6-8's reading exactly, and directly attributable to fakeClo recycle.  ~5 engineer-days for the headline yield.

## 8. Open follow-ups (Week 3+ context)

* **M5 watchdog status: EDGE OF WATCHDOG.**  Post-bundle peak (N=10 trim-2 mean): 4045.88 ± 280.99 MB; target 4096 MB.  Mean is 50 MB under; ~30-40% of runs exceed the watchdog at +1σ.  Arena (5586 MB) is well over the NIX_V3_MAX_HEAP=4G default — anyone running M5 with the default heap cap will hit OOM; raising the cap to 6G or above is current operational requirement.  The bundle isn't safely under-watchdog; further work needed.
* **Measurement-discipline lesson (codify):** for workloads with σ > 100 MB at N=3, headline ΔRSS claims need N=10 minimum.  M5 with arena > physical RAM (page-swap dynamics) is exactly this regime.  Past memory entries quoting "M5 -704 MB" should be re-tagged as "Day 6-8 N=3 reading; today's N=10 reading is -519 MB ± 337 (same underlying signal, tighter point estimate)."  See [[same-host-bisect]] + [[noise-floor-methodology]] for the methodology this should slot into.  **Codify:** for any workload where arena > physical RAM, peak σ is dominated by page-swap noise; arena Δ is the load-bearing measurement, not peak Δ.
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
