# IR-CHECK fixtures

LLVM-FileCheck-compatible (subset) fixtures for the v3 IR optimizer
pipeline.  Each `*.nix` file in this directory is BOTH:

1. A valid Nix expression — `# CHECK:` lines are line-comments to the
   Nix parser, so `v3-eval --emit-ir %s` can lower and dump the IR.
2. A directive source — `# CHECK:` etc. lines tell `v3-check` what
   the dumped IR should look like.

The fixtures run under `test/run-ir-checks.sh` (also as the
`v3-ir-checks` meson test).

## Anatomy of a fixture

```nix
# RUN: v3-eval --expr '<expr>' --emit-ir | v3-check %s
# RUN: v3-eval --expr '<expr>' --emit-ir-raw | v3-check %s --check-prefix=RAW
#
# Free-form documentation explaining what the fixture asserts and why.

<the-nix-expression>   # OR a `# RUN:` that uses %s (file-mode)

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitInt 42
# CHECK-NOT: Lambda
```

The runner:
1. Walks every `*.nix` in this directory.
2. Extracts `# RUN:` lines (one shell command each).
3. Substitutes `%s` → fixture path, `%t` → per-test temp file.
4. Executes each RUN as a shell command.  Fixture PASSes iff every
   RUN exits 0.

## RUN line forms

- **Expression mode** (no fixture body needed):
  ```
  # RUN: v3-eval --expr 'EXPR' --emit-ir | v3-check %s
  ```
- **File mode** (Nix expression IS the fixture body):
  ```
  # RUN: v3-eval --file %s --emit-ir | v3-check %s
  ```
- **Multi-RUN with prefixes** (compare same source under different
  modes):
  ```
  # RUN: v3-eval --file %s --emit-ir-raw | v3-check %s --check-prefix=RAW
  # RUN: v3-eval --file %s --emit-ir     | v3-check %s --check-prefix=OPT
  ```

## Supported directives

| Form | Meaning |
|------|---------|
| `# CHECK: pat` | Forward search; advance cursor past match |
| `# CHECK-NOT: pat` | Forbid `pat` between cursor and next positive match |
| `# CHECK-LABEL: pat` | Strong anchor; resets cursor.  Subsequent CHECK-NOT cannot scan back past it |
| `# CHECK-NEXT: pat` | Must match the line **immediately after** the prior positive directive |

Comment-prefix character: `#` (Nix), `;` (LLVM `.ll`), or `//`
(C-style) all accepted.

`# RUN:` and `# COM:` lines are NOT parsed as CHECK directives — lets
RUN: shell commands containing `v3-check` coexist with CHECK lines.

## Pattern syntax

- **Default**: substring match.  `LitInt 42` matches any line
  containing `LitInt 42`.
- **`{{regex}}`**: embedded regex.  Literal text outside `{{...}}` is
  escaped; text inside is taken as a regex fragment.  Example:
  `v{{[0-9]+}} = LitInt {{[0-9]+}}` matches any `v<digits> = LitInt
  <digits>`.

## --check-prefix

A custom prefix replaces "CHECK" in directive names.  With
`--check-prefix=RAW`, the parser looks for `RAW:`, `RAW-NOT:`,
`RAW-LABEL:`, `RAW-NEXT:` instead of the default `CHECK*` set.  Used
in multi-RUN fixtures to assert different shapes per RUN.

## Authoring guide

1. **Pick the optimization phase you want to test** (e.g. Phase B
   constantFold).
2. **Find a minimal Nix expression** that exercises the phase.  Smaller
   = less brittle.  E.g. `2 * 3` for Mul folding, not `let a = ...;
   in 2 * 3 + a`.
3. **Run `v3-eval --file %s --emit-ir`** by hand (substituting `%s`
   with your fixture's path) to see the actual post-opt IR.  Do NOT
   use `--expr 'EXPR'` for fixtures: the `--file %s` form is the
   canonical convention (see `src/libexpr-v3/CLAUDE.md`).
4. **Write the CHECK directives** asserting the SPECIFIC thing the
   phase does:
   - `CHECK:` — what should appear (the desired post-opt shape).
   - `CHECK-NOT:` — what should be eliminated (the pre-opt shape).
5. **Add LABEL anchors** at block boundaries so CHECK-NOT can't drift
   across unrelated blocks.
6. **Use `{{regex}}`** for VarIds, FuncIds, and other shape-stable but
   numerically-unstable identifiers.
7. **Avoid asserting incidental scaffolding** — don't pin block
   structure unless that's specifically what the pass guarantees.

## When fixtures fail

`v3-check` emits a diagnostic showing the failing directive line, the
pattern, and the surrounding context.  Typical failure modes:

- **"expected to find: 'X'"** — the pattern didn't appear.  Run the
  RUN command by hand to see what the actual IR looks like; the
  optimizer might have produced a different shape than expected.
- **"forbidden pattern: 'X' found at line N"** — CHECK-NOT caught the
  pattern.  Either the pass regressed, or the fixture is too strict
  (e.g. forbids something that's legitimately present).

## Canonical RUN: line convention

All fixtures use the **`--file %s`** form (per the plan's §501-525
canonical convention):

```
# RUN: v3-eval --file %s --emit-ir | v3-check %s
```

The Nix expression body of the fixture is the test source.  `%s` is
substituted by the runner with the fixture's own path, so `v3-eval`
parses the fixture's Nix expression directly and `v3-check` reads
the same file for `# CHECK:` directives.  This mirrors LLVM's
`.ll`-file pattern exactly: one self-contained file, one logical
test scenario, one source body, optionally multiple RUN: lines
under different compiler modes (with `--check-prefix=`).

For different SOURCES (different Nix expressions), use SEPARATE
fixture files — that's how the `ifFold-true-pos.nix` /
`ifFold-false-pos.nix` / `appSpineFold-n2-pos.nix` /
`appSpineFold-n3-pos.nix` etc. families are organized.

## Files

Phase A — beta-reduce:
- `betaReduce-composition-pos.nix` — `(x: x*2) 21 → LitInt 42` (multi-RUN RAW/OPT).
- `betaReduce-nested-lambda-neg.nix` — refuses `(x: y: x + y) 5` (nested Lambda body).

Phase B — const-fold + primop-fold:
- `constantFold-arith-pos.nix` — `2 * 3 → LitInt 6`.
- `primOpFold-length-pos.nix` — `length [1..5] → LitInt 5`.

Phase C — stream fusion:
- `streamFusion-foldlMap-pos.nix` — `foldl' op nul (map f xs) → __foldlMap` App-chain.

Phase D — lambda lift precondition:
- `lambdaLift-capture-free-pos.nix` — capture-free lambda has no freeVars.
- `lambdaLift-capturing-neg.nix` — capturing lambda has freeVars + nUp=1.

Phase E — selector recognition:
- `selectorLambda-recognition-pos.nix` — `(p: p.name)` IR shape preserved through opt.

Phase F — App-spine fold:
- `appSpineFold-n2-pos.nix` — `(x: y: x*y) 6 7 → LitInt 42`.
- `appSpineFold-n3-pos.nix` — `(x: y: z: x*y*z) 2 3 4 → LitInt 24`.
- `appSpineFold-n4-pos.nix` — 4-arg curry (ConcatStrings due to `+` ambiguity).
- `appSpineFold-impure-arg-neg.nix` — refuses impure args.

Phase G — if-fold:
- `ifFold-true-pos.nix` — `if true then 100 else 200 → LitInt 100`.
- `ifFold-false-pos.nix` — `if false then 100 else 200 → LitInt 200`.
- `ifFold-with-less-cond-pos.nix` — `if 1<2 ...` (Phase B + G composition).
- `ifFold-with-eq-cond-pos.nix` — `if (1==2) ...` (Phase B + G composition).

Phase H — genList unroll:
- `genListUnroll-n4-pos.nix` — N=4 unrolls to 4 MkThunks + ListExpr.
- `genListUnroll-n1-pos.nix` — N=1 boundary case still unrolls.
- `genListUnroll-n16-neg.nix` — N=16 above kMaxUnrollN=8; PrimOpCall preserved.

Cleanup composition:
- `elimRedundantForce-inline-pos.nix` — inlineTrivialBindings + elimRedundantForce.

## Adding a new fixture

For every IR phase E-H landing, add ≥1 positive fixture asserting the
phase's exit criterion (see lode/IR_OPTIMIZATION_PLAN_2026-05-18.md
§2.5 for per-phase exit criteria).  Per the falsification rule (Rule
0), the fixture is the falsifiable claim: if the pass stops firing,
the fixture must FAIL.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.
