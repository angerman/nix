# A12b bytecode-primop conversion — 2026-05-17 summary

This document closes the bytecode-primop conversion arc that began
with `memory/project_a12b_depth5000.md`'s plan to convert callback-
heavy primops to v3 bytecode, eliminating their C-recursive callback
dispatch (`callClosure` per iteration).

## What landed

**Infrastructure** (T0 + T0b):
  - `commit 0a751f669` — install pipeline scaffold (parse + lower +
    compile + bridge via `v3ToTreeWalkerPublic`).
  - `commit 5104a7270` — dispatch hook + cu-pointer-after-move fix.
    Three install paths (TW builtins / v3 PrimOp side-table / v3
    static vBuiltins).  OP_LIT_PRIMOP redirect, lower.cc skip-check,
    opt_primop_fuse skip-check.

**Converted to bytecode** (8 callback-heavy primops):

  | T  | Primop     | Commit       | Win                         |
  |----|------------|--------------|------------------------------|
  | T1 | foldl'     | ca7365c38    | -32% on 10k-elem sum         |
  | T2 | map        | 93c85c7d3    | lazy via genList             |
  | T3 | filter     | 61116989a    | iterative via foldl'         |
  | T4 | all        | 2fd57377d    | short-circuit tail-recursive |
  | T5 | any        | 2fd57377d    | short-circuit tail-recursive |
  | T6 | concatMap  | af8c661f8    | via foldl' + ++              |
  | T9 | partition  | ee3a94b94    | via foldl' + dual accumulator|
  | T10| groupBy    | 537e06460    | via foldl' + // merge        |

Each one's recursive `go` loop is rewritten by emit.cc's tail-call
peephole to OP_TAIL_CALL — O(1) vm.frames regardless of list size.
Each callback invocation dispatches via OP_CALL (iterative per
commit 7f5a392f4), NOT via the C-recursive `callClosure` that the
C primops used.

**Kept as C** (foundation lazy-builders + deferred):

  - **T7 genList**: foundation lazy builder (builds Tag::App entries
    in a ListVec — requires C-level allocation).  Other bytecode
    primops compose on top of it.
  - **T8 mapAttrs**: foundation lazy builder for attrsets.  Same
    rationale as genList.
  - **T11 sort**: complex (mergesort in Nix is substantial bytecode).
    Lower priority; revisit if profiling shows sort C-recursion.
  - **T12 zipAttrsWith**: complex (attr-name union); same.

**Reverted** (T13-T17): catAttrs, concatLists, listToAttrs,
removeAttrs, intersectAttrs.  These don't take user lambdas; their
C versions are O(N) while the bytecode `foldl' + ++` / `foldl' +
//` versions are O(N²) — measured +60.4% regression on
`attrset-build-1k`.  Non-callback primops are NOT A12b targets.

## Validation

  - **143/143** cutover-parity lang tests pass.
  - **1450/1450** property tests pass (25 cases × 58 primops).
  - **7/7** A12 cache + OP_CALL iter-force tests pass.
  - **Bench**: real -32% improvement on `fold-add-10k v3-direct`.
    No v3-direct regressions on non-callback workloads
    (post-T13-T17-revert).

## Disable gates

  - `NIX_V3_NO_BYTECODE_PRIMOPS=1` — global off-switch (skips the
    entire `installAllBytecodePrimops` call in run.cc).
  - `NIX_V3_NO_BC_FOLDL` / `_MAP` / `_FILTER` / `_ALL` / `_ANY` /
    `_CONCATMAP` / `_PARTITION` / `_GROUPBY` — per-primop bypass.
  - `V3_DBG_BYTECODE_PRIMOP=1` — trace each install.

## What did NOT close

This work targets the **callback C-recursion** part of A12b — the
inner `for (each elem) callClosure(op, elem)` loops that grew the
C-stack one frame per iteration.  Pre-existing A12b residuals NOT
addressed here:

  - `forceValue → dispatchLoop` recursion when forcing a thunk
    inside a primop body (and `callClosure → dispatchLoop` for
    closure dispatch from C).  These are STILL C-recursive; the
    bytecode primops shift the C-recursion site from "primop body"
    to "thunk-body force inside dispatchLoop", which is a smaller
    multiplier but still a depth cost.

  - hello.name on real nixpkgs: still SIGSEGVs at depth ~5000.  The
    eval chain has its own non-callback recursion sources
    (stdenv.mkDerivation's finalPackage / commonAttrs chain) that
    these conversions don't touch.

The closer for A12b is either (a) extend iterative-force to ALL
helper sites, or (b) bump main-thread stack via link-time flag.
Both remain open follow-ups; see `project_a12b_depth5000.md` for
the remaining specific call sites.
