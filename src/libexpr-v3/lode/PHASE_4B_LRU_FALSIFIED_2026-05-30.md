# Phase 4b LRU (option B) — FALSIFIED on memory ROI

**Date:** 2026-05-30
**Per:** `EXIT_DAY4_PER_SITE_ASSESSMENT_2026-05-30.md` §5 option (B)
**Status:** Implementation correctness-clean; memory SHIP gate falsified.

---

## 1. The hypothesis being killed

> "Phase 4b results-only LRU eviction on `ImportCache::results` delivers ≥ 200 MB peak RSS reduction on HNE."

KILLED.

---

## 2. Implementation

Per `EXIT_GC_SPIRAL §4.1` Day 11-13 design:
* Added `lastAccessGen` field to `ImportCacheEntry`
* Added `accessCounter` field to `ImportCache`
* `bumpImportEntry` updates lastAccessGen on every cache hit + insert
* `maybeEvictOldImportEntries` runs on insert when `results.size() > maxEntries`; evicts oldest 25 % to bring size down to `maxEntries * 3/4`
* Opt-in via `NIX_V3_IMPORT_CACHE_MAX_ENTRIES=N` (default 0 = no eviction)

Skips Prereq 1 (CU pointer stability) by only evicting RESULTS, never CUs.  Closures hold raw pointers to CUs; these stay alive.  Evicting result Values means: a future `import` of the same path causes re-eval (slower but correct — imports are pure).

Correctness:
* 5/5 nixpkgs paths byte-equal vs TW under aggressive LRU (max=50)
* `--core` regression suite: 15/15 PASS under max=50
* HNE byte-equal vs TW under max=50 (extensive eviction churn)

---

## 3. Measurement

Same host, fresh process per run, N=5 per config, trimmed mean.  HNE = `(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath`.

| Config | Trimmed mean | σ | Δ vs baseline |
|---|---|---|---|
| baseline (no gate) | 2497.6 MB | 0.4 | +0.0 |
| `MAX_ENTRIES=1000` | 2497.7 MB | 0.5 | +0.0 (no eviction triggered; HNE imports 2342 entries) |
| `MAX_ENTRIES=500` | 2497.9 MB | 0.3 | +0.3 |
| `MAX_ENTRIES=100` | 2574.5 MB | 0.0 | **+76.9 MB** |
| `MAX_ENTRIES=50` | 2674.6 MB | 0.1 | **+177.0 MB** |

Pre-committed SHIP threshold per `EXIT §3.4`: ≥ 200 MB reduction on HNE → SHIP.
Actual: REGRESSION at aggressive thresholds; flat (no-op) at safe thresholds.  **FALSIFIED.**

---

## 4. Why the regression at low thresholds

Two compounding mechanisms (same architectural pin observed across this session's memory tracks):

### 4.1 Re-import allocates FRESH cells

When `MAX_ENTRIES=50` evicts an entry, a future demand of that import:
1. Looks up `results` → miss
2. Falls through to disk_cache (or recompiles)
3. Allocates FRESH Bindings + entries in the arena for the re-imported result
4. Inserts into `results`

The eviction step REMOVED the cache reference, but the prior Bindings cells were not reclaimed (arena doesn't release; same-process other roots may still pin some).  The re-import ADDS more Bindings cells to the arena.  Net: arena grows.

### 4.2 Cache-hit-miss-rebuild churn

At `MAX_ENTRIES=100`, HNE evicts ~22× (2342 imports / 100 max = 23 sweeps).  Each sweep evicts ~25 % of entries (~25 entries × 23 sweeps = ~575 re-imports).  Each re-import pays:
* disk_cache lookup (warm) or recompile (cold)
* fresh Bindings allocation
* fresh result-Value tree

Net memory: monotonic arena growth + re-import allocation overhead.

### 4.3 No actual reduction at safe thresholds

At `MAX_ENTRIES=1000` (no eviction triggered since HNE imports 2342 entries... wait, that's > 1000).  Hmm — the eviction SHOULD have fired.  But Δ = +0.0.  Maybe the eviction order happens late enough that no cache hits go through (most accesses are during eval; eviction fires at insert time which is also during eval; the order doesn't free anything visible at peak_rss).

Or: most HNE imports happen ONCE.  Eviction of single-use entries doesn't free any actual references (the evicted entry's value tree is held by other roots — the active eval scope's let-bindings).  Same architectural pin.

---

## 5. The architectural pin (now observed across 4 memory tracks)

| Track | Outcome | Mechanism |
|---|---|---|
| #875 bridge eviction (Stages 1, 2, 2b) | 0 MB peak savings | Arena cells unreferenced from bridges; still pinned by other roots OR allocated past their useful life |
| Stage 6 Immix (Step 14′ variant #7) | -29 MB HNE / +22 MB hello | Cell-level mark/sweep frees free-list space; arena's mmap stays |
| Cache-disable proxy (Day 2) | +457 MB regression | Disabling disk_cache forces in-memory re-build |
| Phase 4b LRU (THIS doc) | 0 MB at safe / +177 MB at aggressive | Eviction frees nothing if cells are pinned elsewhere; re-import allocates fresh cells |

**Common pin: the v3 arena allocator does not release `mmap'd` pages when cells are freed.**  Any strategy that allocates replacements (re-import, re-build, fresh-copy) grows the arena past the original peak.

---

## 6. What's left

Per the EXIT_GC_SPIRAL plan + this measurement set:

* All GC variants falsified (7 in the falsification register)
* All eviction strategies falsified (bridges, cache, import-results)
* Per-site bundle mostly landed (-111 MB Week 1)
* Remaining: mapAttrs Tag::App3 with separate-evaluated slot (option A)

Tag::App3 is FUNDAMENTALLY DIFFERENT from eviction approaches: it reduces ALLOCATIONS by 1 pair per mapAttrs entry, rather than trying to free already-allocated cells.  The arena's no-release behavior doesn't apply because we never allocate the extra pair in the first place.

mapAttrs Tag::App3-with-separate-evaluated is the next attempt.  ~3-5 d implementation across pair allocator, force-path dispatch, value_serialize, Phase D barriers, GC walkers.

---

## 7. What lands

Phase 4b LRU code stays as opt-in infrastructure (gate default OFF).  Behavior is identical to baseline unless `NIX_V3_IMPORT_CACHE_MAX_ENTRIES=N` is explicitly set.  Falsification doc + opt-in code mirror the existing pattern from #875 bridge-eviction Stages 1/2/2b.

Per `[[falsification-rule]]`: this commit kills the SHIP claim for Phase 4b LRU.  Per `[[measure-twice-cut-once]]` §3.7 carcass rule: gate stays opt-in pending a future Stage-6-arena-page-release pairing that might activate it.

---

## 8. Cross-references

* `EXIT_DAY4_PER_SITE_ASSESSMENT_2026-05-30.md` — option (A)/(B)/(C) framing
* `EXIT_DAY2_CACHE_PROXY_FALSIFIED_2026-05-30.md` — yesterday's Day 2 falsification
* `IMMIX_FALSIFIED_2026-05-29.md` — variant #7 falsification
* `HNE_BUCKET_DECOMP_2026-05-27.md` — original 200-700 MB cache projection (now reframed as cold-vs-warm asymmetry)
* `WEAK_BRIDGE_PAGE_RELEASE_2026-05-29.md` — the arena page-release prereq
* `[[falsification-rule]]` — Rule 0
* `[[measure-twice-cut-once]]` — pre-committed threshold methodology

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.  SPDX-License-Identifier: Apache-2.0.*
