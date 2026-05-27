# Phase E v0.2 Day-1 stress validation — SHIP-GATE MET

**Date**: 2026-05-27
**Tool**: `bench/phase-e-stress-validate.sh` (commit `d77be72ab`)
**Per**: `PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` §4 Day 1
**Outcome**: ✓ SHIP gate MET on all 3 anchor workloads under
  `NIX_V3_GC_STRESS=1000` — Phase E v0.2 is correctness-clean on
  HNE-class + nixpkgs-class real workloads.

## Result table

```
  Workload    Verdict  Note
  hello       PASS     byte-identical to TW; 0 BRUTE/AUDIT lines; rc=0
  firefox     PASS     byte-identical to TW; 0 BRUTE/AUDIT lines; rc=0
  hne         PASS     byte-identical to TW; 0 BRUTE/AUDIT lines; rc=0

  SHIP gate: MET
```

## Conditions verified per workload

For each of hello.drvPath / firefox.drvPath / HNE
(`packages.x86_64-linux.hello.drvPath`):

* Eval completed under `NIX_V3_NURSERY=1 NIX_V3_PHASE_E=1
  NIX_V3_GC_STRESS=1000` without SIGSEGV / SIGBUS (exit code 0)
* stdout BYTE-IDENTICAL to TW baseline (no v3 output divergence)
* ZERO `v3 SCAVENGE BRUTE` stderr lines
* ZERO `v3 SCAVENGE AUDIT: nursery ... reachable via` stderr lines

The `--quick` + `--core` test suites have separately been clean
throughout this session (commits 1-31, validated repeatedly).

## What this closes

AR7 (`NEXT_STEPS_2026-05-25.md` §AR risk-table): "Phase E v0.2
known stress-mode missed-root" — the documented risk that
flipping nursery default-on could trip a missed-root signature on
real workloads.  **The Day-1 stress validation passes on the 3
anchor workloads → AR7 is empirically closed for production-class
patterns.**

## What remains before flipping default-on

Per `PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` §4 Day-2:

* **Mortality measurement** (per GC_VS_TW_ANALYSIS §4.1):
  * scavenge count + interval per workload
  * mortality rate per scavenge (dead vs survivor bytes)
  * total tenured promotions
  * wall delta vs default-off baseline
  * peak RSS delta vs default-off baseline

* **Pre-committed Day-2 decision rules**:
  * mortality ≥ 50 % AND wall delta ≤ 5 % regression: **flip
    default-on**
  * mortality 30-50 %: tune trigger threshold; re-measure
  * mortality < 30 %: kill nursery for this workload class
  * wall regression > 10 %: investigate per-scavenge cost

* **Flip mechanics** (per §4.3):
  * Edit 3 gate read sites:
    - `nursery.hh:428` — `NIX_V3_NURSERY` semantics flip
    - `barrier.cc:70` — same
    - `vm.cc:2535-2536` — same
  * Add `NIX_V3_NO_NURSERY` opt-out gate
  * Update `lint-no-inline-getenv.sh` whitelist
  * Update CLAUDE.md §6.3 marker
  * Run `--quick` + `--core` + `--brute` under flipped state
  * Run nixpkgs hello / firefox + HNE byte-equality post-flip

## Cross-references

* `PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` — full handoff design
* `bench/phase-e-stress-validate.sh` — the harness that produced
  this result
* `lode/GC_VS_TW_ANALYSIS_2026-05-23.md` §4 — mechanical path
* `NEXT_STEPS_2026-05-25.md` AR7 — risk now empirically closed
* `T1_3_CLOSURES_ATTR_2026-05-27.md` — the 144 MB fakeClo lever
  this flip will close automatically
* commit `9958eb4a5` — the Phase E v0.2 force-walk-tenured fix
  that made this Day-1 pass possible
* `[[memory-first-class]]` — SHIP gate framework
* `[[falsification-rule]]` — Day-1 PASS is empirical evidence;
  Day-2 + flip remain to falsify against

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
