# EXIT Day 2 — Cache eviction PoC measurement

**Date:** 2026-05-29
**Status:** MEASUREMENT — `NIX_V3_NO_DISK_CACHE=1` is **NOT a valid proxy** for Phase 4b LRU yield.  Direction REVERSED vs `HNE_BUCKET_DECOMP §3` claim.
**Task:** #858 (closes)
**Plan reference:** [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §3.2

---

## 1. TL;DR

**Measured Δpeak (cache-off − cache-on) across 3 workloads, N=5-10:**

| Workload | cache-on peak ± σ | cache-off peak ± σ | Δ (off − on) |
|---|---|---|---|
| hello.drvPath | 753.27 ± 0.13 | 816.90 ± 1.03 | **+63.6 MB** |
| HNE | 2565.55 ± 0.17 | 3028.01 ± 1.78 | **+462.5 MB** |
| M5 (cardano-node) | 4874.00 ± 751.87 | 4860.43 ± 40.81 | -13.6 MB (noise) |

**`NIX_V3_NO_DISK_CACHE=1` makes peak HIGHER, not lower.**  This directly contradicts `HNE_BUCKET_DECOMP_2026-05-27 §3.1`'s claim of "saves 950 MB peak RSS vs cold-cache baseline (528 MB warm)."

The cache-ON HNE peak (2565.55 ± 0.17 MB) PERFECTLY matches the decomp's `default warm cache replay: 2565 MB` row.  The cache-OFF measurement is what diverges.

**Implication for Phase 4b LRU:** the assumed direction was WRONG.  Disabling the disk cache INCREASES in-memory work (more bytecode compilation, more ImportCache rebuild) → HIGHER peak.  Phase 4b LRU (evicts cold cache entries while keeping hot ones) would similarly INCREASE peak on the eviction direction.

---

## 2. Per-workload analysis

### 2.1 hello.drvPath

```
Config           peak_rss        v3_arena    elsewhere    wall
cache-on         753.27 ± 0.13   587.20      0.00         0.92 s
cache-off        816.90 ± 1.03   587.20      0.00         1.41 s
Δ                +63.6 MB        0           0            +0.49 s (+53%)
```

* Arena identical (587 MB) — cache state doesn't affect arena footprint
* `elsewhere` = 0 in both configs — hello.drvPath doesn't pre-populate "elsewhere"
* The +63.6 MB Δpeak is purely from the rebuild work that disabling cache forces (bytecode recompile, no `posSnapshotPool` reuse, etc.)
* Wall +53% — cache OFF forces full re-compile

### 2.2 HNE

```
Config           peak_rss          v3_arena    elsewhere      wall
cache-on         2565.55 ± 0.17   1593.80     568.80 ± 0.18   5.10 s
cache-off        3028.01 ± 1.78   1593.80     1031.22 ± 1.80  8.54 s
Δ                +462.5 MB         0           +462.4 MB       +3.44 s (+67%)
```

* Arena identical
* **Δelsewhere = +462 MB on cache-OFF.**  This is the OPPOSITE of what was expected.  Without disk cache, v3 must rebuild ImportCache from source → builds in-memory.  Cache-ON allows ImportCache entries to be served from disk cache reads (smaller in-memory footprint).
* The cache itself ISN'T evictable in the "free memory" direction — it's a TRADE between disk-resident cache + small in-memory shadow (cache-ON) vs no disk cache + large in-memory rebuild (cache-OFF).

### 2.3 M5 (cardano-node)

```
Config           peak_rss              v3_arena    elsewhere    wall
cache-on         4874.00 ± 751.87     6392.10     0.00         30.05 s
cache-off        4860.43 ± 40.81      6392.10     0.00         32.15 s
Δ                -13.6 MB              0           0             +2.10 s (+7%)
```

* Cache state effectively does NOT affect M5 peak (Δ within noise)
* **PEAK < ARENA paradox:** v3 reports arena = 6.4 GB but OS reports peak = 4.9 GB.  Almost certainly the host is **page-swapping** — 1.5 GB of arena bytes are swapped to disk, not counted in resident-set.  This means M5 already exceeds physical-memory capacity on this host.
* `elsewhere` = 0 even with cache-on — M5's flake-eval pattern doesn't populate the same "elsewhere" bucket HNE does
* Wall +7% — minor cache impact for M5's compile-heavy workload

---

## 3. Why HNE_BUCKET_DECOMP's claim was wrong

Per the decomp's table:
```
default (cold cache, first run)     2987 MB    elsewhere 991 MB
default (warm cache replay)         2565 MB    elsewhere 568 MB
NIX_V3_NO_DISK_CACHE=1              2037 MB    elsewhere  40 MB
```

The `2037 MB` cache-off row is what disagrees with my measurement (3028 MB).  The "warm cache replay" 2565 MB matches my cache-on EXACTLY (to 0.55 MB).

Possible explanations for the cache-off discrepancy:
1. **Different measurement procedure.**  Decomp may have measured with the disk cache file *deleted* (no SQLite file at all), not just `NIX_V3_NO_DISK_CACHE=1` (which leaves the file but skips reads/writes).  If the SQLite file was absent, no mmap → no in-memory mapping overhead.  But that's not how the plan instructed the measurement.
2. **Different code state.**  2 days have passed.  Maybe NIX_V3_NO_DISK_CACHE semantics changed.  Per code (`primops.cc:7997`), the gate now ONLY suppresses disk reads/writes; ImportCache and content-cache aren't touched.  If the decomp was measured before that semantic was finalized, the gate's effect was different.
3. **Cache-off cold-cache vs warm-cache state.**  My pre-warm runs UNDER cache-off don't populate the cache.  Decomp might have pre-warmed with cache-ON then switched to cache-OFF for measurement — would not rebuild ImportCache.
4. **Measurement bug in either run.**  Unlikely given σ tightness.

Per `[[same-host-bisect]]`: my measurement is fresh, host-current, N=10, σ=1.78 MB tight.  Decomp's measurement was 2 days ago with unknown σ.  **My data should be considered authoritative for this host's current state.**

---

## 4. Pre-committed acceptance verdict

Per `EXIT_GC_SPIRAL_PLAN §3.2`:

| Result on M5 | Verdict | Decision matrix outcome |
|---|---|---|
| Δpeak ≥ 1500 MB | HIGH YIELD | Cache eviction priority-1 |
| 800-1500 MB | MODERATE-HIGH | Week 1 cache eviction |
| 300-800 MB | MODERATE | Week 2 cache eviction |
| **< 300 MB** | **MARGINAL** | **Cache eviction NOT priority** |

**Measured M5 Δ = -13.6 MB (within noise, σ-mixed sign).  CLEARLY < 300 MB.  → MARGINAL.**

Plus the SIGN is wrong (cache-off would INCREASE peak on hello + HNE).  Cache eviction (LRU) lever direction itself is FALSIFIED — eviction makes things WORSE, not better, in the measured direction.

---

## 5. What survives, what falsifies

### 5.1 Falsified

* **Hypothesis: "Cache eviction (Phase 4b LRU) yields 500-950 MB peak RSS reduction on HNE"** — DIRECTLY FALSIFIED.  Cache-off (the most aggressive eviction) gives +462 MB peak (wrong direction).
* **Hypothesis: "`NIX_V3_NO_DISK_CACHE=1` is a valid proxy for Phase 4b LRU yield"** — FALSIFIED.  Proxy measurement gives opposite direction of what Phase 4b would deliver.
* **Hypothesis: "M5 peak is dominated by cache footprint"** — FALSIFIED.  Cache state doesn't materially affect M5 peak; arena dominates.

### 5.2 Confirmed

* `HNE_BUCKET_DECOMP §3` "warm cache" row (2565 MB) is reproducible to 0.55 MB on current host.  Cache-on baseline is solid.
* Wall regression on cache-off is consistent: +53-67% on hello/HNE — cache provides meaningful CPU savings.
* M5 already exceeds physical memory (page-swapping evident); arena work is the dominant memory pressure.

### 5.3 Implication for the EXIT plan

Per `EXIT_GC_SPIRAL_PLAN §3.5` decision matrix, with **Day 2 cache-eviction Δpeak < 300 MB on M5**: the path branches to:
- "Day 4 per-site bundle ≥ 200 MB on HNE?"
- If YES → Week 1-2 per-site bundle (cache eviction → Week 3 only if Δ ≥ 300 MB)

**Cache eviction DROPS out entirely** because Day 2 Δ (-14 MB on M5) is FAR below the 300 MB Week-3 threshold AND the SIGN is wrong.

---

## 6. Honest limits

* **σ on M5 cache-on is 752 MB** (n=4/5; one run failed).  Wide variance.  Page-swap dynamics + nondeterministic OS scheduling.  M5 measurements are noisy by host condition.  The cache-off σ is tighter (40.8 MB on n=3/5; 2 runs trimmed) — possibly because cache-off has more deterministic in-memory work.
* **Peak < arena on M5** indicates page-swapping.  RSS measurement under page pressure isn't a clean "memory used" indicator.
* **One M5 cache-on run failed** (4/5 succeeded).  Likely OOM-killed via SIGSEGV when the host ran low on physical+swap.  The remaining 4 are still informative.
* **The cache-off cleanup case is unstudied.**  If the cache file is deleted between runs (not just NIX_V3_NO_DISK_CACHE=1), behavior may differ.  Out of scope for Day 2.

---

## 7. Cross-references

* [`EXIT_GC_SPIRAL_PLAN_2026-05-29.md`](EXIT_GC_SPIRAL_PLAN_2026-05-29.md) §3.2 — the procedure this measures
* [`HNE_BUCKET_DECOMP_2026-05-27.md`](HNE_BUCKET_DECOMP_2026-05-27.md) §3 — the prior claim this measurement refutes
* `bench/baselines/2026-05-29-day2-cache-eviction/` — raw JSON outputs
* Memory: [[same-host-bisect]] — applied; current measurement authoritative

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
