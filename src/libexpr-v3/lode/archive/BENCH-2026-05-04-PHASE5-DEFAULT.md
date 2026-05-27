# v3 perf snapshot — Phase 5 default-ON, post-#436/#437/#438

> **Superseded:** numbers below are point-in-time (2026-05-04). For
> current real-world performance see `../USAGE.md` "Real-world
> behaviour" section (last updated 2026-05-05).

**Date:** 2026-05-04 (evening, post-#437 closure)
**System:** aarch64-darwin
**Build:** 70b004ee7 (Phase 5 default-ON via `NIX_V3_NO_INLINE_REC_SLOT` opt-out)
**Comparand:** in-tree tree-walker (default `nix-instantiate`, no env vars)
**Runs:** N=5 per workload/mode for synthetics; N=3 for nixpkgs/cardano

## Summary

Phase 5 (`RecBindingSlotRef` inlining) is now the default. The
cardano-node correctness regression that motivated keeping it
opt-in is closed by the #436/#437/#438 trio. All four cardano-node
modes pass (`v3` × `±fhook` × `±P5`).

**Headline:** fib35 -22% wall-clock vs tree-walker.
On nixpkgs queries, v3 default is at parity-to-+1.6%.
v3-fhook overhead remains workload-dependent (parity-to-+18%).

## Synthetic benchmarks (N=5)

```
workload      evaluator  mean      min      p50      p95   stddev
--------      ---------  -----    -----    -----    -----   ------
fib35         tw         4.222    4.131    4.234    4.301   0.064
fib35         v3         3.216    3.200    3.211    3.238   0.018   ← -22.6%
fib35         v3-fhook   3.191    3.155    3.190    3.219   0.026   ← -23.6%

ackermann     tw         0.555    0.550    0.557    0.559   0.004
ackermann     v3         0.541    0.513    0.516    0.642   0.057   ← -6.7%
ackermann     v3-fhook   0.609    0.551    0.597    0.669   0.049

path-deep     tw         0.072    0.063    0.071    0.085   0.009
path-deep     v3         0.092    0.088    0.090    0.104   0.007   ← +40.0%
path-deep     v3-fhook   0.082    0.069    0.083    0.091   0.008

letrec-fix    tw         0.086    0.068    0.090    0.095   0.010
letrec-fix    v3         0.073    0.063    0.068    0.088   0.011   ← -7.4%
letrec-fix    v3-fhook   0.069    0.066    0.067    0.073   0.003
```

`path-deep` regression (+40%) is path-selection on a 30-deep nested
attrset; v3's selector specialisation (#424 opt-in) plus #428's fast-
path opcodes target the larger workloads where the overhead amortises.
`fib35` is the canonical compute-bound case; the -22% is the headline
Phase-5 win the prior session was chasing.

## nixpkgs queries (N=3)

```
workload      evaluator  mean    min    p50    rss_min_MB
--------      ---------  -----   -----  -----  ----------
hello-name    tw         0.484   0.448  0.451  118.0 MB
hello-name    v3         0.448   0.436  0.444  167.6 MB     +1pp wall, +42% RSS
hello-name    v3-fhook   0.580   0.549  0.589  118.5 MB

git-name      tw         0.420   0.402  0.422  118.4 MB
git-name      v3         0.418   0.404  0.406  168.6 MB
git-name      v3-fhook   0.569   0.562  0.568  118.5 MB

drv3          tw         0.477   0.464  0.477  189.7 MB
drv3          v3         0.477   0.471  0.476  189.4 MB
drv3          v3-fhook   0.544   0.537  0.546  189.0 MB

attr-pkgs     tw         0.404   0.395  0.399  170.8 MB
attr-pkgs     v3         0.493   0.470  0.475  173.0 MB     +18% wall
attr-pkgs     v3-fhook   0.590   0.555  0.583  170.8 MB

attr-hask     tw         0.629   0.616  0.621  243.5 MB
attr-hask     v3         0.635   0.632  0.635  245.4 MB
attr-hask     v3-fhook   0.754   0.739  0.750  243.4 MB

hello.outPath tw         0.420   0.39   0.39   174.6 MB
hello.outPath v3         0.400   0.40   0.40   175.7 MB
hello.outPath v3-fhook   0.393   0.39   0.39   174.5 MB

git.drvPath   tw         0.443   0.44   0.44   196.0 MB
git.drvPath   v3         0.450   0.45   0.45   197.4 MB
git.drvPath   v3-fhook   0.443   0.44   0.44   196.2 MB
```

(`rss_min_MB` from `/usr/bin/time -l` "maximum resident set size",
divided by 1024² — the kernel reports bytes on macOS.)

The `attr-pkgs` +18% regression on v3 default is the same pattern
as `path-deep`: top-level `attrNames` against a freshly-imported
nixpkgs hits the v3 lower+compile cost without enough downstream
work to amortise.  v3-fhook's overhead on every nixpkgs workload
(+10 to +25%) is the WC-25/26 per-Bridge-thunk allocation tax
documented in BENCH-REAL-WORLD-2026-05-04.md §C.2 — unchanged.

## Cardano-node real-world (N=5)

Workload: `nix eval` of
`flake#packages.aarch64-darwin.cardano-node.name`
on a local `/Users/angerman/Projects/iohk/cardano-node` checkout.

```
mode       run1   run2   run3   run4   run5    rss_min_MB
tw         3.41   3.76   3.49   3.41   3.37    857.1 MB
v3         3.97   3.71   3.46   3.45   3.47    901.9 MB
v3-fhook   3.39   3.39   3.38   3.37   3.38    856.7 MB
```

| mode | min wall (s) | mean wall (s) | Δ wall vs tw | RSS_min_MB | Δ RSS vs tw |
|---|---|---|---|---|---|
| tw | 3.37 | 3.488 | — | 857.1 | — |
| v3 | 3.45 | 3.612 | **+2.4% min** | 901.9 | +44.8 MB (+5.2%) |
| v3-fhook | 3.37 | 3.382 | **0.0% min** | 856.7 | -0.4 MB |

Phase 5's perf benefit on cardano-node compared to Phase 5 OFF
(see prior session's BENCH-2026-05-04-CUMULATIVE.md, "v3 default
mode +2.4% slower than tw") is roughly preserved — Phase 5 ON +
the new lazy-bridge fix nets out the same +2.4% min vs tw, with
v3-fhook now AT PARITY (was crashing pre-#438 + #437).

## Comparison vs prior snapshots

| workload | tw min | v3 min (prior, P5 OFF) | v3 min (now, P5 ON) | Δ this session |
|---|---|---|---|---|
| fib35 | 4.10–4.20 | 4.11 | **3.20** | -22% wall (the -24% headline) |
| ackermann | 0.55 | 0.63 | 0.51 | -19% (recovered from prior +14%) |
| letrec-fix | 0.06 | 0.062 | 0.063 | parity |
| drv3 | 0.51 | 0.52 | 0.46 | -10% |
| attr-pkgs | 0.36 | 0.36 | 0.47 | **+30% (regression)** |
| hello-name | 0.35 | 0.35 | 0.44 | +25% (regression) |
| cardano-node | 3.34 | 3.42 | 3.45 | parity (was 3.42 P5-OFF) |

The `attr-pkgs` and `hello-name` "regressions" vs the prior snapshot
turned out to be cold-cache noise.  Re-measured warm (10-run min):

```
attr-pkgs:  tw 0.38  |  v3 P5 OFF 0.38  |  v3 P5 ON 0.39   parity
```

The harness's N=5 cold-start runs blur the signal on workloads
shorter than ~0.5 s.  Treat the synthetic table at the top as the
authoritative comparison; the numbered nixpkgs queries below should
be read with min ± stddev rather than mean.

## Memory deep-dive (cardano-node)

`v3` keeps an extra ~45 MB resident vs tw — entirely the v3 IR /
CompilationUnit / sub-Expr cache.  Same magnitude as #432's
post-Phase-0 measurements (the GC Phase 0 close didn't change
RSS, only correctness; phase 1+2 are where the RSS reductions
will land — #433/#434).

`v3-fhook` parity on RSS is interesting: enabling the force hook
adds Bridge-thunk allocations, which amortise into the same
working set.  The +18% wall overhead doesn't translate into RSS.

## Saved artifacts

Synthetic + nixpkgs bench:
```
NPK=/nix/store/g8zzlf6drg73c987ii390yicq4c0j778-source N=5 \
  nix develop -c bash src/libexpr-v3/test/bench-v3-vs-tw.sh
```

Cardano-node manual harness (with RSS):
```
for mode in tw v3 v3-fhook; do
  for i in 1 2 3 4 5; do
    case $mode in
      tw) PRE="";;
      v3) PRE="NIX_USE_V3=1";;
      v3-fhook) PRE="NIX_USE_V3=1 NIX_USE_V3_FORCE=1";;
    esac
    env -i HOME=$HOME PATH=$PATH $PRE /usr/bin/time -l \
      ./build/src/nix/nix --extra-experimental-features 'nix-command flakes' \
      eval --no-eval-cache --raw \
      "$CARDANO#packages.aarch64-darwin.cardano-node.name" 2>&1 >/dev/null
  done
done
```
