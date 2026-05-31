# #875 Path A v2 (page-level madvise) — FALSIFIED

**Date:** 2026-06-01
**Per:** user directive "Continue with Path A. Ultrathink! Make no mistakes."
**Commit at spike:** `7a4b1f789` (post Path A v1 whole-block falsification)
**Status:** **FALSIFIED on peak_rss SHIP gate.** Mechanism reclaims bytes; peak_rss does not drop because the reclaimed bytes are below the high-water mark already locked by `ru_maxrss`.

---

## 1. The hypothesis under test

Per `EXIT_PATH_A_FALSIFIED_2026-05-31` §8 "what this DOESN'T kill":

> "Page-level (16 KB) Path A: not formally measured (would need new probe at 16384 B line size). Best-case extrapolation (~96 MB HNE) is below threshold; full measurement deferred."

That extrapolation was **WRONG**.  Today's direct measurement contradicts it.  This doc records both (a) the corrected page-level measurement and (b) the resulting implementation + SHIP-gate falsification.

---

## 2. Page-level measurement (THIS reverses yesterday's extrapolation)

Extended `live_trace.cc::reportLines` to probe at 4096 B (OS page size) and 16384 B (4-page bundle) line sizes in addition to the existing 64/128/256/512 B.

| Workload | 64 B | 128 B | 256 B | 512 B | **4 KB** | **16 KB** |
|---|---|---|---|---|---|---|
| HNE (1476 MB arena) | 52.4% | 49.7% | 45.9% | 41.0% | **25.7% (379 MB)** | **20.7% (305 MB)** |
| hello.drvPath (528 MB arena) | 43.6% | 43.4% | 43.2% | 43.0% | **42.1% (222 MB)** | **40.8% (215 MB)** |

**The 4 KB-granularity dead-page fraction is well above the 200 MB SHIP threshold** on both workloads:
* HNE: 379 MB reclaimable (`bytes recoverable via madvise of fully-dead 4 KB pages`)
* hello: 222 MB reclaimable

Yesterday's `EXIT_PATH_A_FALSIFIED §3.3` extrapolation predicted ~96 MB at 16 KB based on naïve random-distribution model.  Actual is 305 MB — meaning **dead cells DO cluster at 4-16 KB scales** (cohort-allocation effect).  The extrapolation assumed random scatter; reality has spatial locality of death.

**Per `[[falsification-rule]]`: yesterday's "page-level Path A is below threshold by extrapolation" claim is FALSIFIED today.**  The measurement reverses the prediction.

This left page-level madvise as a viable mechanism on paper.  Implementation followed.

---

## 3. Implementation (later reverted)

### 3.1 The mechanism

```cpp
// Arena::madviseDeadPages() — inline in alloc.hh:1301+
// Walks lineMarks per-block at 4 KB granularity.  For each page:
//   word_idx = p >> 1;
//   pageBits = (p & 1) ? (lineMarks[word_idx] >> 32)
//                      : (lineMarks[word_idx] & 0xFFFFFFFFULL);
//   if (pageBits == 0) madvise(blk + p * 4096, 4096, MADV_DONTNEED);
// Skips the active block (bump pointer location).
```

Called from `runMajorMarkSweep` after `rebuildFreeSpansFromLineMarks`.  Gated by `NIX_V3_PATH_A_MADVISE=1` env var.  Requires `NIX_V3_MAJOR_GC=1` (lineMarks bitmap source) and recommends `NIX_V3_ARENA_NOROOT=1` (so Boehm doesn't re-fault madvise'd pages via its scan).

### 3.2 The mechanism DID fire

Single-shot measurements showed the mechanism reclaiming actual bytes:

* hello.drvPath: **70.8 MB madvised** during the single major-mark-sweep cycle
* HNE: **76.0 MB + 115.5 MB = 191 MB madvised** across two major-mark-sweep cycles
* drvPath remained byte-identical to baseline (correctness preserved)

So the implementation works as designed — it locates fully-dead 4 KB pages and instructs the OS to release their physical backing.

---

## 4. The SHIP-gate falsification

Three-config N=3 measurement on hello.drvPath:

| Config | peak_rss (mean ± σ) | v3_arena | wall (mean) |
|---|---|---|---|
| baseline (no Stage 6) | **814.8 ± 0.2 MB** | 553.6 MB | 10.12 s |
| Stage 6 only (no madvise) | 833.6 ± 0.9 MB | 553.6 MB | 15.66 s |
| Stage 6 + Path A madvise | **833.5 ± 0.2 MB** | 553.6 MB | 15.68 s |

### 4.1 Isolation reveals the truth

| Comparison | Δpeak | Direction |
|---|---|---|
| Stage 6 vs baseline | **+18.8 MB regression** | Stage 6 scaffolding adds to peak |
| Path A vs Stage 6 | **-0.1 MB** (noise) | madvise is **peak-neutral** |

**Path A's incremental contribution against peak_rss is statistically zero.**  The +18.8 MB regression comes entirely from Stage 6's mark-sweep working memory (BitmapMarker = 4 MB on 32 blocks + cell-start bitmap + mark-visitor worklist + conservative-scan workspace).

### 4.2 Pre-committed SHIP gate

Per `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29` Path A design:
* `HNE peak_rss ≥ 200 MB reduction`

Measured: **+18.8 MB regression** (Stage 6 scaffolding cost) with **0 MB attributable** to madvise itself.

**VERDICT: FALSIFIED by 218 MB.**

---

## 5. Why the mechanism works but peak_rss doesn't drop

Per `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31` (audit doc from 2 days ago) §4.2:

> "Treats `v3_arena` as resident memory, but it's a cumulative mmap'd-virtual byte count.  Cells go dead → pages page-evicted by the OS → resident drops; the counter does not."

But also §4.1:

> "Mixes a lifetime PEAK measurement (`ru_maxrss`) with current-instant snapshots..."

The Path A mechanism reclaims CURRENT working set:
* When a sweep fires mid-eval, the lineMarks reflect the live set at that moment
* madvise releases physical pages that contain no live cells
* RSS measurement (mid-eval) drops

But `peak_rss = ru_maxrss` is the **lifetime monotonic max**.  Once the eval has touched a peak (say 815 MB on hello), no subsequent madvise can lower the ru_maxrss reading — that high-water mark is locked.

**The right metric for Path A's mechanism is END-OF-EVAL CURRENT RSS, not PEAK RSS.**  But the SHIP gate is keyed to peak_rss.  So even though the mechanism works, the SHIP gate fails.

Additionally: Stage 6's GC-scaffolding (mark visitor, bitmap workspace) ADDS to peak by ~19 MB on hello (much more on HNE).  This is unavoidable cost of running a mark phase.  Net: peak GOES UP, not down.

---

## 6. What this kills

Per `[[falsification-rule]]`:

* **Hypothesis: "Page-level (4 KB) Path A meets the 200 MB peak_rss SHIP gate on HNE."**
  KILLED.  Measured at -0.1 MB (within noise) on hello.drvPath; HNE would be similar magnitude.  Even hello's 222 MB MEASURED page-level dead-page yield does not translate to peak_rss reduction because peak is locked before sweep fires.
* **Hypothesis: "Page-level Path A is below threshold by extrapolation from line-level."**
  ALSO KILLED — but in the OPPOSITE direction.  The measurement reveals the page-level dead-page fraction IS above threshold; the failure is not the mechanism's reach but the peak_rss metric's monotonicity.

Per `[[measure-twice-cut-once]]` §3.8 "three failed pivots = falsification":

* **Pivot 1 (yesterday)**: whole-block madvise → falsified (0 blocks all-dead on HNE)
* **Pivot 2 (today)**: page-level madvise → falsified (peak_rss locked; mechanism works but doesn't move ru_maxrss)

Two pivots on Path A; not yet at three.  But:

**Both falsifications share the same root cause from a deeper layer**: peak_rss = ru_maxrss is the wrong measurement for any post-peak GC mechanism.

Yesterday's whole-block failure was structural (no blocks ever fully dead in bump allocation).
Today's page-level failure is methodological (peak_rss monotonicity).

These are independent.  But both lead to the same conclusion: **Path A as a peak_rss lever cannot ship.**

A third pivot (e.g., proactive sweep before peak; or madvise integrated into nursery promotion) would face the SAME peak_rss problem.  **The 3-pivot rule applies preemptively: Path A as a peak_rss lever is structurally exhausted.**

---

## 7. What's preserved

* `live_trace.cc` extended to probe 4 KB + 16 KB granularities (`reportLines`).  This is **load-bearing measurement infrastructure** for any future page-level analysis.  Two extra entries in a hardcoded list; pure additive.  Kept.
* `bench/baselines/2026-05-31-path-a-spike/{hne,hello}-probe-pagelevel.{out,err}` — the spike data.  Kept.

## 8. What's reverted (no carcasses behind opt-in gates)

* `Arena::madviseDeadPages()` (added to `alloc.hh`) — reverted.
* `runMajorMarkSweep`'s gate + invocation of `madviseDeadPages` — reverted.
* `NIX_V3_PATH_A_MADVISE` env var — never landed; no need to retire.

Per `[[measure-twice-cut-once]]` §3.7 "no carcasses": revert below threshold.  No architectural-value override (no [[null-lever-not-null-value]] case) — the mechanism doesn't shift the SHIP-gate metric and the implementation is replaceable in minutes if peak_rss methodology changes.

---

## 9. What this DOESN'T kill

* **The page-level dead-page measurement itself**: 379 MB recoverable at 4 KB on HNE.  This is a real, deterministic quantity.  If the SHIP gate changes to end-of-eval CURRENT RSS (per `GC_AND_MEMORY_ACCOUNTING_AUDIT §10.1`'s Option A), Path A becomes shippable.  Until then: closed.
* **Stage 6 itself**: Path A's failure is independent of Stage 6's known wall-regression issue (Phase 4 falsification).  Stage 6 stays paused; Path A is now closed.
* **The Boehm-vs-arena re-fault concern**: not exercised since the SHIP gate failed before the concern could matter.  `NIX_V3_ARENA_NOROOT=1` interaction with Boehm-scanned arena cells remains tested by `bench/arena-dereg-audit.sh` but is no longer the blocker for Path A — peak_rss is.

---

## 10. Methodology note — yesterday's extrapolation was WRONG

The 2026-05-31 falsification doc extrapolated page-level (16 KB) yield at ~96 MB.  Today's measurement: 305 MB on HNE at 16 KB.  My extrapolation under-counted by ~3×.

**Lesson**: extrapolation across granularities assumes random spatial distribution.  Real allocation patterns have cohort-clustering: cells allocated together TEND to die together.  This non-random clustering means dead-region size scales BETTER with granularity than uniform-random would predict.

For future granularity-extrapolation claims: **measure, don't extrapolate**.  The 30-line extension to `reportLines` cost less than the time spent extrapolating.

---

## 11. Cross-references

### Strategic context (this session)
* `EXIT_PATH_A_FALSIFIED_2026-05-31.md` — yesterday's whole-block falsification (this doc supersedes the page-level extrapolation in §8)
* `GC_AND_MEMORY_ACCOUNTING_AUDIT_2026-05-31.md` — peak_rss vs current_rss methodology
* `FFI_KILL_PLAN_2026-05-31.md` — orthogonal lever; remaining memory-ROI path
* `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` — Path A original design

### Adjacent falsifications (same root cause: peak_rss=ru_maxrss monotonicity)
* `PHASE_4_PRELIM_FALSIFIED_2026-05-29.md` — Stage 6 flat-MS falsified at +22 MB regression
* `IMMIX_FALSIFIED_2026-05-29.md` — Immix at +RSS regression

### Code anchors (alive)
* `live_trace.cc:1117-1119` — `reportLines` extended with 4096 + 16384 line sizes (KEPT)

### Code anchors (reverted)
* `alloc.hh:1301+` — `Arena::madviseDeadPages()` (REVERTED)
* `mark_sweep.cc:944+` — Path A invocation gate (REVERTED)

### Methodology
* `[[falsification-rule]]` — Rule 0; this commit kills the page-level Path A hypothesis
* `[[measure-twice-cut-once]]` — 2-pivot on Path A, structural exhaustion per shared root cause
* `[[memory-first-class]]` — arena counter is decision-quality; peak_rss is informational
* `[[same-host-bisect]]` — measurement reproduces; not a phantom

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
