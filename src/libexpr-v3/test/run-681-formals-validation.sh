#!/usr/bin/env bash
# Regression test for #681 — formals validation must match TW for:
#   (a) arg not an attrset           → `expected a set but found <type>`
#   (b) missing required formal      → `function 'X' called without required argument 'Y'`
#   (c) extra arg in non-ellipsis    → `function 'X' called with unexpected argument 'Y'`
#   (d) lambda's contextual name     → bound name (`foo`) or `anonymous lambda`
#
# Pre-#681 v3 either silently produced the body result (ellipsis-only
# lambdas like `({ ... }: 1) 42` — needForce was false so the type
# check was skipped) or surfaced a generic `v3 OP_ATTRS_SELECT: not
# an attrset` from the formal-destructure path.  Both broke the
# user-visible contract of formals-lambdas.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

last_error_line() {
  grep -E "^[[:space:]]*error: " | tail -1
}
strip_ws() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  echo "$s"
}

run_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  if [[ "$tw" == "$v3" || "$tw" == "$v3"* ]]; then
    printf "  MATCH    %-35s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  DIVERGE  %-35s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

# Positive cases — without these, the always-on arg force (the fix's
# core change) could silently regress legitimate formals call patterns
# under workloads that today rely on the laziness.  These verify each
# shape the validation touches still WORKS for valid args.
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
# NEGATIVE — type-check on non-attrset arg.
run_case "ellipsis-int"        '({ ... }: 1) 42'                                  || fail=$((fail+1))
run_case "ellipsis-string"     '({ ... }: 1) "x"'                                 || fail=$((fail+1))
run_case "ellipsis-null"       '({ ... }: 1) null'                                || fail=$((fail+1))
run_case "ellipsis-list"       '({ ... }: 1) [ ]'                                 || fail=$((fail+1))
run_case "formal-int"          '({ a }: a) 42'                                    || fail=$((fail+1))
# NEGATIVE — missing required argument (with + without lambda name).
run_case "missing-anon"        '({ a, b }: a) { a = 1; }'                         || fail=$((fail+1))
run_case "missing-named"       '(let foo = { a, b }: a; in foo) { a = 1; }'       || fail=$((fail+1))
# NEGATIVE — extra argument (with + without lambda name).
run_case "extra-anon"          '({ a }: a) { a = 1; b = 2; }'                     || fail=$((fail+1))
run_case "extra-named"         '(let bar = { a }: a; in bar) { a = 1; b = 2; }'   || fail=$((fail+1))

# POSITIVE — happy path for each shape the validation touches.
run_pos_case "pos-simple-formals"   '({ a, b }: a + b) { a = 1; b = 2; }'           '3'      || fail=$((fail+1))
run_pos_case "pos-default-arg"      '({ a, b ? 99 }: b) { a = 1; }'                 '99'     || fail=$((fail+1))
run_pos_case "pos-default-override" '({ a, b ? 99 }: b) { a = 1; b = 2; }'          '2'      || fail=$((fail+1))
run_pos_case "pos-ellipsis-empty"   '({ ... }: 42) { }'                             '42'     || fail=$((fail+1))
run_pos_case "pos-ellipsis-extra"   '({ a, ... }: a) { a = 1; b = 2; c = 3; }'      '1'      || fail=$((fail+1))
run_pos_case "pos-at-pattern"       '({ a, ... } @ args: args.b) { a = 1; b = 2; }' '2'      || fail=$((fail+1))
run_pos_case "pos-default-uses-arg" '({ a ? 1, b ? a + 1 }: b) { }'                 '2'      || fail=$((fail+1))
run_pos_case "pos-thunked-arg"      '({ a, b }: a + b) (let x = { a = 1; b = 2; }; in x)' '3' || fail=$((fail+1))
run_pos_case "pos-empty-formals"    '({ }: 42) { }'                                 '42'     || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-681: PASS (9 negative + 9 positive shapes match TW)"
  exit 0
else
  echo "run-681: FAIL ($fail divergence(s))"
  exit 1
fi
