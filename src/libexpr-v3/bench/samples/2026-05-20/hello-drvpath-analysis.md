# hello.drvPath — Rule 0 measurement on perf-trace tool

**Date**: 2026-05-20  
**Tool**: `src/libexpr-v3/bench/perf-trace.py` (just landed)  
**Workload**: `(import <nixpkgs> {}).hello.drvPath`  
**Nixpkgs**: `/Users/angerman/Projects/zw3rk/nixpkgs` (24.05)  
**Runs**: 3 per mode, 50 ms sampler cadence  
**Hardware**: M4 darwin (this machine)  

## Summary

| Mode | wall p50 | peak heap | peak RSS | avg CPU | Boehm alloc throughput |
|---|---|---|---|---|---|
| TW alone | 649 ms | 403 MB | 179 MB | 64% | 181 MB/s |
| v3-direct (v3-native) | 10204 ms | 403 MB | 4343 MB | 93% | 38 MB/s |

## Headline number

**v3-native is 15.7× slower than TW** on `hello.drvPath`.  
Down from the historical 30× of `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` — recent work
(#680/#687 strictness, #694 lazy-with-arg cleanup, etc.) has chipped at the gap, but not closed it.

## What this kills (Rule 0)

**Hypothesis from EXTEND_DERIVATION_INVESTIGATION §"factor (b) GC scan time"**: 
> *"Boehm conservative GC inherited from cppnix has no generational separation,
> doesn't shrink the heap once grown, and conservatively retains pointer-shaped words.
> Stage 3 — landing the Cheney nursery default-on — is the architectural fix for the
> GC-scan factor of the 200× gap."*

**Falsified.**  The measurement shows:

- Boehm arena size (`GC_get_heap_size()`) is **identical at 403 MB** in both modes.
- Boehm `free_bytes` stays at **~402 MB throughout v3 eval** — the arena is essentially
  empty from Boehm's perspective; v3 isn't allocating much **through Boehm**.
- Boehm allocation throughput is **35-39 MB/s on v3** vs **134-156 MB/s on TW**.
  v3 hits Boehm *less*, not *more*.

- Yet RSS grows from **54 MB to 4.3 GB on v3** (vs 33 → 182 MB on TW).
  That ~4 GB of RSS lives **outside the Boehm arena**.

- v3 sustains **93% CPU steady-state** vs TW's **bursty 67%**.
  GC-scan-dominated would look like the opposite: long idle pauses between scans, not
  steady saturation.

## Where v3's RSS actually goes

v3 has its own non-Boehm allocators (`Alloc::allocBindings`, `allocChars`,
`allocClosure`, the closure-pool, possibly the optional Cheney nursery `NIX_V3_NURSERY`).
These mmap pages directly and don't go through Boehm's `GC_malloc`.  On `hello.drvPath`,
**95-99 % of v3's allocations bypass Boehm**.  That has two consequences:

1. **Boehm GC isn't the bottleneck.**  Whatever the 15.7× gap is, it isn't GC scan time.
2. **v3's own allocator has no reclamation pressure.**  RSS grows monotonically to
   ~4 GB and never shrinks during the eval.  TW's Boehm cycle bounds RSS at 182 MB.

Speculative but the most likely candidates for the 4 GB:

- The closure-pool and binding-pool slabs accumulate without compaction.
- Each bytecode primop call allocates working buffers (e.g. forceValue writeback
  buffers, OP_LIST_CONCAT temporary lists, OP_ATTRS_UPDATE auxiliary bindings) that
  the existing pool allocators retain.
- The optional Cheney nursery, if enabled, has a fixed 2 GB capacity — but it's NOT
  enabled by default; we'd see `NIX_V3_NURSERY=1` in the env, and we didn't set it.

**The next falsifier is to identify which allocator (Alloc, closure-pool, nursery,
or something else) owns the 4 GB.**  That needs an in-process allocator-counter dump,
not just `GC_get_*`.

## What this kills about the ROADMAP

`ROADMAP_TO_VISION_2026-05-15.md` Stage 3 (nursery default-on) is currently positioned
as **load-bearing** for closing the hello.drvPath gap.  This measurement says that's
only partly right:

- Nursery solves the GC-scan factor only **if Boehm is the bottleneck**.  Today Boehm
  isn't being hit hard enough on `hello.drvPath` for nursery to make a difference.
- Nursery's actual win on this workload would come from **reducing the 4 GB non-Boehm
  allocator's growth**, by routing v3's intermediate allocations through the nursery
  (bump-allocate + cheap recycle).  But that's a Phase D write-barrier dependency.

Refactor needed for ROADMAP Stage 3 motivation: the load-bearing rationale shifts from
"GC scan dominates" to "v3's non-Boehm allocator has no reclamation; the nursery is
the reclamation mechanism."  Same intervention, different mechanism.

## Followups

- **(NEXT)** In-process allocator-counter dump: instrument `Alloc::*` and pool
  allocators to log peak + cumulative bytes at exit. ~30 LoC.  This narrows the 4 GB.
- Re-run with `NIX_V3_NURSERY=1` to see whether the nursery is the 4 GB or whether
  it's downstream.  If RSS doesn't change with NURSERY=1, the nursery isn't the owner.
- Re-measure `hello.drvPath` after each Stage-3 sub-phase to see whether the 15.7×
  closes.  Today's number is the new floor.

## Evidence

- Raw JSONL: `bench/samples/2026-05-20/hello-drvpath-{tw,v3-native}-run{1,2,3}.jsonl`
- SVG overlay: `bench/samples/2026-05-20/hello-drvpath.svg`
- Summary: `bench/samples/2026-05-20/hello-drvpath-summary.md`
