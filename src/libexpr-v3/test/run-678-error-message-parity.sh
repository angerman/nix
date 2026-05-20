#!/usr/bin/env bash
# Regression test for #678 — user-facing error messages must match TW's
# core text byte-for-byte.  Pre-fix v3 emitted v3-internal debug names
# ("v3 OP_ATTRS_SELECT: attribute 'b' not found", "v3 primop head: empty
# list or wrong type", etc.) that TW never produces.  This drives users
# to filter logs by v3-specific strings and breaks downstream tooling
# that greps for TW's canonical phrases.
#
# This driver compares only the MESSAGE text after `error:`, not the
# surrounding stack-trace or source-position info (which v3 still
# lacks; tracked separately).  TW emits a multi-line error like:
#
#   error:
#          … while calling the 'head' builtin
#            at «string»:1:1
#                ...
#          error: 'builtins.head' called on an empty list
#
# The driver grep's the LAST `error: <msg>` line, which is the core
# message both evaluators must agree on.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "run-678: nix not executable at $NIX" >&2
  exit 2
fi

# Pull the LAST `error: <msg>` line (TW has multiple; v3 has one).
last_error_line() {
  grep -E "^[[:space:]]*error: " | tail -1
}

run_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  # Strip leading whitespace; both should match after that.
  tw="${tw#"${tw%%[![:space:]]*}"}"
  v3="${v3#"${v3%%[![:space:]]*}"}"
  if [[ "$tw" == "$v3" && -n "$tw" ]]; then
    printf "  MATCH    %-30s => %s\n" "$label" "${tw:0:80}"
    return 0
  else
    printf "  DIVERGE  %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

# Positive tests — verify the happy-path of each primop/operator still
# produces the correct value.  The error-class fixes in #678 touched
# the same throw sites as the success paths; without these positive
# tests a future change could silently break legitimate use.
run_pos_case() {
  local label="$1" expr="$2" expected="$3"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
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
# NEGATIVE — error-message text must match TW (modulo trailing value).
run_case "attr-missing-select" '{ a = 1; }.b'                       || fail=$((fail+1))
run_case "getAttr-missing"     'builtins.getAttr "x" { }'           || fail=$((fail+1))
run_case "head-empty"          'builtins.head [ ]'                  || fail=$((fail+1))
run_case "tail-empty"          'builtins.tail [ ]'                  || fail=$((fail+1))
run_case "elemAt-oob"          'builtins.elemAt [ ] 0'              || fail=$((fail+1))
run_case "elemAt-oob-2"        'builtins.elemAt [ 1 2 ] 5'          || fail=$((fail+1))
run_case "div-by-zero-int"     'builtins.div 1 0'                   || fail=$((fail+1))
run_case "add-str-int"         '1 + "x"'                            || fail=$((fail+1))

# POSITIVE — happy path of each touched primop/operator still works.
run_pos_case "pos-attr-select"     '{ a = 42; }.a'                  '42' || fail=$((fail+1))
run_pos_case "pos-getAttr"         'builtins.getAttr "a" { a = 42; }' '42' || fail=$((fail+1))
run_pos_case "pos-head"            'builtins.head [ 1 2 3 ]'         '1'  || fail=$((fail+1))
run_pos_case "pos-tail"            'builtins.tail [ 1 2 3 ]'         '[ 2 3 ]' || fail=$((fail+1))
run_pos_case "pos-elemAt-in-range" 'builtins.elemAt [ 10 20 30 ] 1'  '20' || fail=$((fail+1))
run_pos_case "pos-div-ok"          'builtins.div 10 2'               '5'  || fail=$((fail+1))
run_pos_case "pos-add-int"         '1 + 2'                           '3'  || fail=$((fail+1))
run_pos_case "pos-add-str"         '"a" + "b"'                       '"ab"' || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-678: PASS (8 negative + 8 positive shapes match TW)"
  exit 0
else
  echo "run-678: FAIL ($fail divergence(s))"
  exit 1
fi
