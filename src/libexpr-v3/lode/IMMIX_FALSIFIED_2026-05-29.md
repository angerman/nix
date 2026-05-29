# Immix Stage 6 Step 14′ — SHIP gate FALSIFIED

**Date:** 2026-05-29
**Per:** `GC_DECISION_2026-05-29.md` §3 New Step 14′ + pre-committed §9 thresholds
**Status:** BINDING — Step 14′ falsification.  Per the decision doc: "If any FAIL: write `IMMIX_FALSIFIED_2026-XX-XX.md`, revert behind opt-in, escalate to Step 10 re-litigation (likely SCOPE-PIVOT-CACHE)."

---

## 1. The hypothesis being killed

> "Immix mark-region allocator (Steps 11′-13′ already landed) delivers peak_rss reduction ≥ 200 MB on hello.drvPath AND ≥ 500 MB on HNE under `NIX_V3_MAJOR_GC=1 V3_DBG_IMMIX_ALLOC=1` vs default (gates OFF)."

KILLED.

---

## 2. Measurement

Same-host, fresh process per run, `NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1`, `NIX_V3_MAX_HEAP=4G` where needed.  N=5 per config.  Trimmed mean drops min + max (robust against first-run page-cache effects observed across all configs).

### hello.drvPath (N=5 each)

| Config | Raw | Trimmed mean | σ (trimmed) |
|---|---|---|---|
| Baseline (gates OFF) | 855.0, 758.2, 758.1, 758.3, 758.6 | **758.4 MB** | 0.3 |
| `NIX_V3_MAJOR_GC=1 V3_DBG_IMMIX_ALLOC=1` | 780.8, 781.2, 780.9, 781.0, 781.5 | **781.0 MB** | 0.2 |

**Δ peak_rss = +22.6 MB (REGRESSION).**  Pre-committed gate: ≥ 200 MB reduction → SHIP.  **Miss by 222.6 MB.**

### HNE (`(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath`)

| Config | Raw | Trimmed mean | σ (trimmed) |
|---|---|---|---|
| Baseline (gates OFF) | 2433.4, 2498.3, 2497.0, 2497.2, 2497.9 | **2497.4 MB** | 0.5 |
| `NIX_V3_MAJOR_GC=1` only | 2098.8, 2483.6, 2537.5, 2536.1, 2618.8 | **2519.1 MB** | 30.7 |
| `NIX_V3_MAJOR_GC=1 V3_DBG_IMMIX_ALLOC=1` | 2108.7, 2492.4, 2467.6, 2468.5, 2469.0 | **2468.4 MB** | 0.7 |

**Δ peak_rss = -29.0 MB (reduction).**  Pre-committed gate: ≥ 500 MB reduction → SHIP.  **Miss by 471.0 MB.**

### Correctness (passing)

| Gate | Result |
|---|---|
| Byte-equal vs TW on `hello.name / .pname / .drvPath / .outPath / firefox.name` | 5/5 ✓ |
| `--core` regression suite (15 cases including 143 lang + 58 property) | 15/15 ✓ |

Both correctness gates pass.  The falsification is on memory ROI alone.

---

## 3. Why the prior projection diverged

`IMMIX_LINE_REGION_ALLOC_2026-05-29.md` + `IMMIX_RECYCLE_POLICY_2026-05-29.md` projected HNE peak −355 MB.  Empirical: −29 MB trimmed.  An order of magnitude lower.

Hypotheses for the gap (not falsified here, observation only):

1. Prior projections used different baseline / measurement methodology (per-baseline-doc).  Same-host re-measurement against `gates-OFF baseline` gives a more honest comparison than the prior staged Step 11 → 12 → 13 trajectory deltas.
2. Step 12′ Immix line-region allocator's hit rate was previously measured at <30% ("Step 12′ acceptance MISSED").  An under-hitting allocator falls back to fresh-block bump, which doesn't recover dead lines — explaining why the savings are smaller than projected.
3. The first-run-low pattern (855 MB hello, 2099/2109 MB HNE on baseline #1 / gateOnly #1 / gateImmix #1) is an OS page-cache / Boehm-init-state artifact.  Including/excluding it swings the mean by 50-100 MB.  Trimmed mean is the principled choice.

---

## 4. Falsification register update (now 7 variants × 0 MB shipped)

| # | Variant | Falsified by | Doc |
|---|---|---|---|
| 1 | Ditch Boehm | gc_count=1, gc_total_ms=0 | `GC_DITCH_BOEHM_FALSIFIED_2026-05-27.md` |
| 2 | Boehm tuning §6.2 | `boehm_unmapped` stuck at 0 | `BOEHM_TUNING_FALSIFIED_2026-05-27.md` |
| 3 | Periodic GC_gcollect() | +14 MB / 9× wall | `BOEHM_TUNING_FALSIFIED_2026-05-27.md` |
| 4 | Standalone arena dereg | 0-1 MB | `ARENA_DEREG_FALSIFIED_2026-05-27.md` |
| 5 | Cheney semispace | +486 MB regression | `STAGE_6_CHENEY_FALSIFIED_2026-05-27.md` |
| 6 | Flat mark-sweep | +22 MB hello / -38 MB HNE | `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md` |
| **7** | **Immix Step 14′ SHIP gate** | **+22.6 MB hello / -29.0 MB HNE** | **THIS DOC** |

Plus the bridge-eviction stages 1/1.5/2/2b from the parallel #875 track (also 0 MB).

The trajectory is consistent: every GC-family variant tried has missed the SHIP gate by an order of magnitude on at least one workload.

---

## 5. Per-design action

Per `GC_DECISION_2026-05-29.md §3 New Step 14′`:

> If any FAIL: write `IMMIX_FALSIFIED_2026-XX-XX.md`, revert behind opt-in, escalate to Step 10 re-litigation (likely SCOPE-PIVOT-CACHE).

* **This doc: WRITTEN.**
* **"Revert behind opt-in":** the gates `NIX_V3_MAJOR_GC=1` + `V3_DBG_IMMIX_ALLOC=1` are ALREADY opt-in (default OFF per `g_majorGcEnabled` / `g_dbgImmixAllocEnabled`).  No revert needed.  The infrastructure stays in-tree as gated scaffolding.
* **Step 10 re-litigation:** the binding choice was Immix; the falsification routes to the SCOPE-PIVOT-CACHE branch.  Per `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` §2: cache eviction (Phase 4b LRU) — projected 500-950 MB on HNE, 2-3 wk effort after 3 prereqs.  That's the next lever.

---

## 6. What this doesn't say

* **Doesn't say Immix is conceptually broken.**  The Immix line-region allocator IS landed correctness-clean (5/5 nixpkgs + --core 15/15 here).  The infrastructure works; the empirical memory ROI is below threshold.
* **Doesn't retract Steps 11′-13′ commits.**  They stand as gated infrastructure.  Per `[[measure-twice-cut-once]]` §3.7 carcass rule, retirement would be considered at Week-4 EXIT_GC_SPIRAL re-evaluation IF the orthogonal path converges on a non-GC ship.
* **Doesn't preclude Immix+X composition.**  If a future orthogonal lever cuts the heavy-bridge retention by ≥ 600 MB, the remaining ~400 MB peak could be in Immix's reachable range.  Bookmark only.

---

## 7. Honest limits

* **N=5 sample size:** σ in raw data is moderate (especially gateOnly).  Larger N would tighten confidence intervals.  Decision: even with the most-favorable interpretation (full mean instead of trimmed), HNE is at -84 MB vs ≥500 MB gate.  N is enough to falsify.
* **First-run artifact:** consistent across all configs, removed by trimmed-mean.  If a future Immix variant claims to depend on cold-state, that's a separate measurement design.
* **Methodology cross-check:** `bench/measure-peak-noise-floor.sh` was not used here — direct shell invocation gave equivalent fresh-process data.  For a more rigorous re-measurement: noise-floor script with `gate-off-cache-off` vs `gate-on-reuse-on-stress` (note: `V3_DBG_IMMIX_ALLOC=1` is not currently a gate option in that script's config matrix).
* **Wall regression not measured separately.**  Pre-committed gate: ≤10% hello / ≤15% HNE.  Eval times were comparable across configs (no timeouts).  Formal hyperfine measurement deferred — moot given the peak_rss falsification.

---

## 8. What lands as a result

This commit lands ONLY the falsification doc.  No code changes — Immix infrastructure remains opt-in.

Cross-references:
* `GC_DECISION_2026-05-29.md` — the binding decision this falsifies
* `GC_PAUSE_2026-05-29.md` — Rule-0 GC track pause; this re-validates the pause
* `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` — companion 4-week plan; Step 14′ falsification is the trigger for cache-eviction-path commitment
* `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md` — flat MS falsification (variant #6)
* `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` — Path B (Stage 6 / Immix) was the dependency for #875 page-release
* `[[falsification-rule]]` — Rule 0 (this doc is the kill)
* `[[measure-twice-cut-once]]` — pre-committed threshold; data falsifies; document; move on

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
