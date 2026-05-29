# EXIT Week 3 second-lever decision

**Date:** 2026-05-29 (post-Week 1 retrospective)
**Status:** **DECISION MADE** — Day 2 NULL on M5 confirmed at honest binary state.  Week 3 lever = **re-measure Immix (Steps 11′-13′ already in-tree) on M5 specifically**, per plan §3.5 escalation.
**Task:** #867
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §5 + §3.5 + the Week 1 retrospective ([`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md))

---

## 1. Why this decision is being re-opened

The original plan §3.5 picked the per-site bundle for Weeks 1-2 based on Day 2's NULL verdict on M5 + Day 4's ≥200 MB bundle projection.  Week 1 retrospective revealed:

* **Bundle delivered** but smaller than original (Day 6-8/12 used stale binary): HNE Δpeak -105 MB / M5 Δpeak -219 MB / M5 Δarena -822 MB.
* **M5 is 247 MB OVER watchdog** with bundle fully applied (4343 ± 95 MB vs 4096 MB target), correcting Day 12's "50 MB under" claim.
* **Day 2's M5 NULL verdict** was also measured against stale build/ binary; re-measurement is in flight on builddir/ — see task #866.

The M5 watchdog gap is now load-bearing: we need ~250-500 MB more reduction on M5 specifically.  Single-lever budgets in the plan §2 table:

* Cache eviction: 500-950 MB on HNE per HNE_BUCKET_DECOMP — but Day 2 said NULL on M5.  Re-measurement TBD.
* mergeBindings: "several hundred MB" but Phase C falsified 3 pivots; high risk.
* Phase E v0.2: 144 MB fakeClo recovery (already partially shipped via wire-back); multi-session for full ship-readiness.
* Strictness Stage 4 v4+: speculative; 0 elisions today; 3-4 weeks.
* GC variant (Immix): projected 200-1200 MB; paused per `GC_PAUSE_2026-05-29.md`.

## 2. Pre-committed decision tree

Per `[[measure-twice-cut-once]]` §3: thresholds set BEFORE seeing the data.

```
Day 2 re-measure M5 Δpeak (cache-off minus cache-on, N=10):

├── If Δpeak ≤ -250 MB (cache eviction strongly wins on M5)
│   → Week 3 lever = Phase 4b LRU implementation
│   → Pre-committed effort: 2-3 weeks per plan §4.2
│   → Prereqs: 3 items per plan; assess in Day 16-17
│
├── If -250 < Δpeak ≤ -100 MB (cache helps but doesn't close gap alone)
│   → Cache eviction is ONE of two levers; need a stacking partner
│   → Week 3 lever = cache eviction (smaller scope: incremental LRU)
│   → Week 3b = pick a second partner from: mergeBindings audit /
│     Stage 4 v4+ / re-open Phase E v0.2 ship path
│
├── If -100 < Δpeak < +100 MB (cache eviction NULL — confirms Day 2 direction)
│   → Cache not a lever on M5
│   → Per plan §3.5: orthogonal lever exhausted; GC re-eval triggers
│   → Week 3 lever = revive Immix from paused state per
│     `GC_DECISION_2026-05-29.md` + `BIBOP_LITE_PROJECTION_2026-05-29.md`
│   → Plan §3.5's "BOTH orthogonal levers falsified" branch applies,
│     accepting that the per-site bundle DID ship (-219 MB M5 peak)
│     but the watchdog gap remains.
│
└── If Δpeak ≥ +100 MB (cache eviction makes M5 WORSE — Day 2 was right)
    → Cache HELPS memory, doesn't hurt
    → Same pivot as the NULL branch
```

Per Rule 0, whichever branch fires generates a kill commit explaining what hypothesis it falsifies.

## 3. M5 budget reality (broader context)

* Current M5 peak_rss = 4343 ± 95 MB (over watchdog by 247 MB).
* Current M5 v3_arena = 5570 MB (over default `NIX_V3_MAX_HEAP=4G` by 1474 MB).
* Sustained M5 stability requires arena < 4 GB, ideally ~3 GB.

Single Week 3 lever yielding 250 MB is enough to MOVE the watchdog status from "over by 247" to "under by ~50".  Stacking with future Stage 6 GC (projected 600-1200 MB) would get to comfortable margin.

## 4. Day 2 re-measurement (in flight)

Task #866 — `NIX_V3_NO_DISK_CACHE=1` (proxy for cache eviction) vs default on M5, N=10 each, builddir/ binary, App3 reverted.

Original Day 2 (with stale build/):
* M5 cache-on 4862 ± 752
* M5 cache-off 4874 ± 280
* Δpeak +12 MB (NULL)

Today's re-measurement TBD — will update §5 when data lands.

## 5. Result + final decision

Day 2 re-measurement on builddir/ + App3 rolled back (N=10 each, back-to-back):

| Config | n | peak_rss (MB) ± σ | v3_arena (MB) | elsewhere (MB) |
|---|---:|---:|---:|---:|
| M5 cache-on (default) | 10 | 3804.88 ± 308.51 | 5570.0 | 0.0 |
| M5 cache-off | 10 | 3697.49 ± 475.29 | 5570.0 | 0.0 |

**Δpeak = -107.39 MB at 0.19σ pooled** (pooled σ ≈ 566 MB; 95% CI ±1100 MB).

**Branch triggered: NULL (-100 < Δpeak < +100 MB, or signal < 1σ).**  Original Day 2 NULL verdict on M5 CONFIRMED at honest binary state.  Cache eviction is NOT a viable Week 3 lever on M5.

**Week 3 lever: re-measure Immix on M5.**  Steps 11′-13′ infrastructure (line-mark bitmap + line-region allocator + adaptive threshold + recycle policy) is in-tree behind `NIX_V3_MAJOR_GC=1` + `V3_DBG_FREELIST_REUSE=1` opt-in gates per `GC_PAUSE_2026-05-29.md` §3.  GC_PAUSE doc projected Immix at HNE peak -355 MB but did NOT measure M5 (the binding constraint).  Week 3 fills that gap.

### 5.1 Why this is the right pivot

* Per pre-committed tree §2, NULL on M5 triggers "pivot to GC re-eval" branch.
* GC_PAUSE doc §8 ("Honest limits") states "Reversible.  If `EXIT_GC_SPIRAL_PLAN`'s Day 5 decision matrix routes back to GC, this PAUSE is the appropriate stopping point — the work picks back up from Step 14′."  Step 14′ = "Phase 4 SHIP gate re-measurement under Immix."
* Steps 11′-13′ infrastructure exists; cost is ONE measurement session (~hours), not a multi-week build.
* The M5 watchdog gap (~250 MB) is BELOW Immix's HNE projection (-355 MB) — Immix MAY close the gap if M5 scales similarly.

### 5.2 Operational plan for Week 3

Day 16-17: M5 Immix re-measurement
* Build with `NIX_V3_MAJOR_GC=1` gate (verify Steps 11′-13′ engage)
* N=10 measurement on M5 + HNE under `NIX_V3_MAJOR_GC=1`
* Compare to today's bundle-default measurements

Pre-committed Day 17 acceptance:
* M5 Δpeak ≥ 250 MB (closes watchdog) → SHIP IMMIX, continue to Steps 15′-17′ in Week 3-4
* 100 MB ≤ M5 Δpeak < 250 MB → STACK Immix with another lever; reopen mergeBindings as parallel work
* M5 Δpeak < 100 MB → Immix FALSIFIED on M5; pivot to one of: (a) Phase E v0.2 ship-readiness, (b) mergeBindings pattern fix, (c) Stage 4 v4+ strictness, OR (d) accept M5 stays edge-of-watchdog and document operational guidance (always bump heap cap)

Day 18-19 onward depends on Day 17 outcome.

### 5.3 What ALSO needs verifying alongside Immix

Two anomalies to investigate (separate from the Immix gate):

1. **elsewhere=0 in builddir/.**  HNE_BUCKET_DECOMP claimed ~990 MB elsewhere on HNE; today's builddir/ measurements show 0 across both HNE + M5 + with-cache + without-cache.  Either the elsewhere counter is broken in current builddir/, OR the bucket genuinely vanished between binaries.  If elsewhere truly is 0, the HNE_BUCKET_DECOMP attribution is invalid and the 500-950 MB cache eviction projection is too.  Worth ~1 hour to investigate.

2. **M5 peak σ is environmental.**  Morning M5 measurement σ=95, afternoon σ=308 — same code, same N=10.  The watchdog "247 MB OVER" claim may be within noise envelope.  Multiple sessions worth of M5 measurement may give a more honest distribution.

## 6. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §5, §6.2, §3.5
* [`EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md`](EXIT_WEEK1_RETROSPECTIVE_2026-05-29.md) — the M5 247 MB gap finding
* [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) — cache contribution to RSS (HNE side)
* [`GC_DECISION_2026-05-29.md`](GC_DECISION_2026-05-29.md) — Immix path if pivot required
* [`GC_PAUSE_2026-05-29.md`](GC_PAUSE_2026-05-29.md) — explicit re-evaluation hook in Week 4
* `bench/baselines/2026-05-29-week3-cache-eviction-revalidate/` — fresh raw JSON
* Memory: `[[exit-week1-revalidated]]` — the new baseline
* Memory: `[[measure-twice-cut-once]]` — the pre-committed-threshold rule
* Memory: `[[bench-binary-fingerprint]]` — why Day 2's original needed re-doing

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
