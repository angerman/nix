# Lode/ cleanup review — 2026-05-27

**Date:** 2026-05-27
**Author:** session synthesis
**Status:** strategic — categorisation + archival recommendations for all `lode/` docs
**Triggering context:** `lode/` has grown to 142 .md files + a sub-directory (`REVIEW_2026-05-11/` with 12 more) totalling ~47K lines. The README.md from 2026-05-15 references ~20 docs; the other ~120+ are uncategorised. Doc proliferation has its own cost — onboarding, lookup, stale-reference risk. This review categorises every doc and recommends archival actions.

Companion docs:
- [`LODE_REVIEW_2026-05-18.md`](LODE_REVIEW_2026-05-18.md) — earlier lode/ review (mostly addressed; this doc is the next-cycle review)
- [`README.md`](README.md) — categorises ~20 docs as of 2026-05-15; needs update
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md) — informs which docs are still strategically active

---

## 1. Position (TL;DR)

**142 .md files in `lode/`. After categorisation:**

| Category | Count | Action |
|---|---|---|
| **CANONICAL** (top-priority strategic reference) | 5 | KEEP; CLAUDE.md loads these |
| **CURRENT STRATEGIC** (recent active reference) | 41 | KEEP; review monthly |
| **LIVING RCA / PROGRESS LOGS** (ongoing) | 6 | KEEP; update or close as work completes |
| **KILL MEMOS** (revival reference) | 2 | KEEP (Stages 5/6, Stage 9) |
| **SNAPSHOTS** (point-in-time progress) | 4 | KEEP latest; tag older as SUPERSEDED |
| **AOT PHASE 1 SERIES** (consolidatable) | 5 | KEEP; consolidate into one summary doc post-verdict |
| **PHASE 4B INVESTIGATION** (recent; some superseded) | 3 | KEEP newer; SUPERSEDE older |
| **SUPERSEDED** (newer doc covers; mark with banner) | 15 | Add SUPERSEDED banner pointing to successor |
| **ARCHIVE CANDIDATES** (truly historical) | 61 | Move to `lode/archive/` subdirectory |
| **TOTAL** | 142 + 12 in REVIEW_2026-05-11/ subdir | |

**Recommended cleanup actions in priority order:**

1. **Create `lode/archive/` subdirectory** and move 61 archive candidates (~50 % of current `lode/` files)
2. **Add SUPERSEDED banners** to 15 docs pointing to their successors (instead of moving — preserves cross-references that link to them)
3. **Consolidate the AOT Phase 1 daily logs** into `AOT_PHASE1_SUMMARY.md` once Phase 1 verdict closes
4. **Update README.md** to reflect the cleaned-up structure
5. **Add a `STATUS:` frontmatter field** to all KEEP docs going forward (CANONICAL / CURRENT / LIVING / KILL / SNAPSHOT)

**Estimated effort:** 2-3 hours for the moves + banners; 1-2 hours for README.md refresh. **Total: ½ day.**

**Risks:** cross-doc references to archived files will break (mitigated by archive-in-place strategy: move to subdir, links can use `archive/` prefix in updates).

---

## 2. Methodology

Reviewed all 142 .md files in `lode/` by:
- Looking at filename + date
- Cross-checking against current strategic position (per `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`)
- Checking explicit "SUPERSEDED" / "archived" / status markers in doc bodies
- Tracking pre-2026-05-15 docs (largely covered by old README.md) vs post-2026-05-15 strategic doc set

Each doc assigned one of seven categories:

| Category | Definition |
|---|---|
| **CANONICAL** | Top-priority strategic reference; explicit per old README + CLAUDE.md |
| **CURRENT STRATEGIC** | Post-2026-05-15 strategic doc; still actively referenced |
| **LIVING RCA / PROGRESS LOG** | Ongoing investigation log; team actively updates |
| **KILL MEMO** | Documents a Rule-0 kill; preserved for revival triggers |
| **SNAPSHOT** | Point-in-time progress; supersedes prior snapshots |
| **SUPERSEDED** | Newer doc covers same ground; preserve content + banner pointing to successor |
| **ARCHIVE CANDIDATE** | Historical doc; pre-strategic-doc-set or one-off RCA |

---

## 3. CANONICAL (5 docs; KEEP as-is)

These are the top-priority strategic reference docs loaded by `CLAUDE.md` and listed in current `README.md`.

| Doc | Date | Notes |
|---|---|---|
| `ACTION_PLAN_2026-05-15.md` | 2026-05-15 (mod 05-26) | 8-week phased plan; Rule 0 source |
| `ALIGNMENT_SCORECARD_2026-05-15.md` | 2026-05-15 (mod 05-26) | Vision-vs-reality scorecard |
| `LESSONS_LEARNED_2026-05-15.md` | 2026-05-15 (mod 05-26) | Codified constraints + Nix domain knowledge |
| `ROADMAP_TO_VISION_2026-05-15.md` | 2026-05-15 (mod 05-26) | Long-horizon stages; current-state lookup → ROADMAP_PROGRESS_SNAPSHOT |
| `README.md` | 2026-05-15 | Categorises old docs; **needs refresh** per §10 below |

---

## 4. CURRENT STRATEGIC (41 docs; KEEP, review monthly)

Recent strategic docs (post-2026-05-15) that remain active reference. Group by topic:

### 4.1 Operating-rule + methodology docs (5)

| Doc | Topic | Status |
|---|---|---|
| `MEASURE_TWICE_CUT_ONCE_2026-05-23.md` | Core methodology rule | Living rule |
| `LODE_REVIEW_2026-05-18.md` | Prior lode review (~½ executed) | This doc is the successor |
| `NEXT_STEPS_2026-05-25.md` | Tactical Tier A/B/C/D + Tier R + AR list (~98 KB) | Most-referenced operational doc |
| `DIRECTION_NOTE_2026-05-26.md` | Progress vs direction observation | Active |
| `LODE_CLEANUP_REVIEW_2026-05-27.md` | This doc | Active |

### 4.2 Architectural review docs (4)

| Doc | Topic |
|---|---|
| `ARCHITECTURE_CRITIQUE_2026-05-26.md` | 4-agent cross-cutting review; 15 new ARs |
| `CR1_CR2_AUDIT_RESULTS_2026-05-26.md` | R1 trigger evaluation |
| `FORK_REVIEW_2026-05-21.md` | 7-agent fork-vs-upstream review |
| `DATA_STRUCTURE_AUDIT_2026-05-21.md` | 5-agent data-structure review |

### 4.3 Strategy + design docs (16)

| Doc | Topic |
|---|---|
| `AOT_DISTRIBUTION_2026-05-26.md` | R8a + R8b split + R7 revival |
| `IDEAL_GC_DESIGN_2026-05-26.md` | Hand-roll vs Whippet design |
| `MEMORY_REDUCTION_AVENUES_2026-05-26.md` | Post-falsification inventory (supersedes MEMORY_REDUCTION_OPPORTUNITIES but both kept) |
| `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` | Original 5-agent memory audit |
| `EVAL_CACHE_ARCHITECTURE_2026-05-23.md` | mmap'd L2 design (R8a format) |
| `WARM_EVAL_AND_INSTRUMENTATION_2026-05-23.md` | Warm-eval framing + V3_RELEASE spec |
| `GC_VS_TW_ANALYSIS_2026-05-23.md` | Nursery default-on decision rules |
| `GC_BUILD_VS_BUY_2026-05-21.md` | Custom-vs-Whippet decision rules |
| `BOEHM_DEPENDENCY_2026-05-21.md` | Whippet R6 escalation criteria |
| `JIT_CONFIDENCE_2026-05-23.md` | Stage 12 deferred-with-data |
| `OPTIMIZATION_STRATEGIES_2026-05-23.md` | 5-agent post-kill literature review |
| `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md` | Stage 13 candidate analysis |
| `IFD_DEEP_DIVE_2026-05-21.md` | IFD strategy / materialization retirement |
| `LINKING_DESIGN_2026-05-17.md` | R1 Phase L1 spec |
| `PERF_STRATEGY_2026-05-17.md` | Stages 10-12 candidate analysis |
| `IFD_CACHE_DESIGN_2026-05-23.md` | #741 engineering plan |

### 4.4 FFI / V3-NATIVE (4)

| Doc | Topic |
|---|---|
| `FFI_AUDIT_2026-05-20.md` | Initial FFI / TW fallback inventory |
| `FFI_AUDIT_2026-05-24.md` | Updated with V3-NATIVE arc empirical data |
| `V3_TRUE_NATIVE_PLAN_2026-05-24.md` | #795-#808 arc plan |
| `V3_TRUE_NATIVE_RCA_2026-05-24.md` | Living RCA log for arc + cache-coherence rules |

### 4.5 Profiling + instrumentation (4)

| Doc | Topic |
|---|---|
| `PROFILING_AUDIT_2026-05-24.md` | Gap audit |
| `PROFILING_IMPROVEMENTS_2026-05-24.md` | 3-tier improvement plan |
| `PERF_TRACE_TOOL_DESIGN_2026-05-20.md` | Time-series profiler design |
| `PERF_AUDIT_2026-05-23.md` | 5-agent perf review |

### 4.6 UX pillar (3; deferred post-perf+post-IFD per ROADMAP)

| Doc | Topic |
|---|---|
| `ERROR_UX_DESIGN_2026-05-20.md` | Stage 14 design |
| `NIX_PROFILER_DESIGN_2026-05-21.md` | Stage 15 design |
| `LINT_INFRASTRUCTURE_DESIGN_2026-05-22.md` | Stage 17 design |

### 4.7 Memory + GC infrastructure (5)

| Doc | Topic |
|---|---|
| `NURSERY_PHASE_D_DESIGN_2026-05-18.md` | Phase D design (default-on now) |
| `NURSERY_PHASE_D_DECISION_2026-05-21.md` | Phase D decision rules |
| `CHENEY_NURSERY_DESIGN.md` | Phase A/C nursery design |
| `CELL_INVARIANTS.md` | Cell-update protocol |
| `GC_AUDIT_ROUND_2_2026-05-21.md` | GC audit round 2 findings |

### 4.8 Other (4)

| Doc | Topic |
|---|---|
| `V3_NATIVE_CALL_FLAKE_DESIGN_2026-05-20.md` | callFlake v3-native design |
| `MAX_HEAP_LIMIT_DESIGN_2026-05-18.md` | NIX_V3_MAX_HEAP design |
| `IR_OPTIMIZATION_PLAN_2026-05-18.md` | Optimizer pipeline plan |
| `IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md` | IR test infrastructure |
| `FORMAL_VERIFICATION_ANALYSIS_2026-05-22.md` | TLA+ verification analysis |
| `UNISON_IDEAS_2026-05-07.md` | **EXPLICITLY UN-ARCHIVED per memory entry; canonical reference** |
| `CARDANO_NODE_FEASIBILITY_2026-05-18.md` | Strategic workload definition; **needs M5-baseline correction** |
| `BRIDGE_TELEMETRY_2026-05-26.md` | Recent bridge telemetry data |
| `RCA_VALUEPAIR_EVALUATED_2026-05-21.md` | RCA active |
| `DEFAULT_ON_VALIDATION_2026-05-24.md` | #794 post-default-on validation |
| `CARDANO_NODE_M5_2026-05-26.md` | Corrected M5 baseline |
| `IFD_S4_FALSIFIED_2026-05-27.md` | Recent falsification |

---

## 5. LIVING RCA / PROGRESS LOGS (6 docs; KEEP, update or close as work completes)

These are actively updated investigation logs:

| Doc | Status |
|---|---|
| `RCA_815_CROSS_WORKLOAD_2026-05-25.md` | Living RCA; #815 closed but doc is the reference for the closure analysis |
| `HNE_MEMORY_ATTRIBUTION_2026-05-26.md` | Living; A1 measurement source |
| `WORKLOAD_HETEROGENEITY_AUDIT_2026-05-23.md` | Active cross-workload measurement |
| `V3_TRUE_NATIVE_RCA_2026-05-24.md` | Living; #795-#808 arc log |
| `V3_TRUE_NATIVE_PLAN_2026-05-24.md` | Living; companion to RCA |
| `IFD_S4_FALSIFIED_2026-05-27.md` | Recent; closes part of IFD work |

---

## 6. KILL MEMOS (2 docs; KEEP for revival triggers)

| Doc | Status |
|---|---|
| `STAGE_5_6_KILLED_2026-05-23.md` | Revival triggers in ROADMAP table |
| `STAGE_9_KILLED_2026-05-22.md` | Revival triggers in ROADMAP table |

---

## 7. SNAPSHOTS (4 docs; KEEP latest, SUPERSEDE older)

| Doc | Status | Action |
|---|---|---|
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md` | Oldest | **Add SUPERSEDED banner pointing to 05-27** |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md` | Pre-Tier-A | **Add SUPERSEDED banner** |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md` | Morning version | **Add SUPERSEDED banner** |
| `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md` | Current | KEEP |

---

## 8. AOT Phase 1 series (5 docs; consolidate post-verdict)

| Doc | Action |
|---|---|
| `AOT_PHASE1_DAY1-3_2026-05-26.md` | Daily log; KEEP through verdict |
| `AOT_PHASE1_DAY4-6_2026-05-26.md` | Daily log; KEEP through verdict |
| `AOT_PHASE1_DAY10-12_2026-05-26.md` | Daily log; KEEP through verdict |
| `AOT_PHASE1_DAY13-15_2026-05-26.md` | Daily log; KEEP through verdict |
| `AOT_PHASE1_VERDICT_2026-05-27.md` | KEEP; current verdict status |

**Once Phase 2 commits (or revert per pre-committed threshold):** consolidate all 5 into one `AOT_PHASE1_SUMMARY.md`; move Day-* logs to `archive/aot-phase1/`.

---

## 9. PHASE 4B INVESTIGATION (3 docs; mixed)

| Doc | Status | Action |
|---|---|---|
| `PHASE_4B_SCALE_TEST_2026-05-23.md` | First scale test (wall-neutral) | **SUPERSEDED** by 05-24 version (which adds RCA + scope-bug fix) |
| `PHASE_4B_SCALE_TEST_2026-05-24.md` | Post-RCA scale test (wall-positive) | KEEP |
| `PHASE_4B_MULTI_IFD_2026-05-24.md` | Multi-IFD validation | KEEP |

---

## 10. SUPERSEDED (15 docs; ADD BANNER, don't move)

Newer docs cover the same ground. Mark with a banner pointing to the successor. **Preserve in place** so existing cross-references don't break.

| Doc | Superseded by | Reason |
|---|---|---|
| `NEXT_TASKS_2026-05-22.md` | `NEXT_STEPS_2026-05-25.md` | Newer plan supersedes old task list |
| `SESSION_ARC_2026-05-23.md` | `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27` | Session-arc captured in snapshot series |
| `ALIGNMENT_NOTE_2026-05-23.md` | `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27` | Alignment captured in snapshot series |
| `ROADMAP_ALIGNMENT_POST_741_2026-05-23.md` | `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27` | Alignment captured in snapshot series |
| `CARDANO_NODE_M5_2026-05-21.md` | `CARDANO_NODE_M5_2026-05-26.md` | 919 MB baseline was wrong; corrected version supersedes |
| `BINDINGS_ATTRIBUTION_2026-05-21.md` | `HNE_MEMORY_ATTRIBUTION_2026-05-26.md` | Newer HNE-specific attribution supersedes |
| `FFI_PLAN_2026-05-06.md` | `FFI_AUDIT_2026-05-20.md` | Plan superseded by audit |
| `FFI_PLAN_2026-05-06b.md` | `FFI_AUDIT_2026-05-20.md` | Plan superseded by audit |
| `FFI_AUDIT_2026-05-20.md` | `FFI_AUDIT_2026-05-24.md` | Updated with V3-NATIVE arc empirical data |
| `CELL_UPDATE_AUDIT_2026-05-08.md` | `CELL_INVARIANTS.md` | Audit findings codified into invariants doc |
| `PROFILE_HELLO_NAME_2026-05-18.md` | `HNE_MEMORY_ATTRIBUTION_2026-05-26.md` | Newer per-workload attribution |
| `BASELINE_COMPARISON_2026-05-18.md` | `ROADMAP_PROGRESS_SNAPSHOT_2026-05-27` | Baseline captured in snapshot |
| `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` | `BRIDGE_TELEMETRY_2026-05-26.md` | Bridge telemetry confirmed 0.014 % wall; investigation closed |
| `OPTION_4_COMPLETE_2026-05-18.md` | Stage 2 closure | Completion captured in V3_TRUE_NATIVE_RCA + ROADMAP closure marker |
| `ITERATIVE_FORCE_AUDIT_2026-05-18.md` | `V3_TRUE_NATIVE_RCA_2026-05-24` | Audit absorbed into V3-NATIVE arc |
| `ITERATIVE_FORCE_STATE_2026-05-17.md` | `V3_TRUE_NATIVE_RCA_2026-05-24` | State doc absorbed |
| `VM_TW_TIMING_VISIBILITY_2026-05-20.md` | `PROFILING_IMPROVEMENTS_2026-05-24.md` | Visibility addressed in profiling improvements |

**Banner template:**

```markdown
> **SUPERSEDED 2026-05-27**: Newer doc covers this ground better. See [`<successor.md>`](<successor.md>). Preserved here for historical reference + back-link integrity.
```

---

## 11. ARCHIVE CANDIDATES (61 docs + REVIEW_2026-05-11 subdir; MOVE to `lode/archive/`)

These are truly historical — pre-strategic-doc-set point-in-time artifacts. The current README.md already explicitly marks several as "point-in-time" / "Forward-looking sections superseded."

### 11.1 Multi-agent review snapshots (8 docs)

```
REVIEW_2026-05-03.md
REVIEW_2026-05-04.md
REVIEW_2026-05-05.md
REVIEW_2026-05-06.md
REVIEW_2026-05-06b.md
REVIEW_2026-05-07.md
REVIEW_2026-05-08.md
REVIEW_2026-05-09.md
```

Per README.md: "multi-agent review snapshots, in date order." All pre-strategic-doc-set; covered by LESSONS_LEARNED §0 + ARCHITECTURE_CRITIQUE.

### 11.2 REVIEW_2026-05-11/ subdirectory (12 docs)

Entire subdirectory of multi-agent review docs from 2026-05-11. Pre-strategic-doc-set.

### 11.3 Benchmark logs (5 docs)

```
BENCH-2026-05-04-CUMULATIVE.md
BENCH-2026-05-04-PHASE5-DEFAULT.md
BENCH-2026-05-08-POST-530.md
BENCH-2026-05-09-POST-DEFER-GETENVCACHE.md
BENCH-REAL-WORLD-2026-05-04.md
```

Per README.md: "perf under specific commits / configurations. Superseded by USAGE.md."

### 11.4 One-off RCA / audit docs (28)

```
CALLPACKAGE_BUG_2026-05-09.md          (resolved)
CLEANUP_AUDIT_2026-05-09.md            (already executed)
EVAL_ORDER_DIVERGENCE_2026-05-08.md
PATH_B_INVESTIGATION_2026-05-16.md
path_b_full_frame_dump_2026-05-17.txt
PUBLISH_RECOVERY_USE_AUDIT_2026-05-08.md
RCA_FAMILY_DIVERGENCE_2026-05-11.md
RCA_FAMILY_DIVERGENCE_A4_2026-05-11.md
RCA_FAMILY_DIVERGENCE_A7_2026-05-11.md
RCA_FAMILY_DIVERGENCE_DEEPER_2026-05-11.md
RCA_FAMILY_DIVERGENCE_FINDINGS_2026-05-11.md
RCA_FAMILY_DIVERGENCE_ROOTCAUSE_2026-05-11.md
REAL_WORLD_BENCH_2026-05-09.md
REDUCTION_AUDIT_2026-05-09.md
SLOT_AUDIT_HOOK_REENTRY_2026-05-08.md
SLOT_TAGGING_AUDIT_2026-05-07.md
STG_DEFAULT_ON_2026-05-09.md
STG_INVENTORY_2026-05-09.md
TEAM_A_B.md
TEST_INFRA_AUDIT_2026-05-08.md
V3_DIRECT_NIXPKGS_2026-05-09.md
V3_DIRECT_RCA_PROGRESS_2026-05-09.md
V3_NATIVE_CONSTRAINT_2026-05-09.md  (codified as memory rule)
V3VALUE_CANON_AUDIT.md
VM_TW_MEASUREMENT_2026-05-09.md
WC38_FIX_PLAN.md  (resolved per README "cautionary case study")
WITH.md
A12B_BYTECODE_PRIMOPS_2026-05-17.md  (older sub-letter doc)
A12B_PRINT_PATH_DEFERRAL_2026-05-22.md
```

### 11.5 Older plans (5)

```
INVERSION_PLAN_2026-05-08.md           (Phase 1 landed; later phases obsolete)
LEXICAL_WITHS_PLAN_2026-05-08.md       (resolved)
OPT_OCCUR_PLAN_2026-05-08.md           (executed)
OPTIMIZER_REPORT_2026-05-07.md         (covered by OPTIMIZATION_STRATEGIES)
SESSION_PLAN_2026-05-20_BINDINGS_STG3_STG4.md  (older session plan)
```

### 11.6 Older infrastructure (5)

```
ENV_VAR_INVENTORY_2026-05-15.md        (could move to test/ instead)
GC-REVIEW.md                           (Phase 0 closed; later phases via newer GC docs)
OPTIMIZATION_PLAN.md                   (177 KB; per README "Forward-looking sections superseded")
TRAFFIC-OWNERSHIP-REVIEW.md            (per README: contradicted by code at time of writing)
V3_LOWER_BOOTSTRAP_2026-05-22.md       (point-in-time bootstrap doc)
CELL_UPDATE_EVERYWHERE_2026-05-12.md   (superseded by CELL_INVARIANTS)
```

### 11.7 Special — the giant OPTIMIZATION_PLAN.md (177 KB)

This is the largest doc in lode/. Per README: "chronological engineering log; Phases 1-4 landed pre-2026-05-05. Phase 5 status: see USAGE.md (default-on). Forward-looking sections superseded."

**Recommendation:** archive. The forward-looking content is superseded by OPTIMIZATION_STRATEGIES_2026-05-23 + IR_OPTIMIZATION_PLAN_2026-05-18 + PERF_STRATEGY_2026-05-17. The chronological log value is historical.

---

## 12. Risks of cleanup execution

### 12.1 Cross-doc reference breakage (the main risk)

Many KEEP docs reference older docs (e.g., V3_TRUE_NATIVE_RCA references EXTEND_DERIVATION_INVESTIGATION; MEMORY_REDUCTION_AVENUES references BINDINGS_ATTRIBUTION). Moving an archived doc breaks the link.

**Mitigation strategies (choose one):**

1. **Archive-in-place via subdirectory:** move to `lode/archive/<filename>`; update references via grep + sed. ~1 hour additional work but clean structure.

2. **SUPERSEDED banner instead of move:** preserve in place; banner directs reader to successor. Cross-refs stay valid. Already-recommended for SUPERSEDED category (§10).

3. **Hybrid:** archive only files with NO incoming references (grep first; move only orphans). Maximally safe.

**Recommended:** Hybrid. Grep `lode/` for each archive-candidate filename; archive only those with zero incoming references; keep with SUPERSEDED banner where referenced.

### 12.2 Memory entries referencing archived files

The `~/.claude-io/projects/.../memory/` entries reference many lode/ docs. Memory entries are point-in-time; moving lode/ docs doesn't break memory functionality but creates stale references.

**Mitigation:** acceptable; memory entries are explicit point-in-time per the memory system's design.

### 12.3 CLAUDE.md + USAGE.md references

`src/libexpr-v3/CLAUDE.md` references the canonical strategic doc set. `USAGE.md` references some lode/ docs. Check both before moving.

**Mitigation:** grep CLAUDE.md + USAGE.md for `lode/` references before any move; update if needed.

---

## 13. Suggested cleanup execution sequence

### Phase 1 — Banner additions (1 hour)

Add SUPERSEDED banners to the 15 docs in §10 + 3 older snapshots in §7. No moves; just banners. Zero risk of cross-ref breakage.

### Phase 2 — Cross-ref audit (1 hour)

Grep `lode/` (excluding archive-candidate files) for each archive-candidate filename. Build a list of orphans (zero incoming refs) vs referenced. Archive orphans; banner the rest.

### Phase 3 — Archive moves (1 hour)

Create `lode/archive/` subdirectory. Move orphan files (estimated 40-50 of the 61 candidates) into it. Update remaining cross-refs to `archive/<filename>`.

### Phase 4 — README refresh (1 hour)

Update `README.md` to:
- Reflect new structure (archive subdir + tier categories)
- List CANONICAL docs explicitly (5)
- List CURRENT STRATEGIC docs by topic group (the §4 grouping above is a good template)
- Explain LIVING / SNAPSHOT / SUPERSEDED conventions
- Direct readers to `LODE_CLEANUP_REVIEW_2026-05-27.md` (this doc) + the latest `ROADMAP_PROGRESS_SNAPSHOT_*.md` for current state

### Phase 5 — AOT Phase 1 consolidation (when verdict closes; ~1 hour)

Once R8a Phase 1 ships or reverts per pre-committed threshold:
- Consolidate 5 AOT_PHASE1_* docs into `AOT_PHASE1_SUMMARY.md`
- Archive Day-* logs to `archive/aot-phase1/`

**Total Phase 1-4 effort: ~4 hours. Phase 5 is post-event; ~1 hour additional when triggered.**

---

## 14. Going forward: doc-creation discipline

To prevent re-accumulation:

1. **Add `STATUS:` frontmatter** to every new lode/ doc:
   ```
   **Status:** CURRENT STRATEGIC | LIVING | SNAPSHOT | etc.
   ```

2. **Snapshot-at-cadence rule:** new SNAPSHOT docs auto-supersede the previous; the prior snapshot gets a banner at creation time.

3. **6-doc-per-week soft cap?** The recent pace (~25 docs in 12 days) is genuinely productive, but each doc adds maintenance + onboarding cost. Reviewer judgement; not enforced.

4. **Quarterly lode/ review:** every ~3 months, run this cleanup process. ~½ day each time.

---

## 15. Honest limits

- **Some category assignments are subjective.** A doc could plausibly be CURRENT STRATEGIC or LIVING depending on team focus.
- **15 SUPERSEDED docs may have additional unique content** not in the successor. Spot-check before adding banners; if unique content exists, add a "Unique content preserved here:" note in the banner.
- **The 61 ARCHIVE candidates** assume the strategic-doc-set covers all important content. If a specific historical doc has unique knowledge nowhere else, it should KEEP not archive.
- **OPTIMIZATION_PLAN.md (177 KB)** may have unique historical context worth preserving more visibly than archive. Consider splitting into "OPTIMIZATION_HISTORY_CONTEXT.md" + archive the rest, OR archive whole-file with a note in successor docs.
- **Memory pointer updates** are separate work; this doc only covers lode/ files.
- **The "doc-creation discipline" suggestions in §14** are author opinion; team should validate.
- **REVIEW_2026-05-11/** subdirectory contains 12 docs that haven't been examined individually. May contain unique content worth surfacing before archive.
- **AOT Phase 1 daily logs** could be consolidated NOW if Phase 1 verdict is clear; or kept as logs through verdict. Recommendation is "post-verdict" because that's when the consolidated summary becomes meaningful.
- **README.md refresh** scope is open — could be a minimal "see LODE_CLEANUP_REVIEW for current state" pointer, OR a full rewrite. Recommend minimal pointer + the categorisation table from §1.
- **The "doc count vs value" question** is genuinely hard. 142 docs feels like a lot; if the team's discipline is working, it's also producing genuine value. The cleanup is about ergonomics, not condemnation.

---

## 16. Cross-references

- [`LODE_REVIEW_2026-05-18.md`](LODE_REVIEW_2026-05-18.md) — earlier lode/ review; covered ~½ ground; this doc continues
- [`README.md`](README.md) — categorises ~20 docs; needs §14 refresh
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md) — current strategic state; informs which docs are LIVING
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — most-referenced operational doc; many KEEP docs cross-link here
- `../CLAUDE.md` — loads CANONICAL docs; check before moves
- `../../USAGE.md` — may reference lode/ docs; check before moves

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
