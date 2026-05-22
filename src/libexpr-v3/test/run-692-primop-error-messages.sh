#!/usr/bin/env bash
# Regression test for #692 — align 5 more primop error messages to
# TW's exact phrasing.  Pre-#692 v3 emitted:
#
#   builtins.fromTOML "broken"          v3 primop fromTOML: <c++-impl>
#   builtins.addDrvOutputDependencies   v3 addDrvOutputDependencies: empty string context
#   builtins.readFile "/nonexistent"    v3 primop readFile: cannot open /nonexistent
#   builtins.readDir "/nonexistent"     filesystem error: ... [libc++-specific]
#   builtins.pathExists null            v3 primop pathExists: expected string or path
#
# TW:
#
#   while parsing TOML: <toml-impl-msg>
#   context of string 'abc' must have exactly one element, but has 0
#   path '/nonexistent' does not exist
#   path '/nonexistent' does not exist
#   cannot coerce null to a string: null
#
# Tier 1 finding from continuing-the-audit work after Tier 3.  Each
# fix is small (drop "v3 primop X:" prefix, use TW's exact text).
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
  if [[ "$tw" == "$v3" ]]; then
    printf "  ERR-OK   %-35s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  ERR-FAIL %-35s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0
run_err_case "fromTOML-broken"          'builtins.fromTOML "broken syntax"' || fail=$((fail+1))
run_err_case "addDrvDeps-empty"         'builtins.addDrvOutputDependencies "abc"' || fail=$((fail+1))
run_err_case "readFile-missing"         'builtins.readFile "/nonexistent"' || fail=$((fail+1))
run_err_case "readDir-missing"          'builtins.readDir "/nonexistent"' || fail=$((fail+1))
run_err_case "pathExists-null"          'builtins.pathExists null' || fail=$((fail+1))
run_err_case "pathExists-int"           'builtins.pathExists 42' || fail=$((fail+1))
run_err_case "pathExists-list"          'builtins.pathExists []' || fail=$((fail+1))
run_err_case "pathExists-set"           'builtins.pathExists { a = 1; }' || fail=$((fail+1))

# Positive cases (don't break the happy path)
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

run_pos_case "fromTOML-ok"   'builtins.fromTOML "x = 1"'      '{ x = 1; }'  || fail=$((fail+1))
run_pos_case "pathExists-yes" 'builtins.pathExists /nix'      'true'        || fail=$((fail+1))
run_pos_case "pathExists-no"  'builtins.pathExists /nonexistent' 'false'    || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-692: PASS (8 negative + 3 positive shapes match TW)"
  exit 0
else
  echo "run-692: FAIL ($fail divergence(s))"
  exit 1
fi
