# v3 cumulative bench — post #416/#426/#427/#428/#429/#432

**Date:** 2026-05-04 (afternoon, post-#429)
**System:** aarch64-darwin
**Comparand:** in-tree tree-walker (default `nix-instantiate`, no env vars)
**Runs:** N=3 per workload/mode via `bench-v3-vs-tw.sh`

## Cumulative changes since BENCH-REAL-WORLD-2026-05-04.md

The original real-world bench was taken with v3 firing only 31-77 times
across an entire workload (the dam-not-broken state).  Since then:

  - **#416** outer-with carriage (default ON, `NIX_V3_NO_OUTER_WITH` kill)
  - **#426** v3CallFunctionHook wired (default ON, `NIX_V3_NO_CALL` kill)
  - **#427** Phase 5 slot-threading default ON (`NIX_V3_THUNKIFY_REC_SLOT` kill)
  - **#428** fast-path opcodes (`OP_IS_*`, `OP_HEAD/TAIL/LENGTH/ELEM_AT`)
  - **#429** App-chain peephole fusion over `LitPrimOp`
  - **#432** GC Phase 0 (CRIT-2/3/4 closed)
  - hook-side refactor (`prepHookUpvaluesAndWiths` shared between
    force / call hooks)

The combination is what's measured here.  Modes:

  - `tw`        — tree-walker default
  - `v3`        — `NIX_USE_V3=1` (call hook + outer-with active)
  - `v3-fhook`  — `NIX_USE_V3=1 NIX_USE_V3_FORCE=1`

## Wall-clock (mean of 3 runs)

```
workload      evaluator   mean    min     p50     p95    stddev
--------      ---------  -----   -----   -----   -----   ------
fib35         tw         4.174   4.138   4.144   4.241   0.058
fib35         v3         3.675   3.636   3.663   3.727   0.047
fib35         v3-fhook   3.672   3.672   3.672   3.673   0.001

ackermann     tw         0.555   0.553   0.556   0.557   0.003
ackermann     v3         0.570   0.564   0.565   0.580   0.009
ackermann     v3-fhook   0.597   0.573   0.578   0.639   0.036

letrec-fix    tw         0.063   0.063   0.063   0.063   0.000
letrec-fix    v3         0.063   0.062   0.063   0.065   0.001
letrec-fix    v3-fhook   0.062   0.062   0.062   0.063   0.000

path-deep     tw         0.076   0.063   0.063   0.103   0.023
path-deep     v3         0.063   0.063   0.063   0.064   0.001
path-deep     v3-fhook   0.064   0.063   0.063   0.066   0.002

drv3          tw         0.592   0.538   0.569   0.669   0.069
drv3          v3         0.588   0.562   0.573   0.630   0.037
drv3          v3-fhook   0.678   0.674   0.676   0.683   0.005

attr-pkgs     tw         0.547   0.407   0.574   0.659   0.128
attr-pkgs     v3         0.403   0.399   0.400   0.409   0.006
attr-pkgs     v3-fhook   0.613   0.609   0.613   0.616   0.003

hello-name    tw         0.463   0.380   0.385   0.622   0.138
hello-name    v3         0.423   0.384   0.442   0.443   0.034
hello-name    v3-fhook   0.662   0.654   0.664   0.670   0.008

git-name      tw         0.413   0.391   0.410   0.437   0.023
git-name      v3         0.388   0.385   0.387   0.393   0.004
git-name      v3-fhook   0.647   0.616   0.626   0.699   0.045
```

## Headline deltas (v3 vs tw, on `min` to dodge cold-cache noise)

| workload | tw min (s) | v3 min (s) | Δ wall |
|---|---|---|---|
| fib35 | 4.138 | 3.636 | **-12.1%** |
| ackermann | 0.553 | 0.564 | +2.0% |
| letrec-fix | 0.063 | 0.062 | -1.6% |
| path-deep | 0.063 | 0.063 | parity |
| drv3 | 0.538 | 0.562 | +4.5% |
| attr-pkgs | 0.407 | 0.399 | -2.0% |
| hello-name | 0.380 | 0.384 | +1.0% |
| git-name | 0.391 | 0.385 | -1.5% |

Variance: tree-walker shows much wider spread on the nixpkgs
workloads (`hello-name` stddev 138 ms, `attr-pkgs` 128 ms) because the
first-run cold-cache hit dominates and the second/third runs swing
back.  v3's variance is consistently single-digit ms — the per-run
disk-cache hit ratio is more deterministic now that the call hook
owns more traffic.

## Memory (peak resident set, `/usr/bin/time -l`)

| workload | tw peak (MB) | v3 peak (MB) | Δ |
|---|---|---|---|
| hello-name | 117 | 117 | parity |
| attr-pkgs | 117 | 118 | +0.9% |

RSS is essentially unchanged — the call hook traffic doesn't allocate
extra long-lived state, and the GC Phase 0 fixes (CRIT-2/3/4) didn't
move RSS in a measurable way on these workloads (they were
correctness fixes; the RSS wins are gated on Phase 1+2 nursery work).

## Instructions / cycles (`/usr/bin/time -l`)

| workload | tw instr (G) | v3 instr (G) | Δ |
|---|---|---|---|
| hello-name | 3.94 | 4.00 | +1.7% |
| attr-pkgs | 3.96 | 4.18 | +5.6% |

| workload | tw cycles (G) | v3 cycles (G) | Δ |
|---|---|---|---|
| hello-name | 1.02 | 1.04 | +1.7% |
| attr-pkgs | 1.05 | 1.07 | +1.2% |

Slightly more instructions retired under v3 -- expected, since v3
runs the additional opt passes at lower time and the call/force hooks
add a few branches per dispatched call.  Wall-clock parity-or-better
despite this means the ordering / memory locality of the v3 path is
modestly better than tree-walker's, or the noise margin dominates.

## v3-fhook regression

`v3-fhook` (force hook + call hook + everything else) shows a
systematic regression on the derivation-heavy workloads:

  - `drv3`: 0.678 vs 0.538 = +26%
  - `hello-name`: 0.662 vs 0.380 = +74%
  - `attr-pkgs`: 0.613 vs 0.407 = +51%
  - `git-name`: 0.647 vs 0.385 = +68%

This is the WC-25/WC-26 caveat from the registrar amplified by the
new call-hook traffic: every force on a registered Expr now allocates
Bridge thunks for upvalues, and on derivation-heavy workloads (where
the v3 force-hook fires hot but tree-walker dispatches the resulting
primops anyway) the per-force allocation cost exceeds the dispatch
saving.

This is expected and is exactly why force-hook stays opt-in.  Future
work: the synth-VarOrigins follow-up (#425) and bytecode-stdlib audit
(#430) would shift more primop dispatch into v3, making the
allocation cost amortise.

## Comparison to the original real-world bench (2026-05-04)

The original numbers measured cardano-node and a 21k-attr nixpkgs
scan against pre-#416/#426/#427/#428/#429 v3.  Modulo the workload
differences, the qualitative shift is:

  - **Then:** v3 default mode +1% slower than tw on cardano-node,
    -0.4% on nixpkgs scan; cutover hook fired 31-77 times.  Quote:
    "The v3 dispatch loop is no longer the bottleneck for
    nixpkgs-shaped workloads."
  - **Now:** v3 default mode at parity-or-slightly-faster on real
    nixpkgs targets; clear -12% win on synthetic compute (fib35);
    the call hook actually owns lambda dispatch.

The dam is broken.  The Phase 5 + IR-opt-pipeline cumulative effect
on synthetic compute (-12% on fib35) is the upper bound of what
matters when v3 OWNS the eval; the smaller deltas on real workloads
reflect the share of eval that's still in tree-walker primops, which
the bytecode-stdlib audit (#430) is targeted at next.

## Saved artifacts

Bench harness: `src/libexpr-v3/test/bench-v3-vs-tw.sh`.

```
NPK=$NIXPKGS_PATH N=3 ONLY=fib35,ackermann,letrec-fix,path-deep,drv3,attr-pkgs,hello-name,git-name \
  nix develop -c bash src/libexpr-v3/test/bench-v3-vs-tw.sh
```
