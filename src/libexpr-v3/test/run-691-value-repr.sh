#!/usr/bin/env bash
# Regression test for #691 — Tier 2 valueRepr() helper.  Renders Value
# as TW-equivalent text for error-message value suffixes.  Closes
# accumulated PREFIX-class gaps across MULTIPLE error sites in one
# shared helper:
#
#   - coerceToString errors (`cannot coerce X to a string: <V>`)
#   - valueLess errors (`cannot compare X with Y; values are <V1> and <V2>`)
#   - formals validation (`expected a set but found <T>: <V>`)
#   - OP_LENGTH type errors (`expected a list but found <T>: <V>`)
#
# Output matches TW's `ValuePrinter(state, v, errorPrintOptions)`:
#   - maxDepth=10, maxAttrs=10, maxListItems=10, maxStringLength=1024
#   - force=false (does NOT force lazy values — would risk recursing
#     into the very error we're rendering)
#   - Float: default ostream format (`1.5`, not `1.500000`)
#   - String: escaped + quoted
#
# Pre-fix v3 emitted PREFIX-form placeholders (e.g. `{ ... }` / `[ ... ]`
# / `1.500000` instead of `1.5`).  Several existing test drivers
# accepted prefix-match as PASS for these cases; #691 lets them
# upgrade to exact-MATCH.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

last_error_line() { grep -E "^[[:space:]]*error: " | tail -1; }
strip_ws() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  echo "$s"
}

# Exact-MATCH only — no prefix acceptance here.  The whole point of
# #691 is closing the prefix gap.
run_err_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  if [[ "$tw" == "$v3" ]]; then
    printf "  EXACT    %-30s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  DIVERGE  %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0
# coerceToString — value-suffix rendering
run_err_case "coerce-int"       '"x" + 1'                                    || fail=$((fail+1))
run_err_case "coerce-float"     '"x" + 1.5'                                  || fail=$((fail+1))
run_err_case "coerce-bool"      '"x" + true'                                 || fail=$((fail+1))
run_err_case "coerce-null"      '"x" + null'                                 || fail=$((fail+1))
run_err_case "coerce-emptylist" '"${[]}"'                                    || fail=$((fail+1))
run_err_case "coerce-list-ints" '"${[1 2 3]}"'                               || fail=$((fail+1))
run_err_case "coerce-list-flt"  '"${[1.5 2.5]}"'                             || fail=$((fail+1))
run_err_case "coerce-emptyset"  '"${{}}"'                                    || fail=$((fail+1))
run_err_case "coerce-set"       '"${{ a = 1; b = 2; }}"'                     || fail=$((fail+1))

# OP_LENGTH — type-error with value
run_err_case "length-string"    'builtins.length "abc"'                      || fail=$((fail+1))
run_err_case "length-int"       'builtins.length 42'                         || fail=$((fail+1))

# Formals — `expected a set but found <T>: <V>`
run_err_case "formals-int"      '({ a }: a) 42'                              || fail=$((fail+1))
run_err_case "formals-string"   '({ a }: a) "hello"'                         || fail=$((fail+1))
run_err_case "formals-list"     '({ a }: a) [1 2 3]'                         || fail=$((fail+1))
run_err_case "formals-bool"     '({ a }: a) true'                            || fail=$((fail+1))

# valueLess — comparison with values
run_err_case "compare-mixed"    '1 < "a"'                                    || fail=$((fail+1))
run_err_case "compare-null"     'null < null'                                || fail=$((fail+1))
run_err_case "compare-set"      '[] < {}'                                    || fail=$((fail+1))
run_err_case "compare-list-set" '[1 2] < { x = 1; }'                         || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-691: PASS (18/18 exact-MATCH shapes match TW)"
  exit 0
else
  echo "run-691: FAIL ($fail divergence(s))"
  exit 1
fi
