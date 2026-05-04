# Real-world benchmark — cardano-node + nixpkgs scan

**Date:** 2026-05-04
**System:** aarch64-darwin
**Build:** v3 at HEAD (this branch's full optimisation set landed)
**Comparand:** in-tree tree-walker (`build/src/nix/nix*`, no env vars)

## Setup

* `tw`        — tree-walker (default, no env)
* `v3`        — cutover hook only (`NIX_USE_V3=1`)
* `v3-fhook`  — cutover + force hook (`NIX_USE_V3=1 NIX_USE_V3_FORCE=1`)

5 runs each; means reported.  `/usr/bin/time -l` for wall + RSS + perf
counters.  `sample` for CPU profiles.  `heap` for mid-eval snapshots.

## Findings

### A.1 v3 prerequisite: cardano-node correctness regression found and fixed

While probing, hit `infinite recursion encountered` on the
`cardano-node` flake under `NIX_USE_V3=1`.  Tree-walker succeeded.
Bisected to commit `ed17d4606` ("EVAL-COMP §3.3: skip synthetic
LetRec for default-less formals").  Root cause: `OP_ATTRS_SELECT`
eagerly forces `Tag::App` entries via the Phase-13.3 mapAttrs memo
path (vm.cc:2440 / 2486).  The optimisation bound formals via a
direct `AttrSelect` IR node at function entry, which forced *all*
`Tag::App` formals -- including ones the body never references --
exposing eval-order cycles tree-walker resolves by only touching
demanded formals.

Reverted in `857227578`.  Re-enabling requires either an
`OP_ATTRS_SELECT_NO_FORCE` opcode for the formals path, or
deferring the AttrSelect to reference time.

### B.1 cardano-node — `packages.aarch64-darwin.cardano-node.name`

| evaluator | real    | user    | RSS      | instructions |
|-----------|---------|---------|----------|--------------|
| tw        | 2.69 s  | 2.32 s  | 654 MB   | 31.04 G      |
| v3        | 2.72 s  | 2.37 s  | 679 MB   | 31.55 G      |
| v3-fhook  | **2.66 s** | 2.33 s  | 654 MB   | 31.03 G      |

Δ vs tw: v3 +1.1% real / +2.2% user / +3.8% RSS.  v3-fhook
**-1.1% real / parity user/RSS / -0.0% instructions**.

### B.2 nixpkgs scan — count derivable top-level packages (~21k attrs)

Expression: `builtins.length (builtins.filter (n:
(builtins.tryEval (builtins.isAttrs pkgs.${n})).value)
(builtins.attrNames pkgs))`

| evaluator | real    | user    | RSS       | instructions |
|-----------|---------|---------|-----------|--------------|
| tw        | 4.91 s  | 5.16 s  | 1549 MB   | 74.86 G      |
| v3        | 4.89 s  | 5.71 s  | 1547 MB   | 78.77 G      |
| v3-fhook  | **4.80 s** | 5.16 s  | 1549 MB   | 74.35 G      |

v3-fhook: **-2.2% real / parity user/RSS / -0.7% instructions**.

### C.1 Why the wins are modest: hook-firing rate

`NIX_VM_STATS=1` reports:

* cardano-node:   `evalEntries=31  cacheHits=0   cacheMisses=24  forceEntries=0`
* nixpkgs scan:   `evalEntries=77  cacheHits=0   cacheMisses=61  forceEntries=0`

The cutover hook fires **31 / 77 times total** across these workloads
-- a tiny fraction of the actual eval traffic.  Most evaluation
happens inside C++ primops (`prim_listToAttrs`, `prim_map`,
`prim_attrNames`, `prim_foldlStrict`, `prim_derivationStrict`,
`prim_getAttr`, `prim_isAttrs`, `prim_filter`, `prim_tryEval`)
called from tree-walker's `EvalState::callFunction`, NOT from the
top-level `Expr::eval` virtual dispatch where v3 hooks.  v3's
PIC / formals / selector-thunk optimisations only matter when v3
is actually running, which is a small slice.

Sample profile leaf-frame counts on nixpkgs scan are within ~3% across
all three modes; the same set of primops dominates on each:

```
prim_getAttr          ~8200    (top)
prim_derivationStrict ~8400
prim_isAttrs          ~5600
prim_filter           ~4150
prim_length           ~3970
prim_tryEval          ~3350
prim_concatMap        ~2700
prim_listToAttrs      ~2300
prim_map              ~2050
prim_attrNames        ~1950
```

### C.2 Why v3-fhook helps

The force hook (`NIX_USE_V3_FORCE=1`) lets v3 own per-thunk-body
forces in addition to top-level evals.  This exercises more of the
v3 code path and avoids some allocation-heavy tree-walker thunk
evaluation.  The wins:

* cardano-node: 30 ms wall (2.66 vs 2.69)
* pkgs scan:   110 ms wall (4.80 vs 4.91)

Instruction count goes DOWN slightly (-0.7% on pkgs scan), suggesting
real work elimination, not parallelism.

The cutover-only mode (`NIX_USE_V3=1` alone) is at parity-or-slightly-
slower because the lower+compile cycle adds overhead for the few
top-level Exprs it owns, with negligible runtime offset.

## Implications

1. **The v3 dispatch loop is no longer the bottleneck** for
   nixpkgs-shaped workloads.  Optimising v3 further (PIC, selector
   thunks, etc.) yields diminishing returns until more of the eval
   actually flows through v3.  This is exactly the EVAL-COMP §4.1
   finding: "the bottleneck is record-shaped, not graph-shaped" --
   primop dispatch and attrset access dominate, both shared between
   evaluators.

2. **Bigger wins live in primop bodies**, not in the eval shell.
   Per-primop changes (e.g., `prim_getAttr`'s Bindings::get binary
   search, `prim_listToAttrs`'s allocation pattern) would benefit
   both evaluators -- worth measuring before further v3 work.

3. **`v3-fhook` is the recommended mode** for real-world use.
   Modest but real wins on both bench workloads, no regressions.

## Caveats

* macOS aarch64 only; Linux/x86 not measured.
* Single revision benchmarked; this is a snapshot, not a track of
  per-commit deltas.
* `cardano-node` flake is mostly haskell.nix glue; the dominant
  cost is import-graph traversal + flake-cache eval, not heavy
  package construction.
* Fully cold runs (cleared eval-cache) would surface different
  costs; not measured here.

## Saved artifacts

In `/tmp/v3-bench/`:
* `cardano-node.csv`     — cardano-node 5-run / 3-mode timing
* `cardano-3way.csv`     — same (newer parser)
* `pkgs-scan.csv`        — nixpkgs scan 5-run / 3-mode timing
* `pkgs.{tw,v3,v3-fhook}.sample` — `sample` CPU profiles (~5 s windows)
* `heap.{tw,v3,v3-fhook}.txt`    — `heap` snapshots mid-eval

These are not committed -- run the bench harness
(`src/libexpr-v3/test/bench-v3-vs-tw.sh`) to regenerate.
