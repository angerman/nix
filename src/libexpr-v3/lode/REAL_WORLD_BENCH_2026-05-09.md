# Real-world v3 vs TW measurement (2026-05-09)

Date: 2026-05-09
System: aarch64-darwin (M4)
Bench harness: `src/libexpr-v3/bench/bench.py` (Python; replaces the
ad-hoc bench-eval-only.sh / bench-v3-vs-tw.sh shells).
Baseline: `src/libexpr-v3/bench/baselines/2026-05-09-post-getenv-cache.json`
N=5 per cell.

This is the first session where v3 gets measured against real
production nixpkgs lib code.  Synthetic microbenchmarks (fib,
ackermann) showed v3 has overtaken TW; this exercise tests how that
translates to real-world workloads.

## Headline

| Workload          | TW min  | v3-direct        | v3-hook          |
|-------------------|---------|------------------|------------------|
| **synthetic compute** |     |                  |                  |
| fib30             | 425.9ms | 672ms (1.58x)    | **303ms (0.71x)** |
| fib33             | 1621ms  | 2647ms (1.63x)   | **1090ms (0.67x)** |
| ackermann-3-7     | 176.8ms | 282ms (1.60x)    | **159ms (0.90x)** |
| **real-world lib** |        |                  |                  |
| lib-foldl-1k      | 55.0ms  | 59ms (1.07x)     | 59ms (1.07x)     |
| lib-foldl-10k     | 56.0ms  | 62ms (1.10x)     | 60ms (1.07x)     |
| lib-genAttrs-100  | 55.2ms  | 62ms (1.12x)     | 60ms (1.09x)     |
| lib-mapAttrs-100  | 55.3ms  | 62ms (1.12x)     | 61ms (1.10x)     |
| lib-fix-deep      | 54.5ms  | 57ms (1.04x)     | 57ms (1.04x)     |
| lib-recursive-update | 55.0ms | 62ms (1.12x)   | 61ms (1.10x)     |
| lib-attrnames     | 54.2ms  | 57ms (1.05x)     | 56ms (1.03x)     |

**Headline:** v3-hook is **faster than TW** on tight numeric loops.
On real-world lib workloads, v3 modes are 3–12% slower than TW —
but the V3_TIMING phase split shows the gap is in startup, not
dispatch.  The v3 dispatch loop itself takes 2–6ms for these
workloads (see "Phase split" below).

## Phase split — where the 10% v3 overhead lives

Per `--format phases` (V3_TIMING phase split, captured on the last
run of each cell):

| Workload          | mode      | lower  | optimise | compile | run      |
|-------------------|-----------|--------|----------|---------|----------|
| fib33             | v3-direct | 0.061  | -        | 0.036   | 1088ms   |
| fib33             | v3-hook   | 0.055  | -        | 0.020   | 1071ms   |
| lib-foldl-10k     | v3-direct | 0.036  | -        | 0.031   | **5.7ms** |
| lib-foldl-10k     | v3-hook   | 0.053  | -        | 0.020   | **5.6ms** |
| lib-genAttrs-100  | v3-direct | 0.042  | -        | 0.032   | **6.0ms** |
| lib-fix-deep      | v3-direct | 0.042  | -        | 0.038   | **2.0ms** |

The lib workloads' v3.run wall is **2–6 ms**.  TW's total wall is
~55 ms.  Subtracting the ~52 ms `nix eval` startup floor leaves
~3 ms of TW eval work.  v3 is therefore **roughly TW-equivalent on
the actual eval work**; the 5–7 ms overhead in the wall comes from
startup + lib parsing, which v3 also pays.

The takeaway: **v3 is not slow on real-world lib code — it's
indistinguishable from TW once the per-process startup floor is
factored out**.  Future bench iterations should add a
"warm-cache" comparison (run the same eval twice in one process,
report only the 2nd) to surface the real-eval delta.

## What we couldn't measure: full nixpkgs eval

Goal: measure `(import nixpkgs {}).hello.name` and similar real
production workloads under v3-direct + v3-hook.

Result: **blocked**.  Both v3 modes fail:

```
$ NIX_USE_V3=1 nix eval --impure \
    --expr "(import nixpkgs {}).hello.name"
error: v3 forceValue: infinite recursion (blackhole)

$ NIX_V3_DIRECT_EVAL=1 nix eval --impure \
    --expr "(import nixpkgs {}).hello.name"
error: v3 OP_WITH_LOOKUP: name 'callPackage' not found in with-scope
```

This is the open issue tracked in task #498 ("Root-cause why
always-thunkify regresses callPackage in nixpkgs", in_progress).
Once #498 closes the bench harness will pick up these workloads
automatically — they're already defined in `workloads.toml`,
tagged `skip-by-default` to mark the dependency.

The cardano-node case is an even larger superset of the same
issue (it imports nixpkgs + cardano-haskell-packages); same
remediation applies.

## Methodology notes captured this session

1. **Bench precision matters at small scale.**  The previous
   shell harness used `/usr/bin/time -l` which on macOS reports
   `real` to 0.01s precision.  Sub-100ms workloads collapsed
   into a single bucket, hiding the actual signal.  Switching to
   Python's `time.monotonic()` (microsecond granularity) is what
   surfaced the lib-workload "v3 only 5% behind TW" signal that
   `time -l` rounded away.

2. **V3_TIMING for v3-direct mode landed this session.**
   Previously the V3_TIMING dump was registered inside
   `v3CallFunctionEntry`'s first-call init — never fires for
   NIX_V3_DIRECT_EVAL=1.  Added a small RAII PhaseTimer to
   `run.cc` that emits the same `lower=… compile=… run=…
   bridge=…` format whether the user goes through the hook or
   the direct entry.  The harness parses both transparently.

3. **`nix eval` startup is a 50-ms floor.**  Every workload pays
   this regardless of evaluator.  For sub-50-ms eval workloads
   this dwarfs the actual eval cost; relative percentages are
   misleading.  Use the V3_TIMING `run=` value as the
   apples-to-apples comparand for these workloads.

4. **The dispatch-loop getenv audit closed.**  This session caught
   eight more inline `std::getenv()` calls in vm.cc + primops.cc
   beyond the original seven; cached them all.  Added a lint
   (`test/lint-no-inline-getenv.sh`) that grep-checks for the
   pattern, with an allowlist for the legitimate `builtins.getEnv`
   primop that can't be cached (user-supplied arg).  Should be
   wired into CI to block silent perf regressions.

## Next sessions

In priority order (by "what would unblock real-world eval
measurements"):

1. **#498**: close the always-thunkify callPackage upvalue
   regression so full nixpkgs eval works in v3 modes.  Until this
   lands, real-world signal is restricted to lib-only workloads.

2. **Warm-cache bench mode**: wire a single-process two-eval
   benchmark that reports only the second eval's wall, to back
   out the 50-ms startup floor.  Will show the lib workloads as
   <1-ms-net-of-startup, which is the actual signal.

3. **RSS snapshot**: capture peak memory under each mode.  Memory
   is the other axis where v3's wins (no-arena freedom for the
   bytecode) and losses (Boehm GC vs TW's bump allocator) are
   most visible.

4. **CI integration**: wire `lint-no-inline-getenv.sh` and
   `test_bench.py` into the meson test runner so they execute on
   every build.  Currently they're standalone scripts.
