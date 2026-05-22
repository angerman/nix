# 2026-05-22 CPU profile — v3-direct vs TW

Sampled via macOS `/usr/bin/sample` (1 ms intervals) on
hello.drvPath + cardano-node M5.

## Files (all `.sample.txt.gz` — `gunzip -c` to view)

  * `hello-drvpath-v3-direct.sample.txt.gz` — v3-direct, 1.4 s
    sample after 0.3 s startup; main-thread total = 692 samples.
  * `hello-drvpath-v3-post766a.sample.txt.gz` — same workload AFTER
    `#766a` (opt_strictness + opt_const_fold migrated to FlatBlockMap
    scratch); main-thread total = 405 samples on-CPU.  Most of the
    drop is the eliminated `std::__hash_table<VarId,Expr*>::emplace`
    work (50 → 2 inclusive samples).  **Wall clock is identical to
    pre-#766a (1.37 s ± 0.03 vs 1.38 s ± 0.01 hyperfine n=10) —
    the saved CPU lands in idle time, not in faster eval.**
  * `hello-drvpath-tw.sample.txt.gz` — TW, 0.5 s sample; only 1
    main-thread sample (eval finishes faster than the sample
    window — TW eval on hello.drvPath is sub-half-second).
  * `cn-m5-v3-direct.sample.txt.gz` — cardano-node M5 v3-direct,
    14 s sample.  Sample window captures eval phase; main-thread
    total = 10 545 samples.
  * `cn-m5-tw.sample.txt.gz` — cardano-node M5 TW, 8 s sample.
    Note: sample window captured ONLY the flake-fetch phase
    (lockFlake + addToStore via fiber-based source/sink).  Actual
    eval was too fast to overlap with the sampling window.

## Headline findings (hello.drvPath)

Self-time leaves on v3-direct main thread (n=692):

| Category | Samples | % of main |
|---|---|---|
| `dispatchLoop` body (interpreter) | 51 | 7.4% |
| `mergeBindings` lambda / outer | 53 | 7.7% |
| `primIntersectAttrs` | 25 | 3.6% |
| Allocator (`_xzm_free`/`_free`/`malloc`/`bzero`) | ~110 | 15.9% |
| IR pipeline (parser, hash tables, lower) | ~50 | 7.2% |
| `phaseDActive` flag check | 13 | 1.9% |
| Other / kernel I/O | ~390 | 56.4% |

Inclusive: `nix::v3::run` accounts for ALL 692 main-thread
samples, with ~99% inside `dispatchLoop` (and its callees).

## Headline findings (cardano-node M5)

Inclusive main-thread breakdown (10 545 samples):

| Region | Inclusive samples | % |
|---|---|---|
| `dispatchLoop` (VM bytecode runtime) | 4718 | 44.7% |
| `make_fcontext` / fiber_entry (FFI bridge: store, source/sink) | 1600 | 15.2% |
| Non-dispatch under `runRootExpr` (IR pipeline + cached lower of imported modules) | ~2624 | 24.9% |
| Pre-`CmdEval::run` (startup, parser tables, store init) | ~1603 | 15.2% |

NOT comparable to TW directly — TW's 3515 main-thread samples
were ALL inside `nix::flake::lockFlake → addToStore`, i.e. the
flake-fetch phase.  TW's actual eval phase happens after the
sample window closes.  The cardano-node v3 numbers above include
BOTH the lockFlake phase and the v3-native eval; the v3-direct
sample window covers more of the eval.

## What's surprising

  1. **Allocator (`libsystem_malloc`) is ~16% of compute on
     hello.drvPath.**  Despite Phase D being default-on (per
     `#731`), 14-16% of cycles are spent inside the C allocator
     (`_xzm_free`, `_free`, `malloc_type_malloc`).  The nursery
     bump-allocates so most cells should bypass libc malloc;
     this points at either (a) allocations falling through to
     `threadArena` (malloc-backed) more often than expected, or
     (b) C++ STL containers inside dispatchLoop /
     mergeBindings doing realloc-and-free.

  2. **`mergeBindings` is still 7.7% even after #747 two-pass.**
     The two-pass landing eliminated the duplicate-key slack but
     the per-merge work is still the second-biggest leaf.  Looks
     like merge is fundamental to `//`-chains in nixpkgs.

  3. **`primIntersectAttrs` 3.6% even after #750 fix.**  The
     two-pass intersect landed; remaining cost is the O(n) scan
     itself (or a similar fundamental).

  4. **`phaseDActive` flag check 1.9%.**  This is a global flag
     consulted at every Phase D barrier emit.  2% on a flag
     read suggests it's not inlined to a hot constant — possibly
     a global var read rather than a `__builtin_expect`.

  5. **IR pipeline is significant on cardano-node M5 (~25%).**
     cardano-node imports many modules; each pays the
     parse + lower + optimize cost.  hello.drvPath sees less of
     this since one file (the user expression) drives everything.

## Recommended next step

The flame graph points at:

  * **E1. Allocator audit (~16% on hello.drvPath).**  Are
    allocations hitting `threadArena` (malloc-backed) when they
    should hit the nursery bump-allocator?  Or is the cost in
    STL realloc/free inside dispatchLoop body?  A single-session
    instrumentation spike (per-alloc-site nursery-vs-arena
    decision counter) can falsify this.

  * **E4. `phaseDActive` flag specialisation (~2%).**  Quick
    win if it's a global-var read; promote to a process-wide
    `static const bool` cached at init time + `__builtin_expect`.

  * **E2. `mergeBindings` deeper rework (~8%).**  #748 persistent-
    spine Bindings was estimated multi-week.  Could revisit with
    a smaller scope (e.g., specialize the small-key-count case).

E1 is the biggest single lane and a 1-2 hour investigation can
either confirm a misrouted allocation (concrete fix) or
falsify the hypothesis (cost lives elsewhere).

Compared to the original A / C ranking:
  * A (closure audit, ~256 MB byte ceiling) — still relevant but
    smaller than E1.
  * C (thunk emission audit) — still relevant; might intersect
    with E1 if many thunks are malloc-backed.

## Methodology notes (for the next profiler)

  * macOS `sample <pid> <duration>` is the simplest tool.  It
    requires a PID, so spawn the eval in background, sleep ~0.3 s
    to clear startup noise, then `sample`.  Symbols resolve
    cleanly against `libnixexprv3.dylib`.
  * `samply record -- <cmd>` is the modern alternative (profile
    in Firefox Profiler).  Not used here but available via
    `nix shell nixpkgs#samply`.
  * Background-sampling a SHELL parent (`bash -c '...'`) gives
    you the shell's profile, not the child's.  Use `exec` or the
    process directly.
  * For workloads with a fetch phase (any flake-driven workload),
    the FETCH phase dominates short profiles.  Either pre-warm
    the cache, or sample only after a known "fetch complete" log
    line.
  * macOS `sample` aggregates across ALL threads in the process,
    including the signal-handler thread and Boehm GC mark
    threads.  These show up as `__psynch_cvwait` / `__sigwait`
    inclusive samples.  Subtract them to get useful main-thread
    numbers (or filter the call graph by `main-thread`).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
