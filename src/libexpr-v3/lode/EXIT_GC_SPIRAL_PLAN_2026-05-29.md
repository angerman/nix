# Exit the GC spiral — 4-week tactical plan

**Date:** 2026-05-29
**Author:** session synthesis (after 6th GC falsification)
**Status:** STRATEGIC TACTICAL PLAN — proposes exiting the GC-variant-spiral; pre-committed thresholds at every decision point
**Triggering question:** "Detailed step-by-step breakdown on what the team should be working on next."

Companion / supersession context:
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) — flat MS recommendation **falsified 2026-05-29** per `GC_DECISION_2026-05-29.md` / `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`
- [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md) — closed today by `L_TIME_SERIES_DATA_2026-05-29.md`
- [`STRING_DEDUP_AUDIT_2026-05-28.md`](STRING_DEDUP_AUDIT_2026-05-28.md) — spike pending
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) §Category 1 — quantified lever inventory
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — cache bucket sizing
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3, §5.7, §5.8 candidate
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — broader strategic plan this supplements (does not replace)

---

## 0. TL;DR

**Stop the GC-variant spiral. Spend 1 week quantifying orthogonal levers. Execute the top-2 in weeks 2-3. Re-evaluate GC in week 4 with new arena profile.**

The team has 6 GC falsifications (ditch-Boehm wall, Boehm tuning §6.2, periodic-GC, arena-dereg, Cheney, flat MS) and 0 MB shipped RSS reduction in 3 weeks. Starting Immix next would be the 7th iteration of the same loop. The orthogonal levers (cache eviction quantified 500-950 MB on HNE; string dedup unknown; mergeBindings pattern fix several-hundred-MB) have been displaced for 3 weeks.

**The hard goal: cardano-node M5 under the 4 GB watchdog.** Currently 5760 MB. Need ≥1.76 GB reduction. **No single lever known to deliver this; combined approach required.**

This plan: Week 0 stops the spiral and quantifies; Weeks 1-2 execute highest-yield lever; Week 3 executes second-highest; Week 4 integrates + re-evaluates GC + measures M5 against watchdog.

---

## 1. Honest framing

### 1.1 What's working

- **Methodology is sound.** Pre-committed thresholds, falsification rule, measure-twice — all operating correctly. The thresholds caught flat MS's failure before ship.
- **Infrastructure is solid.** Stage 1 (`tagIsPointer`), Stage 3 (`walkAllV3Roots`), Stage 5 (GcRoot), `bridge_root_registry`, `live_trace.cc`, `mark_sweep.cc` substrate — all reusable across future GC designs.
- **Falsification register is honest.** Each variant has a Rule-0 doc explaining what was killed.

### 1.2 What's not working

- **0 MB shipped RSS reduction in 3 weeks** (current arc).
- **The selection criterion for "next GC variant" has not itself been falsifiable.** Each variant fails on its own pre-committed gate; the META-claim "GC is the right primary lever" has never been the hypothesis under test.
- **Higher-quantified-yield levers have been deprioritized** for the entire arc.

### 1.3 Why I'm cautious about this plan

I wrote yesterday's confident GC_DESIGN_POST_CHENEY recommending flat MS with literature support, 3-agent synthesis, and pre-committed thresholds. Today flat MS falsified. My own design recommendation was wrong; the methodology caught it. **I should not now write a confident "do Immix" or "do mark-compact" plan.** This document instead proposes pausing the GC track and grounding the next 4 weeks in measurement-first parallel-lever-evaluation.

The team has ground-truth I don't have (engineer hours, undocumented integration context, what's actually painful in the code). This plan is a structured proposal; team owns the calibration.

---

## 2. The lever landscape (quantified vs unquantified)

| Lever | Quantified yield | Effort | Track status |
|---|---|---|---|
| **Cache eviction (Phase 4b LRU)** | **500-950 MB on HNE** (HNE_BUCKET_DECOMP) | 2-3 wk after 3 prereqs | "blocked by 3 prereqs" — untouched 3 wk |
| ~~**String dedup runtime**~~ | ~~unknown — 0-300 MB potential~~ | ~~0.5-1 d spike + 1-2 wk impl~~ | **NO-LEVER 2026-05-29** commit `2bffaacc5`: 7 MB hello / 36 MB HNE; <50 MB threshold |
| **mergeBindings pattern fix** | "several hundred MB" estimated | high; Phase C 3-pivot-falsified | guard memo in place |
| **fakeClo pool wire-back** | **measured -98.4 MB HNE / -704 MB M5** | ~1 d (DONE: commit `40e6abbdb`) | LANDED + KEPT. Original 2026-05-27 exclusion overridden 2026-05-29 by measurement. Conditional retirement: delete when Phase E v0.2 ships default-on OR Stage 6 production GC lands — see §4.3 amendment + `EXIT_WEEK1_DAY6-8_FAKECLO_2026-05-29.md`. |
| **mapAttrs 2-pair App chain** | **100 MB on HNE** (T1.3 Pairs) | per-site fix | known; not done |
| **Tiny capWiths ListVec** | **13 MB on HNE** (T1.3 Lists) | per-site fix | known; not done |
| **Phase E v0.2 ship-readiness** | **resolves 144 MB fakeClo + nursery generally** | multi-session per PHASE_E_V02_DAY2_FALSIFIED Paths A+B | blocked by +129-252 MB peak RSS regression on default-on attempt |
| **Strictness Stage 4 v4+** | reduces N upstream | 3-4 wk | plumbing exists; 0 elisions |
| **GC variant (Immix)** | projected ~300 MB hello, ~1200 MB HNE | 4-6 wk | NEXT in current spiral if not paused |

**Combined potential of known, quantified, non-GC levers: ~750-1200 MB on HNE** before any GC ships, before string dedup or strictness measurement. **Equivalent to or greater than the projected GC ceiling.**

---

## 3. Week 0 — STOP and QUANTIFY (days 1-5)

### 3.1 Day 1 — GC Pause Declaration

**Action:** Write `GC_PAUSE_2026-05-29.md` (Rule-0 doc) with:
- Hypothesis killed: *"GC is v3's primary RSS lever, addressable by sequencing GC variants until one ships"*
- Evidence: 6 falsifications, 0 MB shipped, equivalent or higher-yield orthogonal levers untouched
- Falsification cost: ~3 weeks of engineer time on GC variants
- What gets paused: Immix implementation; new GC design docs
- What continues: existing gated code (Cheney + flat MS behind opt-in gates — already non-disruptive); methodology (falsification rule, measure-twice); MS Phase 4 RESULTS doc closes the loop
- What changes: Week 0-3 plan focuses on orthogonal levers; GC re-evaluation in Week 4 with new data

**Acceptance:** doc lands; CLAUDE.md strategic table updated; `GC_DECISION_2026-05-29.md` cross-referenced.

**Effort:** ~2 hours.

**Why this matters per Rule 0:** explicit hypothesis kill prevents the spiral from continuing through inertia. Without this commit, "let's just start Immix" is the path of least resistance.

### 3.2 Day 2 — Cache eviction proof-of-concept measurement

**Action:** Use existing `NIX_V3_NO_DISK_CACHE=1` workaround (per HNE_BUCKET_DECOMP) to measure baseline cache-disable RSS impact.

**Procedure:**
1. Run `bench/m5-cron.sh --full` on hello.drvPath, HNE, M5 — gate-OFF baseline
2. Run same with `NIX_V3_NO_DISK_CACHE=1` — cache-disable
3. Hyperfine ≥10 runs each; capture peak_rss
4. Compute delta per workload

**Pre-committed acceptance (binary):**

| Result on M5 | Verdict | Implication |
|---|---|---|
| Δpeak ≥ 1500 MB | **HIGH YIELD** | Cache eviction is the priority-1 lever; Weeks 1-2 dedicated |
| 800-1500 MB | **MODERATE-HIGH** | Priority-1 still cache eviction; Week 1 dedicated |
| 300-800 MB | **MODERATE** | Schedule for Week 2; Week 1 takes the next-best lever |
| < 300 MB | **MARGINAL** | Cache eviction NOT top priority; reorder |

**Hard limit:** if `NIX_V3_NO_DISK_CACHE=1` breaks correctness (drvPath diverges from TW), abort — workaround is invalid as proxy.

**Effort:** 4-6 hours including hyperfine variance handling.

**Why M5 specifically:** the goal is M5 under 4 GB. HNE delta is a proxy; M5 is the target. Measure both.

### 3.3 Day 3 — ALREADY CLOSED: strings = NO-LEVER

**Status:** RESOLVED 2026-05-29 commit `2bffaacc5` per [[strings-no-lever-2026-05-29]] / `project_strings_attr_2026-05-29.md` in memory.

**Finding:** `NIX_V3_STRINGS_ATTR=1` measured total raw `allocChars` bytes: **7 MB hello / 36 MB HNE.** Upper-bound dedup savings ≤25 MB on HNE. **BELOW the 50 MB no-lever threshold from the audit doc.** Strings are 1.2% of arena vs Bindings 84%. Not a lever.

**Implication for Day 5 decision matrix:** drop string dedup from the comparison. Reduces the matrix to cache-eviction vs per-site-bundle.

**Day 3 reallocation:** use the day for **Boehm bucket investigation** + **arena-vs-elsewhere ratio re-measurement on M5 specifically.**

**Procedure:**
1. Run `NIX_VM_STATS=1` on cardano-node M5 (`bench/m5-cron.sh --full` extended) — capture v3_arena / boehm / elsewhere split on M5 (not just HNE)
2. Confirm M5 elsewhere bucket size; this anchors the cache-eviction Day-2 measurement
3. Sanity check Boehm bucket: is it bounded ~400 MB regardless of workload, or does it scale on M5?

**Pre-committed acceptance:**
- M5 bucket split documented with hyperfine variance
- If M5 elsewhere is < 500 MB → cache eviction is a smaller lever than expected; rebalance
- If M5 elsewhere > 1.5 GB → cache eviction is even higher-priority than HNE suggested

**Effort:** 4 hours.

**Why this matters:** the HNE bucket decomp showed elsewhere = 990 MB. M5 is bigger; I've been estimating elsewhere proportionally (~2-3 GB). If actually measured at M5, the cache-eviction yield estimate is grounded, not extrapolated.

### 3.4 Day 4 — mergeBindings + per-site lever assessment

**Action:** Quantify three known per-site levers without implementation.

**Sub-actions:**
1. **mergeBindings audit (1-2 hours):** run `NIX_V3_BINDINGS_ATTR=1` (already exists per #746) on M5; measure mergeBindings residency vs intermediate slack vs final live. Question: what fraction of the 82% Bindings-on-HNE pattern is reclaimable by pattern-fix vs structurally live?
2. **fakeClo dead-code (1 hour assessment):** estimate effort to revive pool OR retire residual code (current memory: 144 MB on HNE; ~1 day wire-back). Confirm in `T1_3_CLOSURES_ATTR_2026-05-27`.
3. **mapAttrs 2-pair (1 hour):** estimate effort for `primops.cc:1965+1970` per-site fix. Currently 100 MB on HNE.

**Pre-committed acceptance:**
- Per-site levers totaling ≥ 200 MB at ≤ 1 wk total effort = **bundle and ship in Week 2**
- mergeBindings pattern-fix: separate decision; needs Phase C 4-prerequisite plan per memory [[chain-bindings-phase-c-falsified]]

**Effort:** 4-6 hours.

### 3.5 Day 5 — Decision day

**Action:** Tabulate Day 2 + Day 4 measurements (Day 3 redirected per §3.3); pre-commit Week 1-2 plan.

**Updated decision matrix (string-dedup branch removed; closed 2026-05-29):**

```
Day 2 cache-eviction Δpeak on M5 ≥ 800 MB?
├── YES → Week 1-2: Cache eviction Phase 4b LRU (3 prereqs + impl)
│         Day 4 per-site bundle goes to Week 3
└── NO  → Day 4 per-site bundle ≥ 200 MB on HNE?
          ├── YES → Week 1-2: Bundled per-site fixes (fakeClo + mapAttrs + capWiths)
          │         Cache eviction goes to Week 3 if Day 2 Δpeak ≥ 300 MB
          └── NO  → BOTH orthogonal levers falsified → Immix becomes Week 1
                    Plan's own Rule 0 kicks in
```

**Pre-committed escalation:** if BOTH remaining measurements fail (cache <300 MB AND per-site <100 MB), the orthogonal-lever hypothesis ITSELF is falsified, GC track is genuinely the priority, and Immix becomes Week 1's work. **This is the falsifier for the meta-claim of this plan.**

**Note:** post-2026-05-29 strings-NO-LEVER, the orthogonal-lever space has narrowed to TWO candidates (cache eviction + per-site bundle), not three. The plan still holds; the decision tree is now binary at each level.

**Effort:** half-day write-up + commit + CLAUDE.md update.

---

## 4. Weeks 1-2 — Execute highest-yield lever (days 6-15)

### 4.1 If cache eviction (Phase 4b LRU) wins

**Day 6-7: Audit 3 prereqs blocking Phase 4b LRU.**

Per memory [[#701-deferred-emittreeattrs]]: prereqs are
1. CU pointer stability
2. ImportCacheEntry nursery concerns
3. Hash-bucket co-eviction

Action: read the deferred docs; for each prereq, classify as (a) trivially-resolvable (b) needs design (c) blocking-and-hard. Write `PHASE_4B_PREREQ_AUDIT_2026-05-30.md`.

**Day 8-10: Address smallest prereq.**

Pick the one classified as (a) or (b)-but-cheap. Implement. Verify with `all-v3-tests --quick` + `--core`.

**Day 11-13: Implement Phase 4b LRU.**

Design: LRU eviction policy for ImportCache + disk_cache in-memory shadow. Trigger: configurable threshold (default ~500 MB). Eviction: oldest entry by access time.

**Pre-committed SHIP gate (Day 14-15 measurement):**
- M5 peak RSS reduction ≥ Day 2 measured-Δ - 20% margin (acknowledges LRU vs full-disable is conservative)
- All `--quick 6/6` + `--core 15/15` PASS
- byte-identical drvPath on hello + HNE + M5
- Wall regression ≤ 10% on hello (cache miss cost)
- Wall regression ≤ 20% on M5 (cache miss cost)

**Falsification:** if SHIP gate fails after 2 weeks, write `PHASE_4B_FALSIFIED.md` + pivot to next-priority lever.

### 4.2 If string dedup wins

**Day 6-7: Hot-site enumeration.**

From Day 3 spike data: identify top 5-10 `allocChars` sites by bytes. Classify:
- High-dup, structural (drv hashes, store paths) → intern
- High-dup, derived (concat results) → consider compile-time partial
- Low-dup, large (file contents) → don't intern

**Day 8-10: Per-site intern table.**

Process-wide hash table (mirrors `globalSymbolTable` pattern). Heterogeneous lookup. Per-site dispatch: only intern at sites flagged in Day 6-7. NOT a universal intern.

**Day 11-13: Validate + measure.**

**Pre-committed SHIP gate:**
- HNE string bytes reduction ≥ 50 MB (lower than original threshold; accounts for intern table own cost)
- M5 peak RSS reduction ≥ 30 MB
- All correctness tests PASS
- Wall regression ≤ 5%

### 4.3 If per-site bundle wins

**Day 6-8: mapAttrs 2-pair App chain fix at `primops.cc:1965+1970`** (100 MB on HNE per T1.3 Pairs)
**Day 9-10: capWiths tiny ListVec fix at `vm.cc:3831`** (13 MB on HNE per T1.3 Lists)
**Day 11-13: Re-run T1.3 trio with sharper filtering** to find additional small per-site wins
**Day 14-15: Bundled measurement.**

**fakeClo pool revival — clause amended 2026-05-29 evening.**  Original clause (above this paragraph in prior revisions) EXCLUDED the wire-back per `T1_3_CLOSURES_ATTR_2026-05-27` + `PHASE_E_V02_DAY2_FALSIFIED_2026-05-27` user pushback: "144 MB is a SYMPTOM of Phase E v0.2 not shipping; reversing Phase D Step 12 (`ddb52d3a7`) is wrong direction; cure is Phase E ship-readiness."

The implementation diverged: `EXIT_DAY3-5_DECISION_2026-05-29 §3.1` quietly added the wire-back as bundle path #1; commit `40e6abbdb` (Day 6-8) landed it.  **Decision (user, 2026-05-29 evening): KEEP the wire-back.**  Measured -98.4 MB HNE / -704 MB M5 (deterministic on Δarena) is a real, standalone yield; the architectural objection is downgraded to a future cleanup, not a present-day prohibition.

**Conditional retirement criterion** (supersedes the inline `alloc.hh` "delete gate when SHIP-gate clears" criterion):

> The fakeClo pool (`Alloc::allocFakeClo` / `recycleFakeClo` / `kFakeCloMagic` / `NIX_V3_NO_CLOSURE_POOL` gate) MAY be retired AFTER the rest of v3's GC reaches a state where it reclaims the 144 MB unaided.  Concretely: when Phase E v0.2 ships default-on at acceptable wall+RSS, OR Stage 6 production precise GC lands.  Until then the pool stays default-on and the sentinel infrastructure stays in `alloc.hh`.

The ROADMAP_TO_VISION 2026-05-15 §"Retire `_pad = 0xFA5E`, `CFF_FAKECLO_TAINTED`, `kFakeCloMagic`" item is therefore **DEFERRED** with this condition attached, not cancelled.

**Pre-committed SHIP gate (revised down from prior 150/100 MB):**
- Combined HNE peak RSS reduction ≥ 80 MB
- M5 reduction ≥ 50 MB
- Tests PASS

**Honest scope note:** the per-site bundle on its own is ~113 MB on HNE (was claimed as 257 MB until 2026-05-29 fakeClo correction). If Day 5 picks this lever, it's the smaller win compared to cache eviction. Still positive ROI for ≤2 weeks of focused work; just don't oversell it.

---

## 5. Week 3 — Second lever (days 16-22)

**Action:** Execute whichever lever was second in Day 5's matrix.

**Same template as Weeks 1-2** with proportionally smaller timebox.

**Decision point at Day 22:**
- If both Week 1-2 + Week 3 levers shipped: combined M5 measurement Day 23-25
- If Week 3 lever falsified: skip to Day 23 with Week 1-2 ship only

---

## 6. Week 4 — Integration, M5, GC re-evaluation (days 23-30)

### 6.1 Day 23-25 — Combined measurement

**Action:** All shipped levers ACTIVE; measure end-to-end.

**Measurements:**
1. hello.drvPath: gate-OFF baseline; combined-ON
2. HNE: same
3. M5: same; **THIS IS THE KEY MEASUREMENT**
4. Wall regression aggregate
5. Byte-identical correctness

**Pre-committed:**
- M5 peak RSS < 4096 MB (under watchdog) → MAJOR WIN; declare M5 unblocked
- M5 peak between 4096-5000 MB → MODERATE WIN; estimate Δ-to-watchdog; identify residual lever
- M5 peak > 5000 MB → INSUFFICIENT; orthogonal levers didn't deliver target; pivot needed

### 6.2 Day 26-28 — GC re-evaluation with new data

**Action:** With orthogonal levers shipped + L(t) data (from Day 0's `L_TIME_SERIES_DATA_2026-05-29`) in hand, re-evaluate GC.

**Questions to answer:**
1. After caches evicted, what is the new arena fraction? (likely arena is now larger relative to total RSS)
2. Does L(t) distribution show low-L trigger moments?
3. Does sweep cost projection on the SMALLER post-eviction arena pass the F2 threshold?
4. Is there a different GC variant (sticky mark-bits over flat MS? selective Immix?) that the data suggests?

**Pre-committed:**
- If clear data-driven GC design emerges + projected ≥150 MB additional reduction → schedule for Week 5+
- If no clear data → GC track suspended indefinitely; v3 ships at post-orthogonal-lever level
- If M5 already under watchdog from Day 23-25 → GC becomes deprecated for now; revisit when next constraint hits

### 6.3 Day 29-30 — M5 watchdog validation

**Action:** Run cardano-node M5 under realistic CI conditions.

**Tests:**
1. `NIX_V3_MAX_HEAP=4G` cap — does eval complete?
2. ≥3 consecutive runs at different times (host-noise smoothing)
3. byte-identical to TW on the M5 expression
4. Wall vs TW within 4× (current ratio is ~3×; should not regress past 4×)

**Pre-committed deliverable:** `M5_WATCHDOG_VALIDATED_2026-06-XX.md` if pass; `M5_WATCHDOG_GAP_2026-06-XX.md` if not (with quantified residual to bridge).

---

## 7. Risks and abort criteria

### 7.1 Plan-level risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Day 2-4 measurements all return < threshold | low-medium | escalation in §3.5 → GC track resumes (Immix) |
| Day 6-15 lever implementation longer than 2 wk | medium | timebox; pivot to Week 3 lever |
| Wall regression on cache eviction exceeds threshold | medium | tune LRU threshold; partial-eviction policy |
| Combined effort introduces correctness regression | medium-low | run brute audit + nixpkgs matrix after each ship |
| Day 26-28 GC re-eval produces another spiral | medium | enforce: GC re-eval is INFORMATION not COMMITMENT; no new GC implementation in Week 4 |
| Team disagrees with plan structure | high (likely) | this is a proposal; team has ground-truth |
| Phase 4b LRU prereqs each require a week | medium | timebox; if all three are (c)-blocking, pivot to other lever |
| My orthogonal-lever yield estimates are wrong | medium | Day 2-4 is precisely the measurement that catches this |

### 7.2 Step-level abort criteria

Every step has explicit abort criteria in its section. Repeating the meta-rule: **if all three Day 2-4 measurements return below threshold (Day 5 escalation), this plan's meta-claim is falsified and Immix becomes Week 1's work.** That's the plan's own Rule 0.

### 7.3 What this plan does NOT cover

- **Wall performance work** — continues on its own track (M5 OP_ATTRS_SELECT_DYN, OP_TAIL_CALL optimization)
- **Correctness regressions** — continue running test suites; halt other work if regression
- **IFD / cardano-node feature work** — independent track
- **UX pillar (error messages, profilers)** — Stage 14/15/17 deferred per ROADMAP
- **Schema migrations** — independent
- **AOT cache work (R8a/R8b)** — independent track; can run in parallel if engineering capacity allows

---

## 8. Pre-commit honest acknowledgments

### 8.1 What I don't know

- Phase 4b LRU's 3 prereqs may each be a week of work. I've not read those design docs deeply. The Day 6-7 audit is precisely to find out.
- Cache eviction's `NIX_V3_NO_DISK_CACHE=1` proxy may not accurately predict LRU's impact. LRU keeps hot entries; full-disable doesn't. The measurement is informative directional, not absolute.
- M5 baseline (5760 MB) was measured on a specific host; today's measurement may differ ±10%. Cron ledger drift detection (`m5-cron.sh`) covers this.
- String dedup yield is genuinely unknown until Day 3 measures.
- The combined M5 measurement (Day 23-25) may show levers don't compose additively (cache eviction frees memory that arena GC would have freed; combined < sum-of-parts).

### 8.2 What might be wrong about this plan

- Cache eviction prereqs may take 6 weeks not 2. Then Week 1-2's lever choice is wrong.
- An engineer might find a fast GC variant (Whippet integration, e.g.) that ships in 1 week and obsoletes this whole plan. I don't know what's in `GC_DECISION_2026-05-29.md` yet.
- The team may have undocumented context that says "cache eviction is impossible because X". In that case Day 5 decision pivots to next lever.
- L(t) data (from today's spike) may reveal something I haven't seen that changes the GC calculus immediately.
- The 4-week timebox may be too aggressive; some levers reasonably take 3-4 weeks alone.

### 8.3 What I'm confident about

- 6 GC falsifications in 3 weeks is a structural signal, not a methodology problem.
- Cache eviction yield is quantified higher than any GC variant has projected.
- Per-site bundle (fakeClo + mapAttrs + capWiths) is 257 MB on HNE for ≤1 week of work — should ship regardless of other choices.
- The plan's Day 5 decision matrix is genuinely binary (pre-committed thresholds; no post-hoc adjustment).
- M5 under 4 GB watchdog requires multiple levers; no single lever has been projected to bridge the gap alone.

---

## 9. Acceptance criteria for this plan itself

This plan is itself a hypothesis. Its acceptance criteria:

- [ ] Day 5: orthogonal lever measurement complete; decision matrix triggers a unique Week 1-2 choice
- [ ] Day 15: first lever's SHIP gate result is binary (passed or falsified)
- [ ] Day 22: second lever's SHIP gate result is binary
- [ ] Day 25: M5 measurement is performed with quantified delta vs 5760 MB baseline
- [ ] Day 30: a strategic doc summarizes which levers shipped + remaining M5 gap

If by Day 30 the team has shipped ≥ 1 GB peak RSS reduction on M5 (measurable), this plan was correct in priority ordering. If shipped < 500 MB, this plan was wrong about lever yields and Immix-or-similar GC variant should have been priority.

---

## 10. Cross-references

### Strategic docs (read alongside this)
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — broader 8-week plan; this doc supplements weeks 0-3
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) §7 (scope reality) — anchors the "GC alone doesn't solve M5" claim
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — quantified lever inventory
- [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — cache bucket sizing
- [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — L(t) data from today's spike (feeds Day 26-28)
- [`PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`](PHASE_4_PRELIM_FALSIFIED_2026-05-29.md) — flat MS falsification
- [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) — current GC pivot doc (probable Immix); this plan PAUSES that pivot

### Methodology
- [`MEASURE_TWICE_CUT_ONCE_2026-05-23.md`](MEASURE_TWICE_CUT_ONCE_2026-05-23.md) §3 (pre-commit), §5.7 + §5.8 candidate (moment-vs-distribution), §3.8 (threshold recalibration)
- [[falsification-rule]] — every step has a kill criterion
- [[memory-first-class]] — RSS-primary framing
- [[same-host-bisect]] — applicable to M5 measurements
- [[head-5-counter-trap]] — applicable to all measurements

### Memory entries (most relevant)
- [[gc-design-post-cheney-2026-05-28]]
- [[string-dedup-audit-2026-05-28]]
- [[l-measurement-gap-2026-05-28]]
- [[chain-bindings-phase-c-falsified]]
- [[fakeclo-pool-dead-code]]
- [[#701-deferred-emittreeattrs]] (cache eviction prereqs)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
