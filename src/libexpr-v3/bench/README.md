# v3 benchmark suite

A statistical, multi-mode, multi-workload benchmark harness for the
v3 evaluator vs. the in-tree tree-walker.

## Layout

```
src/libexpr-v3/bench/
├── bench.py           # main driver (Python 3.11+; uses tomllib only)
├── workloads.toml     # workload definitions (TOML)
├── test_bench.py      # self-tests for the harness (positive/negative/regression)
├── baselines/         # JSON snapshots for trending / regression detection
└── README.md          # this file
```

## Quick start

```bash
# 1. Build nix.
nix develop -c make build

# 2. Run all default workloads (TW vs v3-direct).
nix develop -c python3 src/libexpr-v3/bench/bench.py

# 3. Run all modes, more samples, on a wider set.
nix develop -c python3 src/libexpr-v3/bench/bench.py \
    --modes tw,v3-direct,v3-hook -n 5 --format ratio
```

## Modes

| mode            | env vars set                              | when to use                                          |
|-----------------|-------------------------------------------|------------------------------------------------------|
| `tw`            | (none)                                    | baseline (in-tree tree-walker)                       |
| `v3-direct`     | `NIX_V3_DIRECT_EVAL=1`                    | pure-v3 path; bypasses TW eval entirely (#458)       |
| `v3-hook`       | `NIX_USE_V3=1`                            | TW-primary, v3-as-leaf via the cutover hook (#426)   |
| `v3-hook-fhook` | `NIX_USE_V3=1` + `NIX_USE_V3_FORCE=1`     | both eval AND force hook on v3                       |

## Workloads

Defined in `workloads.toml`.  Three kinds:

- **synthetic** — pure-v3 workloads with no I/O (fib, ackermann,
  list-build, attrset-build, with-deep, letrec-fix).  Always runnable.

- **lib** — real lib (nixpkgs/lib) workloads loaded directly via
  `import @NIXPKGS@/lib`, bypassing pkgs/all-packages.nix.  As of
  2026-05-09 these are the load-bearing real-world signal because
  full pkgs eval is blocked on #498 in v3 modes.

- **nixpkgs** — full `import @NIXPKGS@ { ... }` workloads.  Currently
  tagged `skip-by-default` because they fail in v3 modes; will
  re-enable when #498 closes.

## Output formats

| `--format`   | description                                                 |
|--------------|-------------------------------------------------------------|
| `table`      | wide table, all stats per cell                              |
| `ratio`      | TW min / mode ratio (default eyeball view)                  |
| `phases`     | V3_TIMING phase split (lower / compile / run / bridge)      |
| `markdown`   | GFM table (paste into reports)                              |
| `csv`        | one row per individual run (for downstream analysis)        |
| `json`       | structured snapshot (use with `--save` / `--baseline`)      |
| `all`        | all three of: timing table, ratio table, phases             |

## Filtering

```
--only fib33,ackermann-3-7   # comma-separated workload names
--tag real-world             # workloads matching any --tag are kept
--skip-tag skip-by-default   # exclude workloads matching the tag
```

## Baselines & regression detection

Save a snapshot:

```bash
nix develop -c python3 src/libexpr-v3/bench/bench.py \
    --modes tw,v3-direct,v3-hook -n 5 \
    --save src/libexpr-v3/bench/baselines/2026-05-09-post-getenv-cache.json
```

Diff a current run against it:

```bash
nix develop -c python3 src/libexpr-v3/bench/bench.py \
    --modes tw,v3-direct,v3-hook -n 5 \
    --baseline src/libexpr-v3/bench/baselines/2026-05-09-post-getenv-cache.json \
    --threshold 5
```

Cells where `min` regresses by more than `--threshold` percent are
flagged `REGRESSION`; cells that improved by more than the threshold
are flagged `IMPROVEMENT`.

## Self-tests

```bash
nix develop -c python3 src/libexpr-v3/bench/test_bench.py
```

Currently 30 tests covering: workload loading, filtering (positive +
negative cases), stats math (including all-failed and partial-failed
edge cases), JSON round-trip, baseline diff in three directions
(no-change, regression, improvement), and an end-to-end smoke test
that runs `nix eval` under TW + v3-direct.

## Adding workloads

Append to `workloads.toml`:

```toml
[my-workload]
kind = "synthetic"  # or "lib" or "nixpkgs"
note = "one-line description shown in tables"
expr = "<Nix expression>"   # @NIXPKGS@ and @SYSTEM@ substituted
tags = ["compute-bound", "list"]   # arbitrary; used by --tag
```

Add `"skip-by-default"` to the tags list if the workload should
require explicit `--only` to run.

## Why a Python harness rather than a shell script

`bench-v3-vs-tw.sh` and `bench-eval-only.sh` (still present, kept
for backwards-compat) hit several walls:

- macOS `/usr/bin/time -l` reports `real` to 0.01s precision; sub-
  100ms workloads collapse into a single bucket.  Python's
  `time.monotonic()` has microsecond granularity.
- Statistical reporting beyond min/max requires `awk` gymnastics.
- JSON output for trending / baseline diffs is much easier in
  Python (built-in `json` module) than POSIX shell.
- TOML workload definitions decouple the workload list from driver
  logic — adding a workload doesn't require editing the script.
- Self-tests (`test_bench.py`) exercise filter, stats, diff
  contracts; that's tractable in Python, opaque in shell.

The shell scripts are kept for the existing wired-up Makefile
target paths — both produce the same physical measurement, so they
can co-exist until everyone migrates.

## Operational notes

### Variance management

- Run with `-n 5` minimum for any decision-quality measurement.
- Pin CPU governor to `performance` if available (Linux); on macOS,
  close other applications and don't run on battery.
- Min wall is more reliable than mean — best run is closer to the
  underlying speed; worse runs have noise from unrelated system
  events.
- The harness automatically uses the `min` for ratio / baseline
  diff comparisons.

### Process startup floor

Every workload pays ~50ms for `nix eval` startup (binary load +
libstore init + parse).  Anything sub-50ms wall is dominated by
that floor and not a useful eval-perf signal.  Use the `--format
phases` view to see `v3.run` (just the dispatch loop time), which
strips startup.

### Running real-world nixpkgs workloads

The bench harness auto-resolves `@NIXPKGS@` from `nix flake archive
--json` if there's a `flake.nix` with a `nixpkgs` input.  Override
with `NIXPKGS=/path/to/nixpkgs python3 ...`.

Currently full pkgs workloads (anything calling `import nixpkgs
{...}`) fail in v3 modes due to #498.  Lib-only workloads (importing
`nixpkgs/lib` directly) work and exercise real production lib code.

### Lint

`src/libexpr-v3/test/lint-no-inline-getenv.sh` guards against
re-introducing per-call `std::getenv()` in the dispatch loop hot
path.  Run as part of CI to detect performance regressions before
they land.
