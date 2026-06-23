# P0.1 (#140) — Change-1 strictness ceiling RCA — 2026-06-23

De-risk the keystone of ARCH_BEAT_TW_PROGRAM: before committing to the (hard,
byte-id-risky) speculative-strictness build, MEASURE how much thunk allocation it
could actually eliminate, and WHY the existing static pass doesn't.

## Instrument

`NIX_V3_STRICT_CEILING=1` (gated, retire at the #141 gate).  Added a runtime
`LambdaDescriptor::strictArgsMask` (mirror of `ir::Function::strictArgs`, populated
at emit; foundational for Change 1, not throwaway).  At each closure call site
(direct `OP_CALL` formal-0 + saturated `OP_CALL_N` formals 0..n-1) it counts:
- suspended-thunk args (a thunk wrap that COULD be eager-eval'd);
- of those, how many the callee is STATICALLY proven to force (`strictArgsMask`);
- of those, how many are ACTUALLY forced at runtime (side-set tracked to the
  Suspended→Blackhole force-entry) — the true speculative ceiling, independent of
  what the static analysis proves.  Run with a big nursery so moving-GC pointer
  reuse doesn't alias the side-set.

## Result (cache-off, darwin-class laptop; deterministic counters)

| workload | closure-call arg-slots | suspended-thunk args | static-strict | **runtime-forced** |
|---|---|---|---|---|
| firefox.drvPath | 349,230 | 172,008 (49.3%) | **142 (0.1%)** | **100,781 (58.6%)** |
| M5 cardano-node.name | 460,982 | 192,484 (41.8%) | **64 (0.03%)** | **134,310 (69.8%)** |

## Three findings

1. **The bottleneck is ANALYSIS PRECISION, not dynamic-dispatch reach.**  The
   static `strictArgs` pass proves strictness for **0.1% / 0.03%** of thunk-args,
   but **58.6% / 69.8%** are actually forced at runtime.  The static analysis
   captures **~0.1% of the realizable opportunity** (142 of 100,781; 64 of
   134,310).  So extending the existing static pass to dynamic callees (the
   original Change-1 hypothesis) is nearly empty — the analysis itself proves
   almost nothing (it requires "every path forces the formal before branching,"
   which real branchy code rarely satisfies).  **The right mechanism is RUNTIME
   SPECULATION** (observe the callee forces the arg → speculate + guard + deopt),
   NOT a better static proof.

2. **The opportunity is REAL but MODEST.**  100,781 (firefox) / 134,310 (M5)
   closure-arg thunks are forced at runtime and could be eager-eval'd.  On firefox
   that is ~3.5% of the 2.88M total thunks (direct-call path only — UNDERCOUNTS
   curried PAP completions, where `fun` is a `Tag::App` the instrument skips, and
   indirect/deep forcing).  So the call-site ceiling is ~3.5%→ maybe 5–10% with
   PAPs.

3. **Most thunks are lazy DATA, not call args — and strictness can't touch them.**
   Closure-call arg-slots (349K firefox) are TINY vs total thunks (2.88M) and
   opcodes (36.86M).  The thunk population is dominated by lazy attrset values /
   list elements / let-bindings (forced by their consumers, or — 65.7% — never
   forced at all in this eval).  Lazy data is exactly where eager eval is UNSAFE
   (forcing an unused attr → divergence + huge over-evaluation).  So Change-1's
   ceiling is structurally bounded by the call-arg + known-strict-operand fraction
   — it is NOT a path to eliminating the bulk of thunks.

## Implications for the program

- **Change 1 (strictness) is a modest CPU+RSS lever (~single-digit % of thunks),
  not a dominant one** — tempers the earlier "biggest shared lever" framing.  Its
  value is real (fewer allocs + forces on the hottest call args) but bounded.
- **Mechanism pivot:** #143 must be RUNTIME speculation (the static pass is a dead
  end at 0.1%).  #142 (strict-by-default lowering for if-cond / BinOp operands /
  select roots) targets a DIFFERENT, non-call subset and should be measured
  separately — it may be the larger, lower-risk half.
- **The bulk of the thunk population (lazy data + 65.7% unforced) is correctly
  lazy and out of strictness's reach** → the bigger CPU/RSS levers remain HAMT
  (Change 2, attacks the attrset data itself) and JIT (Change 4, attacks dispatch).

## #141 gate (refined from this data)

Prototype runtime speculation on one hot pattern (#141).  PRE-COMMITTED: proceed to
the full #143 build only if the prototype realizes a warm CPU and/or RSS
improvement ABOVE the darwin-4 noise floor on firefox (byte-id + deopt correct
under --brute).  Given the ~3.5–10% thunk ceiling, if the realized win is below
noise, RE-SCOPE: ship only #142 (strict-by-default lowering) if it measures up, and
shift program weight to Change 2 (HAMT) + Change 4 (JIT).

## Caveats / loose ends

- Instrument undercounts: direct-call formal-0 + saturated CALL_N only; curried PAP
  completions (`fun = Tag::App`) are skipped → the call-arg ceiling is a LOWER
  bound.  A complete bound would tag arg-thunks at creation (thunkifyForArg) and
  count forced-arg-thunks at the force site (captures all call paths).
- M5 here shows 460K closure-calls (substantial v3 work), inconsistent with the
  earlier `.name` thunk-body count of 2085 (lode/M4_THUNK_AVOIDANCE_RCA) — the
  2085 run was anomalous/measured a truncated path; this run (big nursery, 180 s
  wall) ran fuller.  Flagged; does not affect the ratio conclusion.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
