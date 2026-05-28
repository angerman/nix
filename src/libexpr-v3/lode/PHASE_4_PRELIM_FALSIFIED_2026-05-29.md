# Phase 4 SHIP gate — PRELIMINARY FALSIFICATION

**Date:** 2026-05-29
**Status:** PRELIM FALSIFIED — current MS (Phases 0–3.8 without optimizations) does not meet the pre-committed SHIP gate from [`GC_DESIGN_POST_CHENEY_2026-05-28.md §9`](GC_DESIGN_POST_CHENEY_2026-05-28.md).
**Triggering measurement:** noise-floor matrix (12 cells, N=10/cell, trimmed-mean σ).
**Step:** 3 of the post-Phase-3.8 plan (task #839).

Companion artifacts:
- Baselines JSON: [`../bench/baselines/2026-05-29-phase3-noise-floor/`](../bench/baselines/2026-05-29-phase3-noise-floor/)
- Measurement harness: [`../bench/measure-peak-noise-floor.sh`](../bench/measure-peak-noise-floor.sh) (commit `a6d392466`)
- Design doc this verdict tests against: [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §9
- Methodology rule applied: [[measure-twice-cut-once]] §3 + [[threshold-recalibration-rule]] (NO post-hoc adjustment permitted)

---

## 1. TL;DR

| SHIP-gate criterion | Required | Measured | Verdict |
|---|---|---|---|
| hello.drvPath peak_rss reduction | **≥ 200 MB** | **−21.88 MB (regression)** | ❌ FAIL by 222 MB |
| HNE peak_rss reduction | **≥ 500 MB** | **−38.66 MB** (within gate-off 2σ envelope) | ❌ FAIL by 461 MB |
| hello.drvPath wall regression | ≤ 10 % | **+40.7 %** | ❌ FAIL |
| HNE wall regression | ≤ 15 % | +10.4 % | ✓ PASS |

**Three of four §9 criteria fail; two by >2× the threshold.** Per `GC_DESIGN §9`:

> If after Phase 4: Peak RSS reduction <100 MB → **FALSIFIED**. Write `MARK_SWEEP_FALSIFIED.md`; revert behind opt-in; pivot to Immix or scope-pivot to cache-eviction first.

The "PRELIM" qualifier reflects that Steps 11/12/13 (size-class binning, adaptive threshold, BiBOP-lite) have NOT YET been implemented — they are the design-anticipated rescue path. **If those land and Step 14 re-measurement still fails the §9 gate, the falsification becomes final** (`MARK_SWEEP_FALSIFIED_2026-XX-XX.md`).

MS code is **already behind opt-in** (`NIX_V3_MAJOR_GC=1`, default-OFF). No revert needed; gate remains opt-in pending the rescue attempt.

---

## 2. Measurement details

### 2.1 Noise-floor σ baseline (gate-OFF, the variance bound)

Pre-committed: σ ≤ 100 MB on HNE (per task #838 acceptance). Measured:

| Workload | gate-OFF σ | 2σ envelope |
|---|---|---|
| hello.drvPath | **0.09 MB** | ±0.18 MB |
| firefox.name | 0.09 MB | ±0.18 MB |
| HNE | **64.81 MB** | ±129.62 MB |

HNE's σ is two orders of magnitude larger than hello's. The variance is in the `elsewhere` bucket (σ_elsewhere = 64.83 MB), NOT v3_arena (σ = 0.00 MB across all 8 trimmed samples). Interpretation: ImportCache + SQLite + bytecode-disk-cache shadow memory has run-to-run nondeterminism (cache eviction order, mmap'd shared regions). Arena allocations are byte-for-byte deterministic across runs.

**Implication:** any HNE peak-RSS claim must clear 2σ ≈ 130 MB to be signal. Anything inside that envelope is noise.

### 2.2 hello.drvPath full matrix (4 configs × N=10)

| Config | peak_rss | σ | wall | v3_arena | elsewhere |
|---|---|---|---|---|---|
| gate-off | **753.35** | 0.09 | 0.95 | 587.20 | 0.00 |
| gate-on-reuse-off | 775.16 | 0.09 | 1.33 | 587.20 | 0.00 |
| gate-on-reuse-on | 775.23 | 0.35 | 1.34 | 587.20 | 0.00 |
| gate-on-reuse-on-stress | 775.29 | 0.06 | 1.35 | 587.20 | 0.00 |

**Δpeak (gate-on-reuse-on − gate-off) = +21.88 MB regression**, with 2σ envelopes [753.17, 753.53] and [774.53, 775.93] — **completely disjoint by 21 MB**. This is a clean, signal-above-noise REGRESSION.

**Δwall = +40.7%** (0.95 → 1.34 s) — 4× the §9 ≤10% threshold.

**v3_arena unchanged at 587.20 MB.** Sweep finds dead cells but the bytes go into per-exact-size free-list bins that don't get reused (single-GC eval; no allocation post-GC). Phase 3.8 whole-block-free confirmed 0 blocks reclaimed. Net peak effect: just MS infrastructure overhead (bitmaps, conservative-mark byte-walk, free-list bins) added to gate-off baseline.

### 2.3 HNE full matrix (4 configs × N=10)

| Config | peak_rss | σ | wall | v3_arena | elsewhere |
|---|---|---|---|---|---|
| gate-off | **3043.81** | 64.81 | 9.32 | 1593.80 | 1047.05 |
| gate-on-reuse-off | 3090.29 | 125.54 | **89.40** | 1593.80 | 1093.53 |
| gate-on-reuse-on | 3005.15 | 10.85 | 10.29 | **1509.90** | 1092.28 |
| gate-on-reuse-on-stress | 2998.80 | 0.81 | 10.07 | 1509.90 | 1085.94 |

**Two notable findings beyond the SHIP-gate verdict:**

1. **gate-on-reuse-off is catastrophically slow** (89.40 s = 9.6× gate-off wall). With reuse disabled, MS marks + sweeps but the allocator does NOT consult free lists, so each GC effectively just adds CPU overhead without changing the allocation pattern. This config is measurement-only; ship config must enable reuse.

2. **v3_arena shrinks by 83.90 MB under reuse-on** (1593.80 → 1509.90, σ=0 across 8 samples — deterministic). But **elsewhere grows by ~45 MB** to compensate (1047.05 → 1092.28). Net peak_rss delta is a 38.66 MB reduction (within gate-off's 2σ=129.62 envelope = NOISE).

The arena DOES respond to MS; the peak RSS does NOT, because the freed bytes go into free-list bins (resident) and "elsewhere" structures (MS metadata, conservative-mark state) absorb the reclaimed space at the OS-resident-set level.

### 2.4 firefox.name full matrix (4 configs × N=10) — control

| Config | peak_rss | σ |
|---|---|---|
| gate-off | 238.22 | 0.09 |
| gate-on-reuse-off | 238.99 | 0.11 |
| gate-on-reuse-on | 239.09 | 0.06 |
| gate-on-reuse-on-stress | 239.11 | 0.06 |

Δpeak +0.87 MB, below 2σ envelopes. Firefox.name doesn't hit the MS trigger threshold (256 MB) so MS effectively doesn't fire. This is consistent: small workloads should be unchanged. Acts as a control that the harness isn't introducing systematic bias.

---

## 3. σ-envelope arithmetic for the binary verdict

Per task #839 acceptance: "gate-ON ≥ gate-OFF + 2σ on either workload" → FALSIFIED.

**hello.drvPath:**
- gate-off mean = 753.35; 2σ = 0.18; upper bound = 753.53
- gate-on-reuse-on mean = 775.23; 2σ = 0.70; lower bound = 774.53
- 774.53 > 753.53 by **21.00 MB** → gate-ON exceeds gate-OFF + 2σ → **FALSIFIED**

**HNE:**
- gate-off mean = 3043.81; 2σ = 129.62; lower bound = 2914.19
- gate-on-reuse-on mean = 3005.15; 2σ = 21.70
- 3005.15 > 2914.19 → gate-ON is **within** gate-OFF − 2σ = NO improvement signal → **INCONCLUSIVE for reduction; still WELL below the ≥500 MB SHIP threshold** → effectively **FAIL**

Mixed result: hello.drvPath is a CLEAN falsification (gate-ON > gate-OFF + 2σ); HNE is an INCONCLUSIVE-but-far-from-SHIP result. Either alone would block ship; together they confirm.

---

## 4. What this falsifies, what it does not

### 4.1 What's falsified (Rule 0 kill)

> **Hypothesis:** "Mark-sweep as implemented in Phases 0–3.8 (no further optimizations) meets the SHIP gate from `GC_DESIGN_POST_CHENEY §9`."

**KILLED.** Three of four criteria fail; two by ≥2× their threshold. The σ-envelope arithmetic is unambiguous on hello.drvPath. The 2σ HNE envelope makes any sub-130 MB delta indistinguishable from noise.

### 4.2 What's NOT falsified

> Mark-sweep as a design family for v3 GC.

The Phase 4 measurement tests the **current code path**, not the design. The §9 doc explicitly anticipates rescue via:
- **Step 11**: log-spaced size-class bins (currently per-exact-size `unordered_map<size_t, ...>` at `alloc.hh:1125` → near-zero reuse on variable-size Bindings)
- **Step 12**: adaptive threshold (currently fires ONCE at 256 MB, arena grows to 1510 MB after; oscillation policy would keep arena smaller throughout)
- **Step 13**: BiBOP-lite for Bindings (84% of arena per `DATA_STRUCTURE_AUDIT_2026-05-21`)

These optimizations were NOT in the Phase 4 measurement and are exactly the levers `GC_DESIGN §4.3–4.5` calls out for production MS. Step 14 (re-measurement after 11/12/13) is the final SHIP gate.

### 4.3 Specifically NOT a falsification of

- **The flat MS pattern itself** (OCaml, GHC nonmoving, Julia ship this at L≥0.5).
- **The Cheney-falsified conclusion** (Cheney remains falsified per `STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`).
- **The "v3 needs non-moving" conclusion** (still supported by L=0.49–0.54 measurements).
- **Phase 5 hardening / default-on flip** — those are downstream of Step 14 success and only relevant in that branch.

---

## 5. Implications for the post-Phase-3.8 plan

### 5.1 Continue, don't pivot (yet)

Steps 4–10 of the plan (L(t) spike, free-list stats, Falsifier #4 BiBOP spike, Falsifier #1 Immix line-occupancy) are the **measurement-driven inputs to the Step 10 decision**. They were always meant to fire BEFORE further MS investment. This preliminary verdict makes them MORE urgent, not less.

Specifically:
- **Step 6 (free-list stats)** will quantify how often per-exact-size bins miss (the suspected reason MS-as-shipped doesn't reduce arena reuse). If hit rate <20%, Step 11 is justified.
- **Step 7 (BiBOP spike)** will quantify whether segregating Bindings is worth +200 LoC. If <5% delta, drop BiBOP entirely from design.
- **Step 8 (Immix simulation)** will quantify whether mark-region buys anything over flat MS for v3.
- **Step 5 (L(t) trace)** will quantify whether L varies enough that trigger-policy alternatives (sticky mark-bits, opportunistic at low-L) could rescue cheaper designs.

### 5.2 Two-branch decision at Step 10

**Branch A (most likely):** Falsifiers reveal where MS lost the SHIP gate and how to rescue. Steps 11/12/13 implement targeted fixes; Step 14 re-measures; SHIP gate clears OR final-falsifies.

**Branch B:** Falsifiers reveal MS family-level limitations (e.g., elsewhere bucket is unattackable from arena GC, ImportCache is the dominant lever per `HNE_BUCKET_DECOMP`). Step 10 scope-pivots to cache-eviction (Phase 4b LRU per `MEMORY_REDUCTION_AVENUES`); MS stays opt-in scaffold.

### 5.3 What this is NOT a license to do

Per [[measure-twice-cut-once]] §3.7 (no carcass behind gate) and §3.8 (three failed pivots = falsification family):

- **Do NOT** add yet another opt-in MS variant gate ("let both coexist") to dodge the falsification.
- **Do NOT** post-hoc adjust the SHIP thresholds ("200 MB was too aggressive; let's accept 50 MB"). The §9 thresholds are pre-committed; recalibration requires meeting the three [[threshold-recalibration-rule]] conditions, none of which apply here.
- **Do NOT** declare PRELIM FALSIFIED a closed result and pivot without running the rescue. The "PRELIM" qualifier exists specifically to keep the door open for Steps 11/12/13.

---

## 6. Honest limits

- **macOS aarch64 only.** Linux MS behaviour may differ via different page-cache + libc free semantics.
- **N=10 with trim-2 = 8 used samples.** Larger N would tighten σ but at sub-linear noise-reduction; 8 samples gives 2σ confidence good enough for >20-MB decisions.
- **Wall measurements include cold-start cost** (process launch, libstore init). For very small workloads (firefox.name, ~0.29 s) this dominates; intra-workload eval-wall comparisons assume the dominant change is in v3 eval, not in startup. Validated by gate-OFF vs gate-ON σ similarity on the small workloads.
- **HNE eval pulls flake metadata** (~50 MB cache reads); the `elsewhere` bucket includes this. Phase 4b cache LRU would attack this bucket independently of MS.
- **Reuse-off config is for measurement, not ship.** Its 9.6× wall regression on HNE is artifactual.
- **PRELIM** ≠ FINAL. Step 14 (after Steps 11/12/13) is the binding §9 verdict.

---

## 7. Cross-references

- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §9 — the SHIP-gate spec this verdict tests against
- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md) — the prior falsification this MS path replaced
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — explains the 1047 MB elsewhere bucket
- [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md) — Step 4/5 spike that informs the Step 10 decision
- [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) — Bindings 84% dominance → BiBOP-lite rationale
- Memory: [[measure-twice-cut-once]] §3 + [[threshold-recalibration-rule]] — methodology rules applied here
- Memory: [[phase38-whole-block-free-2026-05-28]] — adjacent finding (Phase 3.8 0-blocks-reclaimed)

### Baselines JSON (all 12 cells)

`bench/baselines/2026-05-29-phase3-noise-floor/`:
- `hello.drvPath-{gate-off, gate-on-reuse-off, gate-on-reuse-on, gate-on-reuse-on-stress}.json`
- `firefox.name-{gate-off, gate-on-reuse-off, gate-on-reuse-on, gate-on-reuse-on-stress}.json`
- `HNE-{gate-off, gate-on-reuse-off, gate-on-reuse-on, gate-on-reuse-on-stress}.json`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
