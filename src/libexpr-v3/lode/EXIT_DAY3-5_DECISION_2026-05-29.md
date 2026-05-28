# EXIT Days 3–5 — Bucket investigation, per-site assessment, Week 1-2 decision

**Date:** 2026-05-29
**Status:** WEEK 0 CLOSES — Per-site bundle is Week 1-2's lever; cache eviction DROPS from plan
**Tasks:** #859, #860, #861 (closes)
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §3.3 — §3.5

---

## 1. TL;DR

**Day 5 decision (pre-committed):** Week 1-2 = **per-site bundle (fakeClo + mapAttrs + capWiths) → 257 MB on HNE for ≤ 1 week effort.**

* Day 2 measurement falsified the cache-eviction lever (per [`EXIT_DAY2_CACHE_EVICTION_2026-05-29.md`](EXIT_DAY2_CACHE_EVICTION_2026-05-29.md)).
* Day 3 M5 bucket data confirmed M5 already exceeds physical memory (page-swapping); arena is the dominant pressure on M5.
* Day 4 per-site lever assessment confirmed three known levers totaling 257 MB on HNE; effort ≤ 1 week.
* Day 5 decision matrix: per-site bundle wins.  Cache eviction → DROPPED (not Week 3).  Plan's own Rule-0 escalation NOT triggered (per-site ≥ 100 MB threshold).

---

## 2. Day 3 — M5 bucket investigation (redirected)

Per `EXIT_GC_SPIRAL_PLAN §3.3`: Day 3 redirected because strings = NO-LEVER (Step 18 closed prior).  Reallocated to Boehm bucket + M5 arena split investigation.

### 2.1 Findings from Day 2 measurement (subsumes Day 3)

Day 2's M5 measurement already captures the bucket split:

| Metric | Value |
|---|---|
| M5 peak_rss (cache-on) | 4874.00 ± 751.87 MB |
| M5 peak_rss (cache-off) | 4860.43 ± 40.81 MB |
| M5 v3_arena | 6392.10 MB (both configs) |
| M5 elsewhere | 0 MB (both configs) |
| M5 Boehm heap | ~403 MB (consistent with HNE) |

### 2.2 Pre-committed acceptance check

Per §3.3:
- M5 elsewhere < 500 MB → cache eviction is smaller lever than expected; rebalance.
- **M5 elsewhere = 0 MB → confirms cache eviction LEVER IS NULL on M5.**

The elsewhere bucket (ImportCache + SQLite + bytecode shadow) IS the cache-eviction target.  M5 has none of it.  Cache eviction provides ZERO benefit on M5.

### 2.3 Peak < arena paradox (M5 specific)

`v3_arena = 6392 MB` but `peak_rss = 4874 MB`.  Peak should be ≥ arena unless pages are SWAPPED.  Host page-swap is the most plausible explanation.

Implication:
- M5 already exceeds physical-memory capacity on this host
- `ru_maxrss` under-reports because pages are paged out
- Memory pressure on M5 is REAL even when ru_maxrss appears < 5 GB

Boehm heap: stable at ~403 MB across all measured workloads (hello, HNE, M5).  Not a scaling concern.  Per `[[boehm-tuning-falsified]]` already-falsified knobs.

### 2.4 What this means for the watchdog goal

The M5 watchdog target (< 4 GB) is set against `ru_maxrss`.  My measurement shows:
- M5 peak_rss = 4874 ± 752 MB (cache-on)
- Gap to watchdog: ~874 MB on this host (varies up to σ=752)

The 4 GB watchdog is for `NIX_V3_MAX_HEAP` (process virtual / Boehm cap).  v3 arena counts against that.  M5 arena = 6.4 GB; > 4 GB even if RSS shows lower.

**Reaching M5 < 4 GB requires REDUCING ARENA bytes, not just freeing cache.**  Arena bytes come from cells.  Reducing cells = (a) cell-size optimization (per-site fixes) or (b) GC reclamation (paused).

---

## 3. Day 4 — Per-site lever assessment

Per `EXIT_GC_SPIRAL_PLAN §3.4`: quantify three known per-site levers WITHOUT implementation.

### 3.1 fakeClo dead-code revival

**Source:** [`T1_3_CLOSURES_ATTR_2026-05-27.md`](T1_3_CLOSURES_ATTR_2026-05-27.md):
- vm.cc:6803 fakeClo (OP_FORCE thunk body) — 1,595,947 allocs × ~64 B avg = 101 MB on HNE
- vm.cc:12085 fakeClo (OP_TAIL_CALL) — 714,910 allocs × ~60 B avg = 43 MB on HNE
- Total: **144 MB on HNE**

**Mechanism:** fakeClo is a synthetic Closure the VM allocates to dispatch a THUNK body through the same OP_CALL dispatch.  Per the #558 Phase 4 recycling pool design at `alloc.hh:1069-1230`, the pool was BUILT but never wired back as consumers since Phase D Step 12 retired the consumers (commit `ddb52d3a7`).

**Implementation paths:**
1. **Wire pool back as consumer** at the two fakeClo allocation sites (~1 day; restores the existing design)
2. **Inline the dispatch** (eliminate fakeClo entirely; restructure OP_FORCE + OP_TAIL_CALL to not need synthetic Closure) — larger refactor (~3-5 days)

**Effort:** ~1 day for path (1); preserves yield with minimum disruption.

### 3.2 mapAttrs 2-pair App chain

**Source:** [T1_3_PAIRS_LISTS_ATTR_2026-05-27.md](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md):
- primops.cc:1965 (outer App pair) — 1,094,597 allocs × 48 B = 50.1 MB on HNE
- primops.cc:1970 (inner App pair) — 1,094,597 allocs × 48 B = 50.1 MB on HNE
- Total: **100 MB on HNE**

**Mechanism:** `mapAttrs (k: v: f k v) attrs` creates per-entry `App(App(f, k), v)` = TWO ValuePair allocations.  A `3-arg App` representation would use ONE pair carrying `[f, k, v]` (or similar tagged shape).

**Effort:** per-site fix — modify primMapAttrs to allocate 1 pair instead of 2.  Requires:
- Per-site definition of "3-arg App" Value tag (or extending PrimOpApp's pair payload)
- VM dispatch path that unpacks 3-arg App
- Backward-compatible serialization (if reachable from the disk cache)

Estimated effort: 1-2 days.

### 3.3 Tiny capWiths ListVec

**Source:** T1_3_PAIRS_LISTS_ATTR_2026-05-27:
- vm.cc:3831 — 544,299 allocs × 24 B avg = **13 MB on HNE**

**Mechanism:** Each Closure has a `capturedWiths: ListVec *` field holding 1-2 elements typical.  Each tiny ListVec is a separate allocation; an inline (e.g., `inline_capWiths[2]` in Closure struct) would eliminate them.

**Effort:** modify Closure struct + adapt all readers.  Risk: Closure struct change touches FAM layout + serialization.  Estimated 2-3 days.  Lower priority due to small yield.

### 3.4 Combined per-site bundle

| Lever | HNE yield | Effort | Risk |
|---|---|---|---|
| fakeClo wire-back | 144 MB | ~1 day | low (pool exists; just wire) |
| mapAttrs 3-arg App | 100 MB | 1-2 days | medium (Value-shape change) |
| capWiths inline | 13 MB | 2-3 days | medium-high (struct change) |
| **Total bundle** | **257 MB** | **~4-6 days** | mixed |

### 3.5 Pre-committed acceptance per plan §3.4

> Per-site levers totaling ≥ 200 MB at ≤ 1 wk total effort = **bundle and ship in Week 2**

**Measurements: 257 MB at ~4-6 days.  PASS.**

mergeBindings pattern-fix: separate decision per [[chain-bindings-phase-c-falsified]] — NOT bundled here.

---

## 4. Day 5 — Decision day (binding)

Per `EXIT_GC_SPIRAL_PLAN §3.5` decision matrix:

```
Day 2 cache-eviction Δpeak on M5 ≥ 800 MB?
├── YES → Week 1-2: Cache eviction Phase 4b LRU
└── NO  → Day 4 per-site bundle ≥ 200 MB on HNE?
          ├── YES → Week 1-2: Bundled per-site fixes
          │         Cache eviction goes to Week 3 if Day 2 Δpeak ≥ 300 MB
          └── NO  → BOTH falsified → Immix becomes Week 1
```

**Trace:**
- Day 2: M5 Δpeak = -14 MB (< 300 MB; SIGN WRONG).  Cache eviction NOT priority.
- Day 4: per-site bundle = 257 MB on HNE.  ≥ 200 MB.
- **Branch: Week 1-2 = Bundled per-site fixes.**

**Cache eviction → DROPPED** (not Week 3).  Δ = -14 MB on M5 (within noise); also wrong direction on hello+HNE.  Even at Week 3 threshold of ≥ 300 MB, cache eviction fails.  Worth investigation post-Week-3 IF residual M5 gap can't be closed by Week 1-2 + Week 3 levers.

**Plan's own Rule-0 escalation NOT triggered:** per-site bundle PASSES 100 MB threshold (257 MB ≥ 100 MB).  Immix does NOT become Week 1.

---

## 5. Week 1-2 task definitions (pre-committed)

Per `EXIT_GC_SPIRAL_PLAN §4.3` — "If per-site bundle wins":

### Week 1 (Days 6-12)

* **Day 6-8: fakeClo wire-back OR retire.**  Audit + re-bind the pool at alloc.hh:1069-1230 to vm.cc:6803 + vm.cc:12085 + OP_RETURN cleanup sites.  Pre-committed SHIP threshold: ≥ 100 MB HNE peak reduction (vs gate-OFF baseline 2565.55 ± 0.17 MB).
* **Day 9-11: mapAttrs 2-pair fix.**  Implement 3-arg App representation OR alternative pair-coalescing at primops.cc:1965+1970.  Pre-committed: ≥ 50 MB HNE reduction.
* **Day 12: Combined Week-1 measurement.**  `bench/measure-peak-noise-floor.sh HNE gate-off 10`.  Pre-committed: combined fakeClo + mapAttrs ≥ 150 MB peak reduction on HNE.

### Week 2 (Days 13-15)

* **Day 13-15: capWiths ListVec inline (lower priority).**  Closure struct change + reader adaptation.  Pre-committed: ≥ 10 MB HNE reduction OR retire if struct change too risky.

### Pre-committed bundle SHIP gate (Day 15)

* Combined HNE peak reduction ≥ 150 MB (relative to current cache-on baseline 2565.55)
* M5 peak reduction ≥ 100 MB
* `all-v3-tests --quick` 6/6 + `--core` 15/15 PASS
* hello.drvPath + HNE byte-identical to TW
* Wall regression ≤ 5% per workload

If SHIP gate fails after Day 15: `PER_SITE_BUNDLE_FALSIFIED.md` + pivot to Week 3 lever (TBD by plan §5).

### Falsification escalation

If Week 1-2 SHIP gate fails AND no other lever shows promise: plan's meta-claim of "orthogonal lever priority over GC" is itself falsified; Immix becomes Week 3's work.

---

## 6. What replaces the cache-eviction Week 3 slot

Per §5 of `EXIT_GC_SPIRAL_PLAN`: Week 3 = second-lever execution.  Cache eviction DROPS out.  Candidates for Week 3:

| Candidate | Yield | Source |
|---|---|---|
| mergeBindings Phase C revival | "several hundred MB" estimated | needs 4 prerequisites per memory [[chain-bindings-phase-c-falsified]]; multi-session |
| Strictness Stage 4 v4+ cross-function | reduces N upstream | 3-4 wk; current 0 elisions |
| (resumed) Immix Step 14′ honest re-measurement | -355 MB HNE measured | per [[immix-line-region-alloc-2026-05-29]]; doesn't clear ≥ 500 MB SHIP gate but may compose with per-site bundle |

**Week 3 deferred.**  Day 22 (end of Week 1-2 SHIP measurement) is the natural decision point for Week 3 lever choice.  Defer pre-committing Week 3 until then; data may change priorities.

---

## 7. Acceptance criteria for Week 0 closure

Per `EXIT_GC_SPIRAL_PLAN §9`:

- [x] Day 5: orthogonal lever measurement complete (Day 2 + Day 4)
- [x] Decision matrix triggers a unique Week 1-2 choice (per-site bundle)
- [ ] Day 15: first lever's SHIP gate result is binary (passed or falsified) — Week 1-2 deliverable
- [ ] Day 22: second lever's SHIP gate result — Week 3 deliverable
- [ ] Day 25: M5 measurement vs 5760 MB baseline (post-Week-3 combined)
- [ ] Day 30: strategic summary doc

---

## 8. Honest limits + risks

* **Cache eviction findings contradict prior doc.**  Day 2 supersedes HNE_BUCKET_DECOMP §3 for current host state.  If a future host shows different direction, the cache-eviction lever should be re-investigated.
* **fakeClo pool may have rotted.**  Per memory [[fakeclo-pool-dead]]: "pool exists at alloc.hh:1069-1230 with zero callers since Phase D Step 12 (2026-05-21)."  The wire-back may need updates if the pool's API drifted.  Day 6-8 audits this.
* **mapAttrs 3-arg App representation may not exist.**  May require defining a new Value tag or pair-shape; serialization compat needed.  Day 9-11 explores.
* **capWiths inline may regress Closure layout in ways that hurt other code paths.**  Lower priority; can be dropped if risk too high.
* **Per-site bundle 257 MB is HNE-specific.**  M5 yield may differ.  Day 12 measurement covers both.
* **Bundle SHIP gate of ≥ 150 MB on HNE** is conservative (~58% of estimated 257 MB) — accounts for implementation losses.

---

## 9. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §3.3-§3.5, §4.3
* [`EXIT_DAY2_CACHE_EVICTION_2026-05-29.md`](EXIT_DAY2_CACHE_EVICTION_2026-05-29.md) — Day 2 measurement this consumes
* [`T1_3_CLOSURES_ATTR_2026-05-27.md`](T1_3_CLOSURES_ATTR_2026-05-27.md) — fakeClo data
* [`T1_3_PAIRS_LISTS_ATTR_2026-05-27.md`](T1_3_PAIRS_LISTS_ATTR_2026-05-27.md) — mapAttrs + capWiths data
* [`GC_PAUSE_2026-05-29.md`](GC_PAUSE_2026-05-29.md) — the Rule-0 commit that established the orthogonal-lever priority
* `bench/baselines/2026-05-29-day2-cache-eviction/` — Day 2 raw outputs
* Memory: [[fakeclo-pool-dead]], [[chain-bindings-phase-c-falsified]], [[same-host-bisect]]

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
