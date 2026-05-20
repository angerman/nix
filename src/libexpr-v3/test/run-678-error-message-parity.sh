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

fail=0
run_case "attr-missing-select" '{ a = 1; }.b'                       || fail=$((fail+1))
run_case "getAttr-missing"     'builtins.getAttr "x" { }'           || fail=$((fail+1))
run_case "head-empty"          'builtins.head [ ]'                  || fail=$((fail+1))
run_case "tail-empty"          'builtins.tail [ ]'                  || fail=$((fail+1))
run_case "elemAt-oob"          'builtins.elemAt [ ] 0'              || fail=$((fail+1))
run_case "elemAt-oob-2"        'builtins.elemAt [ 1 2 ] 5'          || fail=$((fail+1))
run_case "div-by-zero-int"     'builtins.div 1 0'                   || fail=$((fail+1))
run_case "add-str-int"         '1 + "x"'                            || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-678: PASS (8/8 error-message shapes match TW)"
  exit 0
else
  echo "run-678: FAIL ($fail divergence(s))"
  exit 1
fi
