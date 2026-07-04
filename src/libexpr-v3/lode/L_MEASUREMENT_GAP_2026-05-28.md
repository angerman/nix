# Live-fraction measurement gap — research note

**Date:** 2026-05-28
**Author:** session synthesis (methodology audit)
**Status:** RESEARCH NOTE — identifies a measurement gap underlying multiple GC decisions; proposes a cheap spike
**Triggering question:** "Have we actually measured L properly? Am I right that non-moving would be good?"

Companion docs:
- [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) — the end-of-eval L data we currently have
- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md) — the n=1 mid-eval L inference
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §2.3 — "v3's L is structurally high" claim that rests on this gap
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 — methodology audit precedent

---

## 1. TL;DR

**Every GC claim that depends on "v3 has high L" rests on ONE end-of-eval data point per workload plus ONE mid-eval inference from the Cheney falsification.** We don't have:

- Time-series L(t) during eval
- L distribution across many candidate GC-trigger moments
- L_trough (the minimum — the moment best for copying)
- L vs allocation-rate correlation
- Per-workload variance (hello/HNE/M5)

The Cheney falsification is real and survives this critique (Cheney DID hit a high-L moment at its trigger). But the conclusion **"v3's heap profile demands non-moving"** is conditional, not absolute. A different trigger policy could expose lower-L moments where copying works.

**Mark-sweep is still the safer choice** because it's INSENSITIVE to L variance — but the strength of the argument is "robust to unknown" not "L is high."

**Proposed spike: ~1 day.** Add `NIX_V3_LIVE_TRACE_PERIODIC=K` that runs the existing precise-root live-walk every K MB of allocation. Outputs L(t) CSV. Grounds future GC decisions in distribution, not a single point.

---

## 2. What L actually means for GC

For Cheney semispace over an arena of size N with live fraction L:

| Variant of L | Definition | Drives what |
|---|---|---|
| **L_end** | live(end-of-eval) / allocated(cumulative) | "what fraction of all my work persisted to the end" |
| **L_end_resident** | live(end-of-eval) / resident(end-of-eval) | "what fraction of current arena is still useful" |
| **L_peak** | live(at peak arena) / resident(at peak arena) | drives Cheney's peak penalty `N(1+L_peak)` |
| **L_trigger** | live(at GC-fire moment) / resident(at fire moment) | drives ACTUAL Cheney peak per cycle |
| **L_trough** | min(live(t) / resident(t)) | best-case L if trigger could find it |

**The L that matters for Cheney's 2× peak is L_trigger.** Every other variant is a proxy.

For mark-sweep, L_trigger drives reclamation efficiency (1 - L_trigger = fraction freed) but does NOT drive peak RSS (mark-sweep has no copy peak). This is the deep reason MS is robust.

## 3. What we've actually measured

### 3.1 Single L_end data points

Per [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md), end-of-eval walks via `NIX_V3_LIVE_TRACE=1`:

| Workload | Live (end) | Allocated (cumulative) | L_end |
|---|---|---|---|
| hello.drvPath | 285.67 MB | 525.07 MB | **0.54** |
| HNE | 617.26 MB | 1380.00 MB | **0.45** |
| synthetic genList | 352 B | 125.13 MB | ~0 |

These are computed against CUMULATIVE allocation, not resident. To get L_end_resident:

| Workload | Live (end) | Arena resident (end) | L_end_resident |
|---|---|---|---|
| hello.drvPath | 286 MB | 587 MB (per HNE_BUCKET_DECOMP) | **0.49** |
| HNE | 617 MB | 1594 MB | **0.39** |

**Two different "L_end" numbers in our docs depending on what's in the denominator.** Neither is L_trigger.

### 3.2 ONE mid-eval inference

From the Cheney falsification (commit `eda44711a`): gate-ON peak_rss = 1239 MB on hello. Subtracting non-arena state (~218 MB):

- arena peak during scavenge ≈ 1021 MB
- pre-scavenge arena ≈ 587 MB (assumed steady-state)
- TO-space filled = 1021 - 587 = 434 MB
- inferred L_trigger ≈ 434 / 587 ≈ **0.74**

But this is:
- n=1 measurement
- depends on WHEN Cheney fired (default trigger: 256 MB threshold; actual moment depends on adaptive)
- doesn't tell us L at OTHER moments
- could be anywhere in the L(t) distribution — peak, mean, trough, none of these

### 3.3 The methodology hole

Per [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 ("the measurement says A; therefore A is true" anti-pattern):

Our chain of reasoning has been:
1. End-of-eval L_end = 0.54 (measured)
2. Therefore v3's L is "structurally high" (inferred)
3. Therefore Cheney's 2× peak penalty will hit hard (predicted)
4. Cheney measured +486 MB regression (confirmed prediction)
5. Therefore non-moving is right (concluded)

Step 4 confirms step 3, but step 3 was derived from a *single sample* in step 1. **A confirming measurement at a single trigger moment doesn't validate the underlying "L is structurally high across all moments" claim — it validates "L is high at THIS moment."** The conclusion is correct *for the trigger policy tested*; whether it generalizes is unknown.

This is a §5.7-class methodology hole: we treated a measurement as a structural finding.

## 4. What we'd learn with time-series L(t)

A periodic live-walk during eval would tell us:

**A. L_peak / L_trough range.** If L_trough is 0.10 and L_peak is 0.80, the "v3 is structurally high-L" claim becomes "v3 has high L at SOME moments, low at others." This changes the GC design space.

**B. L distribution shape.** Bimodal (high-L during phase work, low-L between phases)? Smooth? Trending up over eval lifetime?

**C. Correlation with allocation rate.** Does L drop right after high-churn phases (mergeBindings bursts)? If yes, trigger-after-burst becomes a powerful policy.

**D. Workload variance.** Is HNE's L profile different from hello's? Cardano-node M5 different from both? If yes, one GC design may not fit all.

**E. The Cheney-revisit answer.** At which trigger moments would Cheney have actually worked? If there are ANY moments where L < 0.3, copying at those moments would be a win.

**F. Mark-sweep optimization.** Even though MS is L-insensitive for peak, it IS sensitive to L for SWEEP COST. Sweep over an arena where 90% is dead (L=0.1) populates lots of free-list entries; sweep at L=0.9 mostly does nothing useful. Knowing L distribution informs when to trigger MS for best wall efficiency.

## 5. Proposed measurement spike

### 5.1 Design

`NIX_V3_LIVE_TRACE_PERIODIC=<K>` — fire the existing `walkAllV3Roots` + live-walk every K MB of allocation. Default K=64 MB.

Output: CSV at end of eval:

```
alloc_offset_mb,resident_mb,live_mb,L_resident,L_cumulative,time_ms
64,68,52,0.76,0.81,12
128,135,89,0.66,0.70,18
192,201,134,0.67,0.70,24
...
587,587,286,0.49,0.54,212
```

Plus aggregate at end:
- L_min, L_max, L_median, L_mean across all samples
- Correlation with allocation rate (∂L/∂alloc)

### 5.2 Effort

~1 day. ~150 LoC over `live_trace.cc` infrastructure. No design risk (reuses existing walker).

Gate default-OFF: each periodic walk adds time proportional to live-set size. With K=64 MB and ~10 samples per hello.drvPath eval, total wall overhead is ~5-15% — acceptable for measurement, not for production.

### 5.3 Pre-committed acceptance per measure-twice §3

- [ ] CSV produced with ≥10 samples per workload
- [ ] L_min, L_max, L_median computed and reported
- [ ] Run on hello.drvPath + HNE + M5
- [ ] Comparison with end-of-eval L_end (cross-validation: last periodic sample should approximate L_end ± 5%)

### 5.4 What this measurement RESOLVES

| Open question | Closed if measurement shows |
|---|---|
| "Is non-moving right for v3?" | L_distribution narrow + always high → YES strong; bimodal → conditional |
| "Could a different trigger policy save Cheney?" | L_trough < 0.3 → YES; L_trough ≥ 0.5 → NO |
| "Should mark-sweep trigger be opportunistic?" | High σ(L) → YES, trigger at low-L; low σ → trigger by threshold |
| "Do workloads differ structurally?" | HNE/hello/M5 L curves differ → YES (per-workload tuning); identical → NO |
| "Is the 'L=0.5-0.75' literature comparison fair?" | Per-cycle L distribution matches → YES; differs → reconsider |

### 5.5 What this measurement does NOT resolve

- Cache eviction yield (orthogonal lever; separate measurement)
- String dedup yield (orthogonal; per [`STRING_DEDUP_AUDIT_2026-05-28.md`](STRING_DEDUP_AUDIT_2026-05-28.md))
- Whether mark-sweep itself will pass its SHIP gate (still need Phase 4 measurement)
- M5 absolute peak (this just refines understanding of arena fraction)

## 6. What this means for the current MS work

**Don't abort.** Mark-sweep is still the safer choice given what we know:
- L_trigger=0.74 was measured directly (Cheney result confirms)
- MS is L-insensitive for peak; robust to L distribution variance
- The trigger-policy variant of "could Cheney work" is exploration, not retreat

**But:** before committing to the NEXT major GC design (post-MS, whatever that becomes), run this spike. Future decisions should be made against L distribution, not L_end_point.

**Sequencing:**
- Phase 4 SHIP-gate measurement for MS: needs to happen this week regardless
- L(t) measurement spike: ~1 day, can run in parallel
- If MS SHIP-gate passes: spike informs Phase 6+ tuning (trigger policy)
- If MS SHIP-gate falsifies: spike data is critical input for the next design

## 7. Methodology lesson — name it

This is the third specific instance of a pattern worth codifying:

| Past instance | What was extrapolated from | What was missing |
|---|---|---|
| 919 MB M5 capability claim | Single benchmark measurement | Same-host bisection ([[same-host-bisect]]) |
| HNE head-5 counter | Early VM_STATS dumps | Cumulative counter awareness ([[head-5-counter-trap]]) |
| **L is "structurally high"** | **End-of-eval single sample** | **Time-series distribution** |

These are all "treated a single measurement as a structural claim." The general rule:

> Before concluding "X is structurally true of the system," verify the measurement supports a distribution claim, not a single-sample claim. Single samples support "X was true at the moment we measured"; structural claims require distribution evidence.

This belongs in measure-twice §5.7 as a sub-case. The pattern is "moment-vs-distribution conflation" — distinct from "the measurement says A; therefore A is true" but related. May propose adding §5.8 in measure-twice.

## 8. Honest limits of this proposal

- The periodic walk itself perturbs the eval (allocations during walk; cache pressure from bitmap scan). Mitigation: gate default-OFF; treat measurements as approximate.
- K=64 MB sampling resolution may miss short-lived peaks/troughs. Lower K = more samples but more perturbation. Tunable.
- L_trigger is what GC sees; periodic L is what we MEASURE — not exactly the same (a periodic walk fires deterministically at K-boundaries, not at GC's actual trigger logic). Useful as proxy; not literal.
- Cardano-node M5 measurement requires the actual flake checkout + may be slow (30+ s wall × 1.1× overhead × 3 runs). Plan ~15 min of compute.
- The "trigger policy could save Cheney" thesis is interesting but should NOT delay the current MS implementation. If MS ships, the Cheney revisit is post-hoc curiosity.
- Per [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md): this spike has a clear pre-committed deliverable; doesn't risk turning into open-ended exploration.

## 9. Cross-references

- [`LIVE_FRACTION_SPIKE_2026-05-27.md`](LIVE_FRACTION_SPIKE_2026-05-27.md) — end-of-eval data this spike extends
- [`STAGE_6_CHENEY_FALSIFIED_2026-05-27.md`](STAGE_6_CHENEY_FALSIFIED_2026-05-27.md) — the inference we're triangulating against
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §2.3 — "v3's L is structurally high" claim that this spike will validate or refine
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — denominator data for L_resident
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §5.7 — moment-vs-structural anti-pattern (§5.8 candidate)
- [`STRING_DEDUP_AUDIT_2026-05-28.md`](STRING_DEDUP_AUDIT_2026-05-28.md) — sibling spike (orthogonal measurement gap)
- `live_trace.cc` (commit `f3491859f` + `5d2b194cb`) — infrastructure this spike extends
- Memory: [[same-host-bisect]], [[head-5-counter-trap]] — prior instances of the moment-vs-distribution pattern

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
