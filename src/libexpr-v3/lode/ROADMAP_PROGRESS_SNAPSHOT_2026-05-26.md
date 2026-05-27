# Roadmap progress snapshot — 2026-05-26

> **SUPERSEDED 2026-05-27**: Newer snapshot supersedes. See [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-27.md). Preserved here for historical reference + back-link integrity.

---


**Date:** 2026-05-26 (evening)
**Author:** session synthesis
**Status:** snapshot — point-in-time state of v3 vs `ROADMAP_TO_VISION_2026-05-15.md`
**Triggering context:** R1 trigger verified fired (`dcfbae871`); R10 Let-floating falsified (`c4c3e7edb`); Tier A complete; 11-day-old ROADMAP needs current-state lookup

Companion snapshots:
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md) — prior snapshot
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md) — post-Stage-2 closure
- This doc — post-R1-fire + R10-falsified + Tier-A-complete

---

## 1. Position (TL;DR)

**Stages 1-2 closed. Stage 8 substantially advanced. Three Rule-0 kills (Stages 5/6/9) reclaimed ~17 weeks of projected calendar time. R10 just falsified joining the kill pile (4th data-falsified item). R1 trigger verified fired — next major scheduling decision. Memory work in measurement-first phase (5 falsifications motivate going back to measurement).**

The team is genuinely AHEAD on the perf+correctness axis (V3-NATIVE arc, Phase 4b wall-positive, haskell.nix unblock) and BEHIND on the memory axis (HNE 5.3× RSS unchanged, 5 Phase C attempts falsified). Standard-workload wall ratio is 1.67× TW post-#814; haskell.nix-example 1.42× wall but 5.3× RSS.

Most significant unresolved decision: **R1 (Full de Bruijn IR) — verified-fired this afternoon**. Schema 14 (`a7b41ddce`) already partial — PosIdx remap closed positional-only DIFF class 353→4. Remaining R1 work may be narrower than original 1-week estimate.

---

## 2. Stage-by-stage status

### 2.1 Committed stages (1-9 + 14/15/17)

| Stage | Description | Status | Last update / evidence |
|---|---|---|---|
| **Stage 1** | Action plan completion | ✓ **DONE** | `e8d7c3885` Action plan exit |
| **Stage 2** | Pure-bytecode evaluation | ✓ **DONE** | `3af813638` (#760 NIX_V3_SKIP_INSTALLABLE_PREEVAL retired 2026-05-22); 119 workloads byte-identical |
| **Stage 3** | Nursery default-on + closure-pool retired | ◐ **PARTIAL** | Phase A/C `#705`, Phase D default-on `c0911aee6`, Phase E v0.2 opt-in `c4be4cfbc`. **B2 default-on flip FALSIFIED on hello/firefox** (`f2c254fd4` 2026-05-26). Selective nursery untried. Closure-pool retired (#720 Step 12, `ddb52d3a7`) |
| **Stage 4** | Uniform STG-shape (strictness, lambda lift) | ◐ **PARTIAL** | v3 strictness `947b215e6`; v4 caller-side `c432184e4`; v4.2 sub-block cloning `13a434ea0`; **v4 / let-floating R10 just FALSIFIED** (`c4c3e7edb` 2026-05-26: 0 lift candidates) |
| **Stage 5** | Hidden classes / attrset shapes | ✗ **KILLED 2026-05-23** | `fe7c17498` (#778): AttrSelect 2.24 % vs 10 % threshold |
| **Stage 6** | Polymorphic Inline Caches | ✗ **KILLED 2026-05-23** | Implicit (tied to Stage 5) |
| **Stage 7** | Selector thunks | ◯ **BLOCKED** | Gated on killed Stage 5/6 |
| **Stage 8** | Thin FFI + primops (parallel) | ◐ **SUBSTANTIAL** | V3-NATIVE arc (#795-#808) achieved **0 bridge crossings** on hello/bash/ifd-heavy-multi; haskell.nix unblocked via #803 H10 (`e364f7695`); haskell-nix-example 74 crossings remain (ForceAttr-dominated) |
| **Stage 9** | Module linking (content-addressed cells) | ✗ **KILLED 2026-05-22** | `37616ecc6` (#772): 1.17×/1.03× dedup vs 2× threshold |
| **Stage 14** | Error-message UX | ◯ **DEFERRED** | "post-perf + post-IFD"; still in perf phase |
| **Stage 15** | Per-line profiler UX | ◯ **DEFERRED** | "post-perf + post-IFD"; still in perf phase |
| **Stage 17** | Pattern-lint UX | ◯ **DEFERRED** | "post-perf + post-IFD"; still in perf phase |

### 2.2 Candidate future stages (10-13)

| Stage | Description | Status | Evidence |
|---|---|---|---|
| **Stage 10** | Salsa (incremental result cache) | ◐ **PARTIAL SUBSET** | #741 substrate validated (Phase 1-5); Phase 4b wall-positive (1.85×/1.91× faster); full salsa not justified — narrower form sufficient |
| **Stage 11** | HAMT (polymorphic attrset) | ⊘ **CANDIDATE** | Verification agent flagged load-bearing premises unverified; no measurement |
| **Stage 12** | JIT decision | ⊘ **DEFERRED-WITH-DATA** | Per `JIT_CONFIDENCE_2026-05-23.md`; dispatch ~5 % wall; revival triggers not active |
| **Stage 13** | Multi-core capabilities | ⊘ **CANDIDATE** | Per `PARALLEL_EVAL_CAPABILITIES_2026-05-18.md`; **R1 prereq just satisfied via trigger fire** |

### 2.3 Tier R (added 2026-05-26, per ARCHITECTURE_CRITIQUE)

| R# | Item | Status | Update |
|---|---|---|---|
| **R1** | Full de Bruijn IR (Phase L1) | 🔥 **TRIGGER VERIFIED FIRED** | `dcfbae871` 2026-05-26 V3_DBG_DESERIALIZE_VERIFY confirms 266-353 DIFFs/warm-pass, all positional-only. Schema 14 (`a7b41ddce`) closed 353→4 via PosIdx remap. R1 should be scheduled near-term; may be narrower than original 1-wk estimate |
| R2 | Stages 5/6 PIC revival | ⊘ Not triggered | |
| R3 | Stage 9 dedup revival | ⊘ Not triggered | Trigger B requires R1; R1 firing makes this measurable post-R1 |
| R4 | Stage 12 JIT revival | ⊘ Not triggered | |
| R5 | Stage 13 multi-core | ⊘ Pre-condition partially satisfied | R1 prereq satisfied; other triggers (process-parallelism < 30 %, etc.) not active |
| R6 | Whippet GC | ⊘ Not triggered | Per `BOEHM_DEPENDENCY_2026-05-21.md`: needs nursery default-on AND tenured Boehm > 10 % wall |
| R7 | mmap'd L2 cache | Priority dropped | Post-Phase 4b wall-positive |
| R8 | AOT distribution | ⊘ Not triggered | Nix-team coordination + schema stability |
| R9 | `.name`-class optimization | ⊘ Not triggered | Workload not user-facing primary |
| **R10** | Stage 4 v4 / let-floating | ✗ **FALSIFIED 2026-05-26** | `c4c3e7edb` 0 lift candidates / 12356 fails; joins kill pile |
| R11 | haskell.nix wall < 1.42× | ⊘ Not triggered | Memory (5.3× RSS) is binding constraint |

---

## 3. Key deltas since ROADMAP_TO_VISION_2026-05-15 (11 days)

### 3.1 Closures (4 items)

| Item | When | Impact |
|---|---|---|
| Stage 5 KILLED | 2026-05-23 | ~6 wks calendar reclaimed |
| Stage 6 KILLED (implicit) | 2026-05-23 | ~6 wks calendar reclaimed |
| Stage 9 KILLED | 2026-05-22 | ~5 wks calendar reclaimed |
| R10 Stage 4 v4 FALSIFIED | 2026-05-26 | Closes 0-elision question definitively |

**Total ~17 weeks of projected calendar reclaimed via Rule 0 discipline.**

### 3.2 Major landings

| Item | When | Impact |
|---|---|---|
| Stage 2 closed | 2026-05-22 | Pure-bytecode achieved on 119 workloads |
| V3-NATIVE arc | 2026-05-23 / 2026-05-24 / 2026-05-25 | 0 bridge crossings standard workloads; haskell.nix unblocked |
| #777 / #781b / #779 / #803 / #814 schema series | 2026-05-23 → 2026-05-25 | Schema 9 → 13; cache productionised; #815 truly resolved |
| Phase 4b wall-positive | 2026-05-24 | First wall-positive #741 result; 1.85×/1.91× faster on IFD workloads |
| Tier A 4/4 landed | 2026-05-26 | A1+A1a (mem attribution + scaffold); A2 V3_RELEASE (−8.4 % wall); A3 T1.1 instrumentation; A4 cache-coherence lint |
| R1 trigger verified | 2026-05-26 afternoon | de Bruijn IR work moves to scheduled near-term |
| Schema 14 | 2026-05-26 | PosIdx remap closed positional-only DIFFs 353→4 |

### 3.3 Major architectural docs added

| Doc | When | Purpose |
|---|---|---|
| MEASURE_TWICE_CUT_ONCE | 2026-05-23 | Methodology rule |
| WARM_EVAL_AND_INSTRUMENTATION | 2026-05-23 | warm-eval framing + V3_RELEASE spec |
| GC_VS_TW_ANALYSIS | 2026-05-23 | nursery default-on decision rules |
| MEMORY_REDUCTION_OPPORTUNITIES | 2026-05-23 | 5-agent memory audit |
| EVAL_CACHE_ARCHITECTURE | 2026-05-23 | mmap'd L2; §13 retraction post-Phase-4b |
| JIT_CONFIDENCE | 2026-05-23 | Stage 12 deferred-with-data |
| OPTIMIZATION_STRATEGIES | 2026-05-23 | 5-agent post-kill review |
| PROFILING_AUDIT + IMPROVEMENTS | 2026-05-24 | T1.1 / T1.2 / T1.3 / T2.x / T3.x specifications |
| FFI_AUDIT | 2026-05-24 | V3-NATIVE arc inputs |
| V3_TRUE_NATIVE_RCA | 2026-05-24 | Living RCA log |
| NEXT_STEPS_2026-05-25 + addenda | 2026-05-25 / 2026-05-26 | Tactical week-plan + Tier R + AR16-AR30 |
| ARCHITECTURE_CRITIQUE | 2026-05-26 | 4-agent cross-cutting review |
| CR1_CR2_AUDIT_RESULTS | 2026-05-26 | R1 trigger evaluation |
| DIRECTION_NOTE | 2026-05-26 | Progress vs direction observation |
| MEMORY_REDUCTION_AVENUES | 2026-05-26 | Post-falsification inventory |
| IDEAL_GC_DESIGN | 2026-05-26 | Hand-roll vs Whippet design exploration |

### 3.4 Architectural risk register growth

| State | Pre-ROADMAP | Post-ROADMAP | Now |
|---|---|---|---|
| Documented architectural risks | 0 (no AR list) | 15 (NEXT_STEPS §8.5) | **30** (post-ARCHITECTURE_CRITIQUE AR16-AR30) |
| Operating rules codified | 4 (Rule 0; V3-NATIVE; memory-first-class; falsification) | 8 | **12+** |
| Tier R trigger-gated items | 0 (didn't exist) | 0 | **11** |

The infrastructure for thinking about v3 has grown substantially. Most of this was needed; the architectural critique surfaced real gaps that the per-subsystem docs didn't capture.

---

## 4. Where ROADMAP framing has aged

The ROADMAP was written 2026-05-15. Things that the original framing didn't anticipate:

1. **The "perf phase → IFD phase → UX phase" sequencing is partially obsolete.** Stage 14/15/17 are gated on "post-perf + post-IFD" but the team is doing perf-AND-IFD simultaneously. Phase 4b is IFD-class work that shipped wall-positive *during* the perf phase.

2. **The "Stage 5/6 PIC achievable" framing was wrong.** Three weeks before, hidden classes + PICs were the central perf bet. They got killed by 2026-05-23 measurement; not a problem with the framing, but a clean refutation.

3. **Stage 8 (Thin FFI) was framed as "parallel, Weeks 9-44" continuous work.** Reality: it had a major sprint (V3-NATIVE arc, ~5 days, #795-#808 closure) that achieved most of the goal on standard workloads.

4. **The "Boehm tenured, Cheney nursery" model was assumed durable.** It still is, but Whippet (Stage 16 R6) escalation was added; the ROADMAP doesn't reflect this yet.

5. **The "convention-encoded invariants" failure mode wasn't recognised.** Four "RESOLVED → reopened" methodology incidents this week motivated MEASURE_TWICE_CUT_ONCE §5.7, ARCHITECTURE_CRITIQUE §6 invariant register, A4 cache-coherence lint, R1 firing. None of these were on the original ROADMAP.

6. **Memory-first-class framing landed mid-arc.** ROADMAP focused on wall ratios; memory framing was codified 2026-05-23 and is now the primary direction. The ROADMAP "Optimization targets" section (codified 2026-05-23) was added but stages weren't reorganised.

7. **The ROADMAP doesn't have a current-state lookup.** Each stage section is intrinsic; finding "what's done today" requires reading each stage. This snapshot doc closes that gap.

---

## 5. Where the team is genuinely AHEAD

- Stage 1 + Stage 2: completed in ~½ the planned time
- Stage 8 V3-NATIVE arc: substantially advanced
- Three Rule-0 kills (Stages 5/6/9): ~17 weeks reclaimed
- R10 fast falsification: 12356 fails → 0 lift candidates surfaced in a day
- Phase 4b shipped wall-positive
- Architectural rigor: CR1/CR2 audits, 4-agent critique, falsifier-per-commit discipline, two cache-coherence CI rules enforced
- Tier A items: 5-day plan executed in ~48 hours

## 6. Where the team is BEHIND

- Stage 3 nursery default-on: stuck (B2 falsified; selective untried)
- HNE 5.3× RSS gap: unchanged since first measurement; 5 falsifications no shipped reduction
- cardano-node M5: not re-measured since 2026-05-18 capability target
- Boehm 99.9 % free pages: known since #702 (2026-05-21); never tuned
- Phase E v0.2 stress-mode missed-root: known issue; blocks default-on flip
- Stages 14/15/17 UX pillar: still indefinitely deferred

## 7. Where the team is in MEASUREMENT-FIRST mode

Memory work has gone through 5 falsifications (A1b, Phase C v1/v2/v3, B2) with 0 RSS reduction shipped. Per `MEMORY_REDUCTION_AVENUES_2026-05-26.md` recommendation: ~4-5 day measurement-first week before more memory implementation. Items in this measurement-first phase:

- HNE NIX_VM_STATS bucket decomposition (≤ 1 hr; never done)
- T1.3 per-alloc-site for Thunks/Closures/ListVecs (1-2 d; never done)
- T1.2 elsewhere bucket decomposition (1 d; per AR23)
- T3.2 common-value tracer (1 d; never done)
- Boehm tuning spike — the 99.9 % free finding (1 d; underexplored)
- Lifetime audit on HNE (1 d; never done; retention not measured)

---

## 8. Open decisions (consolidated from DIRECTION_NOTE §3)

1. **R1 scheduling** — trigger fired this afternoon; Schema 14 partially addresses; R1 may be narrower than 1-wk estimate
2. **A1a Phase C** — three variants falsified; either pivot (per MEMORY_REDUCTION_AVENUES Category 1 measurement-first) OR persist with v4
3. **Cardano-node M5 ritual measurement** — strategic workload drift risk
4. **Boehm tuning spike** — 99.9 % free underexplored; potential 100-300 MB at near-zero cost
5. **Selective nursery** — B2 blanket failed; selective untried

---

## 9. What's next (recommended near-term)

**This week (days 1-3):**
- Schedule R1 implementation (~3-5 days estimated post-Schema-14; could be smaller than original 1-wk)
- HNE NIX_VM_STATS bucket decomposition (≤ 1 hr)
- Boehm tuning spike (1 d)
- Post-V3_RELEASE re-baseline measurement (≤ 1 hr)

**Next week:**
- R1 implementation (if scheduled)
- Memory measurement-first week (per MEMORY_REDUCTION_AVENUES)
- Cardano-node M5 ritual integration (~1 day cron script)
- CR2 CI lint (~30 LoC bash)

**End of cycle:**
- Memory measurement consolidation; targeted attacks per Week 1 findings (per MEMORY_REDUCTION_AVENUES §5)
- Selective nursery investigation (Stage 3 unblock attempt)
- ROADMAP_TO_VISION refresh (this snapshot informs the rewrite)

---

## 10. Honest limits

- **"Stages 1-2 done"** is true per commit signal; team-internal definition of "done" may differ
- **"~17 weeks reclaimed"** uses original ROADMAP estimates; new work has emerged to fill the time
- **"Schema 14 already partial-R1"** is structural observation; whether it counts as a portion of R1's 1-wk effort is not measured
- **"5 falsifications no shipped reduction"** is honest count but counts attempts, not lever-find iterations (the falsifications themselves are progress)
- **"99.9 % free"** is from #702 hello.drvPath; unverified on HNE post-current-state
- **"R1 may be narrower than 1-wk"** is informed speculation, not validated
- **The "AHEAD / BEHIND / MEASUREMENT-FIRST" cuts** are author judgments, not team-internal positions
- **The recommended near-term is not a commitment;** team may have different priorities I'm not aware of

---

## 11. Cross-references

- [`ROADMAP_TO_VISION_2026-05-15.md`](ROADMAP_TO_VISION_2026-05-15.md) — the doc this snapshots; refresh recommended per §4
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-22.md) — prior snapshot
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-23.md) — post-Stage-2 snapshot
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — tactical Tier A/B/C/D + Tier R + AR list
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — cross-cutting risk inventory; 15 new ARs
- [`CR1_CR2_AUDIT_RESULTS_2026-05-26.md`](CR1_CR2_AUDIT_RESULTS_2026-05-26.md) — R1 trigger evaluation source
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) — progress vs direction; 3 open decisions
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — measurement-first recommendation
- [`IDEAL_GC_DESIGN_2026-05-26.md`](IDEAL_GC_DESIGN_2026-05-26.md) — Tier 1 foundation items overlap with Stage 3 / Boehm tuning
- [`JIT_CONFIDENCE_2026-05-23.md`](JIT_CONFIDENCE_2026-05-23.md) — Stage 12 deferral
- [`OPTIMIZATION_STRATEGIES_2026-05-23.md`](OPTIMIZATION_STRATEGIES_2026-05-23.md) — Tier 1 items partially executed via Tier A
- [`EVAL_CACHE_ARCHITECTURE_2026-05-23.md`](EVAL_CACHE_ARCHITECTURE_2026-05-23.md) — Stage 10 partial subset

**Key commits this snapshot tracks:**
- `dcfbae871` R1 trigger verified fired (THIS afternoon)
- `c4c3e7edb` R10 / #776 Let-floating FALSIFIED
- `a7b41ddce` Schema 14 PosIdx remap (partial R1 ahead-of-time)
- `58be90cea` opt_strict_call_unthunk VarId sort (CR6 Option C pattern)
- `f2c254fd4` B2 nursery default-on FALSIFIED
- `46ce47c8a` A2 V3_RELEASE LANDED −8.4 % wall
- `20bd0dfcf` A3 T1.1 cache-hook instrumentation LANDED
- `521277ac9` + `7d14733c0` A4 cache-coherence CI lint LANDED
- `98ca953bb` + `6f8095cd5` + `2cf14fdce` A1a Phase A + B v1 + B v2
- `66b1061cd` + `0f24cda9e` + `920cda88c` A1 measurement + bucket split + A1b falsified
- `1b7496844` #815 truly resolved
- `e364f7695` #803 H10 killed via schema-11
- `3af813638` Stage 2 closed (#760)
- `37616ecc6` Stage 9 KILLED (#772)
- `fe7c17498` Stage 5 KILLED (#778)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
