# Direction note — 2026-05-26 (afternoon)

**Date:** 2026-05-26
**Author:** session synthesis
**Status:** observation — captures direction-check after Tier A execution
**Triggering question:** "Is the team making good progress in the right direction?"

Companion docs:
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — 4-agent cross-cutting review
- [`CR1_CR2_AUDIT_RESULTS_2026-05-26.md`](CR1_CR2_AUDIT_RESULTS_2026-05-26.md) — audit outcomes; R1 trigger evaluation
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — the plan being executed

This doc separates two distinct questions: (1) is progress strong? and (2) is direction sound? They're often conflated; the team's case demonstrates they need separate evaluation.

---

## 1. Position (TL;DR)

**Progress: exceptional.** All 4 Tier A items landed at least foundationally in ~48 hours after a 5-day plan committed. Methodology discipline sustained (falsifier per commit; ship-or-revert per pre-committed threshold).

**Direction: sound with 3 open decisions worth resolving this week.** Not problems; under-named decisions whose unresolved status creates ambient ambiguity in planning.

**The shift to watch:** the team's optimisation focus has correctly moved from wall-positive (#741 Phase 4b series) to memory-positive (A1 series), per `[[memory-first-class]]`. But the **metric of success has not yet shifted to memory measurements**. The 5.3× haskell-nix-example RSS gap is unchanged in published data; A1a Phase A+B are infrastructure; Phase C (the actual recovery) is deferred.

---

## 2. Progress assessment

Per planned vs actual:

| Item | Plan effort | Actual status | Outcome |
|---|---|---|---|
| **A1** HNE memory attribution | 1-2 d | LANDED (multiple commits: `66b1061cd` measurement + `0f24cda9e` bucket split + `920cda88c` A1b falsifier + `98ca953bb` Phase A scaffold + `6f8095cd5` Phase B v1 + `2cf14fdce` Phase B v2) | Measurement + framework + scaffold + consumer readiness; Phase C deferred |
| **A2** V3_RELEASE compile flag | 1 d | LANDED `46ce47c8a` | **−8.4 % wall** on alloc-heavy bench (4× over ≥ 2 % ship threshold) |
| **A3** T1.1 cache-hook instrumentation | 1-2 d | LANDED `20bd0dfcf` (#827) | Permanent gated-zero-cost infrastructure |
| **A4** Cache-coherence CI lint | 1 d | LANDED `521277ac9` + `7d14733c0` | Both schema-bump rules now CI-enforced |

**4 of 4 Tier A items landed in ~48 hours after the plan was committed (vs 5-day estimate).**

Plus parallel work outside the original plan:
- ARCHITECTURE_CRITIQUE 4-agent review (`ec63f1043`)
- CR1 + CR2 audits (`7fa7e79d9`)
- 15 new architectural risks documented (AR16-AR30)
- AR24 closed by CR2
- AR5 trigger evaluation (CR1 conditional fire)
- New operating rules codified (12 total this week)

Sustained discipline indicators:
- **Falsifiers per commit.** A1b implemented + measured + reverted cleanly (0.03 % hit rate). #806a retired + reverted within 3 hours when broken.
- **Pre-committed thresholds drive ship/revert decisions.** A2 shipped because −8.4 % > 2 % threshold; #791 reverted because −4.8 ± 7 ms < 8 ms threshold.
- **Critique-to-action loop is short.** I raised AR1 in NEXT_STEPS §8.5; A2 implementation explicitly preserved the always-on/env-gated distinction per the mitigation.

By standard engineering-team metrics (velocity + methodology + strategic clarity), the team is operating in the top decile of what I've observed.

---

## 3. Three open decisions worth resolving this week

These are not problems. They're *under-named decisions* whose unresolved status creates ambient planning ambiguity. Each can be resolved in ≤ 1 day of dedicated attention.

### 3.1 R1 (Full de Bruijn IR) — commit or defer — **CLOSED 2026-05-26 (evening)**

**Outcome:** **R1-trigger CLOSED unconditionally**; R1-Full deferred.

**Closure path:** the verification proposed in §3.1 not only ran but also drove three structural landings that closed the trigger entirely. V3_DBG_DESERIALIZE_VERIFY DIFFs on warm hello.drvPath: **353 → 4 → 0**.

| state                                    | commit       | DIFFs |
|------------------------------------------|--------------|-------|
| pre-fix baseline                         | `dcfbae871`  | 353   |
| Schema 14: sparse PosIdx remap           | `a7b41ddce`  | (subset) |
| AttrSet entries canonical-string-sort    | `9543834cc`  | 4     |
| AttrSet REC_SET canonical emit           | `b17ab3359`  | **0** |

Bytecode is now process-invariant across SymbolId / PosIdx / local-slot allocators. CU disk cache is cross-process byte-identical for everything measured. test/run-r1-trigger-verify.sh now asserts N_DIFF == 0 as the new regression guard.

**R1-Full status:** deferred. The original 1-week structural refactor of IR is no longer urgent — R1-trigger closure removed the correctness motivation. R1-Full's remaining motivation is the cleaner architecture (Stage 13 parallel eval prereq, IR-subtree dedup). Triggers carried over to NEXT_STEPS §6.5 R1-Full section.

**Downstream effects of R1-trigger closure:**
- T1.f (AOT distribution schema-stability trigger): now **obsolete** — bytecode IS process-invariant at the level R8a needs. R8a Phase 1 spike can proceed without R1-Full as a prereq.
- AR8 (schema-stability vs AOT distribution): R8a path no longer needs the "Full variant" hedge; R8b still benefits from R1-Full eventually.
- The §6.5 R1 entry in NEXT_STEPS has been split into R1-trigger (closed) and R1-Full (deferred with carry-over triggers).

### 3.2 A1a Phase C — when does the actual memory recovery land?

**Current status:** A1a Phase A (scaffold `98ca953bb`) + Phase B (consumer readiness `6f8095cd5` + `2cf14fdce`) landed. Phase C — the actual `mergeBindings` → Chain conversion — explicitly deferred per `fbf995dbe`.

**Why this matters:** Phase A + B are INFRASTRUCTURE. They don't move the memory needle. Phase C is the work that converts the 1M mergeBindings call sites to use the Chain representation, recovering the documented 200 MB - 1 GB target on haskell-nix-example RSS.

**Until Phase C lands, the 5.3× RSS gap is unchanged.** The memory-first-class framing remains hypothetical in terms of measured outcomes.

**Possible orderings:**
- Phase C right after A3 → memory recovery measurement this week
- Phase C gated on R1 decision (3.1) → memory recovery delayed by ≤ 1 week
- Phase C gated on B2 nursery default-on data → memory recovery delayed by ≤ 1 week
- Phase C gated on something else → delay unknown

**Recommendation:** make Phase C scheduling explicit. Even "Phase C is the next item after R1 decision" is better than "deferred." Suggest writing the Phase C trigger explicitly in NEXT_STEPS §3 Tier A continuation.

### 3.3 Cardano-node M5 re-measurement — ritual cadence

**Current status:** last data point is the original capability target of 919 MB peak vs 4 GB watchdog. Since then several defaults have flipped:
- Phase 4b IFD cache default-on (`d22e1bfd3`)
- Formals bridge default-on (`d22e1bfd3`)
- V3_RELEASE not active by default but available
- Schema 13 cache (perf-positive on hello.drvPath)
- Multiple memory infrastructure changes (Phase A/B scaffold)

**No published cardano-node M5 re-measurement** in the visible commit history.

**Why this matters:** cardano-node M5 is the project's strategic workload (per `CARDANO_NODE_FEASIBILITY_2026-05-18.md`). It's NOT in the standard test suite. Optimisations measured on hello.drvPath / firefox.drvPath / haskell-nix-example may not generalise. The 4 GB watchdog headroom is finite.

**Recommendation:** establish a **ritual cadence** — every 1-2 weeks OR after every default-flip, re-measure cardano-node M5 peak RSS + wall ratio. Even a single hyperfine + RSS log per cycle. Could be a `bench/cardano-node-m5-cron.sh` script wired into nightly CI per `CR4` in `ARCHITECTURE_CRITIQUE_2026-05-26.md` §8.1.

Without this ritual, cardano-node M5 status drifts silently while the team optimises for the in-suite workloads.

---

## 4. Bigger picture observation

The team's optimisation focus has shifted from **shipping wall-positive correctness + cache infrastructure** (the past 2 weeks' arc: #777 / #779 / #781b / #803 / #814 / Phase 4b series / V3-NATIVE arc) to **shipping memory infrastructure** (the past 48 hours' arc: A1+A1a Phase A/B / A2 V3_RELEASE).

This is the right pivot per `[[memory-first-class]]` (codified 2026-05-23).

**But the metric of success hasn't fully shifted.** Wall ratios on hello.drvPath are well-trodden (1.41× → 2.55× regression → 1.67× post-#814). Memory ratios are stale:
- hello.drvPath: 1083 MB peak (post-#751/#752, pre-Tier B reduction)
- haskell-nix-example: **3003 MB peak (5.3× TW); UNCHANGED since first measurement**
- cardano-node M5: **919 MB; UNCHANGED since capability target**

**Until A1a Phase C lands AND the haskell-nix-example RSS is re-measured, the memory-first-class framing is operationally a position-paper, not a track record.** The track record has wall-positive achievements (Phase 4b, V3_RELEASE) and memory-INFRASTRUCTURE achievements (A1a Phase A/B), but not yet memory-OUTCOMES achievements.

**This isn't a criticism of the work** — building infrastructure before applying it is correct. **It's an observation about WHERE the next "moment of truth" is.** Phase C is the moment.

---

## 5. Honest balance

If "is the team in the top 10 % of engineering teams I've observed" — yes, clearly:
- Tier A in ~48h vs 5-day plan
- Falsifier per commit
- Ship-or-revert discipline sustained
- Architectural critique surfaced 15 real risks not captured by per-subsystem docs
- Doc → action loop demonstrably working

If "are there decisions on the table that, if made promptly, would compound gains" — also yes:
- R1 verification this week → decision
- A1a Phase C scheduling → memory recovery measurement
- Cardano-node M5 ritual measurement → strategic-workload drift prevention

Both are simultaneously true. The progress is excellent AND the direction has open decisions. **Acknowledging both honestly is the framing that lets the team see what's been done vs what remains to decide.**

The pattern this doc captures: **distinguish progress (work shipped) from direction (which next big thing to commit to).** Conflating the two leads to either complacency ("we're shipping fast, all is well") or pessimism ("there are open questions, we're stuck"). Neither matches reality.

---

## 6. Recommended actions (concrete, this week)

1. ~~**Today/tomorrow:** run V3_DBG_DESERIALIZE_VERIFY re-run~~ — **DONE 2026-05-26 evening.** R1-trigger closed unconditionally; see §3.1 outcome.
2. **Write Phase C trigger explicitly in NEXT_STEPS §3 Tier A.** Even if deferred, the trigger should be explicit ("Phase C scheduled after R1 decision" or similar). With R1 decision now made (decision #1 closed via trigger closure), Phase C scheduling is unblocked.
3. **Wire cardano-node M5 measurement into nightly bench cadence.** ~1 day script + cron integration. Becomes a passive guard against strategic-workload drift. Now elevated to #1 open decision after #1 closure.
4. **Land CR2 CI lint independently** (~30 LoC, ≤ 1 day). Already approved direction; can ship parallel to other work.
5. **NEW (post-R1-trigger closure):** R8a Phase 1 spike Day 1-3 (NIX_V3_AOT_BUILD_MODE flag). Bytecode is now process-invariant; the strategic argument in `AOT_DISTRIBUTION_2026-05-26.md` §7.1 can begin its 2-3 week spike without R1-Full as a prereq.

---

## 7. What this doc is NOT

- **Not a roadmap update.** That's `NEXT_STEPS_2026-05-25.md`; this is observational.
- **Not a new architectural concern.** All concerns are tracked in `NEXT_STEPS §8.5 AR list` + `ARCHITECTURE_CRITIQUE_2026-05-26.md`. This doc surfaces *un-named decisions*, not *new risks*.
- **Not a critique of the team's velocity or quality.** Both are excellent. The critique is exclusively about *which decisions remain implicit when they could be explicit*.
- **Not a recommendation to slow down.** The team should keep the cadence; the three decisions in §3 are ≤ 1 day each.

---

## 8. Honest limits

- **The "top decile" claim in §1 is subjective.** Based on conversation-arc observation; not a calibrated benchmark.
- **The three open decisions may already have implicit answers** the team has discussed off-doc that I'm not aware of. If R1 is already mentally decided (either way), §3.1 is moot. Worth checking before relying on this doc.
- **"Cardano-node M5 hasn't been re-measured" is from commit history search.** If measurements exist outside commits (Slack, internal channels), this point is wrong.
- **A1a Phase C may already be in progress** under a different commit message. The "Phase C deferred" line from `fbf995dbe` is the explicit signal I used.
- **The framing "progress vs direction"** is one cut; other cuts exist (e.g., "tactical vs strategic," "short-horizon vs long-horizon"). The progress/direction cut captures the un-named-decisions pattern but isn't unique.

---

## 9. Cross-references

- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — 4-agent review; AR16-AR30 risk additions
- [`CR1_CR2_AUDIT_RESULTS_2026-05-26.md`](CR1_CR2_AUDIT_RESULTS_2026-05-26.md) — R1 trigger evaluation; AR24 closure; §2.4(a) verification path
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — the plan; multiple sections to update post-R1 decision
- [`HNE_MEMORY_ATTRIBUTION_2026-05-26.md`](HNE_MEMORY_ATTRIBUTION_2026-05-26.md) — A1 measurement source; 200 MB - 1 GB target
- [`MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md`](MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md) — A1a Phase C target work
- [`CARDANO_NODE_FEASIBILITY_2026-05-18.md`](CARDANO_NODE_FEASIBILITY_2026-05-18.md) — strategic workload definition
- `[[memory-first-class]]` (memory file) — the framing this doc cross-checks

**Commits referenced:**
- `46ce47c8a` A2 V3_RELEASE LANDED (−8.4 % wall)
- `20bd0dfcf` A3 T1.1 instrumentation LANDED
- `521277ac9` + `7d14733c0` A4 cache-coherence CI lint LANDED
- `98ca953bb` A1a Phase A scaffold
- `6f8095cd5` + `2cf14fdce` A1a Phase B v1 + v2
- `fbf995dbe` Phase C deferred (the explicit signal for §3.2)
- `920cda88c` A1b FALSIFIED (discipline indicator)
- `ec63f1043` ARCHITECTURE_CRITIQUE landed
- `7fa7e79d9` CR1 + CR2 audit results landed
- `d22e1bfd3` Phase 4b + formals-bridge default-on (defaults-flipped reference for §3.3)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
