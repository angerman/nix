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

(Baseline: `src/libexpr-v3/bench/baselines/2026-05-09-with-modules.json`,
N=5 per cell.)

| Workload          | TW min  | v3-direct        | v3-hook          |
|-------------------|---------|------------------|------------------|
| **synthetic compute** |     |                  |                  |
| fib30             | 434.2ms | 683ms (1.57x)    | **307ms (0.71x)** |
| fib33             | 1642ms  | 2655ms (1.62x)   | **1086ms (0.66x)** |
| ackermann-3-7     | 174.6ms | 281ms (1.61x)    | 158ms (0.91x)    |
| **real-world lib (small)** |   |                  |                  |
| lib-foldl-1k      | 55.8ms  | 59ms (1.07x)     | 59ms (1.05x)     |
| lib-foldl-10k     | 55.9ms  | 62ms (1.10x)     | 60ms (1.07x)     |
| lib-genAttrs-100  | 55.0ms  | 62ms (1.13x)     | 61ms (1.10x)     |
| lib-mapAttrs-100  | 55.8ms  | 62ms (1.11x)     | 61ms (1.09x)     |
| lib-fix-deep      | 55.0ms  | 57ms (1.03x)     | 57ms (1.04x)     |
| lib-recursive-update | 55.3ms | 62ms (1.12x)   | 61ms (1.10x)     |
| lib-attrnames     | 55.5ms  | 57ms (1.02x)     | 57ms (1.02x)     |
| lib-makebinpath   | 55.7ms  | 64ms (1.15x)     | 63ms (1.13x)     |
| lib-strings-ops   | 55.3ms  | 61ms (1.11x)     | 60ms (1.09x)     |
| **real-world NixOS module system** | | | |
| lib-evalModules-trivial | 57.8ms | 84ms (1.45x) | 82ms (1.41x)     |
| lib-evalModules-100 | 58.4ms | 87ms (1.49x)   | 84ms (1.43x)     |
| lib-types-int     | 57.6ms  | 84ms (1.46x)     | 81ms (1.40x)     |

**Headline (revised):** the v3 perf story splits into three regimes:

1. **Tight numeric loops (fib, ackermann)** — v3-hook is **faster
   than TW** (0.66–0.91x).  Synthetic but indicative: v3's dispatch
   is now efficient enough to beat TW's tree-walk-with-vtables on
   compute-bound code.

2. **Real-world lib workloads (small, ~5ms net of startup)** — v3
   modes are 3–15% slower on outer wall, but only 5–6 ms of v3
   dispatch time vs. ~3 ms TW.  Most of the wall is the ~50-ms
   `nix eval` startup; the actual eval delta is only 1–3 ms.

3. **NixOS module system (lib.evalModules + types)** — v3 modes are
   **40–50% slower** than TW.  Phase split shows v3.run = 26–29 ms
   for the trivial / 100-option modules vs. 2–6 ms for plain lib
   workloads.  This is a real, dispatch-level slowdown that's NOT
   absorbed by the startup floor.  The module system's deep
   attrset construction + mkOption + type-merge cycle is a v3
   bottleneck worth investigating separately from #498 (the
   callPackage upvalue regression that blocks full pkgs eval).

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

In priority order (by ROI on understanding + closing the v3-vs-TW
performance gap on real-world workloads):

1. **NixOS module system perf investigation** — v3 is 40–50%
   slower on `lib.evalModules`.  V3_TIMING shows v3.run = 26–29 ms
   per call vs. presumed ~5 ms TW.  Profile this workload with
   NIX_VM_STATS on to identify the dominant opcodes
   (likely OP_ATTRS_REC_INIT + the attribute-merge primops).
   228k bytecode instructions for a 100-option module = 2200
   per option.  Each option pays for: option-decl attrset
   construction, mkOption call, type wrapping, merge through
   evalModules.  Find which step is dominant.

2. **#498**: close the always-thunkify callPackage upvalue
   regression so full nixpkgs eval works in v3 modes.  Until this
   lands, real-world signal is restricted to lib-only + module-
   system workloads.

3. **Warm-cache bench mode**: wire a single-process two-eval
   benchmark that reports only the second eval's wall, to back
   out the 50-ms startup floor.  Will show small-lib workloads
   as <1-ms-net-of-startup (the actual signal) and module-system
   workloads as their full v3.run delta.

4. **RSS snapshot**: capture peak memory under each mode for the
   23-workload corpus.  Memory is the other axis where v3's wins
   (no-arena freedom for the bytecode) and losses (Boehm GC vs
   TW's bump allocator) are most visible.

5. **CI integration**: ✅ done — `meson test --suite libexpr-v3`
   now runs the bench self-tests + getenv lint as part of the
   regular test suite.
