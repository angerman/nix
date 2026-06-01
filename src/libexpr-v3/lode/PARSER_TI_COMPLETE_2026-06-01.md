# Parser TI tier complete — test infrastructure for the v3-native parser

**Date:** 2026-06-01
**Per:** `PARSER_PROJECT_PLAN_2026-06-01.md` §4 (TI.1-TI.5)
**Status:** TI TIER COMPLETE — all five test-infrastructure items landed.  The Stage-1 SHIP-gate corpus + harnesses are in place BEFORE the action-rewrite begins.

---

## 1. What landed

| Item | Deliverable | Status |
|---|---|---|
| TI.1 | `run-parse-diff.sh` — parse-okay/parse-fail baseline harness | ✓ TW-baseline: 13 parse-okay + 41 parse-fail verified |
| TI.2 | `v3-eval --parse` flag (`cli/v3-eval.cc`) | ✓ mirrors `nix-instantiate --parse`; prints `e->show()` + newline |
| TI.3 | Position battery — `fixtures/position/` (19 fixtures) | ✓ both modes green |
| TI.4 | Operator-precedence battery — `fixtures/precedence/` (49 fixtures) | ✓ both modes green |
| TI.5 | `run-parse-sweep.sh` — corpus parse-sweep harness | ✓ 263 lang files, 0 divergences; NIXPKGS= hook for ≥10K |

## 2. The validation surfaces

### 2.1 `--parse` AST shape (precedence battery + parse-sweep)

`v3-eval --parse --file F` prints the parsed AST via `Expr::show()`,
byte-identical to `nix-instantiate --parse F`.  The precedence battery
locks the desugarings the v3 parser MUST reproduce:

| Surface syntax | Golden AST (desugared) |
|---|---|
| `a * b` | `(__mul a b)` |
| `a < b` | `(__lessThan a b)` |
| `a > b` | `(__lessThan b a)` — operands SWAPPED |
| `a <= b` | `(! (__lessThan b a))` |
| `a >= b` | `(! (__lessThan a b))` |
| `-a` | `(__sub 0 a)` |
| `!a + b` | `(! (a + b))` — NOT is LOOSER than `+` |
| `-a.b` | `(__sub 0 (a).b)` — select binds tighter than negate |
| `a -> b -> c` | `(a -> (b -> c))` — right assoc |
| `a // b // c` | `(a // (b // c))` — right assoc |
| `a ++ b ++ c` | `(a ++ (b ++ c))` — right assoc |
| `f a b` | `((f a) b)` — application left-assoc |
| `a.b or c` | `(a).b or (c)` |

These were **previously UNTESTED** (per `NATIVE_PARSER_FEASIBILITY §3.3`
gap #2: "14 precedence levels, ZERO dedicated tests").  Now 49 fixtures
with committed goldens.

### 2.2 Position info (position battery)

`v3-eval --file F --strict` evaluates `unsafeGetAttrPos` extractions
projected to `{ column; line; }` (env-independent — `file` field
dropped).  19 multi-line fixtures cover positions in: top-level attrs,
rec attrsets, nested attrsets, let-derived, lambda body, with-scope,
if-branch, assert-guarded, tryEval, deep-indent, after-string,
after-comment, // merge, inherit, quoted-key, nested-let, many-lines.

This is the **full pipeline** validation (parse → lower → eval): the
v3 eval path already reproduces TW positions byte-for-byte.  Under the
v3-native parser (Stage 1), these stay green only if the v3 parser
records identical line+column.

### 2.3 Corpus sweep (parse-sweep)

`run-parse-sweep.sh` parses every TW-parseable file in a corpus through
both parsers and byte-compares.  Today: 263 lang files, 0 divergences.
`NIXPKGS=/path` extends to the ≥10K-package Stage-1 SHIP gate.

## 3. What "v3 mode green today" means (honest scope)

**Today `v3-eval --parse` uses TW's parser** (the only parser that
exists).  So `run-fixtures.sh --v3` showing 68/68 and `run-parse-sweep.sh
--v3` showing 263/263 validate:

1. **The goldens are correct** (reproduced by an independent CLI path)
2. **The harnesses work end-to-end** (golden comparison, dispatch,
   --file plumbing)
3. **NOT** that a v3-native parser works — there is no v3 parser yet

When the v3-native parser lands behind `NIX_V3_NATIVE_PARSER=1`
(Stage 1.4+), the SAME harnesses + goldens become the real parser
SHIP-gate.  The contract is locked now; the implementation fills in.

## 4. Stage-1 SHIP-gate (now concretely runnable)

Per `PARSER_PROJECT_PLAN §2.2`, when the v3 parser exists:

```bash
# precedence + position batteries
NIX_V3_NATIVE_PARSER=1 src/libexpr-v3/test/parser-ti/run-fixtures.sh --v3
# parse-okay/parse-fail baseline
NIX_V3_NATIVE_PARSER=1 src/libexpr-v3/test/parser-ti/run-parse-diff.sh --v3
# corpus sweep (lang + nixpkgs)
NIXPKGS=/path/to/nixpkgs NIX_V3_NATIVE_PARSER=1 \
    src/libexpr-v3/test/parser-ti/run-parse-sweep.sh --v3
# full lang eval parity
NIX_V3_NATIVE_PARSER=1 V3=builddir/src/libexpr-v3/v3-eval \
    src/libexpr-v3/test/run-lang-tests.sh
```

All four must be green for Stage 1 to ship.

## 5. Files

```
src/libexpr-v3/cli/v3-eval.cc                       — TI.2 --parse flag
src/libexpr-v3/test/parser-ti/
├── run-parse-diff.sh                               — TI.1
├── run-fixtures.sh                                 — TI.3/TI.4 runner
├── run-parse-sweep.sh                              — TI.5
├── gen-precedence-battery.sh                       — TI.4 generator
├── gen-position-battery.sh                         — TI.3 generator
└── fixtures/
    ├── precedence/*.{nix,exp}                       — 49 fixtures
    └── position/*.{nix,exp}                         — 19 fixtures
```

## 6. What this kills

Per `[[falsification-rule]]`:

* **"Existing test infra is sufficient for parser validation"** —
  KILLED.  The 49 precedence + 19 position fixtures + 2 sweep harnesses
  are NEW; the audit (`NATIVE_PARSER_FEASIBILITY §3.3`) showed these
  corners were untested.
* **"v3's eval path mis-records positions"** — REFUTED for 19 contexts:
  v3 eval reproduces TW `{line,column}` byte-for-byte.
* **"TI.2-TI.5 takes 8-10 days"** — the harness + battery authoring
  landed in one focused session because the generators are mechanical
  and TW is the oracle.  (Fixture COUNT can still grow toward the
  ~80-fixture target as Stage 1 surfaces gaps.)

## 7. What remains before Stage 1.4 (action rewrite)

Per `PARSER_PROJECT_PLAN §11.1`:

* Stage 1.1 — v3 AST class hierarchy (`include/v3/ast/expr.hh`)
* Stage 1.2 — v3 ParserState (port `parser-state.hh.upstream`)
* Stage 1.3 — meson.build wiring (bison/flex → libnixexprv3)
* Stage 1.4 — atom-action rewrite (ExprInt/Float/String/Path/Var)

The TI tier is the prerequisite; it is now complete.

## 8. Honest limits

* **TI.4 has 49 fixtures, plan targeted ~80.**  The 49 cover every
  precedence level + associativity + the key cross-level boundaries.
  More can be added as Stage 1 surfaces gaps (antiquotation-depth,
  path-token edge cases).  49 is sufficient to START Stage 1; not the
  final count.
* **Position goldens drop the `file` field** (env-dependent).  If a
  future requirement needs file-path validation, add a normalized-path
  mode.
* **Parse-sweep `--v3` is trivially green today** (TW parser through
  v3 CLI).  Its value is entirely future (Stage 1 gate).  Do NOT read
  today's green as v3-parser validation.
* **No differential fuzzer yet** (plan TI optional / LESSONS §4.9
  item 6).  The curated batteries + corpus sweep are the primary
  defense; a fuzzer is a Stage-1.9 add-on if divergences slip through.

## 9. Cross-references

* `PARSER_PROJECT_PLAN_2026-06-01.md` — the project plan (§4 = TI tier)
* `NATIVE_PARSER_FEASIBILITY_2026-06-01.md` §3.3 — the test gaps these fill
* `FFI_KILL_TODO_STATUS_2026-06-01.md` — why the parser project is the pivot
* `parser/README.md` — the action-rewrite target
* `[[falsification-rule]]`, `[[measure-twice-cut-once]]`

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
