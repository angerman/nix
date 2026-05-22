# TW vs v3-direct primop parity property tests

This suite is the **semantic safety net** for the bytecode-emit-primops
architectural refactor (`memory/project_a12b_depth5000.md`).  As we
rewrite each C primop as a v3 bytecode template (Option A in the
A12b plan), this suite guarantees we don't drift from TW's exact
semantics.

## What it tests

For each primop class (`head`, `tail`, `map`, `filter`, `foldl'`,
`mapAttrs`, `attrValues`, `concatMap`, ...), the runner:

1. Generates N random Nix expressions using a seeded PRNG (default
   seed `20260517`, default 10 cases per primop).
2. Evaluates each expression on **both** the tree-walker (TW) and
   v3-direct (`NIX_V3_DIRECT_EVAL=1`; post-#760 the `SKIP_INSTALLABLE_PREEVAL` gate is unnecessary — v3-direct skips TW pre-eval unconditionally).
3. Asserts parity:
   - **Success case**: byte-exact stdout match (after stripping
     transient warnings like "Git tree dirty" / "search path entry
     missing" / "Failed to increase stack size").
   - **Error case**: both must throw, and the leading error class
     token (`error`, `infinite recursion`, `division by zero`,
     `index out of bounds`, `undefined variable`, `missing attribute`,
     etc.) must match.

## Primop categories covered

- **Pure data**: head, tail, length, elemAt, elem, attrNames,
  attrValues, hasAttr, getAttr (with default), catAttrs.
- **Higher-order**: map, filter, foldl', concatMap, genList, all, any.
- **Attrset construction**: mapAttrs, listToAttrs, removeAttrs,
  intersectAttrs.
- **Strings**: concatStringsSep, substring, stringLength, toString.
- **Arithmetic**: +, -, *, /.
- **Comparison**: <, ==-on-lists, ==-on-attrs.
- **Type predicates**: isInt, isString, isList, isAttrs, isBool,
  isNull.
- **Combinators**: ++ (list-concat), // (attrset-merge), + (string-
  concat), seq, deepSeq, tryEval (both success and throw paths).
- **Error-class parity**: div-by-zero, elemAt out-of-bounds, head
  of empty, missing attribute, type error, throw, assert-false.
- **Nesting / lazy**: list-of-lists, attrs-of-lists, let-rec
  fixpoint, with-attrs, assert-true, string interpolation,
  rec-self-ref, curried application, higher-order composition.

Currently **58 primop categories × default 10 cases = 580 tests** per run.

## Usage

```bash
# Smoke test (3 cases each, ~30s).
./run-property-tests.sh --cases 3

# Full default run (10 cases each, ~1-2 min).
./run-property-tests.sh

# Stress run (25 cases each, ~3-5 min).
./run-property-tests.sh --cases 25

# Focus on one primop.
./run-property-tests.sh --primop foldl_sum --cases 50 --verbose

# Different seed (for finding seed-specific bugs).
./run-property-tests.sh --seed 12345
```

Exit code is 0 if all tests pass, 1 if any fail, 2 on harness error.

## When to run

- **Before any commit that touches a primop implementation** —
  guards against silent semantic regressions.
- **As the first step of converting a primop to bytecode** — locks
  in baseline behavior before the refactor begins.
- **In CI** — catches regressions from refactors that pass lang-test
  parity but break on randomized inputs.

## Adding new test generators

Each generator is a function `gen(rng: random.Random) -> str` that
returns a Nix expression.  Register it in the `TESTS` list with a
short name.  See `property_tests.py` for examples.

Guidelines:
- Use the existing `gen_int`, `gen_string`, `gen_list_int`,
  `gen_attrs_int` helpers for input variation.
- Keep expressions small (< 200 chars) so failure dumps are readable.
- Cover the **boundary cases**: empty list, single element, large
  list, negative numbers, etc.
- For new primops, also add an **error-class** variant (`t_err_*`)
  that triggers the primop's error path.

## How this fits into the v3 architectural plan

Per `memory/project_a12b_depth5000.md`, the path to closing A12b
(depth=5000 SIGSEGV on real nixpkgs) is to convert hot primops from
C functions to v3 bytecode templates emitted by `lower.cc`.  Each
conversion lands as a separate Rule-0 commit:

1. The property suite stays green throughout (no semantic drift).
2. The cutover parity suite stays green (no lang-test regression).
3. The benchmark suite shows the impact (no perf regression > 5%).
4. A new POSITIVE test in `run-583-tag-app-cache-tests.sh` (or a
   dedicated A12b suite) locks in the bytecode-specific behavior
   (e.g., depth doesn't grow on a 1000-step Tag::App cascade).

The 41 primop categories here cover ~80% of the surface that's
likely to be touched by the refactor; new generators can be added
when specific primops are about to be converted.
