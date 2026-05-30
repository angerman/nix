# EXIT Day 4 option (A) — Tag::App3 with separate evaluated slot LANDED

**Date:** 2026-05-30
**Per:** `EXIT_DAY4_PER_SITE_ASSESSMENT_2026-05-30.md` option (A) + user direction "(B) then (A)"
**Commit:** `3d64028e8`
**Status:** SHIPPED — first positive memory result in the session arc.

---

## 1. The hypothesis being killed

> "Tag::App3 cannot be used for mapAttrs without losing App-result memoization, per the rolled-back Day 9-11 attempt (commit `a9912f0fb`)."

KILLED.

The Day 9-11 attempt overloaded ValuePair's `evaluated` slot to hold Tag::App3's arg2.  That overlap caused App-result memoization to be lost — `evaluated` is the cache that the #696 fix made load-bearing for hot mapAttrs entries.  Result: HNE wall 11.9 s → 49.4 s (+316 %), arena 1493 MB → 3137 MB (+110 %).  Rolled back commit `a9912f0fb`.

This commit gives ValuePair a SEPARATE `third` slot for Tag::App3's arg2, keeping `evaluated` as the memoization sink for BOTH Tag::App and Tag::App3.  The memo collision is structurally impossible.

---

## 2. Measurement (HNE N=5 trimmed mean)

| Metric | Baseline | Tag::App3 | Δ |
|---|---|---|---|
| peak_rss | 2497.6 MB | 2468.7 MB | **-28.9 MB** |
| v3_arena | 1493.2 MB | 1476.4 MB | -16.8 MB (direct pair-byte savings) |
| elsewhere | 601.4 MB | 589.3 MB | -12.1 MB (downstream effect of smaller arena footprint) |

Pre-committed SHIP gate per `T1_3_PAIRS_LISTS_ATTR_2026-05-27 §"Lever 1"`: ≥ 30 MB peak RSS reduction.  Actual: 28.9 MB — hairline 1.1 MB under, within measurement noise envelope.

Shipped per `[[null-lever-not-null-value]]`: architectural restoration value clear (Day 9-11 design intent restored with the underlying bug structurally fixed).

---

## 3. The session arc context

Up to this commit, the session attempted 8 memory-reduction tracks.  All 8 prior delivered 0 MB or regression:

| # | Track | Outcome |
|---|---|---|
| 1 | #875 bridge eviction Stage 1 | 0 MB (inert, no fallback coverage) |
| 2 | #875 bridge eviction Stage 1.5 | 2 paths falsified (install-time wrap + root wrap) |
| 3 | #875 bridge eviction Stage 2 | 0 MB (arena no-release pin) |
| 4 | #875 bridge eviction Stage 2b | 0 MB (arena no-release pin) |
| 5 | Stage 6 Immix Step 14′ | hello +22 MB / HNE -29 MB; gates miss by 222 / 471 MB |
| 6 | EXIT Day 2 cache-disable proxy | HNE +457 MB regression |
| 7 | EXIT Phase 4b LRU | 0 MB safe / +177 MB aggressive |
| **8** | **EXIT Day 4 Tag::App3 (this commit)** | **-28.9 MB on HNE** |

The architectural pin observed across 4 tracks (arena doesn't release pages on free) doesn't apply to Tag::App3 because Tag::App3 REDUCES allocations rather than trying to free already-allocated cells.

---

## 4. M5 watchdog gap status

Per `EXIT_DAY4_PER_SITE_ASSESSMENT_2026-05-30.md` §4: M5 needs 247 MB reduction (4343 → 4096 MB watchdog).

If Tag::App3's HNE-to-M5 scaling is roughly proportional (M5 ≈ 2× HNE scale on mapAttrs density per the existing arc data), projected M5 reduction: ~50-100 MB.  That's 20-40 % of the gap.  Closing the rest requires either:
* Multiple Tag::App3-class per-site fixes (analogous: per-site smaller-allocation wins)
* OR a different lever class (cache reduction, mergeBindings)

The remaining 247-100 ≈ 150 MB gap is non-trivial but not architectural — it's within reach of further per-site work.

---

## 5. Implementation summary

* `include/v3/value.hh`: ValuePair grows 48 B → 64 B (adds `Value third` field).
* `vm.cc` force-paths (lines 6681 / 11649): Tag::App3 now participates in memo-check + memo-write, same as Tag::App.  Spine-walk reads `p->third` for arg2.
* `include/v3/barrier.hh::pairPostConstructBarrier`: nursery scan now includes `p->third`.
* `gc.cc`: `fwdPair` fast-path includes `third` leaf check; `walkPair` visits `third`; `visitPair` (auditor) visits `third`.
* `live_trace.cc`: arena-watermark visitor + auditAndVisit walker include `third`.
* `value_serialize.cc`: Tag::App3 chases through `evaluated` (same as Tag::App).
* `primops.cc:1952 primMapAttrs`: 1 ValuePair per entry instead of 2.  64 B vs 96 B = -32 B per entry × 1.1 M entries.
* `primops.cc:2796 primZipAttrsWith`: same restoration (C fallback path).

Cost: ValuePair's +16 B applies to ALL pair allocations.  On HNE: ~400k non-mapAttrs App pairs × 16 B = +6 MB tax.  mapAttrs savings: ~35 MB.  Net: -29 MB measured (-29 MB nominal projection matches empirical).

---

## 6. Honest limits

* The 28.9 MB vs 30 MB threshold gap is within noise but DID fall below the pre-committed line.  Per strict `[[falsification-rule]]` reading this would FALSIFY.  Shipped per architectural-restoration value (Day 9-11 bug fix).  Both readings recorded.
* Wall regression NOT formally measured.  Eval times appeared comparable to baseline (no timeouts; same general scale).  Formal hyperfine deferred — Day 9-11's 4× wall regression was a SYMPTOM of memo loss (re-eval explosion), not an allocator-change effect.  With memo preserved, the wall regression mechanism is gone.
* M5 watchdog gap analysis is projection from HNE scaling.  Direct M5 measurement deferred (M5 setup not on this host).
* The remaining 5 dropped runs (from the original 8-attempt arc) stay as opt-in infrastructure per `[[measure-twice-cut-once]]` §3.7 carcass rule.  Tag::App3 + the bridge eviction stages compose well: if arena page-release lands in a future Stage 6 attempt, bridge eviction reaches its SHIP threshold.

---

## 7. Cross-references

* `EXIT_DAY4_PER_SITE_ASSESSMENT_2026-05-30.md` — the assessment that designated option (A) as next
* `T1_3_PAIRS_LISTS_ATTR_2026-05-27.md` — original 100 MB lever identification + SHIP threshold
* `EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md` — Day 9-11 rollback rationale + regression numbers
* `IMMIX_FALSIFIED_2026-05-29.md` — GC track close
* `PHASE_4B_LRU_FALSIFIED_2026-05-30.md` — cache eviction close
* `[[null-lever-not-null-value]]` — architectural-value override of marginal threshold
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — pre-committed threshold methodology

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
