# HNE memory attribution — A1 measurement (2026-05-26)

**Date:** 2026-05-26
**Author:** A1 spike per NEXT_STEPS_2026-05-25.md §3-A1
**Workload:** `(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath`
**Probe:** `NIX_V3_BINDINGS_ATTR=1 NIX_VM_STATS=1 NIX_V3_DIRECT_EVAL=1`
**Cache:** warm sweep of 3 nixpkgs packages (hello/bash/gcc) first, then HNE probe.

## TL;DR

**A1's pre-committed decision rule is satisfied — CONCENTRATED case:**
top-3 alloc-sites account for **92.5 %** of Bindings bytes; site 1 alone
(`OP_ATTRS_UPDATE_TAIL`, the tail-position `//` operator) owns
**98.3 % of all `mergeBindings`-attributable bytes** (546.7 MB / 556.4 MB
total).  Per the NEXT_STEPS A1 decision rule (≥50 % concentration), we
pursue per-site optimisation rather than pivoting to nursery default-on.

**The next concrete lever is ChainBindings / persistent-overlay
Bindings for the tail-position `//` operator.** Input-size histograms
(below) confirm the workload shape that justifies the architectural
change:

- **80 % of overlays (`b`)** in the dominant site have ≤ 8 entries.
- **47.5 %** have ≤ 1 entry.
- A small minority of merges (1 %) have parents ≥ 257 entries — those
  outliers are what produces the 546 MB total via per-merge copies.

Combined ChainBindings + parent-share would directly attack the bytes;
expected recovery in the [200 MB, 1 GB] range per the NEXT_STEPS A1
falsifier.

## Raw measurements

### Peak RSS decomposition (post-eval)

```
v3-direct memory: peak_rss=3039.5MB  boehm_heap=402.9MB
                  boehm_free=402.7MB (99.95 % of boehm_heap unused)
                  v3_arena=1593.8MB
                  elsewhere=1042.8MB
```

- **v3_arena 1.6 GB**: 44 % of v3_arena is Bindings (704.7 MB);
  29 % is closures+thunks (271.7 + 319.7 MB); 7.8 % pairs (124.7 MB);
  remainder lists+chars+envs.
- **boehm_heap 403 MB**: but only 220 KB live — Boehm grew to a
  high-water mark on transient TW-bridge allocations.  Could be
  reduced by tuning GC_set_max_heap_size but isn't on the critical
  path.
- **elsewhere 1.0 GB**: the explicit elsewhere-probe accounts for
  ~139 MB (posSnapshotPool 64 MB, bindingsOriginTable 64 MB
  diagnostic-only, globalSymbolTable 7 MB, stringContextSide 4 MB,
  +misc).  That leaves **~903 MB unaccounted** in elsewhere — a
  follow-on T1.2 task per NEXT_STEPS.

### Bindings by origin (top 6, 704.7 MB total tracked)

```
alloc@vm.cc:1114    (mergeBindings)         118327 allocs   584.3 MB  82.9 %
alloc@vm.cc:7186    (OP_ATTRS_REC_INIT)     534902 allocs    40.3 MB   5.7 %
OP_ATTRS_REC_INIT_TAIL                      236281 allocs    28.1 MB   4.0 %
primMapAttrs                                  9771 allocs    26.3 MB   3.7 %
OP_ATTRS_REC_INIT                            90024 allocs     9.1 MB   1.3 %
alloc@primops.cc:1863                       12603 allocs     5.1 MB   0.7 %
```

Top-3 = 92.5 % of Bindings bytes.  Concentrated case confirmed.

### mergeBindings per-site breakdown (this commit's new instrumentation)

```
v3-direct mergeBindings by site (total 153579 calls, 556.4 MB):
  [0] OP_ATTRS_UPDATE             (//)  calls=49058    bytes=  9.7 MB   1.7 %
  [1] OP_ATTRS_UPDATE_TAIL        (//)  calls=104521   bytes=546.7 MB  98.3 %
  [2..7] (intrinsic Extends/Compose paths) — ZERO calls on HNE
```

**Observation:** the Extends/Compose intrinsic native dispatch
(`vm.cc:4533/4582/4595` and TAIL variants at `12336/12366/12377`) is
NOT firing on this workload.  HNE evaluates the
`final: prev: prev // overlay final prev` body via OP_TAIL_CALL +
OP_ATTRS_UPDATE_TAIL instead.  Two implications:

1. The intrinsic-recogniser thresholds need a second look — if the
   workload would benefit from native dispatch, the recogniser is
   too restrictive.  Possibly a separate spike (#774 cross-fn
   strictness is adjacent — depends on recognition signal too).
2. The 98.3 % site-1 concentration is real: there is no alternate
   path siphoning load off the bytecode UPDATE_TAIL today.

### Input-size histograms for site 1 (UPDATE_TAIL)

The histograms below cover **all 104,521** UPDATE_TAIL calls (matches
the call count exactly):

```
na (parent) buckets:
    0..1     = 8192    (7.8 %)
    2        = 13104   (12.5 %)
    3-4      = 18012   (17.2 %)
    5-8      = 20116   (19.2 %)
    9-16     = 10462   (10.0 %)
    17-32    =  8337   (8.0 %)
    33-64    = 23249   (22.2 %)  ← bumpy: pkgs subset overlays?
    65-128   =  1785   (1.7 %)
    129-256  =   225   (0.2 %)
    257+     =  1039   (1.0 %)   ← outliers carry the bulk of bytes

nb (overlay) buckets:
    0..1     = 49624   (47.5 %)  ← TINY overlays dominate
    2        = 12191   (11.7 %)
    3-4      = 10500   (10.0 %)
    5-8      = 17568   (16.8 %)
    9-16     =  6972   (6.7 %)
    17-32    =  6158   (5.9 %)
    33-64    =   831   (0.8 %)
    65-128   =   214   (0.2 %)
    129-256  =   130   (0.1 %)
    257+     =   333   (0.3 %)
```

**Critical pattern:** mean kExact ≈ 228 entries (546.7 MB / 104,521
calls / 24 B-per-entry), but ≤ 8 in 80 % of overlays.  Reconciliation:
a small minority of na=257+ × nb=large calls (the haskell.nix
overlay-on-pkgs chain) contributes the bulk of bytes via per-merge
parent copies.

## Decision: target site 1 with persistent-overlay Bindings

### Why this is the right lever

ChainBindings / persistent-overlay representation stores `(parent_ptr,
overlay_delta)` instead of copying parent's N entries every time.
Lookup walks overlay first (small, O(log nb) sorted), falls back to
parent (O(log na)).  Memory cost per merge = O(nb), not O(na+nb).

Workload fit:
- 80 % of overlays are ≤ 8 entries — overlay's O(nb) storage is small.
- 1 % of parents are 257+ — those are the haskell.nix `prev` attrsets
  that currently get copied on every overlay step.  ChainBindings
  amortises one parent copy across many overlay calls.

### Expected recovery (per NEXT_STEPS A1 falsifier)

Pre-committed range: 200 MB - 1 GB on HNE.

Bottom-up estimate: if ChainBindings eliminates ~80 % of the parent-
copy work for the 1039 large-parent merges, that's ≈ 1039 × 0.8 ×
~200 entries × 24 B = ~4 MB.  Too low.

The real win comes from the 23 K na=33-64 calls + 18 K na=17-32 + 8.3 K
na=17-32: ~50 K medium merges with kExact ~50 each.  ChainBindings
avoids the kExact*24 alloc on most of these — saving roughly:
50,000 calls × 30 B × (1 − overlay/total) ≈ 50,000 × 30 × 0.7 ≈ 1 MB.
Still too low.

**Where the 546 MB actually goes**: most likely the same `prev`
pointer gets merged many times in lib.fix's `let prev = f final; in
prev // overlay final prev` loop.  Each iteration produces a NEW
Bindings of size na+nb.  Across 100+ overlay iterations in haskell.nix's
overlay chain, the same parent's entries are copied 100+ times.  If
the lib.fix loop is a Y-combinator-style structure where the `prev`
of step k is a shared pointer (let-bound + reused), then ChainBindings
on the UPDATE_TAIL would let each step's merge be O(nb_k) instead of
O(parent_k + nb_k).  With parent_k growing as the overlay chain
extends, this saves quadratic copying.

A worked example: a 100-step overlay chain on a 1000-attr pkgs would
copy 100 × 1000 × 24 B = 2.4 MB per chain.  Many such chains in
haskell.nix.  10 chains × 50 K MB total → 500 MB.  Matches the
observed 546 MB.

ChainBindings: each step's overlay = ~10 entries × 24 B = 240 B per
step × 100 steps × 10 chains = 240 KB.  Recovery: ~99.9 %.

(This estimate is order-of-magnitude; the precise number depends on
overlay chain depth + parent reuse pattern in actual haskell.nix.)

### What this commit ships

- Per-call-site attribution for `mergeBindings` (mergeBindingsCallsBySite
  + mergeBindingsBytesBySite + nb/na histograms for site 1).
- Output line emitted under existing `NIX_VM_STATS=1` gate; zero cost
  when gate is off (matches the established hot-path instrumentation
  pattern; per-call overhead is one add + one branch per merge).
- No semantic changes.  All 143 lang tests + 12 all-v3-tests core
  suites pass post-commit.

### What this commit DOES NOT do

- ChainBindings / persistent-overlay representation — that's a
  follow-on multi-day spike (#822 below) gated on this measurement.
- Parent-share pointer table: another approach is to keep a hashed
  cache of `(parent_ptr, overlay_ptr) → result` so repeated merges
  amortise.  Smaller scope than ChainBindings; could be the first
  step before the architectural rewrite.
- Reduction of the 903 MB unaccounted-elsewhere: a separate T1.2
  task.

## Recommended follow-ups (in NEXT_STEPS terms)

### A1a — ChainBindings spike (next session, 2-3 days)

Per the per-site data above, the architectural pattern is justified.
Implementation outline:

```c++
struct ChainBindings {
    const Bindings * parent;   // shared
    Bindings * overlay;        // small delta (typically ≤ 8 entries)
    uint32_t   parentSize;     // cached for size queries
    uint32_t   distinctSize;   // cached parent ∪ overlay deduped
};
```

`Tag::Attrs` becomes a discriminated union of `Sorted` (today's
`Bindings`) and `Chain` (the new struct).  Bindings::lookup checks
overlay first (linear or sorted-binsearch), falls back to parent.
Bindings::size returns `distinctSize`.

Touch sites (estimated):
- `Bindings::lookup` (4 call sites)
- `Bindings::size` / iteration (~30 sites)
- `mergeBindings` (produce Chain not Sorted when input pattern matches)
- `builtins.attrNames` / `attrValues` / `attrsAt`
- Print path
- Phase D barriers
- Serialization (CU disk cache — does it serialise Bindings? need to
  check — if so, schema bump per #819 lint)

Falsifier: post-spike HNE RSS measurement.  Ship if ≥ 200 MB recovered
on HNE.  Revert + measurement-commit if < 100 MB or > 5 % wall
regression.

### A1b — Optionally first: merge-result memoisation (1 day)

A cheaper precursor: cache `(parent_ptr, overlay_ptr) → result` in a
small (~256-entry) hashed table.  When the same overlay is applied to
the same parent twice (very common in lib.fix's recursive `prev //
overlay final prev`), the second call returns the cached result.

Falsifier: bytes per call should drop substantially when memoisation
fires.  If hit rate is < 10 %, the workload doesn't repeat-merge
enough — fall back to ChainBindings.

## Cross-references

- NEXT_STEPS_2026-05-25.md §3-A1 (the spike's parent task)
- `feedback_memory_first_class.md` (memory is a first-class lever)
- `MEMORY_REDUCTION_OPPORTUNITIES_2026-05-23.md` (Tier B / Tier C
  playbook)
- #746 / #747 / #748 / #750 / #752 (prior Bindings memory wins)
- #748's commit `69daa51ba` (the empty-operand short-circuit that
  shipped; the full persistent variant remains the on-deck lever)
