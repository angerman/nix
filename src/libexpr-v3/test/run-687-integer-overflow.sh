#!/usr/bin/env bash
# Regression test for #687 — integer-overflow errors must match TW's
# exact phrasing `integer overflow in {adding|subtracting|multiplying}
# X {+|-|*} Y` (libexpr/eval.cc:2515 family).
#
# Pre-fix v3 had MULTIPLE bugs in this area:
#   - OP_ADD/SUB/MUL/STR_CONCAT threw with "v3 OP_*: integer overflow"
#     (no operand values, leaked v3-internal opcode names).
#   - primAdd/primSub/primMul DIDN'T CHECK FOR OVERFLOW at all —
#     `builtins.add INT64_MAX 1` returned the wrapped value
#     -9223372036854775808 silently.  This is a SEMANTIC bug that
#     could mask integer-arithmetic errors in nixpkgs builder math.
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
    printf "  ERR-OK   %-35s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  ERR-FAIL %-35s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
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
    printf "  POS-OK   %-35s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-35s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
    return 1
  fi
}

fail=0
# NEGATIVE — every overflow path (operator + primop, all 3 ops)
run_err_case "op-add-overflow"     '9223372036854775807 + 1'                || fail=$((fail+1))
run_err_case "prim-add-overflow"   'builtins.add 9223372036854775807 1'     || fail=$((fail+1))
run_err_case "op-sub-overflow"     '9223372036854775807 - (-1)'             || fail=$((fail+1))
run_err_case "prim-sub-overflow"   'builtins.sub 9223372036854775807 (-1)'  || fail=$((fail+1))
run_err_case "op-mul-overflow"     '9223372036854775807 * 2'                || fail=$((fail+1))
run_err_case "prim-mul-overflow"   'builtins.mul 9223372036854775807 2'     || fail=$((fail+1))

# POSITIVE — non-overflow arithmetic stays correct.
run_pos_case "op-add-ok"           '1 + 2'                          '3'    || fail=$((fail+1))
run_pos_case "op-sub-ok"           '5 - 3'                          '2'    || fail=$((fail+1))
run_pos_case "op-mul-ok"           '4 * 5'                          '20'   || fail=$((fail+1))
run_pos_case "prim-add-ok"         'builtins.add 1 2'               '3'    || fail=$((fail+1))
run_pos_case "prim-sub-ok"         'builtins.sub 5 3'               '2'    || fail=$((fail+1))
run_pos_case "prim-mul-ok"         'builtins.mul 4 5'               '20'   || fail=$((fail+1))
run_pos_case "mixed-int-float-add" '1 + 1.5'                        '2.5'  || fail=$((fail+1))
run_pos_case "float-add"           '1.5 + 2.5'                      '4'    || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-687: PASS (6 negative + 8 positive shapes match TW)"
  exit 0
else
  echo "run-687: FAIL ($fail divergence(s))"
  exit 1
fi
