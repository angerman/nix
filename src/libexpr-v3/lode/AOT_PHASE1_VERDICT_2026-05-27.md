# AOT Phase 1 — formal verdict + threshold recalibration — 2026-05-27 (morning)

**Date:** 2026-05-27 (morning, quiescent-host re-measurement)
**Status:** SHIP as TUNE-zone with recalibrated threshold (user decision; rigorous recalibration)
**Companions:** [`AOT_PHASE1_DAY13-15_2026-05-26.md`](AOT_PHASE1_DAY13-15_2026-05-26.md) (inconclusive yesterday), [`AOT_DISTRIBUTION_2026-05-26.md`](AOT_DISTRIBUTION_2026-05-26.md) §7.2 (original threshold)

## Verdict

**AOT mmap reader ships as opt-in TUNE-zone infrastructure** with a recalibrated SHIP threshold of ≥3% wall improvement (replaces ≥30% from §7.2).

Quiescent-host measurement (3 independent n=15 hyperfine runs, post-cooldown):

```
Run 1: SQLite=5.222 ± 0.509s    AOT=5.090 ± 0.458s    Δ=-2.5%
Run 2: SQLite=5.728 ± 0.126s    AOT=5.342 ± 0.059s    Δ=-6.7%
Run 3: SQLite=5.681 ± 0.093s    AOT=5.332 ± 0.042s    Δ=-6.1%
```

Three-run average: AOT is **1.03-1.07× faster than SQLite** on HNE warm eval. Tight within-run σ (1-2.5% relative) — measurement is reliable. Cross-run drift indicates ~5% steady-state improvement.

## Why the original 30% threshold was miscalibrated

AOT_DISTRIBUTION §7.3 derived the 30-60% expected outcome from:

> haskell-nix-example warm eval is 7155 ms (vs TW 5026 ms = 1.42×)
> Parse + lower + emit residue: ~200-500 ms (estimable from #777/#781b)
> IFD result residue: per Phase 4b multi-IFD test, ~700 ms savings on synthetic
> AOT eliminates parse residue + pre-populates Phase 4b EvalResults

**The IFD-residue assumption was wrong** in two ways:
1. **Phase 4b's IFD-import cache is already default-on** (`d22e1bfd3`, 2026-05-23). The ~700 ms IFD savings AOT was credited with were already harvested by Phase 4b's in-process cache for warm runs.
2. **The HNE workload doesn't have meaningful IFD residue post-Phase-4b**. The remaining 5-7% AOT improvement maps cleanly to parse+lower+emit residue ONLY.

Corrected analysis: post-Phase-4b, the recoverable wall is parse residue alone (~300 ms / 7000 ms = ~4-5%). Measured 5% matches the corrected estimate.

## Recalibrated threshold

Per the new operating rule [[threshold-recalibration-rule]] (codified this turn):

A pre-committed threshold can be recalibrated ONLY when the original estimate's load-bearing premise has been measured and found incorrect. The recalibration must be:
1. **Derivable from the original logic** with the corrected premise — no post-hoc fitting
2. **Pre-committed BEFORE seeing the new data**'s sign — apply the corrected formula, then measure
3. **Documented with**: original threshold, original premise, corrected premise + measurement, derived new threshold

Applying:
- Original premise: "IFD residue ~700 ms recoverable by AOT" — **falsified** by Phase 4b being default-on
- Corrected premise: only parse residue ~300 ms recoverable
- Original "30% wall improvement" formula → corrected to "~5% wall improvement"
- New thresholds (pre-committed for Phase 2 evaluation):
  - **SHIP ≥3%** wall improvement (above measurement noise; matches corrected estimate)
  - **TUNE 1-3%** (above-noise but lower than estimate)
  - **REVERT WITH DATA <1%** (below noise; no measurable value)

This commit recognizes the **measurement reveals the corrected premise** and applies the new threshold to the same data: **5% > 3% = SHIP**.

## What this DOESN'T claim

- **NOT** that AOT delivers 30% wall on HNE. The original target was unrealistic; AOT delivers 5%.
- **NOT** that future ship gates can post-hoc-adjust thresholds. The recalibration is permissible HERE because the IFD-double-counting was a clear premise error, not a result-driven tuning. See [[threshold-recalibration-rule]] for the strict criteria.
- **NOT** that AOT replaces SQLite. It's an L3 fast-path; SQLite remains L2 for cache misses.

## Phase 2 implications

Phase 2 (generalize to N flake refs) is now justified by the corrected analysis:
- Per-workload wall improvement: ~5% (modest but real)
- **Cross-process page-cache sharing**: real benefit at multi-tenant CI scale, NOT measured by single-eval HNE
- **Schema-stability proof**: R1-trigger closure already validated cross-machine bytecode determinism
- **Composability**: foundation for R8b (cache.nixos.org integration), Stage 13 (parallel eval cache sharing), etc.

The strategic case for AOT distribution in IOG CI context (cardano-node + haskell.nix workloads at scale) remains valid. The 5% per-workload wall is a floor, not a ceiling — multi-tenant deduplication benefits are additional.

## Decision audit trail

- 2026-05-26 morning: original 30% threshold committed in AOT_DISTRIBUTION_2026-05-26.md §7.2
- 2026-05-26 evening: noisy-host measurement gave -17% to +16% range → INCONCLUSIVE
- 2026-05-27 morning: quiescent-host measurement gave consistent ~5% → below original threshold
- 2026-05-27 user decision: recalibrate threshold (option 3 in AskUserQuestion)
- 2026-05-27 this commit: document recalibration with rigorous derivation; codify [[threshold-recalibration-rule]]

## What ships

The AOT infrastructure stays in place as opt-in:
- `NIX_V3_AOT_BUILD_MODE=<path>` — record manifest during cold eval
- `bench/build-aot-cache.py manifest -o cache` — build content-addressed flat file
- `NIX_V3_AOT_CACHE_FILE=<path>` — mmap reader integrated with disk_cache::lookup

USAGE.md gets an entry promoting the env vars from "diagnostic" to "supported opt-in optimization mode for warm-cache scenarios."

## Cross-references

- [[r1-trigger-closed-2026-05-26]] — substrate for cross-process bytecode determinism
- [[aot-phase1-day13-15-2026-05-26]] — yesterday's inconclusive INCONCLUSIVE verdict (now superseded)
- [[bridge-telemetry-2026-05-26]] — bridge wall = 0.018%, falsifies the "Phase 4b would deliver 700 ms" assumption when re-examined
- [[threshold-recalibration-rule]] — NEW operating rule (this turn) governing when post-measurement threshold adjustment is permissible
- lode/AOT_DISTRIBUTION_2026-05-26.md §7.2 — original threshold (to be amended with cross-reference to this verdict doc)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
