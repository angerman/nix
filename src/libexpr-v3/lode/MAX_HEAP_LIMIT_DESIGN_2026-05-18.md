# Max-heap limit — in-process design

**Date**: 2026-05-18.
**Purpose**: design a `NIX_V3_MAX_HEAP=2G`-style memory ceiling that v3 enforces internally, with graceful exception-based handling and useful diagnostics. NO CODE in this doc.

**Why this matters**:
1. **GC tuning**: tight memory caps force the issue. Stage 3 (nursery default-on) and Phase D (write barriers) can only be validated under pressure if pressure exists. Without a cap, v3 grows to 1GB+ on `hello.drvPath` without complaint; with a cap, we surface the actual working-set requirement.
2. **CI predictability**: a 2GB cap on test fixtures fails predictably rather than swap-thrashing the runner.
3. **Production safety**: combined with external cgroup/ulimit, gives a graceful degradation path before the OS-level kill.
4. **Diagnostic value**: when OOM happens, v3 controls the abort and can dump heap stats, hot allocators, etc. instead of leaving an opaque SIGKILL.

## 1. Three approaches and their trade-offs

### Approach A — OS-level limits (`setrlimit` / cgroups / `ulimit`)

External enforcement. Process gets killed (SIGKILL) on overshoot.

**Pros**: hard guarantee; works for ALL allocations including system libraries; standard practice.

**Cons**:
- Ungraceful: SIGKILL with no diagnostic.
- Cross-platform inconsistency: `RLIMIT_RSS` is largely ignored on modern Linux (only a soft hint); `RLIMIT_AS` is virtual memory not RSS; macOS has no equivalent to Linux cgroups.
- Per-process, not per-eval: a daemon-style v3 can't cap individual evals.
- Test infrastructure complexity: need to wrap each test in `ulimit` or a cgroup.

### Approach B — In-process allocator caps (Boehm + nursery)

Cap Boehm's heap via `GC_set_max_heap_size(N)`. On OOM, Boehm calls a user-installed `GC_set_oom_fn` hook that throws a v3 `OutOfMemoryError`. v3's existing exception machinery unwinds cleanly.

**Pros**:
- Cross-platform (Boehm works on Linux + macOS).
- Graceful: throws a typed exception; existing `dispatchLoop` catch handles unwind.
- Precise at allocation boundaries.
- Zero polling cost.
- Easy to vary per-eval (env var or settings.cc).

**Cons**:
- Tracks Boehm heap, not full RSS. Code segment, stack, mmap-d libraries, and non-Boehm allocations (std::string buffers, std::vector, etc.) are NOT counted.
- A 2GB Boehm cap → typically ~2.3-2.5GB actual RSS (rough rule of thumb: +200-400MB for code/stacks/libc).
- Doesn't capture nursery allocations unless we also cap nursery (already do).

### Approach C — Periodic RSS polling

Periodically (every N allocations, or every M opcodes) check actual RSS via `/proc/self/status` (Linux) or `task_info()` (macOS). If over limit, throw.

**Pros**:
- Tracks true RSS.
- Catches non-Boehm allocations.
- Cross-platform.

**Cons**:
- Polling cost (~1 syscall per check; adds up).
- Overshoot between polls (can exceed limit transiently).
- More complex than allocator-level cap.

### Recommendation: hybrid B+C

Primary mechanism: **Boehm heap cap** (Approach B) — catches 90%+ of allocations precisely. Default-on when `NIX_V3_MAX_HEAP` is set.

Optional verification: **periodic RSS poll** (Approach C) — opt-in via `NIX_V3_VERIFY_RSS=1` for dev/debugging. Detects when actual RSS diverges from heap-accounting (e.g., a primop allocating outside Boehm).

External backstop: **let users add `ulimit`/cgroup** as defense-in-depth. v3's cap fires first (graceful); OS cap fires second (kill-switch).

## 2. Implementation sketch (design, no code)

### 2.1 Configuration

```
NIX_V3_MAX_HEAP=2G        # cap on Boehm + nursery + reserved overhead
NIX_V3_NURSERY_SIZE=64M   # part of the above; nursery's share
NIX_V3_MAX_HEAP_RESERVE=128M  # reserved for code/stacks/libc/etc.
NIX_V3_VERIFY_RSS=1       # opt-in: periodic /proc/self/status check
NIX_V3_VERIFY_RSS_PERIOD=10000  # check every N opcodes
```

Compute: `boehmCap = NIX_V3_MAX_HEAP - NIX_V3_NURSERY_SIZE - NIX_V3_MAX_HEAP_RESERVE`. Set via `GC_set_max_heap_size(boehmCap)` at init.

Defaults:
- `NIX_V3_MAX_HEAP=unset` → no cap (current behavior).
- If set: must be ≥ 128MB; otherwise refuse to start (no point trying to evaluate Nix with less).
- `NIX_V3_MAX_HEAP_RESERVE` default: 128MB. Tunable in case experience shows different headroom needed.

### 2.2 Boehm OOM hook

```
// Set at init:
GC_set_oom_fn(v3OomHandler);

// Handler (pseudocode):
void * v3OomHandler(size_t requestedBytes) {
    // Don't recurse — set a flag, log briefly, throw.
    OOMError e;
    e.requested = requestedBytes;
    e.heapSize = GC_get_heap_size();
    e.maxHeap = GC_get_max_heap_size();
    e.topAllocators = snapshotAllocStats();  // from existing allocStats
    e.lastGCs = GC_get_full_gc_total_time();
    throw OutOfMemoryError(e);
}
```

The handler must NOT allocate (it's called inside a failing allocation context). Diagnostic snapshot uses pre-allocated buffers.

### 2.3 Exception type + propagation

New exception type: `v3::OutOfMemoryError : public EvalError`. v3's `dispatchLoop` already has a try/catch hierarchy for EvalError unwinds (per Phase 1 work). Adding one more derived type costs little.

Propagation path:
1. Allocation fails (Boehm internal limit hit).
2. `v3OomHandler` invoked; constructs `OutOfMemoryError`; throws.
3. Exception unwinds through dispatch loop; clears blackmarks per existing protocol.
4. Caller of `v3-eval` (CLI or library API) sees the typed exception with diagnostic.

### 2.4 Periodic RSS verification (opt-in)

If `NIX_V3_VERIFY_RSS=1`:
- Every `NIX_V3_VERIFY_RSS_PERIOD` opcodes, check current RSS.
- Linux: parse `/proc/self/status` `VmRSS:` line.
- macOS: `task_info(mach_task_self(), TASK_BASIC_INFO_64, ...)`.
- If RSS > `NIX_V3_MAX_HEAP * 1.2`: log warning (diagnostic — heap accounting is leaking somewhere).
- If RSS > `NIX_V3_MAX_HEAP * 1.5`: throw OOM (safety fallback).

This catches non-Boehm allocations that escape the Boehm cap.

Cost: one syscall per N opcodes. With N=10000, overhead is well under 1%.

### 2.5 Diagnostics on OOM

When `OutOfMemoryError` is thrown, the diagnostic payload includes:
- `requested`: bytes the failing alloc wanted.
- `heapSize` / `maxHeap`: Boehm's view.
- `actualRSS`: current RSS via syscall.
- `topAllocators`: top-N alloc sites from `allocStats()` (already exists per `V3_DBG_ALLOC_DUMP`).
- `nurseryFillPct`: nursery occupancy at OOM time.
- `lastFullGC` / `nrFullGCs`: GC activity counts.

Printed to stderr by the CLI; available programmatically via the exception object.

## 3. Use cases

### 3.1 GC tuning workflow

```sh
# Force tight memory; observe what fails
NIX_V3_MAX_HEAP=512M NIX_V3_NURSERY=1 v3-eval --file repro-large.nix
# Output on OOM:
# v3: OutOfMemoryError: requested 1024 bytes; heap=502MB/512MB
#   Top allocators:
#     1. mkDerivation buildInputs iteration: 145MB
#     2. lib.evalModules: 89MB
#     3. extendDerivation outputsList: 67MB
#   Nursery: 62MB/64MB (97%)
#   Full GCs: 18 in 12.3s
```

This is exactly the diagnostic Stage 3 needs to validate the nursery actually reduces working set vs the baseline.

### 3.2 CI / test fixture predictability

Test fixtures can specify their memory budget:

```nix
# RUN: env NIX_V3_MAX_HEAP=128M v3-eval --file %s --emit-ir | v3-check %s

# Assert that streamFusion doesn't blow up memory on a 10k-element list
builtins.foldl' (acc: x: acc + x) 0 (builtins.genList (n: n) 10000)

# CHECK: ; OK
```

If the fixture exceeds 128MB, it fails with a clear OOM diagnostic. Catches memory regressions early.

### 3.3 Production safety with external backstop

```sh
# Layered defense:
ulimit -v 4194304  # 4GB hard (OS-level backstop)
NIX_V3_MAX_HEAP=2G nix eval --expr 'pkgs.hello.outPath'
# v3 hits cap at 2GB → throws → user sees Nix-level error
# If something escapes v3 (e.g., bug), ulimit fires at 4GB → process killed
```

### 3.4 Bench harness

Add to `bench/bench.py`: every benchmark run gets a default `NIX_V3_MAX_HEAP=4G` cap. Catches regressions where a "perf improvement" actually doubles memory usage.

## 4. Connection to GC roadmap

This feature is enabling, not blocking, for:

- **Stage 3 (nursery default-on)**: forces measurement of "does the nursery actually reclaim memory?" Without a cap, you can't tell; with a cap, you see whether eval completes or OOMs.
- **Phase D (write barriers)**: under tight cap, missed write barriers cause use-after-free (caught by ASan/Valgrind) OR cause OOM (because survivors aren't promoted correctly). Stress test discipline gets immediate signal.
- **Stage 4 (uniform STG + strictness)**: Stage 4 increases allocation rate; without a cap, you might not notice the increase until eval slows. With a cap, you OOM and notice immediately.
- **Phase D stress mode**: `V3_DBG_GC_STRESS` + tight cap = adversarial test environment for catching memory bugs.

## 5. Open questions

**Q1: What about non-Boehm allocations?** v3 may use std::string, std::vector, etc. via libc malloc. These aren't tracked by Boehm cap. Mitigation: periodic RSS poll (Approach C) catches them at the macroscopic level. Or: route v3's std::allocator through a Boehm-aware allocator (more invasive).

**Q2: What about the TW bridge?** When v3 calls into TW (FFI), TW does Boehm allocations too. These count against the same Boehm cap automatically (good). But TW also does its own state management; under tight cap, TW may OOM and we'd see the same throw. Fine.

**Q3: How does this interact with the closure-pool?** The pool sits on top of Boehm; its allocations are counted. When pool tries to allocate a fresh slot and Boehm refuses, OOM fires. Should be clean.

**Q4: What about long-running daemon mode?** Hypothetical future v3 daemon would want per-eval caps, not per-process. Boehm caps are per-process; can be relaxed/tightened at runtime via `GC_set_max_heap_size`. Acceptable for the per-eval-cap use case.

**Q5: What if Boehm's overhead is high?** Boehm reserves ~10-30% for internal data structures (free lists, mark bits, etc.). A 2GB cap = ~1.4-1.8GB usable. Document this.

**Q6: Should the nursery cap be independent of the heap cap?** Yes — they have different roles. Heap cap = "total memory ceiling." Nursery size = "promotion threshold." Both are independently tunable.

**Q7: What about Tag::App memoization (added today)?** The `evaluated` field stores Values; if the value is in Boehm and Tag::App is in Boehm, allocations are counted. Tag::App memoization may increase memory usage in some workloads — tight cap helps surface this.

**Q8: Cross-platform RSS query — Windows?** Not a v3 target today. Skip.

**Q9: How to test this works?** Three test categories:
- Synthetic: force a runaway allocation in a fixture; assert OOM fires.
- Real workload: `hello.drvPath` under `NIX_V3_MAX_HEAP=256M`; should OOM cleanly with a useful diagnostic.
- Regression: lang-test suite under `NIX_V3_MAX_HEAP=512M`; expect zero spurious OOMs.

## 6. Implementation effort estimate

| Sub-task | Time | Risk |
|---|---|---|
| Boehm cap + OOM hook integration | 0.5 day | Low (well-documented Boehm API) |
| `OutOfMemoryError` exception type | 0.25 day | Low (existing exception hierarchy) |
| CLI env-var parsing (`NIX_V3_MAX_HEAP=2G` size parsing) | 0.25 day | Low |
| Diagnostic snapshot (top allocators, RSS, GC stats) | 0.5 day | Low |
| Periodic RSS poll (Linux + macOS) | 1 day | Low (well-known syscalls) |
| Stress tests: synthetic OOM fixture; lang-test under low cap | 0.5 day | Low |
| Documentation: USAGE.md + diagnostic guide | 0.25 day | Low |
| **Total** | **~3 days focused** | Cumulative: Low |

## 7. Where this lands in the strategic structure

- **Not on the critical path** for Phase 2(R) IR optimization.
- **Strongly recommended before Stage 3 (nursery default-on)** — validates that the nursery actually reduces working set.
- **Useful infrastructure** alongside Phase 1.5 measurement spike (lets the spike measure memory under controlled limits).
- **Insertion point in the action plan**: add as a Phase 1.5 sub-item or as a new Phase 1.6 — small standalone work.

Concrete recommendation: implement during Stage 3 prep (before Phase D), with hooks that let Stage 3 measure progress.

## 8. Defaults and policy

When `NIX_V3_MAX_HEAP` is unset: no cap (current behavior). v3 doesn't change behavior for users who don't opt in.

Suggested presets:
- `NIX_V3_MAX_HEAP=512M`: small evals, tight CI tests.
- `NIX_V3_MAX_HEAP=2G`: default for bench harness; catches regressions.
- `NIX_V3_MAX_HEAP=8G`: nixos-rebuild on small-to-medium configs.
- `NIX_V3_MAX_HEAP=32G`: Hydra evaluator on large jobsets.

Document in `USAGE.md` once landed.

## 9. Cross-references

- `CHENEY_NURSERY_DESIGN.md` — Stage 3 nursery work; this feature complements it.
- `NURSERY_PHASE_D_DESIGN_2026-05-18.md` — Phase D write barriers; tight cap surfaces missed barriers.
- `EXTEND_DERIVATION_INVESTIGATION_2026-05-18.md` — the 1GB+ Boehm arena observation that motivates this.
- `LESSONS_LEARNED_2026-05-15.md` §1.6 — v3 uses Boehm GC today; this feature works through Boehm's existing API.
- `ROADMAP_TO_VISION_2026-05-15.md` Stage 3 — prerequisite of this work landing in a useful form.
- Boehm GC documentation: `GC_set_max_heap_size`, `GC_set_oom_fn`, `GC_get_heap_size`.

## 10. One-paragraph summary

A `NIX_V3_MAX_HEAP=2G` env-var setting caps Boehm's heap via `GC_set_max_heap_size`, hooks the OOM via `GC_set_oom_fn` to throw a typed `OutOfMemoryError`, and provides a diagnostic snapshot (top allocators, nursery state, GC counts). Cross-platform; in-process; graceful. ~3 days of focused work. Strongly recommended before Stage 3 (nursery default-on) — validates that the nursery actually reduces working set. Optional opt-in periodic RSS poll catches non-Boehm allocations. External `ulimit`/cgroup remains the kill-switch backstop. Approximates true RSS as `heap + nursery + ~200MB headroom`; precise RSS enforcement available via Approach C.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.
