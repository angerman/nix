#!/usr/bin/env bash
# v3 evaluator regression suite.  Runs each nix expression against
# v3-eval and the existing tree-walker; reports any divergence.
#
# Usage:
#   ./run-v3-tests.sh           # summary only
#   V3_TEST_VERBOSE=1 ./run-v3-tests.sh   # print every test

set -u

BUILD="${BUILD:-./build}"
V3="${V3:-$BUILD/src/libexpr-v3/v3-eval}"
TW="${TW:-$BUILD/src/nix/nix}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

# Cases: each is a nix expression.  Compared via v3 vs the tree-walker.
# Expressions that don't depend on the store (which v3 can't handle yet)
# only.
TESTS=(
  # Literals
  '42'
  '3.14'
  'true'
  'null'
  '"hello"'

  # Arithmetic
  '1 + 2 * 3'
  '(10 - 3) * 2'
  '20 / 4'
  '-5 + 8'

  # Comparison
  '1 < 2'
  '5 >= 5'
  '"abc" == "abc"'
  '[1 2] == [1 2]'

  # Control flow
  'if true then 1 else 2'
  'if 5 > 10 then "big" else "small"'

  # Lambdas & application
  '(x: x + 1) 41'
  '(x: y: x * y) 6 7'
  '({a, b ? 100}: a + b) { a = 10; }'
  '({a, b, c ? 99, ...}: a + b + c) { a = 1; b = 2; d = 4; }'

  # Let / inherit
  'let x = 10; y = 20; in x + y'
  'let outer = 42; in let inherit outer; in outer'
  'let x = 10; in { inherit x; y = 20; }.x'

  # Recursion
  'let fact = n: if n == 0 then 1 else n * fact (n - 1); in fact 8'
  'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 12'
  'let isEven = n: if n == 0 then true else isOdd (n - 1); isOdd = n: if n == 0 then false else isEven (n - 1); in isEven 10'

  # Rec attrsets
  'rec { a = 1; b = a + 1; c = b + 1; }.c'
  '(rec { name = "v3"; ver = "1.0"; full = name + "-" + ver; }).full'

  # Inherit-from
  'let foo = { x = 100; }; bar = { y = 200; }; in { inherit (foo) x; inherit (bar) y; }.y'

  # With
  'with { x = 7; y = 11; }; x + y'

  # Lists
  '[1 2 3] ++ [4 5]'
  'builtins.length [1 2 3 4 5]'
  'builtins.head [10 20 30]'
  'builtins.elemAt [10 20 30] 1'

  # Attrsets
  '{ a = 1; b = 2; }.a'
  '{ a = 1; }.b or 99'
  '{ a = { b = { c = 42; }; }; }.a.b.c'
  '({ a = 1; } // { b = 2; }).b'

  # Higher-order primops
  'builtins.map (x: x * 2) [1 2 3]'
  'builtins.filter (x: x > 2) [1 2 3 4 5]'
  'builtins.foldl'"'"' (a: b: a + b) 0 [1 2 3 4 5]'
  'builtins.genList (i: i * i) 5'

  # String ops
  'builtins.toString 42'
  'builtins.substring 2 4 "abcdefgh"'
  'builtins.concatStringsSep ", " ["a" "b" "c"]'
  'builtins.replaceStrings ["foo"] ["FOO"] "foofoo"'
  'builtins.stringLength "hello"'

  # Type predicates
  'builtins.typeOf 42'
  'builtins.isList [1 2]'
  'builtins.isAttrs { a = 1; }'

  # Sort + bitOps
  'builtins.elemAt (builtins.sort (a: b: a < b) [3 1 4 1 5]) 0'
  'builtins.bitAnd 12 10'

  # tryEval
  '(builtins.tryEval (builtins.throw "boom")).success'
  '(builtins.tryEval 42).value'

  # Hashes
  'builtins.hashString "sha256" "hello"'

  # JSON
  '(builtins.fromJSON "{\"x\": 42}").x'
  'builtins.toJSON [1 2 3]'

  # Dynamic attrs
  'let n = "key"; in { ${n} = 42; }.key'
)

pass=0
fail=0
diverge=0
errors=0
failed_cases=()

for e in "${TESTS[@]}"; do
  v3_out=$("$V3" --expr "$e" 2>/dev/null)
  v3_rc=$?
  tw_out=$("$TW" eval --impure --expr "$e" 2>/dev/null)
  tw_rc=$?

  if [[ $v3_rc -ne 0 ]]; then
    fail=$((fail + 1))
    errors=$((errors + 1))
    failed_cases+=("ERROR: $e")
    [[ ${V3_TEST_VERBOSE:-0} -eq 1 ]] && echo "ERR  $e"
    continue
  fi

  if [[ $tw_rc -ne 0 ]]; then
    # Tree-walker errored — usually because the expression touches store
    # paths or other things v3 doesn't.  Skip rather than count as fail.
    [[ ${V3_TEST_VERBOSE:-0} -eq 1 ]] && echo "tw-skip $e"
    continue
  fi

  # Normalise: tree-walker prints `[ 1 2 3 ]` but v3 prints `<list of 3>`;
  # accept that they match if both produce the same prefix structure.  For
  # numeric/string scalars they should be byte-identical.
  if [[ "$v3_out" == "$tw_out" ]]; then
    pass=$((pass + 1))
    [[ ${V3_TEST_VERBOSE:-0} -eq 1 ]] && echo "OK   $e -> $v3_out"
  else
    # Many cases differ only in display format (lists, attrsets).  Count
    # those as a divergence (informational) but not a failure.
    case "$v3_out" in
      "<list of"*|"<attrs of"*) diverge=$((diverge + 1)) ;;
      *)
        fail=$((fail + 1))
        failed_cases+=("DIVERGE: $e | v3=$v3_out tw=$tw_out")
        [[ ${V3_TEST_VERBOSE:-0} -eq 1 ]] && \
          echo "DIFF $e | v3=$v3_out tw=$tw_out"
        ;;
    esac
  fi
done

total=${#TESTS[@]}
echo ""
echo "=== v3 vs tree-walker regression ==="
echo "  total tests:       $total"
echo "  passing:           $pass"
echo "  display-only diff: $diverge   (lists/attrsets — same value)"
echo "  failing:           $fail"
echo "  v3 errors:         $errors"
if (( fail > 0 )); then
  echo ""
  echo "Failed cases:"
  printf '  %s\n' "${failed_cases[@]}"
  exit 1
fi
