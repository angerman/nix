<!--
Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
-->
# v3 list-iteration performance RCA (map / foldl' / filter) — 2026-06-08

Root-cause analysis of the v3-vs-tree-walker (TW) gap on per-element list
iteration, the clearest remaining CPU weakness in the v3 baseline
(`project_v3_vs_tw_baseline`). Method: **three parallel source-analysis agents**
(v3 CPU path / v3 memory / TW baseline) + **empirical profiling on the dedicated
idle host darwin-4** (per-pass decomposition, `NIX_VM_STATS`, `sample`/atos).
Ranking is by **measured** contribution; each cause is tagged
*confirmed-by-code* and/or *confirmed-by-measurement*.

## The gap (idle darwin-4, post-fix 2.35.0 binary, byte-IDENTICAL, same binary both arms)

| workload | expr | CPU | RSS |
|---|---|--:|--:|
| map.foldl | `foldl' (a:x:a+x) 0 (map (x:x+1) (genList (i:i) 2M))` | 1.14s vs 0.29s = **3.93×** | 556M vs 324M = **1.71×** |
| filter | `length (filter (x:x>1) (genList (i:i) 2M))` | 1.05s vs 0.22s = **4.77×** | 515M vs 207M = **2.48×** |

These are *constant-factor* gaps (both linear — the O(n²) blowups were fixed
2026-06-08, see `project_bytecode_primop_regressions`). map/foldl' run the **C++
primop** (default post-flip d65c7e353); filter runs the **bytecode primop** (kept
for laziness-correctness, `bytecode_primops.cc:509`).

## Decisive narrowing — per-pass decomposition (measured, best-of-2)

```
pass       TW-s   v3-s   ratio
genList    0.05   0.05   1.00×   ← list CONSTRUCTION is at PARITY
map        0.07   0.25   3.57×
foldl      0.20   0.70   3.50×
mapfoldl   0.28   1.12   4.00×
filter     0.22   1.04   4.73×
```

**Building the 2M list and forcing its spine is at parity.** The gap is entirely
the **per-element operation**: force each element + apply a tiny lambda 2M times.
`NIX_VM_STATS` (final cumulative dump): **`insns = 26,000,033`** for 2M elements =
**13 dispatched bytecode instructions per element**, where TW runs an inlined C++
body. This rules out list representation, the allocator, and any complexity bug as
the *primary* CPU cause.

## CPU root causes — ranked by measured self-time

`sample` of `foldl'(+)0(genList id 15M)`, eval-thread self/leaf samples:

| # | cause | measured | code |
|---|---|--:|---|
| 1 | **TLS access `_tlv_get_addr`** — macOS dylib `thread_local` = indirect call per read, hit per-element via `VMScope` active-VM swap + `getNixEvalState` + gated-flag reads on every per-element `dispatchLoop` re-entry | **1124 (~30%, the #1 leaf, above dispatch)** | `pushActiveVMState`/`VMScope` vm.cc:2650-2681 |
| 2 | **interpreter dispatch** `dispatchLoop` — fetch/decode/switch ×13 ops/elem | 851 (~23%) | vs TW inline `lambda.body->eval` eval.cc:1688 |
| 3 | **GC marking** — marking 30M fat pairs | 344 eval + 3026 concurrent `GC_mark_thread` | `BitmapMarker::tryMark`, `MarkVisitor::*` |
| 4 | **foldl' curries via 2× `callClosure` + a throwaway PAP `ValuePair`/element** (no saturated multi-arg call) | **pairs=6M = 2M genList + 2M map + 2M curry-PAP waste** | primops.cc:1314-1315; only 1-arg `callClosure` vm.cc:13077 |
| 5 | allocation (255) + forcing (100). The per-force `std::vector rights` the source agent ranked **#1** measured **only ~1%** (`vector::__append` 31) | small | forceValue vm.cc:12248 |
| 6 | **gated debug/live-trace checks firing per-element while OFF** (+ their TLS reads inside #1) | 49 + share of #1 | `dbgLogForce*`, `periodicLiveTraceEnabled` |
| 7 | filter-only: indirect `primLessThan` for `>` (no inline `OP_LESS`) | — | vm.cc:10746 |

> **#1 is the methodological payoff:** the TLS-access cost is invisible in source
> (it's a linkage/runtime cost); only the `sample` surfaced it. And measurement
> *demoted* the source agent's #1 (`std::vector rights`) to ~1%. This is why
> profiling, not code-reading alone, drives the ranking (Rule 0).

**Falsified red herrings:** `deepForce` double-pass does **not** fire for map.foldl
(it dispatches through `callClosure`, not the `OP_CALL_PRIMOP` deep-force path);
per-cell allocator cost is cheap (it's the *count* of allocs); no recursion /
C-stack pathology (flat at N=200k).

## Memory root causes — ranked by measured bytes

`NIX_VM_STATS` per-tag bytes (final dump), OS peak-RSS reconciled to the table:

| # | cause | measured | code |
|---|---|--:|---|
| 1 | **`ValuePair` is 64B; one per lazy element — DOMINATES.** `evaluated`+`third` slots (32B) are dead weight for one-shot 2-arg Apps | pairs = **384MB** (map.foldl, 6M) / 256MB (filter, 4M) = **86% of alloc** | value.hh:228 |
| 2 | **list backbone stores inline 16B `Value` vs TW's 8B `Value*`** → 2× | lists = 64MB (2×2M×16B) | alloc.hh:102-107 |
| 3 | filter's **2M singleton `[x]` ListVecs** (`concatLists∘map` bytecode form) — why filter (2.48×) > map.foldl (1.71×) | lists = **2,000,002**, 144MB | bytecode_primops.cc:513 |
| 4 | **non-moving GC keeps dead intermediates resident** (evac default-OFF) → peak RSS = alloc high-water | gc_count=1; `boehm_free=402MB` all-free-but-mapped; arena_pinned > live | mark_sweep.cc:1382 (evac gate) |

**Cross-cutting:** the fat `ValuePair` hurts *both* axes — 384MB of memory **and**
the GC-marking CPU (30M cells to mark, cause CPU#3). Fixing it pays twice.

## Why TW is cheap + lean (baseline, for contrast)

- 8B `Value*` list backbone + small-list (≤2) inline opt (value.hh).
- Per-element call = `callFunction` with a free-list-cached size-1 `Env`, De Bruijn
  slot read, body **tree-walked directly in C++** (no bytecode, no dispatch loop);
  `a+x`/`x+1` are the inline `ExprConcatStrings` int fast path (no primop, no alloc).
- `foldl'` **streams**: one 2-arg `callFunction` per step (no PAP), forcing+dropping
  one element at a time — the mapped intermediate's element values are never all
  live, so it stays lean.
- `filter` collects `Value*` pointers into a stack vector with a `same`-aliasing
  short-circuit (no per-element allocation).

## Recommended fixes — ranked by yield × confidence ÷ risk

Each behind a kill-criterion A/B gate; validate on darwin-4 (the laptop debug build
cannot time these). All V3-NATIVE.

1. **Saturated `callClosureN` / `callClosure2`** (route `primFoldl` and other
   multi-arg-callback C++ primops through it). Removes the **2M PAP `ValuePair`s
   (128MB)** + one `callClosure` dispatch-prologue per element on the **dominant**
   pass. Measured waste, localized (the `OP_CALL_N` body-entry logic already exists,
   vm.cc:6256), low risk, **helps CPU and memory.** START HERE.
2. **Shrink `ValuePair` 64B→32B** for plain 2-arg Apps (drop `evaluated`+`third`;
   the `deepForce` writeback already overwrites the element slot, so the memo is
   redundant for one-shot map/genList entries). Biggest single memory lever +
   reduces GC-marking CPU. Medium risk (core representation).
3. **Cut per-element TLS / dispatch re-entry** — hoist `VMScope` active-VM
   management + `getNixEvalState` + gated-flag reads out of leaf-lambda calls, or
   fast-path trivial lambda bodies so they don't re-enter a full `dispatchLoop`.
   The #1 CPU cost; biggest CPU lever but highest risk/effort.
4. **Remove / hoist the gated per-element debug-log + live-trace checks.** Trivial,
   immediate; also trims their TLS reads (part of cause CPU#1).
5. **filter fusion**: recognise `concatLists ∘ map (λ. [x] | [])` and lower to a
   single in-place builder (no per-element singleton ListVec). Closes filter's extra
   cost over map.foldl while preserving laziness.
6. **Narrow ListVec backbone 16B→8B** (`Value*`-style). 2× backbone memory; medium-
   high risk (core list rep).
7. **Inline `OP_LESS` int-int fast path** (mirror `OP_EQ`). Small, filter-specific.

## What shipped / what reverted — Wave 1 + 2 (2026-06-08, measured on darwin-4 idle)

Per `LIST_ITERATION_FIX_PLAN_2026-06-08.md`, each task carried a pre-committed
keep/revert bar; measured A/B on darwin-4. Re-pinned per-pass after T1+T2+T4:

| pass | baseline v3/TW | after T1+T2+T4 | peak RSS (was → now) |
|---|--:|--:|--:|
| genList | 1.00× | 1.00× | — |
| map | 3.6× | 3.57× | — |
| foldl | 3.5× | **2.70×** | — |
| mapfoldl | 4.0× | **3.57×** | 556M → **422M** |
| filter | 4.7× | **2.41×** | 515M → **256M** |

- **T1 — saturated `callClosure2`** (commit a669a17a9). SHIPPED. `primFoldl`/
  `primFoldlMap` enter the arity-2 callback once with both args in slots,
  dropping the per-element curry-PAP `ValuePair`. mapfoldl `pairs` 6.0M→4.0M
  (−128MB); **FOLDL user-CPU −22.9%** (0.70→0.54s). Gate `NIX_V3_NO_SATURATED_CALL`.
- **T2 — guard-free trace gates** (commit b951a5e30). SHIPPED. Hoisted the
  per-element `dbgLogForce*` function-local statics + the per-iteration
  cross-TU `periodicLiveTraceEnabled()` call to namespace-scope cached bools.
  **FOLDL user-CPU −3.7%** (0.54→0.52s), byte-identical. No gate.
- **T7 — inline `OP_LESS` for `<`/`>`** (rewrite `PrimOpCall(__lessThan)`→`ir::Less`,
  gated `NIX_V3_INLINE_LESS`). **REVERTED.** Engaged correctly + byte-identical,
  but **+1.0%** on filter (2 readings) — fails the −≥3% bar. OP_LESS needs 2
  explicit `Force` opcodes (incl. a redundant one on the literal) that offset
  the primop-dispatch saving, and filter cost is allocation-dominated (cause
  #7 was over-attributed; the real filter lever was T4).
- **T4 — lazy C `primFilter` + flip filter to C++-default** (commit 8480a3c91).
  SHIPPED. Removed `primFilter`'s `deepForceList` (the over-forcing that forced
  filter to stay bytecode) — the C primop is now lazy-correct AND single-alloc,
  eliminating the bytecode form's 2M singleton `[x]` ListVecs. **filter CPU
  −48%** (1.02→0.53s), **peak RSS −259MB** (515→256M), lazy-throw correct
  (`filter (x:true) [1 (throw) 2]` = 3), byte-identical, lang-corpus-neutral.
  filter's RCA gap 4.77×/2.48× → **2.41× / ~1.24×**.

- **T5 — cut per-element TLS / dispatch re-entry.** Residual `sample` profile
  (post-T1/T2/T4 foldl, darwin-4): **`_tlv_get_addr` is the #1 eval-thread leaf,
  ~43% self-time** (macOS dylib general-dynamic TLS), `dispatchLoop` #2 —
  confirming cause #1 persists. Attempted a contained instantiation
  (`dispatchLoop(…, reuseScope=true)` from `callClosure2`, skipping the
  redundant same-vm active-VM push/pop + `tlCurrentDispatchVM` dance per fold
  element; gated `NIX_V3_LEAFCALL_FAST`). Byte-identical, but **FOLDL only −5.7%
  / MAPFOLDL −1.0%** — below the −≥25% keep-bar → **REVERTED.** Finding: the
  per-RE-ENTRY VMScope TLS is only ~5.7%; the dominant TLS is **per-OPCODE**
  inside `dispatchLoop` — `Arena::majorGcEnabled()` + `threadArena()` read every
  iteration in the default-on major-GC safepoint (vm.cc ~2997/3034), plus
  `getNixEvalState()` per primop call. The real T5 lever is hoisting those
  per-opcode thread-local reads to a per-dispatchLoop-entry cache (mirroring the
  existing per-entry `Nursery*` cache, vm.cc ~2842) — a separate measure-first
  change, NOT the leaf-call re-entry. Re-profile per-offset (atos) to confirm
  the dominant per-opcode TLS site before that surgery.

**Deferred:** T3 (ValuePair 64→32B) and T6 (ListVec 16→8B) intersect the
`MEMORY_REPRESENTATION_2026-06-07` Lever B (Value 16→8B), which is design-only /
not in flight — folding them into that lever (per the plan's ⚠ coordination
note) avoids forking the core representation twice. (T3 also requires splitting
the `App3` path off `ValuePair` + dropping the App-memo `evaluated` field —
broad, with shared-App memoization risk.)

## Methodology notes (for the next investigation)

- 3 source agents parallelised over local source (no host contention); the dedicated
  host (darwin-4) was **single-tenant** for all timing/profiling (concurrent load
  was reaped first — contamination is a repeated trap).
- `NIX_VM_STATS` dumps **multiple times** (per re-entrant `runRootExpr`); read the
  **final/cumulative** dump (`tail`, not `head`) — the early dumps show `insns=2` /
  `peak_rss=34MB` and lie. Reconcile internal `peak_rss` against `/usr/bin/time -l`
  OS RSS to catch this.
- `sample`/atos self-time is the ground truth that re-ordered the source-reasoned
  ranking (surfaced TLS#1, demoted `std::vector`). Code reading generates
  hypotheses; profiling ranks them.
