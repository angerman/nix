#!/usr/bin/env bash
# Regression test for #680 — semantic strictness + further error-message
# alignment with TW.
#
# Pre-#680 v3 silently accepted operations TW rejects:
#
#   "x" + 1         → v3 produced "x1" (TW errors)
#   null + 1        → v3 produced "1"  (TW errors)
#   "${1}"          → v3 produced "1"  (TW errors)
#   true + 1        → v3 produced "11" (TW errors)
#   builtins.length "abc"
#                   → v3 returned 3    (TW errors)
#
# All of these are TW type errors.  v3's relaxed coercion / accept-strings-
# in-length could hide bugs in nixpkgs / user code where the wrong type
# leaked into a string-context or list-context call.
#
# Also fixes the v3-internal phrasing of:
#   - infinite-recursion errors (`let x = x; in x`, etc.)
#   - comparison errors (`null < null`, `"a" < 1`)
#
# Both now match TW's core message text (modulo the trailing
# `: <value>` / `(values are X and Y)` suffix that v3 can't produce
# without a full ValuePrinter at error sites — out of scope here).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "run-680: nix not executable at $NIX" >&2
  exit 2
fi

last_error_line() {
  grep -E "^[[:space:]]*error: " | tail -1
}

# Compare core error text (strip leading whitespace, allow TW's optional
# trailing `: <value>` clause — v3 doesn't have ValuePrinter at error
# sites yet, so we accept TW-has-more-detail as a match).
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
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  # Treat as match if exact OR if v3 is a prefix of TW (TW carries the
  # trailing `: <value>` clause that v3 can't reproduce without
  # ValuePrinter; the leading message text is the contract we check).
  if [[ "$tw" == "$v3" || "$tw" == "$v3"* ]]; then
    printf "  MATCH    %-30s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  DIVERGE  %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

# Cases where v3 was SILENTLY producing a value where TW errors — these
# are semantic-strictness regressions in v3, not just message issues.
fail=0
run_case "str-plus-int"     '"x" + 1'                       || fail=$((fail+1))
run_case "null-plus-int"    'null + 1'                      || fail=$((fail+1))
run_case "interp-int"       '"${1}"'                        || fail=$((fail+1))
run_case "bool-plus-int"    'true + 1'                      || fail=$((fail+1))
run_case "null-plus-null"   'null + null'                   || fail=$((fail+1))
run_case "length-string"    'builtins.length "abc"'         || fail=$((fail+1))
# Message-only alignments
run_case "infrec-let-x-x"   'let x = x; in x'               || fail=$((fail+1))
run_case "compare-null"     'null < null'                   || fail=$((fail+1))
run_case "compare-mixed"    '"a" < 1'                       || fail=$((fail+1))
run_case "compare-set"      '{ } < { }'                     || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-680: PASS (10/10 semantic-strictness shapes match TW)"
  exit 0
else
  echo "run-680: FAIL ($fail divergence(s))"
  exit 1
fi
