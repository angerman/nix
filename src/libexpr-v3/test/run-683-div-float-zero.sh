#!/usr/bin/env bash
# Regression test for #683 — float division by zero must raise
# `division by zero` matching TW (libexpr/primops.cc:4703 unconditional
# check).  Pre-fix v3 had THREE separate bugs in the same area:
#
#   (a) primDiv only checked Int/Int — Float/Float, Int/Float, and
#       Float/Int paths silently produced ±inf or NaN.
#   (b) opt_const_fold (Phase B constant folding) folded float
#       division at compile time including div-by-zero, claiming
#       "Mirror tree-walker: float / 0.0 yields ±inf (no throw) per
#       IEEE" — but TW DOES throw.  Folding bypassed the runtime
#       check entirely.
#   (c) primHashString error message lacked the algorithm-list hint.
#
# Pre-fix v3 returned `inf` for `1.0 / 0.0` etc. — silent semantic
# divergence on numeric code that could mask divide-by-zero bugs in
# nixpkgs builder math.
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

run_err_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  if [[ "$tw" == "$v3" || "$tw" == "$v3"* ]]; then
    printf "  ERR-OK   %-30s => %s\n" "$label" "$v3"
    return 0
  else
    printf "  ERR-FAIL %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

run_pos_case() {
  local label="$1" expr="$2" expected="$3"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && "$tw" == "$expected" ]]; then
    printf "  POS-OK   %-30s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-30s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
    return 1
  fi
}

fail=0
# NEGATIVE — all numeric combos of div-by-zero must error.
run_err_case "div-int-int-zero"     'builtins.div 1 0'           || fail=$((fail+1))
run_err_case "div-float-float-zero" 'builtins.div 1.0 0.0'       || fail=$((fail+1))
run_err_case "div-int-float-zero"   'builtins.div 1 0.0'         || fail=$((fail+1))
run_err_case "div-float-int-zero"   'builtins.div 1.0 0'         || fail=$((fail+1))
run_err_case "op-div-float-zero"    '1.0 / 0.0'                  || fail=$((fail+1))
run_err_case "op-div-int-zero"      '1 / 0'                      || fail=$((fail+1))
run_err_case "op-div-neg-float-z"   '(-1.0) / 0.0'               || fail=$((fail+1))
# NEGATIVE — hash algo error has algo-list hint.
run_err_case "hashstring-unknown"   'builtins.hashString "unknown" "x"' || fail=$((fail+1))

# POSITIVE — valid float arithmetic must still work.
run_pos_case "pos-float-div"        '1.0 / 2.0'                  '0.5'  || fail=$((fail+1))
run_pos_case "pos-int-float-div"    '1 / 2.0'                    '0.5'  || fail=$((fail+1))
run_pos_case "pos-float-int-div"    '2.0 / 1'                    '2'    || fail=$((fail+1))
run_pos_case "pos-negative-div"     '(-5.0) / 2.5'               '-2'   || fail=$((fail+1))
run_pos_case "pos-int-div"          'builtins.div 6 2'           '3'    || fail=$((fail+1))
run_pos_case "pos-hashstring-sha256" 'builtins.hashString "sha256" "x"' \
  '"2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881"' || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-683: PASS (8 negative + 6 positive shapes match TW)"
  exit 0
else
  echo "run-683: FAIL ($fail divergence(s))"
  exit 1
fi
