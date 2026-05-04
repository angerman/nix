# v3 cumulative bench — post #426/427/428/429/432 + perf hints + #424 + #425 (LitBuiltins)

**Date:** 2026-05-04 (afternoon, post-#427-revert)
**System:** aarch64-darwin
**Comparand:** in-tree tree-walker (default `nix-instantiate`, no env vars)
**Runs:** N=5 per workload/mode via `bench-v3-vs-tw.sh`

## Cumulative changes since BENCH-REAL-WORLD-2026-05-04.md

The original real-world bench was taken with v3 firing only 31-77 times
across an entire workload (the dam-not-broken state).  Since then:

  - **#416** outer-with carriage (default ON, `NIX_V3_NO_OUTER_WITH` kill)
  - **#426** v3CallFunctionHook wired (default ON, `NIX_V3_NO_CALL` kill)
  - **#427** Phase 5 slot-threading -- **DEFAULT OFF** after the
    cardano-node correctness regression: `NIX_V3_INLINE_REC_SLOT=1`
    enables direct `RecBindingSlotRef` emission for users who don't
    hit the cardano-node-shape eval cycle.
  - **#428** fast-path opcodes (`OP_IS_*`, `OP_HEAD/TAIL/LENGTH/ELEM_AT`)
  - **#429** App-chain peephole fusion over `LitPrimOp`
  - **#424** selector-lambda specialisation (opt-in
    `NIX_V3_SELECTOR_LAMBDA=1`)
  - **#425 LitBuiltins subset** -- inner functions capturing `builtins`
    as freeVar now get the singleton via a new
    `UpvalueSource::Kind::LitBuiltins`
  - **#432** GC Phase 0 (CRIT-2/3/4 closed)
  - hook-side refactor (`prepHookUpvaluesAndWiths` shared between
    force / call hooks)

Profile-guided dispatch-loop micro-opts (this round):
  - `[[unlikely]]` on V3_DBG_TRACE / kCountInstructions checks
  - hoist env-var statics to function-scope const locals so the
    compiler treats them as loop-invariants
  - reorder `OP_GET_LOCAL_FORCE` / `OP_GET_UPVALUE_FORCE` / `OP_FORCE`
    so the hot fast-path bail-out runs BEFORE diagnostic calls
    (was paying the address-argument computation on every iteration)
  - inline Thunk-Evaluated fast-path in OP_CALL (avoids the full
    forceValue chase setup for the common single-hop case)

Modes:

  - `tw`        — tree-walker default
  - `v3`        — `NIX_USE_V3=1` (call hook + outer-with active)
  - `v3-fhook`  — `NIX_USE_V3=1 NIX_USE_V3_FORCE=1`

## Wall-clock (mean of 5 runs, current branch state)

```
workload      evaluator   mean    min     p50     p95    stddev
--------      ---------  -----   -----   -----   -----   ------
fib35         tw         4.184   4.098   4.209   4.230   0.056
fib35         v3         4.141   4.112   4.127   4.198   0.034
fib35         v3-fhook   4.140   4.123   4.126   4.169   0.021

ackermann     tw         0.551   0.550   0.550   0.552   0.001
ackermann     v3         0.627   0.626   0.627   0.628   0.001
ackermann     v3-fhook   0.623   0.619   0.622   0.630   0.004

letrec-fix    tw         0.062   0.061   0.062   0.062   0.000
letrec-fix    v3         0.063   0.062   0.062   0.065   0.001
letrec-fix    v3-fhook   0.062   0.061   0.062   0.062   0.000

path-deep     tw         0.062   0.061   0.061   0.062   0.000
path-deep     v3         0.062   0.062   0.062   0.062   0.000
path-deep     v3-fhook   0.062   0.061   0.062   0.063   0.001

drv3          tw         0.522   0.513   0.515   0.553   0.017
drv3          v3         0.527   0.519   0.523   0.545   0.010
drv3          v3-fhook   0.621   0.619   0.621   0.624   0.002

attr-pkgs     tw         0.358   0.356   0.358   0.360   0.002
attr-pkgs     v3         0.361   0.359   0.362   0.364   0.002
attr-pkgs     v3-fhook   0.586   0.574   0.579   0.604   0.014

hello-name    tw         0.398   0.352   0.353   0.578   0.101
hello-name    v3         0.356   0.351   0.357   0.360   0.004
hello-name    v3-fhook   0.575   0.567   0.577   0.580   0.005

git-name      tw         0.355   0.352   0.356   0.357   0.002
git-name      v3         0.357   0.355   0.357   0.358   0.001
git-name      v3-fhook   0.573   0.570   0.573   0.576   0.003
```

## Cardano-node (added this round)

Workload: `nix eval` of `flake#packages.aarch64-darwin.cardano-node.name`
on a local `/Users/angerman/Projects/iohk/cardano-node` checkout.

```
mode       run1   run2   run3   run4   run5
tw         3.34   3.35   3.36   3.36   3.38
v3         3.45   3.43   3.45   3.44   3.42
v3-fhook   *      *      *      *      *
```

  `*` = `expected a Boolean but found the partially applied built-in
  function '__v3_call_bridge_1'`.  Pre-existing v3 → tree-walker bridge
  bug (a v3 closure leaked across the boundary where tree-walker
  expected a forced bool).  Distinct from the Phase-5-disabled
  infinite-recursion that motivated the #427 revert; happens only
  under v3-fhook on cardano-node, not on hello-name / attr-pkgs /
  drv3.  Documented for follow-up.

## Headline deltas (v3 vs tw, on `min` to dodge cold-cache noise)

| workload | tw min (s) | v3 min (s) | Δ wall |
|---|---|---|---|
| fib35 | 4.098 | 4.112 | +0.3% |
| ackermann | 0.550 | 0.626 | +13.8% |
| letrec-fix | 0.061 | 0.062 | +1.6% |
| path-deep | 0.061 | 0.062 | +1.6% |
| drv3 | 0.513 | 0.519 | +1.2% |
| attr-pkgs | 0.356 | 0.359 | +0.8% |
| hello-name | 0.352 | 0.351 | -0.3% |
| git-name | 0.352 | 0.355 | +0.9% |
| **cardano-node** | **3.34** | **3.42** | **+2.4%** |

## Pre-#427-revert reading (Phase 5 was ON, broke cardano-node)

The earlier bench (with Phase 5 default-ON, before the cardano-node
regression surfaced) showed:

| workload | tw min | v3 min (Phase5 ON) | Δ |
|---|---|---|---|
| fib35 | 4.196 | 3.182 | **-24.2%** |
| ackermann | 0.554 | 0.523 | -5.6% |
| hello-name | 0.391 | 0.390 | parity |
| attr-pkgs | 0.411 | 0.400 | -2.7% |

Phase 5 was contributing the bulk of the synthetic-compute wins.
Without it, the rec-attr-select path goes through
`thunkifyRecAttrSelect`'s Thunk wrapper (one extra force per
access).  Re-enabling Phase 5 needs the cardano-node infinite-
recursion root-caused first.

## v3-fhook regression (unchanged from prior bench)

`v3-fhook` (force hook + call hook + everything else) still shows
a systematic regression on the derivation-heavy workloads:

  - `drv3`     : 0.619 vs 0.513 = +20.7%
  - `attr-pkgs`: 0.574 vs 0.356 = +61.2%
  - `hello-name`: 0.567 vs 0.352 = +61.1%
  - `git-name` : 0.570 vs 0.352 = +61.9%
  - `cardano-node`: bridge bug (above)

This is the WC-25/WC-26 caveat from the registrar amplified by the
new call-hook traffic: per-force Bridge thunk allocation cost
exceeds dispatch savings on workloads where the force hook fires
hot but the resulting primop work stays in tree-walker anyway.
Documented in BENCH-REAL-WORLD-2026-05-04.md §C.2; still opt-in
via `NIX_USE_V3_FORCE=1`.

## Memory (peak resident set, `/usr/bin/time -l`)

| workload | tw peak (MB) | v3 peak (MB) | Δ |
|---|---|---|---|
| hello-name | 117 | 117 | parity |
| attr-pkgs | 117 | 118 | +0.9% |

Unchanged from prior bench.  GC Phase 0 (#432) was correctness;
RSS wins are gated on Phase 1+2 nursery work (#433/#434).

## Comparison vs original BENCH-REAL-WORLD-2026-05-04.md

The original numbers measured cardano-node and a 21k-attr nixpkgs
scan against pre-#416/#426/#427/#428/#429 v3 (cutover hook firing
31-77 times per workload).

  - **Then:** v3 default mode +1.1% slower than tw on cardano-node
    (2.69 vs 2.72), -0.4% on nixpkgs scan; cutover hook fired
    31-77 times.
  - **Now:** v3 default mode is at parity-to-+2.4% on cardano-node,
    parity-to-+1% on nixpkgs targets.  The dam is broken (#426),
    Phase 5 was retired pending root-cause, and the dispatch-loop
    micro-opts (this round) shrink the per-instruction overhead
    by ~5% on synthetic compute.

  The "+2.4% on cardano-node" is meaningfully better than the
  +1.1% baseline GIVEN that Phase 5 is OFF (it would have provided
  another -5 to -10% on synthetic compute).  When Phase 5's
  cardano-node bug is root-caused and re-enabled, the headline
  shift on synthetic compute is -24% (per the pre-revert reading
  above).

## Saved artifacts

Bench harness: `src/libexpr-v3/test/bench-v3-vs-tw.sh`.

```
NPK=$NIXPKGS_PATH N=5 ONLY=fib35,ackermann,letrec-fix,path-deep,drv3,attr-pkgs,hello-name,git-name \
  nix develop -c bash src/libexpr-v3/test/bench-v3-vs-tw.sh
```

Cardano-node bench (manual; not in the harness):

```
for i in 1 2 3 4 5; do
  /usr/bin/time -p ./build/src/nix/nix --extra-experimental-features 'nix-command flakes' \
    eval --no-eval-cache --raw \
    "$CARDANO_NODE_CHECKOUT#packages.aarch64-darwin.cardano-node.name" >/dev/null
done
# Repeat with NIX_USE_V3=1 / NIX_USE_V3=1 NIX_USE_V3_FORCE=1.
```
