# EXIT_GC_SPIRAL Day 4 — per-site bundle assessment

**Date:** 2026-05-30
**Per:** `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` §3.4 Day 4 procedure
**Status:** Per-site lever inventory + decision matrix evaluation.

---

## 1. Per-site lever status (as of 2026-05-30)

| Lever | Yield (HNE) | Status |
|---|---|---|
| fakeClo dead-code revival | -98.4 MB measured | **LANDED** commit `40e6abbdb` (Week 1 of this session) |
| capWiths ListVec singleton | -13 MB measured | **LANDED** (Week 1 bundle: fakeClo + capWiths → HNE Δpeak -105 MB) |
| mapAttrs 2-pair App chain | 100 MB potential | **NOT DONE** — prior Tag::App3 attempt rolled back; memoization-loss issue |

The Day 4 per-site bundle is mostly already CLOSED.  The pre-committed acceptance bar from §3.4 was: "Per-site levers totaling ≥ 200 MB at ≤ 1 wk total effort = bundle and ship in Week 2."

* What's LANDED (Week 1): -111 MB (fakeClo + capWiths) of the available -257 MB total estimated yield.
* What's REMAINING: mapAttrs 2-pair at 100 MB potential.

100 MB alone is BELOW the §3.5 "Day 4 bundle ≥ 200 MB" threshold needed for the per-site path to win the Week 1-2 slot.

---

## 2. mapAttrs 2-pair lever — why it's stuck

Source (`primops.cc:1952-2002`): each mapAttrs entry builds an `Tag::App (Tag::App fn name) value` chain via TWO ValuePair allocations.  HNE: 1,094,597 attrs × 2 pairs = 2,189,194 pairs × 48 B ≈ 100 MB.

Prior attempt (Day 9-11 of an earlier session, ROLLED BACK in commit `a9912f0fb`): introduced Tag::App3 packing all three args into a single 3-Value pair (saves 1 ValuePair per entry).  Regression observed:

* wall: 11.9 s → 49.4 s (4× slower)
* arena: 1493 MB → 3137 MB (1644 MB MORE memory)
* attrsets: 1.11 M → 3.91 M (+352 %)
* thunks: 3.83 M → 11.4 M (+298 %)

Root cause (per primops.cc:1964-1986 inline comment): the OUTER `Tag::App` pair's `pair->evaluated` slot is the App-result MEMOIZATION sink (#696 fix that closed "extendDerivation outputsList forced 64K times" regression).  Tag::App3 overloaded `evaluated` to hold arg2; memoization was LOST → every demand of a mapAttrs entry re-runs the body's allocations.

### Possible Tag::App3-with-memoization design

Add a SEPARATE `evaluated` slot to Tag::App3.  ValuePair today: 48 B (2 Values × 16 B + 16 B header).  Tag::App3-with-eval: 3 Values + evaluated = 4 Values × 8 B header = ~40-48 B (depending on alignment).

* Saves 1 ValuePair allocation per entry (48 B less)
* No memoization regression (separate slot)

Estimated effort: ~3-5 days.  Touches:
* ValuePair / pair allocator
* Force-path App / App3 dispatch in vm.cc
* Serialise (`value_serialize.cc` for #885 PRODUCTION cache compatibility)
* Phase D write barriers (pairPostConstructBarrier)
* GC walkers (precise_root, gc.cc)

Pre-committed SHIP threshold: ≥30 MB peak RSS reduction on HNE + no --core regression.

---

## 3. Decision matrix update (post-Day 4)

Per `EXIT_GC_SPIRAL_PLAN §3.5`:

```
Day 2 cache-eviction Δpeak on M5 ≥ 800 MB?
├── PROXY INVALID per EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md
│   (no signal — proxy broken)
└── (fall through)
    Day 4 per-site bundle ≥ 200 MB on HNE?
    ├── REMAINING bundle = mapAttrs 100 MB alone → BELOW 200 MB threshold
    └── BOTH orthogonal levers fail to clear thresholds
       → Plan's meta-Rule-0 escalation: Immix becomes Week 1's work
```

**Plan's escalation says: Immix Week 1.  But Immix was just FALSIFIED yesterday** (commit `dcb55122b`, `IMMIX_FALSIFIED_2026-05-29.md` — variant #7 in the 0-MB-shipped register).

So the orthogonal-lever path AND the GC-variant path are BOTH formally falsified at their pre-committed thresholds.

---

## 4. State of the union

The current architectural picture (as of 2026-05-30):

| Component | Status |
|---|---|
| GC track (7 variants) | All falsified per `GC_PAUSE_2026-05-29.md` + `IMMIX_FALSIFIED_2026-05-29.md` |
| Cache eviction proxy (Day 2) | Falsified per `EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md` |
| Per-site bundle (Day 4) | -111 MB shipped Week 1; remaining mapAttrs 100 MB BELOW Day 5 threshold |
| Phase 4b LRU (real implementation) | Untouched; 3 prereqs (CU pointer stability + nursery concerns + hash-bucket) |
| mergeBindings pattern fix | Untouched; "several hundred MB" estimate; high effort; Phase C 3-pivot-falsified guard memo in place |
| #875 bridge eviction (Stages 1-2b) | Correctness OK, 0 MB peak savings — arena page-release blocked on Stage 6 |

**M5 status:** trim-2 mean 4343 MB vs 4096 MB watchdog = need 247 MB reduction.

**Combined remaining levers if pursued:** mapAttrs (100 MB) + Phase 4b LRU (UNKNOWN — proxy invalid, real impl untouched) + mergeBindings (UNKNOWN — guard memo + high effort).  None individually clears 247 MB on M5; combined depends on additivity (likely sub-additive per `GC_DESIGN §7` overlap with cache).

---

## 5. Honest call

Per `[[falsification-rule]]` and `[[measure-twice-cut-once]]`:

1. The session has shipped Week 1's per-site bundle (-111 MB).  Real progress.
2. Day 2 and the GC track are formally falsified.
3. The remaining levers have non-trivial implementation cost AND unknown yield (no working proxy).
4. The M5 247-MB gap might be addressable but requires multi-day implementation work in any direction.

Three coherent next moves, each with merits:

**(A) Implement Tag::App3-with-separate-evaluated** for mapAttrs (3-5 d).  Highest-likelihood non-falsified lever per the current data.  Pre-committed SHIP ≥ 30 MB on HNE.  If shipped, plausibly ~200+ MB on M5 (proportional scaling), which could clear the 247-MB gap.

**(B) Implement Phase 4b LRU directly** (multi-week with 3 prereqs).  Higher projected yield but high cost; the proxy that estimated it is now invalid.

**(C) Pause + reset.**  Document the dual-track falsification, regroup on strategic direction with the user.  The session has delivered substantial work already.

Option (A) is the cheapest non-falsified path.  Option (C) is the most honest given the user's last directive ("continue ultrathink").  Option (B) is the highest-yield but highest-cost.

---

## 6. Cross-references

* `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` §3.4, §3.5 — Day 4 procedure + Day 5 decision matrix
* `EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md` — Day 2 proxy fail
* `IMMIX_FALSIFIED_2026-05-29.md` — yesterday's variant #7 fail
* `T1_3_PAIRS_LISTS_ATTR_2026-05-27.md` — the mapAttrs / capWiths data
* `EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md` — Tag::App3 rollback rationale
* `[[memory-first-class]]` — pre-committed SHIP thresholds
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — §3.5 escalation

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
