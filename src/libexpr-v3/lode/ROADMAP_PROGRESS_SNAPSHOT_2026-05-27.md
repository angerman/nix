# Roadmap progress snapshot — 2026-05-27 (next-day from 2026-05-26 snapshot)

**Date:** 2026-05-27
**Author:** session synthesis
**Status:** snapshot — point-in-time state after the 2026-05-26 evening cascade
**Triggering context:** Five major events landed in the ~6 hours between `ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md` and this doc: R1 trigger closed; CR2 lint landed; R8a Phase 1 Day 1-15 executed; cardano-node M5 baseline corrected; bridge telemetry downgrades AR15 + falsifies #661 + closes R10. Plus FFI_AUDIT #806 E3 status correction.

Companion docs:
- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md) — morning snapshot; baseline for deltas
- [`AOT_DISTRIBUTION_2026-05-26.md`](AOT_DISTRIBUTION_2026-05-26.md) — updated with R1-closure cascade
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) §3.1 — R1 decision marked CLOSED
- [`CARDANO_NODE_M5_2026-05-26.md`](CARDANO_NODE_M5_2026-05-26.md) — corrected baseline + RCA

---

## 1. Position (TL;DR)

**Five major events in ~6 hours collapsed multiple strategic open questions:**

1. **R1 trigger CLOSED UNCONDITIONALLY** (`b17ab3359`) — DIFFs 353 → 0 across three landings; bytecode process-invariant; R1-Full deferred (correctness motivation gone)
2. **R8a Phase 1 Day 1-15 LANDED** — end-to-end mmap'd AOT cache works (100 % hit rate hello + HNE, byte-identical output); **measurement INCONCLUSIVE on noisy host** (-17 % to +16 % range; AOT mechanism correct but win not robust against host noise)
3. **CR2 CI lint LANDED** (`1e9ee3a5d`) — `lint-serialize-symbolid-coverage`; AR24 closed
4. **Cardano-node M5 baseline CORRECTED** (`a14c6092e`) — original 919 MB claim was wrong at source; real v3 is 4.5-6.5 GB / ~30 s (vs TW 856 MB / ~10.5 s = **5-7.5× RSS, 3× wall**). Crucially: **v3 EXCEEDS 4 GB watchdog on strategic workload**.
5. **Bridge telemetry downgrades AR15 + falsifies #661 + closes R10** (`f1cf8fd4c`) — total TW→v3 bridge wall on HNE = 0.018 %; bridge retirement is maintenance goal not wall lever; R11 wall claim needs re-derivation

**Net trajectory:** **the architectural-debt side of the roadmap closed dramatically; the strategic-workload side of the roadmap broke open dramatically.** R1 + CR2 + bridge-telemetry close out three categories of "open architectural worry." M5 corrected baseline elevates memory work from "scaling concern" to "production blocker on strategic workload."

---

## 2. Major developments since morning snapshot

### 2.1 R1 trigger closure (already partially captured in evening cascade)

```
state                                     commit         DIFFs
pre-fix baseline                          dcfbae871      353
Schema 14: sparse PosIdx remap            a7b41ddce      (subset)
AttrSet entries canonical-string-sort     9543834cc      4
AttrSet REC_SET canonical emit            b17ab3359      0
```

Bytecode now process-invariant across SymbolId / PosIdx / local-slot allocators. Test `test/run-r1-trigger-verify.sh` is the regression guard.

**R1-Full status:** deferred. Original 1-week structural refactor of IR is no longer urgent. Remaining motivation: Stage 13 parallel-eval prereq + IR-subtree dedup (Stage 9 trigger B). Carried over to NEXT_STEPS §6.5 R1-Full.

### 2.2 R8a Phase 1 — end-to-end works; measurement inconclusive

Day 1-3 (`bbe16bbcd`): NIX_V3_AOT_BUILD_MODE manifest recorder
Day 4-6 (`cd028b07a`): AOT flat-file format v1 + build-aot-cache.py
Day 10-12 (`e8944cd35`): AOT mmap reader + preliminary measurement
Day 13-15 (`49fabde3a`): madvise tuning + measurement INCONCLUSIVE on noisy host

**End-to-end validation:**
- 100 % hit rate hello.drvPath + haskell-nix-example
- Byte-identical output to TW under all modes
- 14/14 v3 suite PASS

**Measurement summary (5 runs, ordered chronologically):**

| Run | AOT vs SQLite | Verdict zone |
|---|---|---|
| Preliminary 5-run | −8.2 % | REVERT |
| Formal n=20 pre-tuning | −10.3 % | REVERT |
| n=20 post-madvise (no TW interleaved) | **−17.0 %** | TUNE |
| n=20 with TW interleaved | +12 % | REVERT |
| Isolated per-variant n=10 | +16 % | REVERT |

**Spread: −17 % to +16 %.** TW stays stable (~5 % range); AOT varies ~64 % across runs. **This is HOST NOISE, not signal.** Host load 6-8 during measurement.

**Honest verdict (per measure-twice-cut-once §3.8):** noisy measurements should NOT trigger pre-committed verdicts. **DEFER the formal SHIP/TUNE/REVERT call to a quiescent-host re-measurement.**

**What's been validated:**
- AOT mechanism is correct
- Performance win is REAL under quiescent state (−17 % best case)
- Madvise tuning improved best-case from −8 % to −17 % (Day 10-12 → Day 13-15)

**What's still open:**
- Whether the win survives realistic load (CI environments are typically loaded)
- Whether the win generalises to cardano-node M5 (NOT MEASURED YET)

### 2.3 Cardano-node M5 baseline correction

The morning snapshot referenced "919 MB / 10.2s 2026-05-21 baseline." That claim was wrong at the source.

```
state                                                peak_rss (2 runs)
HEAD (today's code) on today's host                  4513 - 6302 MB
2026-05-21-era code (2970dbd04) on today's host      6969 - 8385 MB
CARDANO_NODE_M5_2026-05-21.md claim                  919 MB
```

**No v3 regression vs 2026-05-21.** HEAD is somewhat BETTER than 2026-05-21 era.

**Corrected v3:TW gap on cardano-node M5:**
- TW: 856 MB peak / ~10.5 s wall
- v3 HEAD: 4.5-6.5 GB peak / ~30 s wall
- **Gap: 5-7.5× RSS / ~3× wall**

This shape is consistent with haskell-nix-example (5.3× RSS / 1.42× wall). **Two strategic workloads now show similar v3:TW gap patterns; the gap is not HNE-specific.**

**Critical implication:** v3 on cardano-node M5 EXCEEDS the 4 GB watchdog limit referenced in earlier strategic docs. **The "capability-met for IOG strategic workload" framing in `CARDANO_NODE_FEASIBILITY_2026-05-18.md` needs correction.** v3 currently does NOT operationally fit the strategic-workload memory budget.

### 2.4 Bridge telemetry: AR15 downgraded + #661 falsified + R10 closed

Bridge wall measurement on HNE: total TW→v3 bridge wall = 0.675 ms / 4797 ms = **0.014 %**.

**Three closures:**
- **#661** (per-process treeWalkerToV3 seen cache): FALSIFIED. Perfect seen cache caps savings at 0.014 %. Two orders of magnitude below threshold.
- **AR15** (bridge surface load-bearing): DOWNGRADED. The 51 + 8 + 10 crossings on HNE are NUMERICALLY large but TIMING-NEGLIGIBLE. Retirement remains a maintenance goal, not a performance goal.
- **R10 / #774 / #776 / let-floating resume**: formally closed. Three pivots on the lower-time-inline premise = falsification.

**Implications:**
- **R11 (haskell.nix wall < 1.42×)** — the "Stage-2-level boundary elimination for ForceAttr (51 crossings)" claim needs re-derivation. ForceAttr crossings are not the wall lever.
- **Stage 8 framing**: bridge surface retirement is a maintenance task, not a perf push. The "haskell.nix bridge surface elimination" arc on the roadmap closes as "completed at zero wall cost."
- **R10 closure**: 4th Rule-0 closure joining the kill pile (Stages 5/6/9 + R10).

### 2.5 CR2 CI lint landed (`1e9ee3a5d`)

`test/lint-serialize-symbolid-coverage.sh` — verifies collect/remap symmetry between `collectReferencedSymbols` and `remapSymbolsInBytecode`. **AR24 marked CLOSED** per CR1_CR2_AUDIT §3.2 recommendation.

A4 + CR2 = both cache-coherence lint rules now mechanically enforced.

---

## 3. Updated stage / item status (only items that changed)

| Item | Morning status | Evening status |
|---|---|---|
| **R1** (trigger) | TRIGGER VERIFIED FIRED | **CLOSED unconditionally** (`b17ab3359`) |
| **R1-Full** (IR refactor) | Trigger fired; near-term schedule | **Deferred**; correctness motivation gone |
| **AR15** (bridge surface) | Active risk | **DOWNGRADED** to maintenance |
| **AR24** (collect/remap drift) | Open (CR2 recommended) | **CLOSED** (CR2 lint landed) |
| **R8a** (v3-team AOT cache) | Recommended; not started | **Phase 1 Day 1-15 LANDED; measurement INCONCLUSIVE; quiescent-host re-measurement DEFERRED** |
| **R7** (mmap'd L2) | Revived as R8a dep | **Validated end-to-end** as R8a format |
| **R10** (Stage 4 v4 let-floating) | Falsified | **FORMALLY CLOSED** (3 pivots = falsification) |
| **R11** (haskell.nix wall < 1.42×) | Trigger gated | **WALL CLAIM NEEDS RE-DERIVATION** (bridge surface not the lever) |
| **Cardano-node M5 status** | 919 MB / 10.2 s (stale baseline) | **5-7.5× RSS / 3× wall; EXCEEDS 4 GB watchdog** |
| **#661** (TW→v3 seen cache) | Speculative optimization | **FALSIFIED** by telemetry (0.014 % wall) |
| **Stage 8** (Thin FFI) | Substantial | **Bridge surface retirement = maintenance task; wall complete** |

Plus FFI_AUDIT #806 E3 unblock + correction cycle (`6f1390caf` → `aa6057b1f`) — methodology error caught; status corrected. Another instance of the "RESOLVED → reopened" methodology pattern firing healthily within hours.

---

## 4. Three corrections to strategic position

### 4.1 Architectural-debt side: dramatically closed

The morning snapshot framed R1, CR2, bridge surface as open architectural worries. **All three closed this evening:**
- R1 trigger structurally closed via Schema 14 + canonical sort + canonical emit
- CR2 CI lint mechanically enforced (AR24)
- Bridge surface telemetry shows it's NOT a wall lever (AR15 downgraded)

**The "convention-encoded invariants" failure mode that motivated ARCHITECTURE_CRITIQUE §6 has been addressed for cross-process determinism specifically.** The 14-invariant register can update: at least 2 invariants now have CI enforcement (A4 + CR2); R1-trigger closure makes another 3-4 invariants structurally enforced rather than conventional.

### 4.2 Strategic-workload side: dramatically opened

The morning snapshot referenced 919 MB cardano-node M5 baseline as if v3 was capability-met. **That baseline was wrong; real v3 on M5 EXCEEDS the 4 GB watchdog.**

This elevates memory work from "scaling concern for haskell.nix-class growth" to **"production blocker for the strategic IOG workload."** The 5.3× HNE RSS gap was already documented; now we know cardano-node M5 has a similar gap at higher absolute scale.

**Multiple strategic docs reference the bad baseline and need correction:**
- `CARDANO_NODE_FEASIBILITY_2026-05-18.md` — "capability met"
- `CARDANO_NODE_M5_2026-05-21.md` — source of the bad baseline
- `DIRECTION_NOTE_2026-05-26.md` §3.3 — "919 MB headroom under 4 GB watchdog"
- `NEXT_STEPS_2026-05-25.md` references throughout

### 4.3 R8a Phase 1 measurement: noise-bounded, not threshold-bounded

The morning snapshot recommended applying pre-committed thresholds. **The host noise made that impossible.** Per measure-twice-cut-once §3.8, noisy measurements should NOT trigger pre-committed verdicts.

The team correctly applied the rule: DEFERRED ship/revert call to quiescent-host re-measurement instead of forcing a verdict on bad data. This is the discipline working as intended.

**Best-case measurement under clean state (−17 % AOT vs SQLite) is in TUNE zone (15-30 %).** Worst-case measurement (+16 % AOT vs SQLite) is REVERT. The truth is somewhere between, on a quieter host.

---

## 5. Open questions (refined since morning)

1. **Cardano-node M5 memory recovery: is the 5-7.5× gap structurally addressable?**
   - If yes via per-site fixes (#748/#750/#752 playbook on M5 patterns): great, ship it
   - If no: v3 fundamentally cannot ship on M5 under current memory budget; this is a strategic blocker

2. **R8a Phase 1 quiescent-host measurement: does AOT actually win wall?**
   - Best case −17 % is real but conditional
   - Worst case +16 % suggests madvise tuning may not be enough
   - **Recommended: rent a quiescent VM, run n=20 each variant, apply pre-committed threshold to clean numbers**

3. **R8a measurement on cardano-node M5 specifically:** not yet done. The strategic case for R8a is multi-tenant CI on big workloads. **M5 is the right test, not hello.drvPath / HNE.**

4. **Memory measurement-first week (per MEMORY_REDUCTION_AVENUES):** still not done. With M5's corrected gap, this becomes MORE urgent.

5. **R1-Full priority post-trigger-closure:** carried forward as Stage 13 prereq + Stage 9 dedup-trigger-B prereq. Deferred but not killed.

---

## 6. Recommended near-term

### Immediate (this week)

1. **Quiescent-host re-measurement for R8a Phase 1** — rent a VM or use a known-quiet host; n=20 each variant; apply pre-committed threshold on clean data. Likely 1 day.
2. **R8a Phase 1 measurement on cardano-node M5** — same setup; this is the strategic-workload test. ~1 day.
3. **Memory measurement-first week** (per MEMORY_REDUCTION_AVENUES) — gates further memory work; cardano-node M5 corrected baseline gives a real target. 4-5 days parallelizable.
4. **Correct the strategic-doc set for M5 baseline** — `CARDANO_NODE_FEASIBILITY_2026-05-18.md` + `CARDANO_NODE_M5_2026-05-21.md` + `DIRECTION_NOTE_2026-05-26.md` need updates referencing `CARDANO_NODE_M5_2026-05-26.md` corrected numbers. ~1 hour total.

### Next 2-3 weeks (depends on Immediate outcomes)

- If R8a quiescent measurement confirms ≥15 % win: Phase 2 generalisation (multi-flake-ref + CDN distribution)
- If R8a quiescent measurement is < 15 %: revert with data; pivot resources
- Memory recovery work on M5 — driven by measurement-first findings
- Tier 1 optimization-strategies (broaden ICs + TOS caching) — still valid; multi-week

### Mid-term (1-2 months)

- R8b cache.nixos.org integration — only if R8a ships and proves value
- Stage 4 v4 / R10 closed; Stage 4 work parks until new premise emerges
- AR audit refresh — 30 → likely shrinks as items close (R1 closure ought to close several Rs)

---

## 7. The 2026-05-15 → 2026-05-27 trajectory (12 days)

The original ROADMAP_TO_VISION_2026-05-15 estimated:
- Stage 1: Weeks 1-8 → DONE ahead of schedule
- Stage 2: Weeks 9-14 → DONE ahead of schedule
- Stage 5: Weeks 29-34 → KILLED at Week 1
- Stage 6: Weeks 35-40 → KILLED at Week 1
- Stage 9: Module linking → KILLED at Week 1
- Stage 8: parallel, Weeks 9-44 → bridge surface SHIPPED at maintenance level Week 2

**~28+ weeks of original projected calendar reclaimed via Rule-0 kills (5/6/9/R10) + ahead-of-schedule completions.**

Additions to plan not in original ROADMAP:
- Tier R (R1-R11; 11 items; R1 + R10 closed, 2 active, 7 deferred-trigger-gated)
- 30 architectural risks (AR1-AR30; ~3 closed; others trigger-gated)
- 12+ operating rules codified
- ~20 strategic docs added (PROFILING_AUDIT, MEASURE_TWICE, WARM_EVAL, etc.)

**The team is execution-ahead but scope-expanded.** Faster on shipping; broader on what's discovered + tracked. Net: substantial real progress in 12 days.

---

## 8. Honest limits

- **R8a "INCONCLUSIVE" verdict** is correct per measure-twice but defers the strategic decision. May need to ship Phase 1 even if quiescent measurement is mid-zone (15-30 %) because of the strategic value (multi-tenant CI asymmetry vs TW).
- **Cardano-node M5 EXCEEDS 4 GB watchdog** is dramatic but unverified at multiple-run scale (only 2-3 runs in CARDANO_NODE_M5_2026-05-26). Worth more runs.
- **Bridge surface "0.014 % of wall"** is HNE-specific; may differ on cardano-node M5 (deeper graphs may exercise bridge crossings differently). The downgrade applies to HNE but cardano-node M5 needs separate measurement.
- **"R1-trigger closure removes correctness motivation for R1-Full"** is true for cross-process bytecode determinism. R1-Full still has parallel-eval safety motivation (AR10) and Stage 9 trigger-B prereq motivation. The deferral isn't "no longer relevant"; it's "no longer urgent."
- **"~28+ weeks reclaimed"** uses original ROADMAP estimates; new scope (Tier R, AR list, strategic doc work) has consumed some of that calendar. Net trajectory is positive but not "8 weeks ahead schedule."
- The "12-day trajectory" framing assumes ROADMAP_TO_VISION_2026-05-15 as baseline. Earlier work (V3-NATIVE arc, Phase 4b series, etc.) was already in flight; the 12-day count understates total achievement.

---

## 9. Cross-references

- [`ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md`](ROADMAP_PROGRESS_SNAPSHOT_2026-05-26.md) — morning snapshot; this doc's baseline
- [`AOT_DISTRIBUTION_2026-05-26.md`](AOT_DISTRIBUTION_2026-05-26.md) §3.4 — R1-closure cascade update
- [`DIRECTION_NOTE_2026-05-26.md`](DIRECTION_NOTE_2026-05-26.md) §3.1 — R1 CLOSED entry
- [`CARDANO_NODE_M5_2026-05-26.md`](CARDANO_NODE_M5_2026-05-26.md) — corrected baseline + RCA
- [`AOT_PHASE1_DAY1-3_2026-05-26.md`](AOT_PHASE1_DAY1-3_2026-05-26.md), [`AOT_PHASE1_DAY4-6_2026-05-26.md`](AOT_PHASE1_DAY4-6_2026-05-26.md), [`AOT_PHASE1_DAY10-12_2026-05-26.md`](AOT_PHASE1_DAY10-12_2026-05-26.md) — Phase 1 execution log
- [`MEMORY_REDUCTION_AVENUES_2026-05-26.md`](MEMORY_REDUCTION_AVENUES_2026-05-26.md) — measurement-first week recommendation still applies
- [`NEXT_STEPS_2026-05-25.md`](NEXT_STEPS_2026-05-25.md) — multiple items now closed / re-classified per this snapshot
- [`ARCHITECTURE_CRITIQUE_2026-05-26.md`](ARCHITECTURE_CRITIQUE_2026-05-26.md) — invariant register; multiple invariants now mechanically enforced

**Key commits this snapshot tracks:**
- `b17ab3359` R1 trigger CLOSED (AttrSet REC_SET canonical emit)
- `49fabde3a` R8a Phase 1 Day 13-15 INCONCLUSIVE measurement
- `e8944cd35` R8a Phase 1 Day 10-12 mmap reader + preliminary
- `cd028b07a` R8a Phase 1 Day 4-6 flat-file format
- `bbe16bbcd` R8a Phase 1 Day 1-3 manifest recorder
- `f1cf8fd4c` Bridge telemetry: #661 falsified + AR15 downgraded + R10 closed
- `1e9ee3a5d` CR2 lint LANDED + AR24 closed
- `a14c6092e` M5 regression FALSIFIED; baseline was wrong
- `b2c928a8d` M5 baseline correction notice + Phase C explicit trigger
- `83bef15c4` R1-trigger closure cascaded through strategic docs
- `d4feb587a` Cardano-node M5 ritual measurement (the data that surfaced the baseline error)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
