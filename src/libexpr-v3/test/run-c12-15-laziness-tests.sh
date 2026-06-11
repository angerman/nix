#!/usr/bin/env bash
# C-12 / C-13 / C-14 (CODEBASE_REVIEW_2026-06-11) regression driver —
# strictness-analysis / primop laziness divergences (v3 forced what TW didn't).
#
#   C-12  `with x` strictness: f = x: with x; 1 — TW never forces x, so
#         f (throw) = 1. v3's cross-fn strictness analyzer marked the with-attrs
#         forced → call sites de-thunked the arg → threw. (opt_func_strictness)
#   C-13  builtins.elem needle is lazy: elem (throw) [] = false in TW (empty
#         list never compares the needle); v3 eagerly pre-forced it → threw.
#   C-14  foldl' / groupBy do NOT force list ELEMENTS (TW's prim_foldlStrict
#         forces fn + spine only). foldl' (a: x: a) 0 [1 (throw)] = 0 in TW;
#         groupBy (x: "k") [1 (throw)] = { k = [...]; } with the element unforced.
#         (v3's deepForceList bit pre-forced elements → threw.)
#
# C-15 (resolveCalleeLambda uniqueness) has no compact .nix repro (it needs two
# same-block LetRecs sharing an entry name); it is exercised by the full lang +
# nixpkgs suites (over-forcing would surface as a spurious throw there).
#
# Usage: [V3=path] run-c12-15-laziness-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-8s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

check() {
  local label="$1" expr="$2" expected="$3" flags="${4:-}" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" $flags --expr "$expr" 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  else
    echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1))
  fi
}

# C-12 — `with x` does not force x; name resolution still works.
check "C-12 with-lazy-arg     " 'let f = x: with x; 1; in f (throw "boom")' '1'
check "C-12 with-still-resolves" 'let f = x: with x; a; in f { a = 5; }'     '5'

# C-13 — elem needle laziness.
check "C-13 elem-empty-lazy    " 'builtins.elem (throw "x") []'    'false'
check "C-13 elem-present       " 'builtins.elem 2 [1 2 3]'          'true'
check "C-13 elem-forces-on-cmp " 'builtins.tryEval (builtins.elem (throw "x") [1]) ' '{ success = false; value = false; }' --strict

# C-14 — foldl' / groupBy element laziness.
check "C-14 foldl-ignores-elem " 'builtins.foldl'"'"' (a: x: a) 0 [1 (throw "boom")]' '0'
check "C-14 foldl-uses-elem     " 'builtins.foldl'"'"' (a: x: a + x) 0 [1 2 3]'        '6'
check "C-14 groupBy-elem-lazy   " 'builtins.attrNames (builtins.groupBy (x: "k") [1 (throw "boom")])' '[ "k" ]'

echo "--- C-12/13/14 laziness: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
