# HNE bucket decomposition — first end-to-end measurement

**Date**: 2026-05-27
**Workload**: `(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath`
**Mode**: v3-direct + `NIX_VM_STATS=1` + `NIX_V3_LIVE_TRACE=1` + `NIX_V3_BINDINGS_ATTR=1`
**Result**: byte-identical to TW (drvPath unchanged)
**Goal**: Category 1 highest-ROI item per `MEMORY_REDUCTION_AVENUES_2026-05-26.md`:
"HNE NIX_VM_STATS bucket decomposition" — tells us where the 3 GB
lives; gates everything else.

## TL;DR

* **peak_rss = 2987.5 MB** (was last measured at 3003 MB pre-#751/#752
  era; effectively unchanged)
* **v3_arena = 1593.8 MB**, **boehm_heap = 402.9 MB**,
  **elsewhere = 990.8 MB**
* Live-fraction trace: **796.93 MB freeable** at end-of-run — SHIP-GREEN
  for Stage 6 (4× the 200 MB threshold)
* mergeBindings dominates `OP_ATTRS_UPDATE_TAIL` = 546.7 MB
  (98.3 % of merge bytes); ChainBindings Phase C remains the canonical
  ≥ 200 MB lever for this site (but is FALSIFIED ×3 per
  vm.cc:1167-1206 in-code memo)
* **852 MB of "elsewhere" is UNATTRIBUTED** — the single biggest
  unmeasured bucket; next bounded measurement target

## Full bucket breakdown

```
peak_rss      = 2987.5 MB
  boehm_heap  =  402.9 MB  (99.9 % FREE — same dead pages as hello.drvPath)
  v3_arena    = 1593.8 MB  (the headline)
  elsewhere   =  990.8 MB  (the unknown)
```

### v3_arena decomposition (cumulative bytesAllocated)

```
  Bindings = 713.2 MB  (44.7 %)
  Thunks   = 319.7 MB  (20.0 %)
  Closures = 271.7 MB  (17.0 %)
  Pairs    = 124.7 MB   (7.8 %)
  Lists    =  53.5 MB   (3.4 %)
  Strings  =  37.3 MB   (2.3 %)
  Values   =   8.2 MB   (0.5 %)
  Envs     =   0.0 MB
  TOTAL    = 1528.3 MB  (matches arena_pinned within 4 % — 65 MB block-tail slack)
```

### elsewhere probed (138.6 MB total)

```
  stringContextSide    =  3.8 MB
  posSnapshotPool      = 63.7 MB
  bindingsOriginTable  = 64.0 MB  (diagnostic-only — V3_RELEASE strips this)
  globalSymbolTable    =  7.1 MB
  cellOwnerTable / dirtyContainers / standaloneCellRoots / nursery = 0.0 MB
```

**Unattributed elsewhere = 990.8 - 138.6 = 852.2 MB** ← biggest unknown.

## Live-fraction trace (Stage 6 SPIKE on HNE)

```
                 LIVE-objs   LIVE-bytes     ALLOC-bytes    live%
  Closures          80472       5.01 MB       259.12 MB     1.9%
  Thunks          1422270      95.84 MB       304.94 MB    31.4%
  Bindings         313631     399.82 MB       680.17 MB    58.8%
  Lists            591191      15.18 MB        51.06 MB    29.7%
  Pairs           2215186     101.40 MB       118.89 MB    85.3%
  TOTAL                       617.26 MB         1.38 GB    43.6%

  Freeable: 796.93 MB — SHIP-GREEN (≥ 200 MB ship gate)
```

End-of-run live = 617 MB; cumulative allocated = 1.38 GB; **796 MB
freeable** by a precise GC.  This is **3.3× higher than hello.drvPath's
239 MB freeable** — HNE benefits MORE from Stage 6 than hello.drvPath
does in absolute terms.

### Live-fraction insight: Bindings dominate residual

Bindings live = 399.82 MB / 617 MB = 64.8 % of all live bytes.
Closures + Thunks together = 100.85 MB = 16 %.  Pairs are 101 MB
(16 %) but mostly evaluation-residue (App memoization cells).

Implication: the 285 MB Bindings residual on hello.drvPath scales to
400 MB on HNE.  The stdenv attrset chain persists transitively from
the result drvPath — same Amdahl floor as hello.drvPath but bigger.

## mergeBindings dominance — same as hello.drvPath, scaled

```
Total mergeBindings: 153579 calls, 556.4 MB
  [0] OP_ATTRS_UPDATE        : 49058 calls,   9.7 MB  ( 1.7 %)
  [1] OP_ATTRS_UPDATE_TAIL   : 104521 calls, 546.7 MB (98.3 %)
```

OP_ATTRS_UPDATE_TAIL na/nb histograms (parent / overlay sizes):

```
  na (parent)             nb (overlay)
  na=0:    4626 (4%)      nb=0:   12949 (12%)  -- short-circuit
  na=1:    3566 (3%)      nb=1:   36675 (35%)  -- single-key override
  na=2-4: 31116 (30%)     nb=2-4: 22691 (22%)
  na=5-8: 20116 (19%)     nb=5-8: 17568 (17%)
  na=9-16:10462 (10%)     nb=9-16: 6972  (7%)
  na=17-32: 8337 (8%)     nb=17-32:6158 (6%)
  na=33-64:23249 (22%)    nb=33-64: 831 (1%)
  na≥65:   3049 (3%)      nb≥65:    677 (1%)
```

**Headline**: 35 % of merges are `parent // {single-key-overlay}` —
the canonical pattern Chain representation targets.  75 % of merges
have `nb ≤ 8`.

Chain Phase C estimate (FALSIFIED ×3, parked): nb=1 alone would
recover ~172 MB on this workload; nb≤8 would recover ~330 MB.
**Locked behind the 4-condition revival prerequisite in
vm.cc:1167-1206.**

## Bindings per-origin attribution (top sites)

```
Site                                                      allocs       bytes
alloc@vm.cc:1228 (mergeBindings)                        118327      585.3 MB (82.0 %)
alloc@vm.cc:7326                                        534902       44.6 MB ( 6.3 %)
OP_ATTRS_REC_INIT_TAIL                                  236281       30.0 MB ( 4.2 %)
primMapAttrs                                              9771       26.4 MB ( 3.7 %)
OP_ATTRS_REC_INIT                                        90024        9.8 MB ( 1.4 %)
alloc@primops.cc:1878                                    12603        5.2 MB ( 0.7 %)
OP_ATTRS_INIT_DYN                                        19291        4.7 MB ( 0.7 %)
... 16 more origins, top-20 = 100 % of bytes
```

Top-3 = 92.5 % of all Bindings bytes.  mergeBindings alone = 82 %.
After that, vm.cc:7326 (which is REC_INIT internals) at 6.3 %.

## Differential measurement — the 990 MB "elsewhere" decomposed

After the initial decomposition, ran HNE in three configurations to
attribute the elsewhere bucket to specific subsystems.  Important
caveat: **cache state survives between processes via `~/.cache/nix/`
disk cache, so single-run RSS measurements vary by ~400 MB depending
on cache warmth**.  The numbers below are single runs each — directional
but not reproducible to within MB.

```
Configuration                                peak_rss   v3_arena   boehm   elsewhere
default (cold cache, first run)              2987 MB    1594 MB   403 MB   991 MB
default (warm cache replay)                  2565 MB    1594 MB   403 MB   568 MB
NIX_V3_NO_CONTENT_CACHE=1                    2311 MB    1594 MB   403 MB   314 MB
NIX_V3_NO_DISK_CACHE=1                       2037 MB    1594 MB   403 MB    40 MB
both NO_DISK + NO_CONTENT                    2128 MB    1594 MB   403 MB   131 MB
```

### Key observations

* **`v3_arena = 1594 MB` is stable across all configurations** —
  cache state doesn't affect arena.  Stage 6's 797 MB freeable
  pertains to this bucket exclusively.
* **`NIX_V3_NO_DISK_CACHE=1` saves 950 MB peak RSS** vs cold-cache
  baseline (528 MB vs warm-cache baseline).  This is the biggest
  single-flag memory lever available today.
* **The disk cache itself is 312 MB on disk** (`~/.cache/nix/
  v3-bytecode-v3.sqlite`, May 27 timestamp).  In-memory cost is
  multiple-of-300 MB — SQLite page cache + mmap'd file + in-
  memory deserialized CUs.
* **Content cache (in-memory) contributes 200-700 MB depending on
  warmth state** — this is the `ImportCache::cus` `std::deque` +
  `results` map (`primops.cc:7521-7530`) that grows monotonically
  with imports.

### Mechanism

The v3 import cache architecture (`primops.cc:7510-7534`):
```cpp
struct ImportCache {
    std::deque<CompilationUnit> cus;
    std::unordered_map<std::string, ImportCacheEntry> results;
};
```

* `cus` deque holds every imported `.nix` file's compiled bytecode
  + constant pools + LambdaDescriptors + AttrSelectIC tables.
  Stable addresses (deque doesn't realloc) because Closures hold
  raw pointers into the CU.
* `results` map memoizes the Value WHNF of each import path.

Neither structure has eviction.  On HNE: ~thousands of imports
× hundreds of KB per CU = ~hundreds of MB in `cus`.

### Why this matters and what's next

This is **the lever the avenues doc Category 5 documented as "Phase
4b EvalResults size cap + LRU — 1-2 d — cache grows monotonically;
HNE cache size during eval = unknown."**  Now it's known:
**~700 MB on HNE under default settings**.

Implementation prerequisites (multi-session, not this turn):
1. CU eviction is BLOCKED by Closures holding raw CU pointers.
   Either reference-count CUs OR weak-reference + relocate.
2. ImportCacheEntry::result has nursery-pointer concerns post-
   Phase D/E — eviction must coordinate with the GC walker.
3. Hash bucket maps need to evict alongside the deque to recover
   the bucket overhead.

Workaround available today: **`NIX_V3_NO_DISK_CACHE=1`** for
memory-pressured single-shot evals (e.g., CI).  Costs wall time
on warm-cache scenarios; gives ~500-950 MB peak RSS reduction.

## Where the 990 MB "elsewhere" likely lives (HYPOTHESIS — superseded)

The probed 138 MB does not account for:

| Candidate | Estimated range | Why |
|---|---|---|
| libc malloc fragmentation + std::vector capacity slack | 100-300 MB | HNE has 1.1 M Bindings → ~32 std::vector growth events per major data path; OS-side malloc overhead is platform-dep |
| SQLite (disk cache backing) | 50-200 MB | disk_cache lookups=2452, inserts=2340; SQLite page cache scales with insert volume |
| AOT mmap reader (if enabled) | 0-200 MB | NIX_V3_AOT_CACHE_FILE was not set on this run; should be 0 |
| Boehm metadata outside heap | 50-100 MB | Boehm block maps grow with arena (587 MB of registered roots) |
| CompilationUnit bytecode | 20-50 MB | 1.5 M CU bytes / few thousand CUs |
| Fiber stacks | 50-100 MB | NIX_V3_FIBER_STACK_SIZE default × max concurrent fibers |
| std::unordered_map bucket overhead | 50-100 MB | bindingsOriginTable shows buckets=1.6M (the diagnostic only); other maps have similar growth |
| (unknown) | residual | |

**Bounded follow-up spike (~1 d)**: `vmmap $PID` mid-eval + tabulate
per-region label.  Probably reveals 2-3 dominant regions for
targeted optimisation.

## Cross-workload comparison

| Bucket | hello.drvPath | HNE | HNE/hello ratio |
|---|---|---|---|
| peak_rss | 754 MB | 2988 MB | 4.0× |
| v3_arena | 587 MB | 1594 MB | 2.7× |
| boehm_heap | 403 MB | 403 MB | 1.0× (same dead pages) |
| elsewhere (probed) | ~0 MB | 139 MB | huge |
| **elsewhere (unattributed)** | **~0 MB** | **852 MB** | **HUGE** |
| live (end-of-run) | 286 MB | 617 MB | 2.2× |
| freeable | 239 MB | 797 MB | 3.3× |

**Key finding**: hello.drvPath's elsewhere is ~0; HNE's is 990 MB.
The 1000 MB gap is HNE-specific and likely comes from:
* std::vector growth slack in cppnix-side containers
* SQLite (more disk cache work on HNE)
* libc malloc fragmentation
* Possibly fiber stacks (HNE may have deeper nesting)

These are workload-pattern artifacts of HNE specifically — the
haskell.nix `callCabalProjectToNix` style.

## Decision per Rule 0

Two SHIP-GREEN levers confirmed by this data:

1. **Stage 6 precise GC of v3_arena**: 797 MB freeable on HNE
   (4× threshold).  This is the highest-leverage lever in the
   data.  Implementation requires arena deregistration from
   Boehm (per `BOEHM_TUNING_FALSIFIED_2026-05-27.md` follow-up)
   + Stage 5 GC_ROOT prerequisite work; estimated 2-3 weeks.

2. **HNE elsewhere decomposition** (~1 d measurement spike):
   identifies the 852 MB unknown bucket.  Cheap, decision-
   informative, no risk.

ChainBindings Phase C remains the obvious Bindings-specific lever
but is locked behind 4 prerequisites per the vm.cc memo.

## Concrete next steps for future sessions

1. **vmmap-based elsewhere decomposition** (≤ 1 d): identify the
   852 MB unaccounted bytes via OS-level region inspection.
2. **Stage 5 + Stage 6 implementation** (2-3 wk): the validated
   high-leverage path.
3. **ChainBindings Phase C revival** (multi-session): only after
   prerequisites 1-4 in vm.cc:1167-1206 are completed.
4. **Per-type non-Bindings attribution (T1.3)** (1-2 d): templated
   from #746 BINDINGS_ATTR; surfaces Thunks/Closures/Pairs hot
   sites.  On HNE, Thunks (304 MB, 31 % live) and Closures (259 MB,
   1.9 % live!) have huge transient garbage.

## How to reproduce

```bash
NIX_VM_STATS=1 NIX_V3_LIVE_TRACE=1 NIX_V3_DIRECT_EVAL=1 \
  NIX_V3_BINDINGS_ATTR=1 NIX_V3_MAX_WALL_TIME=600s \
  NIX_V3_MAX_HEAP=8G timeout 700s \
  ./build/src/nix/nix --extra-experimental-features 'nix-command flakes' \
  eval --impure --expr \
  '(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath'
```

Tested with v3 commit `6442bf531` (head of session arc).  Wall time
~30-60 s; HNE infrastructure must be checked out at
`/Users/angerman/Projects/iohk/haskell-nix-example`.

## Cross-references

* `MEMORY_REDUCTION_AVENUES_2026-05-26.md` Category 1 — this entry
  closes the "HNE bucket decomposition" gap
* `LIVE_FRACTION_SPIKE_2026-05-27.md` — the live-trace tracer used
* `BOEHM_TUNING_FALSIFIED_2026-05-27.md` — arena-deregistration
  prerequisite for Stage 6
* `HNE_MEMORY_ATTRIBUTION_2026-05-26.md` — prior A1 attribution data
* `[[chain-bindings-phase-c-falsified]]` — why mergeBindings can't be
  immediately attacked
* `[[memory-first-class]]` — ≥ 200 MB peak RSS SHIP gate
* vm.cc:1167-1206 — Phase C 3-pivot postmortem

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
  Input Output Group.
SPDX-License-Identifier: Apache-2.0
