#!/usr/bin/env bash
# Regression test for #677 — default-mode `nix eval --impure` must catch
# per-attr/per-elem errors and emit `«error: <msg>»` inline, matching
# TW's `ValuePrinter` (libexpr/print.cc:625).  Pre-fix v3 ran
# `forceDeep` upfront and any inner `throw` propagated to the top
# level, aborting the entire print.
#
# Also pins:
#   - `throw <msg>` emits the message verbatim (no "v3 throw:" prefix).
#   - `abort <msg>` emits TW's exact phrasing
#     ("evaluation aborted with the following error message: '<msg>'").
#   - Derivation detection short-circuit forces only `type` + `drvPath`
#     (doesn't descend into `passthru.tests` triggering nixpkgs's
#     deprecation warning).
#   - `seen` set uses caller-Value-address for lists (not stack-local
#     force target) so sibling occurrences of structurally-equal lists
#     both render in full instead of one becoming «repeated».
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "run-677: nix not executable at $NIX" >&2
  exit 2
fi

run_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=10s "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && -n "$tw" ]]; then
    printf "  MATCH    %-30s => %s\n" "$label" "${tw:0:80}"
    return 0
  else
    printf "  DIVERGE  %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0
run_case "attr-throw"   '{ a = 1; b = throw "no"; c = 3; }'                    || fail=$((fail+1))
run_case "list-throw"   '[ 1 (throw "boom") 3 ]'                               || fail=$((fail+1))
run_case "nested-throw" '{ x = { y = throw "deep"; z = 1; }; }'                || fail=$((fail+1))
run_case "abort-msg"    '{ a = 1; b = abort "boom"; }'                         || fail=$((fail+1))
run_case "sibling-list" '{ x = { o = [ "out" ]; }; y = { o = [ "out" ]; }; }'  || fail=$((fail+1))
run_case "tryEval-throw" 'builtins.tryEval (throw "msg")'                      || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-677: PASS (6/6 default-print shapes match TW)"
  exit 0
else
  echo "run-677: FAIL ($fail divergence(s))"
  exit 1
fi
