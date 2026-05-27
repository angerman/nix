# Phase E v0.2 stress-mode resolution — 1-3 d session handoff design

**Status**: Ready-to-execute design for a focused future session.
**Estimated effort**: 1-3 days (per CLAUDE.md §6.3 + GC_VS_TW_ANALYSIS
§4.2).  Tight if 9958eb4a5 fully closed the bug; longer if a
remaining root-walk gap surfaces.
**User framing**: this is the "architecture alignment" track —
unblocks nursery default-on, which obviates the fakeClo dead-pool
lever (144 MB on HNE per T1_3_CLOSURES_ATTR), AND aligns with the
Stage 6 production design's architectural direction.
**Prerequisite chain**: none.  Standalone task that unblocks
Stage 6 + the 144 MB Closures lever.

## 1. The goal

Resolve the documented Phase E v0.2 stress-mode missed-root concern
so that `NIX_V3_NURSERY=1` (Cheney semi-space + age-based promotion)
can flip to default-on.

Once nursery is default-on:
* fakeClo allocations land in nursery → scavenged on each cycle
  → tenured pollution stops → recovers ~144 MB on HNE without
  reviving the dead pool
* Generational reclamation works for all transient v3 allocations
  (transient Bindings, Thunks, etc.) — composes with Stage 6's
  tenured precise GC
* Reduces working set: nursery (~32 MiB) + tenured promoted set
  vs current entire-arena monotonic growth

## 2. What's already landed

Per `9958eb4a5` (#738 v0.2 fix, commit 2026-05-21):

> "force-walk tenured under Phase E (close 1 MB stress missed-root).
>  The known limitation flagged in c4be4cfbc is now closed.  At
>  NIX_V3_NURSERY_SIZE=1 MB + Phase E + scavenge stress,
>  hello.drvPath etc. previously hit 'stale callee' — a tenured
>  cell holding a stale Closure pointer."

Root cause was Phase D Step 7's "trust the dirty list" optimization
(skip walking tenured containers reached via fwdX) combined with
Phase E's two-region semantics — the scavenger itself creates new
T→active-S edges that the mutator-side barriers don't pre-register.

`9958eb4a5` made the Phase E path force-walk those tenured edges.
The 1 MB stress test passes.

## 3. What remains open

Per CLAUDE.md §6.3 + GC_VS_TW_ANALYSIS §4.2 + AR7 in NEXT_STEPS_2026-
05-25 §AR (architectural risks), Phase E v0.2 is "still opt-in" not
because of this specific known bug but because:

1. **Real-workload stress validation hasn't happened.**  The 1 MB
   stress test that exercised the bug is a TINY nursery + aggressive
   scavenge mode.  Real production workloads (HNE, cardano-node M5,
   nixpkgs.firefox) haven't been validated under `NIX_V3_GC_STRESS=1000`
   per the AR7 ship criterion.
2. **Phase E mortality measurement hasn't published.**  Per
   GC_VS_TW_ANALYSIS §4.1 the v0.2 two-region delta wasn't measured
   — without that, the wall-cost case for default-on is unproven.
3. **AR7 risk acknowledgement**: shipping default-on without
   real-workload stress evidence means production hits could
   silently corrupt under load.

## 4. The mechanical path (1-3 d)

### Day 1 — Real-workload stress validation (~6 h)

**Automated path** (recommended): use the harness script that
mechanises all 3 workloads and produces a PASS / FAIL verdict
against the SHIP gate:

```bash
bench/phase-e-stress-validate.sh                # all 3 workloads under stress
bench/phase-e-stress-validate.sh --workload hello   # single workload
bench/phase-e-stress-validate.sh --no-stress    # baseline comparison
```

Outputs structured per-workload report:
```
  Workload    Verdict  Note
  hello       PASS     byte-identical to TW; 0 BRUTE/AUDIT lines; rc=0
  firefox     PASS     ...
  hne         PASS     ...

  SHIP gate: MET
```

Exit code 0 means all SHIP gates met; 1 means at least one failure;
2 means harness error.  Logs at `/tmp/phase-e-validate-LOGS/`
including per-workload `.out` (stdout) + `.err` (stderr) + the TW
baseline `.tw.out` for diff inspection.

**Manual path** (if needed for debugging a specific case):

```bash
NIX_V3_NURSERY=1 NIX_V3_PHASE_E=1 NIX_V3_GC_STRESS=1000 \
  NIX_VM_STATS=1 NIX_V3_DIRECT_EVAL=1 \
  NIX_V3_MAX_WALL_TIME=600s NIX_V3_MAX_HEAP=4G \
  ./build/src/nix/nix --extra-experimental-features nix-command \
  eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'
```

For each run, the script verifies:
* **Output byte-identical** to TW baseline (canonical correctness
  gate; any divergence is a missed-root).
* **No `v3 SCAVENGE BRUTE`** stderr line (brute-audit canary).
* **No `v3 SCAVENGE AUDIT: nursery ... reachable via`** stderr line
  (specific missed-root detector).
* **No SIGSEGV / SIGBUS** crash (rc == 0).

If any workload fails: that's the bug to diagnose.

### Day 2 — Mortality measurement (per GC_VS_TW_ANALYSIS §4.1)

Capture, on hello.drvPath + HNE + cardano-node M5:

```bash
NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_PHASE_E=1 \
  NIX_VM_STATS=1 ...
```

Extract from stderr:
* `scavenges=N` — total scavenge count
* `dead bytes / scavenge` — mortality rate
* `promotions to tenured` — survival rate
* `wall delta` vs default-off baseline
* `peak_rss delta` vs default-off baseline

**Pre-committed decision rules** (per GC_VS_TW_ANALYSIS §4.1):
* mortality ≥ 50 % AND wall delta ≤ 5 % regression: **flip
  default-on**
* mortality 30-50 %: tune trigger threshold or smaller nursery
  size; re-measure
* mortality < 30 %: kill nursery for this workload class
* wall regression > 10 %: investigate per-scavenge cost before
  any flip

### Day 3 (if Day 1+2 pass) — Default-on flip (~0.5 d)

Per GC_VS_TW_ANALYSIS §4.3:

1. Change `NIX_V3_NURSERY` to default-on (mirrors Phase D Step 11
   `c0911aee6` retirement pattern).
2. Add `NIX_V3_NO_NURSERY` opt-out for one release for compat.
3. Update tests / `lint-no-inline-getenv.sh` / CLAUDE.md §6.3 to
   reflect the new default.
4. Update SESSION_ARC + memory entry pointing at the closed AR7
   risk.

### Day 3 (if Day 1 fails) — Diagnose the remaining gap

If a real workload fails:

1. Identify the failure signature (SIGSEGV at stale Closure?
   BRUTE-AUDIT hit?  Output divergence?).
2. Use `V3_DBG_NURSERY_BRUTE=1` + `V3_DBG_NURSERY_AUDIT=1` to
   localize the missed root.
3. Cross-reference against `GC_AUDIT_ROUND_2_2026-05-21.md`'s
   cleared findings + 9958eb4a5's fix scope.
4. The likely gaps:
   * Another `Alloc::*` site allocates in tenured but stores a
     nursery payload without barriering (audit ASF7 / Phase D
     Step 12-style review)
   * A walker site (gc.cc Scavenger) misses a field in a struct
     that grew since 9958eb4a5
   * Phase E's post-construct barrier elision misses a specific
     ordering pattern

Each is a 1-day debug task with clear methodology in the existing
`GC_AUDIT_ROUND_2_2026-05-21.md`.

## 5. Pre-committed SHIP gates

Per `[[memory-first-class]]`:

### Correctness gates (all must pass)

* `all-v3-tests --quick` 6/6 PASS under default-on nursery
* `all-v3-tests --core` 15/15 PASS under default-on nursery
* `all-v3-tests --brute` 11/11 PASS under default-on nursery
* HNE + hello.drvPath + firefox.drvPath byte-identical to TW
* `NIX_V3_GC_STRESS=1000` on all 3 workloads: no crash, no audit
  warnings, output byte-identical

### Performance gates

* Wall regression ≤ 5 % on each workload (rolling hyperfine n=10)
* Peak RSS delta: any reduction OR ≤ 50 MB increase
* gc_count: ≥ 5 per workload (showing nursery scavenging is firing)

### Failure-to-ship triggers

* Any correctness gate fails: FALSIFY this commit; investigate
* Wall regression > 10 %: investigate per-scavenge cost
* Mortality < 30 %: investigate generational hypothesis

## 6. What unblocks after this lands

| Lever (post-Phase-E-default-on) | Yield | Effort | Status            |
|---------------------------------|-------|--------|-------------------|
| Closures fakeClo overhead       | 144 MB on HNE | (auto)     | Closed by nursery |
| Transient Bindings via Phase E  | ?     | (auto)     | Test on HNE       |
| Stage 6 production precise GC   | 239-797 MB | 2-3 wk  | Phase E aligns    |

The Stage 6 design (`STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md`)
explicitly references "the gc.cc Scavenger DOES THIS for the nursery"
as Option A's pattern.  If nursery is default-on, Stage 6's safe-point
model + RootVisitor pattern + Cheney semi-space precedent are ALL
already exercised in production.

This is why the user framed it as "architecture alignment" — it's
the architectural pre-step that makes Stage 6's mechanics already
familiar to the codebase.

## 7. Pitfalls to avoid (per this session's findings)

1. **Don't flip default-on without real-workload stress evidence.**
   The 1 MB stress test was a tiny + aggressive mode; production
   hits are different shape.  Per AR7: real-workload stress test
   is the ship criterion.

2. **Don't assume mortality without measuring.**  GC_VS_TW_ANALYSIS
   notes nursery overhead is 2× space.  If real workloads have
   <30 % mortality, nursery is pure cost.

3. **Don't repeat the Phase D Step 12 mistake of retiring before
   replacement is default-on.**  Phase D Step 12 retired the
   fakeClo pool assuming nursery would soon be default-on;
   nursery is still opt-in 6 days later → fakeClo is dead code
   with 144 MB leak.  Lesson: retirement orders matter.  After
   Phase E flips default-on, audit for similar half-flipped state.

4. **Don't skip the `--brute` suite.**  It's the canonical
   missed-root canary built for exactly this scenario.  Per
   `BRUTE/AUDIT in CI 2026-05-21` memo, BRUTE classifies hits as
   LIVE (real missed root) vs DEAD (arena bloat from Boehm
   conservative pinning, harmless).  All-LIVE-counts must be 0.

5. **Don't add NIX_V3_NO_NURSERY opt-out without a retirement
   criterion.**  Per CLAUDE.md Critical Constraint 4 + Rule 0.
   The retirement criterion: when default-on has soaked 2 weeks
   of nixpkgs eval + clean cardano-node M5 measurement, the
   opt-out is removed.

## 8. Cross-references

* `GC_VS_TW_ANALYSIS_2026-05-23.md` §4.2 — original framing of
  this work
* `CLAUDE.md` §6.3 — operational gate
* `NEXT_STEPS_2026-05-25.md` §AR7 — risk acknowledgement +
  ship criterion
* `T1_3_CLOSURES_ATTR_2026-05-27.md` — fakeClo dead-pool finding
  (the 144 MB this unblocks)
* `STAGE_6_PRECISE_GC_DESIGN_2026-05-27.md` — the architectural
  alignment story; semi-space precedent
* `GC_AUDIT_ROUND_2_2026-05-21.md` — methodology for diagnosing
  remaining root-walk gaps
* commit `9958eb4a5` — the v0.2 force-walk-tenured fix already
  landed
* commit `c0911aee6` — Phase D Step 11 default-on (mirrors the
  retirement pattern this commit will follow)
* `[[memory-first-class]]` — SHIP gate framework
* `[[falsification-rule]]` — if any gate fails, FALSIFY this commit

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
