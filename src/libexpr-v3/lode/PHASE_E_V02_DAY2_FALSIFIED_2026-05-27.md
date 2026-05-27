# Phase E v0.2 Day-2 mortality measurement — DEFAULT-ON FALSIFIED

**Date**: 2026-05-27
**Per**: `PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` §4 Day 2 +
  `GC_VS_TW_ANALYSIS_2026-05-23.md` §4.1 decision rules
**Outcome**: ✗ **Day-2 falsifies the default-on flip on the 3
  anchor workloads.**  Day-1 stress validation PASSED (correctness-
  clean per `PHASE_E_V02_DAY1_PASS_2026-05-27.md`), but Day-2
  mortality + peak-RSS measurements fail the pre-committed SHIP
  gate.

## Headline

| Workload      | mortality | scavenges | RSS baseline | RSS w/Phase E | RSS Δ  | Verdict |
|---------------|-----------|-----------|--------------|---------------|--------|---------|
| hello.drvPath |  56.9 %   |     1     |   753.4 MB   |    882.6 MB   | +129 MB (+17 %) | **FAIL** |
| HNE           |  38.3 %   |     4     |  2565.2 MB   |   2816.8 MB   | +252 MB (+10 %) | **FAIL** |

Tuning attempt: NIX_V3_NURSERY_SIZE=8 (8 MB Y + survivors):
| Workload      | mortality | RSS Δ vs baseline | Verdict |
|---------------|-----------|-------------------|---------|
| hello.drvPath |  28.3 %   |  +79 MB (+10 %)   | **FAIL** (mortality < 30 %) |

## Pre-committed decision rules

Per `GC_VS_TW_ANALYSIS_2026-05-23.md` §4.1 + `[[memory-first-class]]`:

* **SHIP**: mortality ≥ 50 % AND wall Δ ≤ 5 % regression AND
  RSS Δ ≤ 50 MB increase OR any reduction
* **TUNE**: mortality 30-50 % — adjust nursery size, re-measure
* **KILL**: mortality < 30 % — workload class doesn't benefit
* **INVESTIGATE**: wall regression > 10 %

## What the data says

### hello.drvPath at 32 MB nursery (default)

* Mortality 56.9 % meets the SHIP rule's mortality clause.
* RSS Δ +129 MB FAILS the ≤ 50 MB increase clause.
* The nursery itself is 100.7 MB Y+S overhead.
* The arena dropped 33.6 MB (some allocs landed in nursery) but
  the nursery's own footprint exceeds the savings.

### HNE at 32 MB nursery

* Mortality 38.3 % is in TUNE range.
* RSS Δ +252 MB also FAILS the SHIP rule.
* Same pattern as hello: nursery overhead exceeds scavenge benefit
  on this workload pattern.

### hello.drvPath at 8 MB nursery (TUNE attempt)

* Mortality dropped to 28.3 % — **below the 30 % KILL threshold**.
* Smaller nursery means fewer scavenges (2 vs 1) because the
  trigger fires less.  Less reclamation pressure → lower mortality.
* RSS Δ +79 MB — still over budget.

## Why the SHIP gate isn't met

The Cheney 2× space overhead is FIXED at ~3× the nursery-size
parameter:
* 32 MB nursery → ~100 MB total (Y + double-buffered survivor)
* 8 MB nursery → ~25 MB total

The scavenge SAVING is bounded by mortality × allocation rate.
On these workloads, the savings DON'T exceed the always-resident
overhead.  The arithmetic:

```
SHIP-GATE pass conditions:
  required savings > total nursery overhead
  total nursery overhead = 3 × N_size (Y + active-S + backup-S)
  savings ≈ mortality × peak-arena-without-nursery
```

For hello at 32 MB nursery:
* overhead ≈ 100 MB
* savings ≈ 56.9 % × ~587 MB = ~334 MB IF nursery captured all
  short-lived allocs.  Observed savings: 33.6 MB.

The 10× gap (334 ideal vs 33.6 actual) means **most short-lived
allocations are NOT going through the nursery on this workload**.
Either:
1. Allocations are large enough to bypass the nursery (huge
   Bindings > kHugeCutoff)
2. The Phase A allocator's nurseryOrArena() decision routes
   long-lived shapes (Bindings entries, etc.) to arena directly
3. Mortality is measured on the SUBSET that goes through nursery,
   not on TOTAL allocations

Likely (3) — mortality of those that DO enter nursery is high
(57 %), but most allocations bypass nursery.

## What this kills (Rule 0)

1. **The "Phase E default-on automatically closes the 144 MB
   fakeClo lever" hypothesis** (per T1_3_CLOSURES_ATTR's
   architectural-path option).  Phase E default-on doesn't ship
   per current measurement, so fakeClo isn't auto-closed.

2. **The "architecture alignment unlocks Stage 6 prereq" plan**
   per PHASE_E_V02_STRESS_DESIGN.  Stage 6 still depends on
   precise root infrastructure (Stages 1+3+5 ✓) but the SAME
   PATTERN AS PRODUCTION assertion isn't validated until Phase
   E is either tuned + default-on OR alternative path found.

## Recommended next steps

Per the design's TUNE clause, two architecturally-aligned
iteration paths:

### Path A — Tune scavenge trigger (1-2 d)

The scavenge currently fires when nursery is FULL.  More frequent
scavenge (e.g., every N MB allocated) might increase observed
mortality.  Test:
* Lower the nursery-fill trigger
* Use NIX_V3_GC_STRESS=10000 as a proxy for "scavenge more often"
* If mortality rises above 50 % at acceptable wall, re-measure
  RSS budget

### Path B — Audit what bypasses nursery (~1 d) — DONE 2026-05-27

Instrumentation landed: `v3-direct nursery routing` line in
NIX_VM_STATS dump (run.cc commit 2026-05-27 evening).  Counts
`tryAlloc` hits (allocation landed in nursery) vs `overflowCount`
(nursery was full → fell through to arena).

**Measurement**:

| Workload      | nursery hits | nursery misses | hit rate | nursery allocBytes |
|---------------|--------------|----------------|----------|--------------------|
| hello.drvPath |    481,557   |    1,065,184   |  31.1 %  |    33.6 MB         |
| HNE           |  1,721,818   |    7,269,995   |  19.1 %  |   126.4 MB         |

**Result**: nursery is STRUCTURALLY UNDERSIZED on these workloads.
The 38.3 % mortality on HNE is measured on the 19 % of allocations
that actually entered the nursery; 81 % bypass to arena because
the nursery is FULL.  The 10× ideal-vs-observed savings gap from
the Day-2 banner is EXPLAINED by the bypass rate.

**Implication**: the real lever is **increasing the hit rate**,
not increasing per-scavenge mortality.  Three sub-paths:

* Larger nursery — more space before overflow → higher hit rate.
  Already tested at 8 MB (hello): mortality dropped to 28.3 %
  → smaller nursery is the WRONG direction.  Larger nursery is
  worth a measurement spike, but the to-space overhead scales
  with size (Cheney 2×) so RSS budget may bind.
* More frequent scavenge — scavenge BEFORE nursery fills →
  reclaim space → next allocs land in nursery → cumulative hit
  rate rises.  This is the right architectural lever IF the
  per-scavenge cost stays bounded.
* Adaptive scavenge trigger — fire at X% full instead of 100%
  full.  X = 50-75% is a reasonable starting point.

### EXPLICITLY NOT RECOMMENDED — fakeClo pool revival

An earlier draft of this doc proposed reviving the
`Alloc::allocFakeClo` / `recycleFakeClo` infrastructure as a
short-term Path C.  **User pushback 2026-05-27 (correctly)
rejected this**: reviving a hand-rolled pool reverses Phase D
Step 12's intentional architectural decision to let generational
reclamation (nursery) handle fakeClo lifecycle.  Going back to
manual pooling is the wrong direction.

The correct framing: the 144 MB fakeClo overhead is a SYMPTOM of
Phase E v0.2 not being default-on in production.  The CURE is
making Phase E v0.2 ship-ready (Paths A + B above), NOT routing
around it with hand-rolled pools.

If Phase E v0.2 can't be made to ship at acceptable wall+RSS
budgets after Paths A + B, the next architectural step is Stage 6
production precise GC — NOT pool revival.

## What stays valid

* **AR7 is empirically closed** for correctness — Phase E v0.2
  stress validation PASSED on all 3 workloads (Day-1).
* Phase E v0.2 infrastructure remains opt-in (`NIX_V3_NURSERY=1`)
  for users who want it.
* Stage 6 design's prereq #4 ("Phase E default-on") needs
  recasting as either "Phase E *tuned to ship-gate*" OR
  "alternative architectural alignment via fakeClo revival."

## Cross-references

* `lode/PHASE_E_V02_DAY1_PASS_2026-05-27.md` — Day-1 stress PASS
* `lode/PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` — original handoff
* `lode/T1_3_CLOSURES_ATTR_2026-05-27.md` — fakeClo lever (144 MB)
* `lode/GC_VS_TW_ANALYSIS_2026-05-23.md` §4 — decision rules
* `[[falsification-rule]]` — Day-2 falsifies the default-on plan
* `[[measure-twice-cut-once]]` — pre-committed thresholds applied
* `[[memory-first-class]]` — SHIP gate is FIRST-CLASS; this
  commit honors it by NOT flipping

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
