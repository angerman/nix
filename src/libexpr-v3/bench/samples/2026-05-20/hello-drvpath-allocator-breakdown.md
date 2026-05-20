# hello.drvPath allocator byte-breakdown (follow-up #702 measurement)

**Date**: 2026-05-20  
**Tool**: `NIX_VM_STATS=1` with per-category byte counters wired into `Alloc::*`
**Workload**: `(import <nixpkgs> {}).hello.drvPath` against local nixpkgs 24.05  
**Determinism**: 3 runs produced byte-identical numbers — v3 alloc is deterministic on this workload.

## Where the 4 GB RSS comes from

| Category   | Bytes      | % of total | Allocations   | Avg size      |
| ---------- | ---------- | ---------- | ------------- | ------------- |
| Bindings   | 1348.3 MB  | **52 %**   |  2 969 815    | 454 B / Bindings (~18 entries) |
| Thunks     |  800.2 MB  |   31 %     | 10 165 547    |  78 B / Thunk (~2 upvalues) |
| Closures   |  289.8 MB  |   11 %     |  3 447 199    |  84 B / Closure (~3 upvalues) |
| Lists      |   82.3 MB  |    3 %     |  1 117 541    |  74 B / List |
| Pairs      |   56.3 MB  |    2 %     |  1 172 915    |  48 B / Pair |
| Chars      |   25.5 MB  |    1 %     |  —            | string buffers (Tag::String / Path) |
| Values     |   12.5 MB  |    0 %     |  —            | boxed Value cells |
| Envs       |    0.0 MB  |    0 %     |  0            | unused on this workload |
| **Total**  | **2 614.9 MB** | | | |
| Arena pinned (16 MB blocks) | **2 667.6 MB** | | | overhead = block-alignment slack |

Process RSS at peak: **4 343 MB** (perf-trace measurement, 2026-05-20).  
Arena-pinned + GC heap (~403 MB) = **3 071 MB**.  
The remaining ~1.27 GB is mmap overhead, string-context side-table, attrPos
side-table, closure-pool slots, the ClosurePool's 128×16-slot table, the
position snapshot pool, libfetchers caches, and Boehm's bitmap regions.

## What this kills (Rule 0)

**Hypothesis 1 (from hello-drvpath-analysis.md)**: "v3's non-Boehm RSS lives
in some single subsystem (the closure-pool, the nursery, fakeClo recycling, or
similar)."  

**Falsified**.  The 2.6 GB is **broadly distributed across 5 allocator
categories**, with Bindings the largest single slice at 52 %.  There is no
"one allocator owns the 4 GB" — it's a system-wide pattern.

**Hypothesis 2 (new)**: "Bindings allocation dominates v3's memory pressure
on hello.drvPath."

**Confirmed**.  Bindings at 1.35 GB is more than the next two categories
(Thunks + Closures = 1.09 GB) combined.  2.97 M Bindings × ~18 entries each
= ~53 M Bindings::Entry slots × 24 B each = ~1.28 GB just in Entry storage.
The remainder (~70 MB) is Bindings headers.

**Hypothesis 3 (new)**: "Thunks at 10 M allocations are evidence of
THUNK-ALL excessive thunkification."

**Plausible but not proven by this measurement alone.**  10 M thunks for a
single hello.drvPath eval (which produces a string) is high.  TW likely
allocates orders of magnitude fewer (~10 K?).  10 M × 78 B = 800 MB of thunk
storage is real cost; many of those thunks are likely never forced
(thunksForced = 3 417 693, so 6.75 M thunks were allocated but never used).

  - Allocated thunks: 10 165 547
  - Forced  thunks:   3 417 693
  - Unforced thunks:  6 747 854 (66 % wasted)

**This is the load-bearing finding for the ROADMAP**: Stage 4 (strictness
analysis) targets the unforced-thunk allocation cost.  6.75 M unforced
thunks × 78 B = **526 MB** wasted on speculatively-thunkified values that
were never evaluated.

## What's NOT the bottleneck

- **Envs (0 MB, 0 allocations)** — confirms that v3's lower.cc keeps
  closures upvalue-only and doesn't fall back to first-class Env objects
  for normal lexical scope.  The Env type exists but is unused on this
  workload.

- **Chars (25.5 MB)** — string-buffer storage is a small fraction.
  String-context side-table dominates the per-string cost, not the
  payload bytes.

- **Boehm GC (38 MB/s alloc; 403 MB arena)** — confirmed once more.
  Boehm is ~1/70 of the bytes that v3's own allocator handles.

## Where to attack next (priority-ordered)

1. **Bindings polymorphism** (Empty / Single / Small / Sorted shapes).
   `alloc.hh` already documents this plan ("the v3 design doc envisages
   Empty/Single/Small/Sorted polymorphism, but a single Sorted form is
   correct and lets us defer the polymorphism work until the perf gap
   motivates it").  This measurement is the motivation.  Estimated win
   on hello.drvPath: 30-50 % of Bindings bytes if half the attrsets are
   small (≤ 4 entries).

2. **Thunk reduction** (Stage 4 strictness analysis).  6.75 M unforced
   thunks → 526 MB wasted.  A pass that elides thunks at provably-strict
   positions (binding RHS of bindings whose name is forced; let-rec RHS
   whose RHS is in WHNF; arithmetic operands) would cut this directly.
   Estimated win: 20-50 % of Thunks bytes.

3. **Nursery default-on (ROADMAP Stage 3)** is **still useful** — it
   wouldn't reduce the 2.6 GB cumulative alloc, but it would let v3
   *reclaim* the bytes that are no longer reachable.  On hello.drvPath,
   if even half the allocations are short-lived (the unforced-thunk
   estimate above suggests at least 526 MB are reclaimable from thunks
   alone), nursery scavenge with a 32 MB young generation would cycle
   through hundreds of times and keep working-set small.

## Tool effort

- 8 byte counters added to `AllocStats` (alloc.hh)
- Bumps in 8 `Alloc::*` functions + 1 in `allocFakeClo` pool-miss path
- Extended dump in `run.cc` NIX_VM_STATS path
- ~30 LoC total, matches the original "in-process allocator-counter dump"
  estimate.

## Reproduction

```bash
NIX_VM_STATS=1 NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_PATH=nixpkgs=/path/to/nixpkgs \
  nix eval --impure --expr "(import <nixpkgs> {}).hello.drvPath" 2>&1 \
  | grep "v3-direct bytes"
```

Output: deterministic across runs.  Use this as the canonical floor for
allocator-budget regressions.

## Copyright

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group. SPDX-License-Identifier: Apache-2.0
