# `lode/` — engineering logbook (current + historical)

Strategic docs + RCAs + design notes for the v3 bytecode VM. Roughly half active reference; the other half preserved for history.

**Last refreshed:** 2026-05-27 (post-cleanup). See [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) for the categorisation methodology + per-doc rationale.

---

## Quick navigation

- **For current state → [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md)** (latest snapshot; new snapshots auto-supersede)
- **For current week's tactics → [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md)** (Tier A/B/C/D + Tier R + AR list)
- **For strategic direction → [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md)** (long-horizon stages; status tags refreshed)
- **For methodology → [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md)** (the rule + §5.7 anti-pattern)

For build commands / supported AST shapes / lang-test status: see `../USAGE.md`.

---

## Document conventions

| Marker | Meaning |
|---|---|
| Header tag like ✓ DONE / ◐ PARTIAL / ✗ KILLED | Stage / item status (in ROADMAP_TO_VISION) |
| `> **SUPERSEDED <date>**: ...` | Newer doc covers this ground; preserved for back-link integrity |
| `> **ARCHIVED-IN-PLACE <date>**: ...` | Historical doc; kept here because still referenced from KEEP docs |
| `archive/` subdirectory | Truly historical docs with no incoming references; moved out of main view |

---

## Canonical strategic docs (read first; loaded by `../CLAUDE.md`)

These four docs are the load-bearing strategic reference. Read them before any older RCA / plan doc.

| Doc | Purpose |
|---|---|
| [`ACTION_PLAN_2026-05-15.md`](ACTION_PLAN_2026-05-15.md) | 8-week phased plan with TODO checklists, exit + kill criteria. **Rule 0** (the falsification rule) is in Part 1. |
| [`LESSONS_LEARNED_2026-05-15.md`](LESSONS_LEARNED_2026-05-15.md) | Distilled from ~1380 commits. Codified constraints (V3-NATIVE / v3-native primops / eager-vs-lazy). §4.9 has the 10-mechanism debug story. |
| [`ALIGNMENT_SCORECARD_2026-05-15.md`](ALIGNMENT_SCORECARD_2026-05-15.md) | Vision-vs-reality scorecard; re-score quarterly. |
| [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md) | Long-horizon Stages 1-17. Current-state lookup via `ROADMAP_PROGRESS_SNAPSHOT_*.md`. |

---

## Current strategic docs (active reference)

41 docs across 8 topic groups. All post-2026-05-15.

### Operating rules + methodology (5)

| Doc | Purpose |
|---|---|
| [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) | Tactical week-of plan + Tier A/B/C/D + Tier R + AR1-AR30 |
| [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) | Pre-commit threshold rule + §5.7 methodology audit |
| [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) | Progress vs direction; 3 open decisions framework |
| [`LODE_REVIEW_2026-05-18.md`](LODE_REVIEW_2026-05-18.md) | First lode review (predecessor of LODE_CLEANUP_REVIEW) |
| [`LODE_CLEANUP_REVIEW_2026-05-27.md`](LODE_CLEANUP_REVIEW_2026-05-27.md) | This cleanup pass; categorisation + archival rationale |

### Architectural reviews (4)

| Doc | Purpose |
|---|---|
| [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) | 4-agent cross-cutting review; 15 new ARs (AR16-AR30) |
| [`CR1_CR2_AUDIT_RESULTS_2026-05-26.md`](CR1_CR2_AUDIT_RESULTS_2026-05-26.md) | R1 trigger evaluation; CR1/CR2 cross-checks |
| [`FORK_REVIEW_2026-05-21.md`](FORK_REVIEW_2026-05-21.md) | 7-agent fork-vs-upstream review |
| [`DATA_STRUCTURE_AUDIT_2026-05-21.md`](DATA_STRUCTURE_AUDIT_2026-05-21.md) | 5-agent data-structure review |

### Strategy + design (16)

| Doc | Purpose |
|---|---|
| [`AOT_DISTRIBUTION_2026-05-26.md`](AOT_DISTRIBUTION_2026-05-26.md) | R8a + R8b split + R7 revival (cache distribution strategy) |
| [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) | Hand-roll vs Whippet design |
| [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) | Post-falsification 7-category memory inventory |
| [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) | Original 5-agent memory audit |
| [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) | mmap'd L2 design (R8a format) |
| [`WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md`](WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md) | Warm-eval framing + V3_RELEASE spec |
| [`GC_VS_TW_ANALYSIS_2026-05-23.md`](GC_VS_TW_ANALYSIS_2026-05-23.md) | Nursery default-on decision rules |
| [`GC_BUILD_VS_BUY_2026-05-21.md`](GC_BUILD_VS_BUY_2026-05-21.md) | Custom-vs-Whippet decision rules |
| [`BOEHM_DEPENDENCY_2026-05-21.md`](BOEHM_DEPENDENCY_2026-05-21.md) | Whippet R6 escalation criteria |
| [`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md) | Stage 12 deferred-with-data |
| [`OPTIMIZATION_STRATEGIES_2026-05-23.md`](OPTIMIZATION_STRATEGIES_2026-05-23.md) | 5-agent post-kill literature review |
| [`PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`](PARALLEL_EVAL_CAPABILITIES_2026-05-18.md) | Stage 13 candidate analysis |
| [`IFD_DEEP_DIVE_2026-05-21.md`](IFD_DEEP_DIVE_2026-05-21.md) | IFD strategy / materialization retirement |
| [`LINKING_DESIGN_2026-05-17.md`](LINKING_DESIGN_2026-05-17.md) | R1 Phase L1 spec |
| [`PERF_STRATEGY_2026-05-17.md`](PERF_STRATEGY_2026-05-17.md) | Stages 10-12 candidate analysis |
| [`IFD_CACHE_DESIGN_2026-05-23.md`](IFD_CACHE_DESIGN_2026-05-23.md) | #741 engineering plan |

### FFI / V3-NATIVE (3 + 1)

| Doc | Purpose |
|---|---|
| [`FFI_AUDIT_2026-05-24.md`](FFI_AUDIT_2026-05-24.md) | Updated with V3-NATIVE arc empirical data (canonical FFI doc) |
| [`V3_TRUE_NATIVE_PLAN_2026-05-24.md`](V3_TRUE_NATIVE_PLAN_2026-05-24.md) | #795-#808 arc plan |
| [`V3_TRUE_NATIVE_RCA_2026-05-24.md`](V3_TRUE_NATIVE_RCA_2026-05-24.md) | Living RCA log + cache-coherence rules |
| [`V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md`](V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md) | callFlake v3-native design |
| [`BRIDGE_TELEMETRY_2026-05-26.md`](BRIDGE_TELEMETRY_2026-05-26.md) | Bridge wall measurement: 0.014 % on HNE |

### Profiling + instrumentation (4)

| Doc | Purpose |
|---|---|
| [`PROFILING_AUDIT_2026-05-24.md`](PROFILING_AUDIT_2026-05-24.md) | Profiling story gap audit |
| [`PROFILING_IMPROVEMENTS_2026-05-24.md`](PROFILING_IMPROVEMENTS_2026-05-24.md) | 3-tier improvement plan |
| [`PERF_TRACE_TOOL_DESIGN_2026-05-20.md`](PERF_TRACE_TOOL_DESIGN_2026-05-20.md) | Time-series profiler design |
| [`PERF_AUDIT_2026-05-23.md`](PERF_AUDIT_2026-05-23.md) | 5-agent perf review |

### UX pillar (3; deferred per ROADMAP "post-perf + post-IFD")

| Doc | Purpose |
|---|---|
| [`ERROR_UX_DESIGN_2026-05-20.md`](ERROR_UX_DESIGN_2026-05-20.md) | Stage 14 design |
| [`NIX_PROFILER_DESIGN_2026-05-21.md`](NIX_PROFILER_DESIGN_2026-05-21.md) | Stage 15 design |
| [`LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md`](LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md) | Stage 17 design |

### Memory + GC infrastructure (5)

| Doc | Purpose |
|---|---|
| [`NURSERY_PHASE_D_DESIGN_2026-05-18.md`](NURSERY_PHASE_D_DESIGN_2026-05-18.md) | Phase D design (default-on now) |
| [`NURSERY_PHASE_D_DECISION_2026-05-21.md`](NURSERY_PHASE_D_DECISION_2026-05-21.md) | Phase D decision rules |
| [`CHENEY_NURSERY_DESIGN.md`](CHENEY_NURSERY_DESIGN.md) | Phase A/C nursery design |
| [`CELL_INVARIANTS.md`](CELL_INVARIANTS.md) | Cell-update protocol |
| [`GC_AUDIT_ROUND_2_2026-05-21.md`](GC_AUDIT_ROUND_2_2026-05-21.md) | GC audit round 2 findings |

### Strategic workload + Other (10)

| Doc | Purpose |
|---|---|
| [`CARDANO_NODE_FEASIBILITY_2026-05-18.md`](CARDANO_NODE_FEASIBILITY_2026-05-18.md) | Strategic workload definition (needs M5-baseline correction) |
| [`CARDANO_NODE_M5_2026-05-26.md`](CARDANO_NODE_M5_2026-05-26.md) | Corrected M5 baseline (replaces 2026-05-21 version) |
| [`MAX_HEAP_LIMIT_DESIGN_2026-05-18.md`](MAX_HEAP_LIMIT_DESIGN_2026-05-18.md) | NIX_V3_MAX_HEAP design |
| [`IR_OPTIMIZATION_PLAN_2026-05-18.md`](IR_OPTIMIZATION_PLAN_2026-05-18.md) | Optimizer pipeline plan |
| [`IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md`](IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md) | IR test infrastructure |
| [`FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md`](FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md) | TLA+ verification analysis |
| [`UNISON_IDEAS_2026-05-07.md`](UNISON_IDEAS_2026-05-07.md) | **Explicitly un-archived**; canonical reference for Items 1-5 |
| [`RCA_VALUEPAIR_EVALUATED_2026-05-21.md`](RCA_VALUEPAIR_EVALUATED_2026-05-21.md) | Active RCA |
| [`DEFAULT_ON_VALIDATION_2026-05-24.md`](DEFAULT_ON_VALIDATION_2026-05-24.md) | #794 post-default-on validation |
| [`IFD_S4_FALSIFIED_2026-05-27.md`](IFD_S4_FALSIFIED_2026-05-27.md) | Recent IFD S4 falsification |

---

## Living RCA / progress logs (6; actively updated)

| Doc | Topic |
|---|---|
| [`RCA_815_CROSS_WORKLOAD_2026-05-25.md`](RCA_815_CROSS_WORKLOAD_2026-05-25.md) | #815 RCA + Light variant lessons |
| [`HNE_MEMORY_ATTRIBUTION_2026-05-26.md`](HNE_MEMORY_ATTRIBUTION_2026-05-26.md) | A1 measurement source |
| [`WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md`](WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md) | Cross-workload measurement |
| [`V3_TRUE_NATIVE_RCA_2026-05-24.md`](V3_TRUE_NATIVE_RCA_2026-05-24.md) | #795-#808 arc log |
| [`V3_TRUE_NATIVE_PLAN_2026-05-24.md`](V3_TRUE_NATIVE_PLAN_2026-05-24.md) | Companion to RCA |
| [`IFD_S4_FALSIFIED_2026-05-27.md`](IFD_S4_FALSIFIED_2026-05-27.md) | Recent closure |

---

## Kill memos (2; preserved for revival triggers)

| Doc | Topic |
|---|---|
| [`STAGE_5_6_KILLED_2026-05-23.md`](STAGE_5_6_KILLED_2026-05-23.md) | Stage 5+6 PIC kill; revival triggers in ROADMAP table |
| [`STAGE_9_KILLED_2026-05-22.md`](STAGE_9_KILLED_2026-05-22.md) | Stage 9 dedup kill; revival triggers in ROADMAP table |

---

## Snapshots (point-in-time progress; latest is canonical)

| Doc | Status |
|---|---|
| [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md) | **Current** |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md` | SUPERSEDED 2026-05-27 |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` | SUPERSEDED |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md` | SUPERSEDED |

---

## AOT Phase 1 daily log (consolidate post-verdict)

| Doc | Purpose |
|---|---|
| [`AOT_PHASE1_VERDICT_2026-05-27.md`](AOT_PHASE1_VERDICT_2026-05-27.md) | Current verdict status |
| `AOT_PHASE1_DAY13-15_2026-05-26.md` | Day 13-15 madvise tuning + INCONCLUSIVE measurement |
| `AOT_PHASE1_DAY10-12_2026-05-26.md` | Day 10-12 mmap reader + preliminary |
| `AOT_PHASE1_DAY4-6_2026-05-26.md` | Day 4-6 flat-file format + build script |
| `AOT_PHASE1_DAY1-3_2026-05-26.md` | Day 1-3 manifest recorder |

Will be consolidated into `AOT_PHASE1_SUMMARY.md` once R8a Phase 1 ship/revert verdict closes; daily logs move to `archive/aot-phase1/`.

---

## Phase 4b investigation log

| Doc | Status |
|---|---|
| [`PHASE_4B_MULTI_IFD_2026-05-24.md`](PHASE_4B_MULTI_IFD_2026-05-24.md) | Multi-IFD validation |
| [`PHASE_4B_SCALE_TEST_2026-05-24.md`](PHASE_4B_SCALE_TEST_2026-05-24.md) | Post-RCA scale test (wall-positive) |
| `PHASE_4B_SCALE_TEST_2026-05-23.md` | SUPERSEDED by 05-24 |

---

## SUPERSEDED docs (preserved in-place; refs still valid)

These 18 docs have newer successors but are preserved here because cross-doc references link to them. Each has a `> **SUPERSEDED <date>**: ...` banner pointing to the successor.

```
NEXT_TASKS_2026-05-22.md              → NEXT_STEPS_2026-05-25.md
SESSION_ARC_2026-05-23.md             → ROADMAP_PROGRESS_SNAPSHOT_2026-05-27
ALIGNMENT_NOTE_2026-05-23.md          → ROADMAP_PROGRESS_SNAPSHOT_2026-05-27
ROADMAP_ALIGNMENT_POST_741_2026-05-23.md  → SNAPSHOT
CARDANO_NODE_M5_2026-05-21.md         → 2026-05-26 (baseline corrected)
BINDINGS_ATTRIBUTION_2026-05-21.md    → HNE_MEMORY_ATTRIBUTION_2026-05-26
FFI_PLAN_2026-05-06.md + 06b.md       → FFI_AUDIT_2026-05-20 → FFI_AUDIT_2026-05-24
FFI_AUDIT_2026-05-20.md               → FFI_AUDIT_2026-05-24
CELL_UPDATE_AUDIT_2026-05-08.md       → CELL_INVARIANTS.md
PROFILE_HELLO_NAME_2026-05-18.md      → HNE_MEMORY_ATTRIBUTION_2026-05-26
BASELINE_COMPARISON_2026-05-18.md     → SNAPSHOT_2026-05-27
EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md  → BRIDGE_TELEMETRY_2026-05-26
OPTION_4_COMPLETE_2026-05-18.md       → V3_TRUE_NATIVE_RCA_2026-05-24
ITERATIVE_FORCE_AUDIT_2026-05-18.md   → V3_TRUE_NATIVE_RCA_2026-05-24
ITERATIVE_FORCE_STATE_2026-05-17.md   → V3_TRUE_NATIVE_RCA_2026-05-24
VM_TW_TIMING_VISIBILITY_2026-05-20.md → PROFILING_IMPROVEMENTS_2026-05-24
PHASE_4B_SCALE_TEST_2026-05-23.md     → 2026-05-24 (post-RCA)
ROADMAP_PROGRESS_SNAPSHOT_2026-05-22 / -23 / -26  → 2026-05-27
```

---

## ARCHIVED-IN-PLACE docs (historical; referenced from KEEP docs)

These 24 docs are historical (pre-strategic-doc-set or one-off RCA) but are referenced from one or more KEEP docs, so they stay in place with `> **ARCHIVED-IN-PLACE 2026-05-27**: ...` banner. Includes:

`CALLPACKAGE_BUG_2026-05-09`, `CELL_UPDATE_EVERYWHERE_2026-05-12`, `CLEANUP_AUDIT_2026-05-09`, `ENV_VAR_INVENTORY_2026-05-15`, `EVAL_ORDER_DIVERGENCE_2026-05-08`, `GC-REVIEW`, `INVERSION_PLAN_2026-05-08`, `OPTIMIZATION_PLAN` (177 KB), `OPTIMIZER_REPORT_2026-05-07`, `OPT_OCCUR_PLAN_2026-05-08`, `RCA_FAMILY_DIVERGENCE_DEEPER_2026-05-11`, `REDUCTION_AUDIT_2026-05-09`, `REVIEW_2026-05-06b`, `SESSION_PLAN_2026-05-20`, `TEAM_A_B`, `TEST_INFRA_AUDIT_2026-05-08`, `TRAFFIC-OWNERSHIP-REVIEW`, `V3VALUE_CANON_AUDIT`, `V3_DIRECT_RCA_PROGRESS_2026-05-09`, `V3_NATIVE_CONSTRAINT_2026-05-09`, `VM_TW_MEASUREMENT_2026-05-09`, `WC38_FIX_PLAN`, `WITH`, `path_b_full_frame_dump_2026-05-17.txt`.

---

## `archive/` — fully archived (no incoming references; 41 files)

Files in `archive/` have no incoming references from KEEP docs and have been moved out of main view. Includes:

- Multi-agent review snapshots: `REVIEW_2026-05-{03,04,05,06,07,08,09}.md`
- `REVIEW_2026-05-11/` subdirectory (12 docs)
- Benchmark logs: `BENCH-2026-05-04-*.md`, `BENCH-2026-05-08-*.md`, `BENCH-2026-05-09-*.md`, `BENCH-REAL-WORLD-*.md`, `REAL_WORLD_BENCH_2026-05-09.md`
- One-off RCAs: `A12B_*`, `PATH_B_*`, `path_b_full_frame_dump*` (kept in main with banner), `PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md`, `SLOT_*`, `STG_*`, `V3_DIRECT_NIXPKGS_2026-05-09.md`, `RCA_FAMILY_DIVERGENCE_{*,A4,A7,FINDINGS,ROOTCAUSE}_2026-05-11.md` (DEEPER kept in main)
- Older plans: `LEXICAL_WITHS_PLAN_2026-05-08.md`, `V3_LOWER_BOOTSTRAP_2026-05-22.md`

For details of categorisation methodology, see `LODE_CLEANUP_REVIEW_2026-05-27.md` §11.

---

## Going-forward conventions (proposed 2026-05-27)

To prevent re-accumulation, future lode/ contributors should:

1. **Add a status line** at the top of every new doc:
   `**Status:** CANONICAL | CURRENT STRATEGIC | LIVING | SNAPSHOT | ...`
2. **Snapshot-at-cadence:** new `ROADMAP_PROGRESS_SNAPSHOT_*.md` auto-supersedes the previous; older snapshot gets a SUPERSEDED banner at creation time.
3. **Quarterly lode/ review:** every ~3 months, run the cleanup process from `LODE_CLEANUP_REVIEW_2026-05-27.md`.
4. **Soft 6-docs-per-week cap:** recent pace (~25 docs in 12 days) is productive but each doc has maintenance + onboarding cost.

---

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
