#!/usr/bin/env bash
# v3 INVERSION step 4 — regression test for the NIX_V3_DIRECT_EVAL
# CLI gate (#526).  Verifies that `nix eval --expr/--file` produces
# correct output via v3's own pipeline (no eval-hook, no TW bridge)
# for a representative set of shapes.
#
# Each test runs the same expression once via TW (baseline) and once
# via v3-direct (NIX_V3_DIRECT_EVAL=1).  Output must match byte-for-
# byte.  Failures mean either the v3-direct path is incorrect OR the
# v3 evaluator itself has a bug that hook-mode was previously masking
# via fallback to TW.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

ok=0
fail=0
fail_names=()

# Each test: name | flags | expr | (optional) attrPath
# Compares TW output vs v3-direct.  Both paths run with --impure to
# avoid pure-eval flag interference.
run_pair() {
  local name="$1" flags="$2" expr="$3"
  local tw v3 status
  tw=$("$NIX_BIN" eval --impure $flags --expr "$expr" 2>/dev/null) || tw="<tw-error>"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" eval --impure $flags --expr "$expr" 2>/dev/null) || v3="<v3-direct-error>"
  if [[ "$tw" == "$v3" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$name  TW=[$tw]  v3=[$v3]")
  fi
}

# Trivial scalars + arithmetic.
run_pair "int-add"      ""        '1 + 2'
run_pair "float"        ""        '3.14'
run_pair "bool-true"    ""        'true'
run_pair "bool-false"   ""        'false'
run_pair "null"         ""        'null'
run_pair "string"       ""        '"hello"'
run_pair "string-raw"   "--raw"   '"hello"'

# Composite values.
run_pair "list"         ""        '[ 1 2 3 ]'
run_pair "attrs"        ""        '{ a = 1; b = "x"; c = [ 1 2 ]; }'
run_pair "nested-attrs" ""        '{ a.b.c = 42; }'

# let / rec / function call.
run_pair "let-binding"  ""        'let x = 1; y = 2; in x + y'
run_pair "let-rec"      ""        'let xs = [ 1 ] ++ [ 2 ]; in builtins.length xs'
run_pair "lambda-call"  ""        '(x: x + 1) 41'
run_pair "lambda-formal" ""       '({ a, b }: a + b) { a = 10; b = 32; }'

# Conditionals + comparisons.
run_pair "if-true"      ""        'if 1 < 2 then "yes" else "no"'
run_pair "if-false"     ""        'if 1 > 2 then "yes" else "no"'

# Primops.
run_pair "head"         ""        'builtins.head [ 1 2 3 ]'
run_pair "tail-len"     ""        'builtins.length (builtins.tail [ 1 2 3 ])'
run_pair "concat"       ""        'builtins.concatLists [ [ 1 ] [ 2 ] [ 3 ] ]'
run_pair "attrnames"    ""        'builtins.attrNames { c = 3; a = 1; b = 2; }'
run_pair "typeof-int"   ""        'builtins.typeOf 42'
run_pair "typeof-list"  ""        'builtins.typeOf [ ]'

# JSON.
run_pair "json-int"     "--json"  '42'
run_pair "json-list"    "--json"  '[ 1 "x" true null ]'
run_pair "json-attrs"   "--json"  '{ a = 1; b = "x"; }'

# Lambda values render: TW shows position info, v3 shows <LAMBDA> —
# accept divergent rendering but ensure neither errors.  We skip
# strict equality here; the `wrapped` test below verifies the value
# is at least computable.
run_pair "lambda-wrapped" "" 'builtins.typeOf (x: x)'

echo "=== v3-direct-eval test results ==="
echo "  passing: $ok"
echo "  failing: $fail"
if (( fail > 0 )); then
  for n in "${fail_names[@]}"; do
    echo "  FAIL $n"
  done
  exit 1
fi
exit 0
