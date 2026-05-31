# #875 Path A whole-block release — FALSIFIED

**Date:** 2026-05-31
**Per:** user directive "Continue with Path A. Ultrathink! Make no mistakes."
**Commit at spike:** `0315e9e8d` (post FFI-kill-plan)
**Status:** **FALSIFIED at premise level** by empirical measurement.  Whole-block madvise / page-release mechanism cannot recover ≥200 MB on HNE (the SHIP-gate workload) because **zero blocks** reach 100% dead under bump-allocation steady state.

---

## 1. The hypothesis under test

Per `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` Path A design:

> "After bridge eviction (or other cell-death), some arena blocks become entirely empty and can be released via `madvise(MADV_DONTNEED)` or `free()`."

**Pre-committed SHIP gate** (same doc §"Path A"):
* `HNE peak_rss ≥ 200 MB reduction` under Stage 2 (sweep) or Stage 2b (pre-evict)

---

## 2. Cheap-spike methodology

Per `[[measure-twice-cut-once]]` §3 — cheap directional proxy BEFORE multi-week impl.

The existing `NIX_V3_BLOCK_PROBE=1 NIX_V3_LIVE_TRACE=1` infrastructure (live_trace.cc:1124-1294) already produces per-block fully-dead-line distributions at four line-size granularities (64 / 128 / 256 / 512 B).  `reportLinesAtSize` reports `blocks all-dead=N mostly-dead=M mixed=K mostly-live=L all-live=P` for each block in the arena at end-of-eval.

**Key field:** `blocks all-dead` — count of arena blocks with >99 % of lines dead.  Each such block is releasable via whole-block madvise/free WITHOUT needing line-region allocator infrastructure.

Spike cost: ~0 LoC (existing infra) + ~5 min measurement.  Pre-committed acceptance:
* `blocks_all_dead × 16 MB ≥ 200 MB` on HNE → Path A whole-block viable → proceed with 1-week impl
* `< 200 MB` → Path A whole-block falsified → close

---

## 3. Measurement data (today, commit `0315e9e8d`)

### 3.1 HNE — the SHIP-gate workload

```
HNE arena = 1476.4 MB (deterministic counter)
N = 87 blocks at kBlockSize=16 MB

  Line size 64 B:   blocks all-dead=0  mostly-dead=8  mixed=77 mostly-live=2 all-live=0
                    pinned (live)=694.7 MB  fully-dead=764.9 MB (52.4 % of lines)
                    bump-realloc-recoverable: 52.4 % of arena bytes

  Line size 128 B:  blocks all-dead=0  mostly-dead=2  mixed=83 mostly-live=2 all-live=0
                    49.7 % line-dead = 725.0 MB recoverable

  Line size 256 B:  blocks all-dead=0  mostly-dead=0  mixed=85 mostly-live=2 all-live=0
                    45.9 % line-dead = 670.1 MB recoverable

  Line size 512 B:  blocks all-dead=0  mostly-dead=0  mixed=85 mostly-live=2 all-live=0
                    41.0 % line-dead = 598.8 MB recoverable
```

**Whole-block-releasable bytes (Path A target):  0 × 16 MB = 0 MB.**

**Vs SHIP threshold (≥200 MB):  fails by 200 MB.  Falsified.**

### 3.2 hello.drvPath — secondary workload

```
hello.drvPath arena = ~528 MB (33 blocks)

  Line size 64 B:   blocks all-dead=11 mostly-dead=3  mixed=0 mostly-live=7  all-live=11
                    43.6 % line-dead

  Line size 128 B:  blocks all-dead=11 mostly-dead=3  mixed=0 mostly-live=5  all-live=13
                    43.4 % line-dead

  Line size 256 B:  blocks all-dead=11 mostly-dead=3  mixed=0 mostly-live=3  all-live=15
                    43.2 % line-dead

  Line size 512 B:  blocks all-dead=9  mostly-dead=5  mixed=0 mostly-live=3  all-live=15
                    43.0 % line-dead
```

**Whole-block-releasable bytes: 11 × 16 MB = 176 MB on hello.drvPath.**

This is **below the 200 MB threshold** even on the favourable workload.  And hello.drvPath is NOT the SHIP-gate workload (Path A's design doc specifies HNE).

Cross-reference: `IMMIX_LINE_OCCUPANCY_2026-05-29.md` measured 13 blocks all-dead on hello at 64 B (208 MB).  Today reproduces at 11 blocks (176 MB) — Tag::App3 (`3d64028e8`) shrank the arena slightly (smaller arena = fewer total blocks = fewer reclaim-eligible blocks).  Confirms reproducibility of the measurement methodology + minor drift from intervening commits.

### 3.3 Distribution shape — why HNE is structurally different from hello

| Workload | all-dead | mostly-dead (>75%) | mixed (25-75%) | mostly-live | all-live |
|---|---:|---:|---:|---:|---:|
| hello (64 B) | 11 (35%) | 3 (10%) | 0 | 7 (22%) | 11 (35%) |
| HNE (64 B) | **0** | 8 (9%) | **77 (89%)** | 2 (2%) | 0 |

**hello.drvPath shows bimodal distribution**: blocks are either fully-dead OR fully-live.  This is consistent with cohort-clustered allocation patterns — phase 1 builds parser state (later all-dead); phase 2 builds drv eval state (still-live).

**HNE shows unimodal mixed distribution**: 89 % of blocks have between 25-75 % of lines dead.  No block is fully-dead.  This is consistent with intermixed-lifetime allocations — bridge-table-retained cells distributed across every block in the arena.

---

## 4. Structural reasoning — why bump-allocation can't deliver

### 4.1 Bump-allocation chronologically distributes cells

The v3 arena (`alloc.hh:866+`) uses bump allocation: cells are placed consecutively in the active block until it fills (16 MB), then a new block is allocated and cells continue.  Cells of varied lifetimes are interleaved by allocation order, not lifetime.

When a long-lived cell (e.g. a bridge-table-retained Bindings) is allocated, it pins the block containing it until that cell becomes unreachable.  Per `BRIDGES_HOLD_RETENTION_2026-05-29.md`, 99.8 % of HNE live arena is held in bridge tables; those entries are scattered chronologically across ALL 87 blocks.

**Therefore every HNE block contains at least one bridge-retained long-lived cell → no block is ever 100% dead.**

### 4.2 Mid-eval doesn't help

The end-of-eval measurement is the MOST favourable case for finding all-dead blocks (smallest live set).  Mid-eval, even more cells are temporarily alive (during peak working set).

Per the `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31` analysis: even if Path A fired mid-eval and released pages BEFORE peak_rss high-water mark, the structural reason (every block has at least one long-lived cell) doesn't go away.

### 4.3 Page-level (16 KB) extrapolation

The 512 B line size data is the nearest we have to page-level granularity.  At 512 B with 0 blocks all-dead and 2 mostly-live, the distribution stays unimodal-mixed.  At 16 KB (32× larger), the dead-fraction per page would be LOWER (more cells per page → harder for ALL to be dead).

Page-level Path A would yield approximately:
* HNE: 8 mostly-dead blocks × ~12 MB (75 % × 16 MB) **IF** the dead cells happened to cluster into 16 KB pages = upper bound ~96 MB.  Realistic yield: substantially less (scattering reduces clustering).
* hello: already over-fit per 11 all-dead blocks; no additional headroom from page granularity.

**Page-level Path A on HNE: best-case ~96 MB; below 200 MB threshold.  Still falsified.**

### 4.4 Cell-clustering would require Immix allocation

For arena blocks to reach 100% dead in steady state, cells must be allocated by COHORT (lifetime) rather than CHRONOLOGY (order).  This is exactly what Immix's line-region allocator does — recent allocations cluster into recent blocks; old blocks may become entirely dead.

Immix is in-tree behind `NIX_V3_MAJOR_GC=1` opt-in.  Phase 4 SHIP gate FALSIFIED at +22 MB regression vs ≥200 MB reduction required (per `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md`).  So even the Immix path doesn't currently deliver Path A's required yield.

---

## 5. Pre-committed acceptance — result

Per `[[measure-twice-cut-once]]`, binary decision:

* HNE `blocks_all_dead × 16 MB = 0 MB`
* Pre-committed threshold: `≥ 200 MB`
* **VERDICT: FALSIFIED.  Path A whole-block release cannot ship.**

Per `[[falsification-rule]]`:

* **Hypothesis killed**: "Path A whole-block madvise mechanism can recover ≥200 MB of HNE peak_rss."
* **Mechanism of falsification**: HNE bump-allocation with bridge-retained long-lived cells distributed across every block.

Per `[[null-lever-not-null-value]]`:

* No architectural-value override.  Path A's distinctive contribution was supposed to be lightweight cell-tracking → madvise.  The mechanism doesn't fire even in principle on HNE.  Nothing to preserve.

---

## 6. What stays (preserved infrastructure)

* The `NIX_V3_BLOCK_PROBE=1 NIX_V3_LIVE_TRACE=1` infra is a load-bearing diagnostic for future GC decisions.  Keep.
* `IMMIX_LINE_OCCUPANCY_2026-05-29.md` and this doc together form a now-load-bearing falsification record — future GC design must reference this data.
* Bridge eviction (`evictBridgeEntry` at `primops.cc:4082`) is correctness-clean; stays.  Its memory contribution is zero today but the infrastructure is benign and could combine with a future page-release scheme.
* `Arena::releaseBlock` (`alloc.hh:1675-1745`) — the whole-block-free path used by Stage 6.  Stays (opt-in `NIX_V3_MAJOR_GC=1` whole-block-free).  Path A's distinct contribution would have been to also do madvise-without-free for partial-block scenarios, which is now falsified.

## 7. What this kills (cleanup candidates — for separate consideration)

* `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` Path A section can now be marked FALSIFIED.  Replace the Path A description with a pointer to this doc.
* `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Path A bullet — same treatment.
* Task #891 — close as FALSIFIED with reference to this doc.

## 8. What this DOESN'T kill

* **Stage 6 Immix**: paused per `GC_PAUSE_2026-05-29` for unrelated reasons (Phase 4 SHIP gate falsified).  Different mechanism (line-region allocator); not refuted by this measurement.
* **The bridge-elimination work** (FFI_KILL_PLAN_2026-05-31): orthogonal.  Eliminating bridges removes the long-lived cells that pin blocks.  If bridge tables shrink substantially (per Phase C of the FFI plan), the per-block live distribution may shift toward bimodal (like hello).  Re-measurement after FFI Phase C is warranted.
* **Page-level (16 KB) Path A**: not formally measured (would need new probe at 16384 B line size).  Best-case extrapolation (~96 MB HNE) is below threshold; full measurement deferred.

---

## 9. What's the next step on memory ROI for HNE / M5?

Given Path A is falsified and Stage 6 Phase 4 is falsified, the ROI candidates remaining (per session-arc inventory):

| Lever | Effort | Mechanism | Status |
|---|---|---|---|
| FFI_KILL_PLAN Phase C (derivationStrict native) | 1-2 wk | eliminates 99.8% bridge retention at source | **PLAN COMMITTED, NOT STARTED** |
| Stage 6 with PROACTIVE trigger (mid-eval, not threshold) | 1-2 wk | distributes GC work BEFORE peak high-water | speculative |
| Page-level Path A spike | 2-3 d | new probe at 16 KB; verify ~96 MB upper bound | not yet measured |
| Combined: FFI Phase C + Stage 6 re-eval | 2-4 wk | post-FFI bridge reduction may re-enable Stage 6 yield | depends on FFI plan |

**Recommendation:** the FFI_KILL_PLAN Phase C (derivationStrict native) is the clearest next bet.  Eliminating the largest bridge-entry source removes the long-lived cells that pin blocks.  Re-measure the block distribution AFTER FFI Phase C lands; if blocks become bimodal (like hello.drvPath), Path A may re-open as a viable lever.  Today it is closed.

---

## 10. Honest limits

* **End-of-eval measurement**: doesn't directly test mid-eval distribution.  But mid-eval can only be WORSE (more cells temporarily alive).
* **N=1 measurement per workload**: deterministic counter at this granularity; σ-envelope not required.
* **`NIX_V3_NO_DISK_CACHE=1` was set**: matches measurement methodology of `EXIT_POST_APP3_BASELINE_2026-05-31`.  Without this gate the cache-warm path skips most v3 work and arena is smaller (~17 MB).
* **Path A's design doc revision (2026-05-29 evening)** acknowledged the cell-size-tracking blocker requires Stage 6 infrastructure.  This spike's structural finding (no all-dead blocks on HNE) is a stronger falsification — the blocker becomes moot because the mechanism doesn't fire even with cell-size tracking.
* **The 176 MB on hello.drvPath** is close to threshold (above 88% of 200 MB).  IF Path A's threshold were keyed to hello rather than HNE, the verdict would be borderline.  But the design doc keys HNE, and HNE is 0 MB.  Falsified.
* **M5 not directly measured**: M5 has similar distribution-of-lifetimes to HNE (cardano-node deep attrsets); expected similar 0-blocks-all-dead.  Could be measured for completeness but the conclusion is unlikely to change.
* **Stage 6 Phase 4 falsification (2026-05-29)** suggests the same conclusion via a different measurement — the underlying issue is bump-allocation cell distribution.  Two independent falsifications of related mechanisms converge on the same root cause.

---

## 11. Cross-references

### Strategic (this session)
- `FFI_BRIDGE_INVENTORY_2026-05-31.md` — the bridge retention finding that motivated Path A revival
- `FFI_KILL_PLAN_2026-05-31.md` — orthogonal lever; recommended next bet
- `EXIT_POST_APP3_BASELINE_2026-05-31.md` — measurement baseline
- `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md` — peak_rss vs current_rss distinction

### Path A design history
- `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` — original Path A design + revision noting Stage 6 dependency
- `PHASE_E_V02_PATH_A_FALSIFIED_2026-05-27.md` — earlier Path A variant falsification (different mechanism)

### Adjacent falsifications
- `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md` — Stage 6 flat-MS falsified at +22 MB regression
- `IMMIX_LINE_OCCUPANCY_2026-05-29.md` — original block-emptiness data (reproduced today)

### Code anchors
- `live_trace.cc:1124-1294` — `reportLinesAtSize` (the probe)
- `live_trace.cc:1534` — `NIX_V3_BLOCK_PROBE` gate
- `alloc.hh:866` — kBlockSize = 16 MB
- `alloc.hh:1675-1745` — `releaseBlock` whole-block-free path (preserved)
- `primops.cc:4082` — `evictBridgeEntry` (preserved)

### Methodology
- `[[falsification-rule]]`
- `[[measure-twice-cut-once]]`
- `[[null-lever-not-null-value]]` — no architectural-value override applies
- `[[memory-first-class]]` — arena counter is primary; HNE 0 blocks all-dead is decisive

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
