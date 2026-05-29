# EXIT_GC_SPIRAL Day 2 — cache-disable proxy FALSIFIED

**Date:** 2026-05-30
**Per:** `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` §3.2 Day 2 measurement
**Status:** Day 2 measurement complete; proxy hypothesis falsified.  Plan re-routes to Day 4 (per-site bundle assessment).

---

## 1. The hypothesis being killed

> "`NIX_V3_NO_DISK_CACHE=1` is a valid proxy for the Phase 4b LRU cache-eviction yield, per the `HNE_BUCKET_DECOMP_2026-05-27.md` claim of 950 MB peak_rss reduction on HNE."

KILLED.

---

## 2. Measurement

Same host, fresh process per run, N=5 each, trimmed mean (drop min+max).  HNE = `(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath`.

### HNE

| Config | Raw | Trimmed mean | σ (trimmed) | elsewhere mean |
|---|---|---|---|---|
| Baseline (warm cache, default) | 2491.9, 2497.9, 2496.8, 2498.2, 2497.8 | **2497.5 MB** | 0.7 | 600.4 MB |
| `NIX_V3_NO_DISK_CACHE=1` | 2953.6, 2954.1, 2953.9, 2952.6, 2954.6 | **2953.9 MB** | 0.6 | 1057.7 MB |

**Δ peak_rss = +456.4 MB REGRESSION.**

### hello.drvPath (cross-check)

| Config | Trimmed mean | elsewhere mean |
|---|---|---|
| Baseline | 758.4 MB | 0.0 MB |
| `NIX_V3_NO_DISK_CACHE=1` | 813.7 MB | 0.0 MB |

Δ = +55 MB regression.  hello.drvPath doesn't significantly use disk cache (elsewhere=0); the regression here is just re-eval overhead.

---

## 3. Why the proxy is invalid

### 3.1 The prior 950 MB claim

Per `HNE_BUCKET_DECOMP_2026-05-27.md` table at lines 148-154:

```
default (cold cache, first run)              2987 MB
default (warm cache replay)                  2565 MB
NIX_V3_NO_DISK_CACHE=1                       2037 MB
```

The 950 MB number was `2987 (cold) - 2037 (no-disk-cache) = 950`.  Cold-cache baseline measures "first eval after `~/.cache/nix/` cleaned" — heavy SQLite mmap + page-cache fault traffic.  Disabling the cache (NIX_V3_NO_DISK_CACHE=1) skips that traffic entirely.

The doc itself flagged: "single runs each — directional but not reproducible to within MB."

### 3.2 What today's measurement compares

Today: baseline = WARM cache (already populated; subsequent run).  no-disk-cache = cache exists but disabled.

In warm-cache mode:
* Baseline: cache loads pre-compiled CUs from `~/.cache/nix/...sqlite` (~312 MB on disk → mmap'd; in-memory cost moderate)
* no-disk-cache: skips disk load, re-compiles each import.  Compilations stay in the in-memory ImportCache (`primops.cc:7510-7534` `std::deque<CompilationUnit> cus`).  This grows MORE than the cache-load equivalent.

Delta = +457 MB, exactly matching the elsewhere bucket growth (600 → 1058 MB).  The increase is in the in-memory ImportCache (`cus` deque + `results` map), NOT in disk_cache.

### 3.3 What Phase 4b LRU actually needs

The 500-950 MB claim was about the IN-MEMORY ImportCache being evictable.  `NIX_V3_NO_DISK_CACHE=1` doesn't evict; it just shifts work from disk-load to fresh-compile.  Both put the result in the in-memory cache.

Phase 4b LRU would need to:
1. Evict ImportCache entries when they exceed a threshold (in-memory)
2. Re-load from disk_cache on cache-miss (the disk side stays as a slower-but-smaller backing store)

That's the actual implementation work — the Day 2 proxy is unrelated to it.

### 3.4 What the prior measurement was actually measuring

Likely a cold/warm DELTA:
* Cold run: 2987 MB (heavy first-fault page-cache traffic)
* no-disk-cache run: 2037 MB (skips first-fault but re-compiles)

Net: cold-fault traffic on disk_cache is more expensive than re-compile cost.  But neither is the same as "in-memory cache eviction."

---

## 4. What's confirmed today

* **Day 2 proxy is invalid** for measuring Phase 4b LRU yield.
* **Baseline HNE peak_rss has DROPPED from 2565 MB (2026-05-27 warm baseline) to 2497 MB (today's measurement)** — ~68 MB improvement from intervening work.  Cache infrastructure or related code has improved.
* **In-memory ImportCache is the load-bearing memory consumer**, not disk_cache — confirmed by the +457 MB elsewhere growth on no-disk-cache.

---

## 5. Decision tree update

Per `EXIT_GC_SPIRAL_PLAN §3.5` decision matrix:

```
Day 2 cache-eviction Δpeak on M5 ≥ 800 MB?
└── PROXY INVALID — cannot answer ≥800 MB without implementing Phase 4b LRU
```

The plan's branching condition is unmet because the chosen proxy doesn't measure what the branch needs.

Two paths forward:
1. **Implement Phase 4b LRU directly** (no proxy), measure post-impl.  Multi-day work per `HNE_BUCKET_DECOMP §"Implementation prerequisites"` — 3 prereqs:
   - CU eviction blocked by Closures holding raw CU pointers
   - ImportCacheEntry::result has nursery-pointer concerns
   - Hash bucket maps need to evict alongside the deque
2. **Skip to Day 4** per-site bundle (fakeClo + mapAttrs + capWiths) — already-quantified, smaller wins, faster.

**Recommendation: Day 4 first.**  Per-site bundle gives ~257 MB on HNE at known-low effort.  Phase 4b LRU is the higher-yield long-term lever but the prereqs are not free.

---

## 6. Cross-references

* `EXIT_GC_SPIRAL_PLAN_2026-05-29.md` §3.2 — Day 2 procedure (this measurement implements it)
* `HNE_BUCKET_DECOMP_2026-05-27.md` — the 950 MB origin claim (now reframed as cold-vs-warm asymmetry)
* `IMMIX_FALSIFIED_2026-05-29.md` — Variant #7 falsification (yesterday)
* `[[falsification-rule]]` — this commit kills the proxy hypothesis
* `[[measure-twice-cut-once]]` — single-run "directional" claims often falsify under N=5 trimmed mean

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
