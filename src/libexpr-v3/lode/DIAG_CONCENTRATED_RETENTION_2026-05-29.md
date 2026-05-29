# DIAG findings synthesis — Concentrated retention reopens GC strategy

**Date:** 2026-05-29 evening
**Status:** **STRATEGIC FINDING** — DIAG-2 Phase 2 reveals retention is concentrated; reverses the audit's tentative "dispersed → BiBOP not viable" framing
**Companion:** [`DIAG_SUITE_LANDED_2026-05-29.md`](DIAG_SUITE_LANDED_2026-05-29.md) (the four-build summary)

---

## 1. The headline finding

DIAG-2 Phase 2 on HNE shows live-bytes attribution after the entry-pos fallback:

| Source position | Live (MB) | % of 519 MB total | Bindings (N) |
|---|---:|---:|---:|
| **pkgs/top-level/all-packages.nix:9112:3** | **311.46** | **62.8 %** | **318,931** |
| _unknown (posHandle=0)_ | 45.85 | 9.3 % | 18,264 |
| lib/customisation.nix:398:48 | 16.59 | 3.3 % | 16,988 |
| pkgs/top-level/python-packages.nix:11492:3 | 5.95 | 1.2 % | 6,096 |
| pkgs/stdenv/generic/make-derivation.nix:567:13 | 5.25 | 1.1 % | 5,379 |
| `<string>:1:720` (eval CLI) | 5.11 | 1.0 % | 5,230 |
| pkgs/development/haskell-modules/hackage-packages.nix:489721:3 | 3.58 | 0.7 % | 3,661 |
| _(top 20 covers ~85 %)_ |  |  |  |

**One source position retains 62.8 % of the 519 MB live set.**  The 319 K bindings at all-packages.nix:9112:3 each average ~1 KB (~28 entries each).  These are the per-package callPackage attrsets emitted by `pkgs/top-level/all-packages.nix`'s root-level overlay.

## 2. What this reverses

Per `DIAGNOSTIC_AUDIT_2026-05-29 §4.3`:

> Per Tag-level "84 % Bindings", we can't tell whether:
> * Concentrated → BiBOP / page segregation works
> * Dispersed → it doesn't

Pre-DIAG-2-Phase-2 (Phase 1 alone with 82.8 % unknown), the resolved subset (17 %) looked dispersed.  Phase 2 reveals **the unknown bucket was masking the concentration.**  The real distribution is HIGHLY concentrated.

This re-opens BiBOP-lite as a candidate (per `BIBOP_LITE_PROJECTION_2026-05-29.md`) AND raises new candidates:

* **Trigger-policy GC** — per `L_MEASUREMENT_GAP_2026-05-28`, HNE's L drops to 0.41 near end-of-eval.  If GC fires at the trough (not at the early 256 MB threshold), it could reclaim the 311 MB nixpkgs attrset that becomes unreachable once `hello.drvPath` is computed.  This is a "late-firing" trigger lever we now have data for.

* **End-of-eval GC hook** — single MS pass after `run()` returns, before result serialization.  Mid-eval triggers fire too early to catch the all-packages.nix attrset reclamation.  Late firing collects the residual.

* **Per-posHandle free-list bins** — allocator-side: same-posHandle bindings allocated together on the same arena block.  When the nixpkgs root drops, whole blocks can free.  This is BiBOP-lite scoped to the dominant retainer.

## 3. What DIAG-1 confirmed (independent lever)

* `markMs : sweepMs = 7 : 1` (mark dominates, sweep is 14 % of GC time).  F2 verdict was projection bug.
* `blocksFreed = 0` every observed cycle — free-list bookkeeping doesn't reduce RSS.
* Implication: any non-moving GC that does per-cell free-list adds, without page-aware allocation strategy, will hit the same RSS-doesn't-drop problem.

Combining the two findings:
* Non-moving GC needs page-aware allocation (BiBOP-lite or per-posHandle cohorts) to actually drop RSS.
* The concentrated retention pattern makes page-aware viable.
* Trigger policy matters — early-firing GC misses the late-eval trough.

## 4. Lever re-prioritization

Given the new data, lever priorities shift:

| Lever | Pre-DIAG view | Post-DIAG view |
|---|---|---|
| Cache eviction | Day 2 NULL on M5 — falsified | Still NULL; Phase 4b unjustified for M5 |
| Per-site bundle (already shipped) | -105 MB HNE / -822 MB M5 arena | Ships; below watchdog gap |
| **BiBOP-lite cohort allocation** | "Add-on; dispersed = no" | **Possibly the lever** — concentrated retention validates |
| **Late-firing / end-of-eval GC** | Speculative; no data | **Quantified target** — 311 MB at all-packages.nix:9112 reclaimable when nixpkgs root drops |
| mergeBindings pattern fix | High risk; Phase C falsified 3 pivots | Still high risk; per-PosIdx data could route specific fix |
| Phase E v0.2 ship-readiness | Multi-session; recovers fakeClo 144 MB | Same; complementary to BiBOP-lite |
| Stage 4 v4+ strictness | Speculative; 0 elisions | Lower priority |
| Flat MS / Immix (full) | F2 verdict made Immix the path | F2 falsified — flat MS may rescue with quadratic fix + page-aware allocator |

## 5. Recommended next step (post-DIAG)

Per `[[measure-twice-cut-once]]`, before committing engineer-weeks, pre-commit a measurement spike.

**Spike candidate: end-of-eval GC hook** (~half day to build + measure).

* Add `NIX_V3_END_OF_EVAL_GC=1` gate.  After `run()` returns in `run.cc` and before result serialization, fire one `runMajorMarkSweep` pass.  Re-measure HNE peak + arena.
* Pre-committed acceptance:
  * If post-end-GC peak drops ≥ 200 MB on HNE → trigger policy is the lever.  Path: implement a trough-detecting trigger; pursue Steps 14′-17′ for production.
  * If 50-200 MB drop → partial; pursue BiBOP-lite in parallel.
  * If < 50 MB drop → end-of-eval timing isn't enough; retention pattern doesn't decay at end-of-eval as expected.  Need to revisit.

This spike is cheaper than committing to BiBOP-lite implementation (4-6 weeks per `GC_DECISION_2026-05-29 §5.4`).  And it produces ACTIONABLE data: either trigger policy ships (~1-2 week project) or we know BiBOP-lite is the path.

## 6. Open question (for user direction)

The DIAG suite is in place.  The strategic finding (concentrated retention) is in.  Three legitimate next moves:

1. **End-of-eval GC spike** (half day) — measure the lower-bound on what trigger policy could reclaim.
2. **DIAG-5 flat MS quadratic fix** (~1 day) — combined with new data may rescue flat MS as a viable GC family.
3. **Non-arena attribution** (mallinfo / malloc_zone_statistics) — addresses the "elsewhere is ominous" directive more fully.

Plus one investigation:
4. **Immix OP_REC_BINDING_SLOT_REF crash** — required before any Immix work; ~1 day to debug.

Recommendation: **(1) end-of-eval spike first** — fastest decision-relevant data; directly tests whether concentrated retention can be exploited by trigger policy.

## 7. Cross-references

* [`DIAGNOSTIC_AUDIT_2026-05-29.md`](DIAGNOSTIC_AUDIT_2026-05-29.md) §4.3 — concentrated-vs-dispersed framing
* [`DIAG_SUITE_LANDED_2026-05-29.md`](DIAG_SUITE_LANDED_2026-05-29.md) — first-measurement findings
* [`BIBOP_LITE_PROJECTION_2026-05-29.md`](BIBOP_LITE_PROJECTION_2026-05-29.md) — re-opened by concentration finding
* [`L_MEASUREMENT_GAP_2026-05-28.md`](L_MEASUREMENT_GAP_2026-05-28.md) — trigger-policy framing
* [`L_TIME_SERIES_DATA_2026-05-29.md`](L_TIME_SERIES_DATA_2026-05-29.md) — L trough at end-of-eval data
* [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) §5.4 — defragmentation deferred
* [`GC_PAUSE_2026-05-29.md`](GC_PAUSE_2026-05-29.md) §8 — reversibility hook

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
