# Session arc 2026-05-29 — what landed, what's falsified, what's next

**Window:** 2026-05-29 multi-turn session, continuing from prior context summary
**Aggregate:** 16 substantive commits + 3 new memory entries + DIAG suite (4 builds + Phase 2) + 6 strategic docs in `lode/`
**Validation throughout:** `all-v3-tests --quick` 6/6, `--core` 15/15, hello byte-identical to TW
**Headline:** chased Week 2 capWiths → discovered methodology errors → rolled back App3 (3× regression) → re-validated Week 1 → pivoted to diagnostic infrastructure per user directive → built distribution-shaped DIAG suite → discovered retention is CONCENTRATED (not dispersed), reopening BiBOP-lite

---

## 1. The narrative arc

The session started in "lever-chasing" mode (continue Day 13-15 capWiths from prior context) and pivoted twice:

* **First pivot** (mid-session): discovered Tag::App3 caused a NET REGRESSION (3× allocations on HNE) + bench script defaulted to stale build/ binary.  Rolled back App3 + fixed bench script + re-validated Week 1.
* **Second pivot** (later): user directed measurement-infrastructure-first.  Built the DIAG suite per `DIAGNOSTIC_AUDIT_2026-05-29 §6`.
* **End-state:** DIAG data reveals retention is concentrated at `pkgs/top-level/all-packages.nix:9112:3` (62.8% of live bytes on HNE), reopening BiBOP-lite + late-firing trigger policy as candidate levers.

## 2. What landed (16 commits, chronological)

| # | Commit | What |
|---|---|---|
|  1 | `25bf129af` | Day 13-15: capWiths singleton interning (gated `NIX_V3_NO_CAPWITHS_INTERN=1`) |
|  2 | `3c7a5204c` | Day 13-15 measurement + LANDED doc — capWiths arena PASS (-16.8 MB HNE deterministic), peak noise-limited |
|  3 | `a9912f0fb` | **ROLLBACK Day 9-11 Tag::App3** at primops.cc sites — A3 caused 3× allocation explosion / +1.6 GB arena / 4× wall on HNE due to lost App-result memoization (#696).  Site-level revert only; dispatch infrastructure left as dead code |
|  4 | `9d8770f7f` | **Week 1 retrospective** with N=10 measurements on builddir/ — bundle SHIPS for real (HNE -105 MB peak / -117 MB arena det.; M5 -219 MB peak / -822 MB arena det.) |
|  5 | `74a4eeb26` | Amend Day 9-11 + Day 12 LANDED docs with retrospective findings (ROLLED BACK / SUPERSEDED tags) |
|  6 | `633d40f87` | `bench/measure-peak-noise-floor.sh` auto-picks newer of `build/` vs `builddir/`; warns if HEAD > binary mtime |
|  7 | `e983919f7` | Day 2 re-measure: cache eviction NULL confirmed on M5 (Δpeak -107 MB at 0.19σ pooled) |
|  8 | `fdba6a951` | **PIVOT** from Immix re-eval to diagnostic builds per user "don't rely on OS RSS" directive + DIAGNOSTIC_AUDIT §9 |
|  9 | `fb6ae0b72` | **DIAG-1**: per-cycle GC CSV via `NIX_V3_GC_CYCLE_CSV=path`.  First measurement on HNE: markMs:sweepMs = 7:1 → F2 verdict (sweep 40-54%) was projection bug; actual sweep 14% |
| 10 | `b6423268b` | **DIAG-3**: per-Tag L(t) extension to periodic CSV.  HNE shows Thunks grow 3× during eval (new finding) |
| 11 | `36aa29916` | **DIAG-4**: per-phase arena bytes via `NIX_VM_STATS` — HNE run-phase = 100% of arena growth |
| 12 | `d0f74ecf2` | **DIAG-2 Phase 1**: per-PosIdx live-bytes top-20.  82.8% unknown bucket (coverage gap) on HNE |
| 13 | `e71bf9e7a` | DIAG suite summary doc |
| 14 | `4aadeb8d0` | **DIAG-2 Phase 2**: per-entry pos fallback drops unknown 82.8% → 9.3% |
| 15 | `b580c2792` | Strategic synthesis: **retention CONCENTRATED (62.8% at all-packages.nix:9112) reopens BiBOP-lite + late-firing trigger** |
| 16 | (this commit) | Session arc + memory updates |

Plus follow-on commits from earlier in this session (pre-context-summary): `bd4ed1edb` / `8b30fd38f` / `356cae026` (Day 12 N=10) and `6d7bf236c` (fakeClo retention amendment).

## 3. New memory entries

* `[[bench-binary-fingerprint]]` — bench script default NIX_BIN can be stale; always verify mtime > HEAD commit time
* `[[exit-week1-revalidated]]` — Week 1 bundle ships honestly (after App3 rollback); M5 trim-2 mean borderline
* Updated `[[app3-mapattrs]]` to mark ROLLED BACK + explain memo regression
* Updated `[[peak-vs-alloc-distinction]]` to add "mid-lifetime" category for memoization-dependent allocations

## 4. Key findings (load-bearing for future sessions)

### 4.1 Methodology errors (resolved)

* **`build/src/nix/nix` was stale** at 18:36 May 28 — older than Day 6-8 fakeClo wire-back at 18:48.  All bench measurements before today's retrospective ran against pre-Week-1 binary.  Fixed by `633d40f87` bench-script auto-pick.
* **Tag::App3 disabled App-result memoization** (#696).  Day 9-11 "peak-neutral retain" decision was measured against the stale binary that didn't actually contain App3.  Honest re-measurement showed +1.6 GB arena regression.

### 4.2 GC verdict re-grading (DIAG-1)

* **F2 (flat MS sweep 40-54% wall) was a projection bug**, not a measurement.  `live_trace.cc:818` set `projectedSweepMs = markMs` (verified per audit).  DIAG-1's REAL per-cycle CSV shows mark:sweep = 7:1 — sweep is 14% of GC time.
* The Immix pivot in `GC_DECISION_2026-05-29.md` rested on the wrong number.  **Flat MS may be rescuable with the quadratic `clearCellStartBitFor` fix** (audit §6.5) + page-aware allocator.

### 4.3 Retention is CONCENTRATED (DIAG-2 Phase 2)

| Source position | Live (MB) | % | Bindings (N) |
|---|---:|---:|---:|
| `pkgs/top-level/all-packages.nix:9112:3` | 311.46 | **62.8%** | 318,931 |
| _posHandle=0 (residual)_ | 45.85 | 9.3% | 18,264 |
| `lib/customisation.nix:398:48` | 16.59 | 3.3% | 16,988 |
| `pkgs/top-level/python-packages.nix:11492:3` | 5.95 | 1.2% | 6,096 |
| (top-20 covers ~81%) |  |  |  |

**One source position retains 62.8% of live bytes.**  319 K callPackage-style attrsets at the nixpkgs root.  Reverses the audit's tentative "dispersed → BiBOP not viable" framing.

### 4.4 Honest M5 watchdog status

Day 12 verdict claimed "trim-2 mean 50 MB under watchdog."  Re-validation on builddir/ shows M5 mean = 4343 ± 95 MB vs 4096 target — **247 MB OVER** on this morning's measurement; afternoon environmental σ moved to 308 → wider envelope.  Honest: edge-of-watchdog, not safely under.

## 5. What's next (pre-committed options)

Per `DIAG_CONCENTRATED_RETENTION_2026-05-29 §6`, four candidate next moves:

1. **End-of-eval GC spike** (~half day) — RECOMMENDED.  Add `NIX_V3_END_OF_EVAL_GC=1` gate; fire one MS pass after `run()` returns; measure HNE Δpeak.  Pre-committed acceptance:
   * ≥200 MB drop → trigger-policy ships (1-2 wk project)
   * 50-200 MB → stack with BiBOP-lite
   * <50 MB → revisit timing assumption
   * **Blocker:** Immix OP_REC_BINDING_SLOT_REF crash on HNE — debug needed (~1 day) before running this spike, since runMajorMarkSweep currently has the same correctness bug under NIX_V3_MAJOR_GC=1.

2. **DIAG-5: fix flat MS `clearCellStartBitFor` quadratic** (~1 day, audit §6.5) — combined with DIAG-1 data may rescue flat MS as a viable GC family without 4-6 wk Immix detour.

3. **Non-arena attribution** (~1-2 days) — mallinfo / malloc_zone_statistics for the "elsewhere" bucket the user flagged as "ominous."  HNE elsewhere = 273 MB single-shot, 454 MB Day 13-15, 561 MB Day 12.  Variable.  Phase decomposition would attribute.

4. **Immix OP_REC_BINDING_SLOT_REF crash debug** (~1 day) — blocking option (1) above + any future Immix work.  Latent correctness bug in Steps 11′-13′ infrastructure.

## 6. Strategic context (for next session)

### 6.1 Where we are on the M5 watchdog goal

* M5 arena = 5570 MB (deterministic; way over 4096 MB target)
* M5 peak = 4343 ± 95 MB to 3805 ± 308 MB (high environmental variance)
* Bundle delivered -822 MB M5 arena; gap remains 5570 - 4096 = 1474 MB arena
* No single lever in the candidate list will close 1474 MB
* Stacking is required: bundle + late-firing GC + BiBOP-lite together might reach

### 6.2 The audit thesis stands

`DIAGNOSTIC_AUDIT_2026-05-29 §5`: "instruments default to aggregate-shape while decisions are distribution-shape."  This session validated the thesis — DIAG builds took ~1 hour of plumbing each (vs audit's 3-5 day estimates) because the data was already in heap; only aggregators were missing.  Future sessions should default to distribution-shape for any new diagnostic.

### 6.3 The plan §3.5 escalation

`EXIT_GC_SPIRAL_PLAN §3.5` defined the escalation: "BOTH orthogonal levers falsified → Immix becomes Week 1."  Strictly:
* Per-site bundle: SHIPS at -105 / -822 MB.  Not falsified.
* Cache eviction: NULL on M5.  Falsified.
* Gap to watchdog: 247-1474 MB depending on metric.

The plan didn't anticipate "bundle ships but doesn't close gap."  Today's data converts that into: "trigger-policy + BiBOP-lite may close it; measurement spike (#1) settles whether to pursue."

## 7. Open follow-ups

* DIAG-2 Phase 2 RESIDUAL: 9.3% of live bytes still posHandle=0.  Tracking remaining alloc sites without entries[0].pos populated.  Coverage gain probably modest (5-10 percentage points) but worth a half-day's investigation.
* DIAG-5: flat MS quadratic fix (audit §6.5)
* DIAG-6: alloc-tick / age-histogram (audit §6.6)
* DIAG-7: retainer-edge sampling (audit §6.7)
* Non-arena mallinfo
* Immix OP_REC_BINDING_SLOT_REF debug
* `MEMORY_REDUCTION_AVENUES_2026-05-26.md` has uncommitted local edits (~3 lines, unrelated)

## 8. Cross-references

* [`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md) — load-bearing measurement
* [`EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md`](EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md) — pivot rationale
* [`DIAG_SUITE_LANDED_2026-05-29.md`](DIAG_SUITE_LANDED_2026-05-29.md) — instrument inventory
* [`DIAG_CONCENTRATED_RETENTION_2026-05-29.md`](DIAG_CONCENTRATED_RETENTION_2026-05-29.md) — strategic finding + next-move menu
* [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) — the audit driving the pivot
* `bench/baselines/2026-05-29-week1-retrospective/summary.json` — raw N=10 data
* Memory: `[[bench-binary-fingerprint]]`, `[[exit-week1-revalidated]]`, `[[arena-over-ram-peak-noise]]`, `[[app3-mapattrs]]` (rolled back), `[[peak-vs-alloc-distinction]]` (reframed)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
