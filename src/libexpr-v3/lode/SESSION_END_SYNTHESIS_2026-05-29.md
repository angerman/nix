# Session-end synthesis 2026-05-29 — the bridge-retention discovery

**Status:** **STRATEGIC PIVOT POINT.**  This single session's 28+ commits reframe v3's memory-reduction strategy from "find a GC variant that ships" to "manage bridge-table lifecycle."

---

## 1. The empirical foundation (load-bearing)

Bridge-clear spike chain on BOTH workloads:

| Workload | Bridge entries | Baseline live | Bridges cleared | Drop |
|---|---:|---:|---:|---:|
| HNE | 30 | 519 MB | 0.9 MB | 99.8 % |
| M5 | 10056 | 731 MB | 1.0 MB | 99.9 % |

**Both workloads: clearing v3 ↔ TW bridge tables reduces "live at end-of-eval" by 99.8-99.9 %.**

Per-entry transitive retention differs (HNE 17 MB/entry, M5 73 KB/entry) but the AGGREGATE retention via bridges is dominant in both.

## 2. What this kills (per Rule 0)

**Hypothesis killed:** "v3 memory is bottlenecked at the GC-variant layer; the right GC family will close the M5 watchdog gap."

**Evidence:** 6 GC falsifications across Cheney / flat MS / Immix / Boehm-tuning / periodic-GC / arena-dereg.  Today's data shows WHY: no precise GC can free bridge-held bytes because they're reachable from global roots by design.  L=0.74 on HNE is HIGH because bridges retain ~95 % of arena.

**Replaces with:** v3 memory is bottlenecked at the bridge-table lifecycle.  The right architectural fix is per-entry retention reduction (weak bridges + fallback re-eval, OR ref-counting via Boehm finalizers, OR end-of-primop bridge sweep).  GC family choice is secondary.

## 3. The lever ladder (next session)

| # | Lever | Effort | Yield estimate | Risk |
|---|---|---|---|---|
| 1 | End-of-eval bridge clear (production) | 1 d | HNE end-of-eval RSS drop; M5 trivial close watchdog | low (spike proven) |
| 2 | Cohort allocation (per-bridge-source) | 2-3 wk | whole-block-free becomes viable; mid-eval peak drop | medium |
| 3 | Weak bridges + fallbackExpr re-eval | 1-2 wk | mid-eval transitive retention drops to ~0 | medium-high (re-eval cost) |
| 4 | Bridge ref-counting via Boehm finalizers | 3-4 wk | correct semantic lifecycle | high (Boehm finalizer reliability) |
| 5 | (Previous GC variant work) | — | — | DOWN-PRIORITIZED |

**Recommended sequencing for next session:**
1. Land production-ized end-of-eval clear (1 d).  Ships immediate RSS-at-eval-return win.
2. Measure mid-eval bridge transitive retention via per-bridge live walk (~1 d instrument).  Decides whether to pursue weak bridges (option 3) or cohort allocation (option 2).
3. Based on (2)'s data, commit to one of options 2-4.

## 4. What this session delivered

Across 28+ commits:

### Implementation
* App3 ROLLED BACK (was a 3× allocation regression)
* capWiths singleton intern (Day 13-15; -17 MB HNE arena deterministic)
* fakeClo pool wire-back retained (per user decision; conditional GC-progress retirement)
* Bench script auto-pick newer of `build/` vs `builddir/` (fixes the stale-binary methodology bug)
* DIAG-1 per-cycle GC CSV
* DIAG-3 per-Tag L(t) time-series
* DIAG-4 per-phase arena bytes
* DIAG-2 Phase 1 + Phase 2 per-PosIdx live-bytes top-N
* DIAG bridge-size instrumentation (NIX_VM_STATS + periodic CSV)
* Bridge clear spikes: `NIX_V3_END_OF_EVAL_CLEAR_{IMPORT_CACHE,BRIDGES}=1`

### Findings (load-bearing for future strategic decisions)
1. **Bridges hold 99.8-99.9 % of live-at-eval-end** on both HNE and M5.
2. **F2 verdict (sweep 40-54 % wall) was a projection bug** per DIAG-1; actual sweep is 14 % of GC time.  The Immix pivot rested on the wrong number.
3. **Bench script default `NIX_BIN` was stale** — all pre-pivot bench measurements ran against pre-Week-1 binary.  Bench measurements before today's retrospective are untrustworthy.
4. **Tag::App3 caused a 3× regression** (lost memoization); rolled back.  Day 9-11 "peak-neutral retain" decision was based on stale-binary measurement.
5. **Retention is concentrated** by source position (HNE 62.8 % at all-packages.nix:9112; equivalent on M5) — but the concentration is bridge-mediated, not workload-property.

### Strategic docs landed
* `EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md` — honest Week 1 measurement
* `EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md` — pivot rationale
* `DIAG_SUITE_LANDED_2026-05-29.md` — inventory + first findings
* `DIAG_CONCENTRATED_RETENTION_2026-05-29.md` — Phase 2 strategic synthesis (partially superseded)
* `BRIDGES_HOLD_RETENTION_2026-05-29.md` — the headline finding
* `SESSION_ARC_2026-05-29.md` — chronological commit table
* This doc (`SESSION_END_SYNTHESIS_2026-05-29.md`) — final synthesis

### Memory updates
* `[[bench-binary-fingerprint]]` — bench script default may be stale
* `[[exit-week1-revalidated]]` — Week 1 ships honestly post-rollback
* `[[bridges-hold-retention]]` — the strategic finding
* Updated `[[app3-mapattrs]]` to mark ROLLED BACK
* Updated `[[peak-vs-alloc-distinction]]` to add "mid-lifetime" category

## 5. M5 watchdog status

* Today's M5 peak: 4343 ± 95 to 3805 ± 308 MB (environmental σ variability)
* Watchdog target: 4096 MB
* Current bundle delivered: -219 MB peak (wide CI) / -822 MB arena (deterministic)
* With bridge lifecycle landing: projected M5 peak ~1.5 GB (Boehm 400 + small live + overhead)
* **Margin under watchdog with bridge lifecycle: ~2.5 GB**

The watchdog goal that has driven 3+ weeks of GC variant work is achievable in ~1-2 weeks of bridge lifecycle work.

## 6. Cross-references

* This session's `lode/` documents are all in `src/libexpr-v3/lode/`:
  * `EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`
  * `EXIT_WEEK3_DIAGNOSTIC_PIVOT_2026-05-29.md`
  * `DIAG_SUITE_LANDED_2026-05-29.md`
  * `DIAG_CONCENTRATED_RETENTION_2026-05-29.md`
  * `BRIDGES_HOLD_RETENTION_2026-05-29.md`
  * `SESSION_ARC_2026-05-29.md`
  * `SESSION_END_SYNTHESIS_2026-05-29.md` (this)
* Raw measurement data in `bench/baselines/`:
  * `2026-05-29-week1-retrospective/` — honest Week 1 N=10
  * `2026-05-29-week3-cache-eviction-revalidate/` — Day 2 null on M5
  * `2026-05-29-week2-capwiths/` — capWiths intern A/B
  * `2026-05-29-bridge-retention/` — the headline spike data

## 7. Honest limits

* DIAG-2 Phase 2 residual unknown (9.3 % on HNE; 27 % on M5) — not all alloc sites have posHandle populated.  DIAG-2 Phase 3 (per #873 follow-up) would close this gap.  Probably not load-bearing for any strategic decision.
* All measurements this session are HNE-host + builddir/ (debug build).  Wall numbers 2× release; allocation counts identical to release.  RSS measurements still valid in absolute terms (debug overhead is small fraction of arena).
* Bridge-clear spike is UNSAFE for `nix repl` (TW callbacks expected mid-process).  Production-ize MUST gate by CLI context (single-shot vs interactive).
* M5 environmental σ is high (95-308 MB across measurement runs same day).  Any "peak Δ" claim < ~150 MB is in noise.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
