#!/usr/bin/env bash
# Regression test for #693 — Tier 1 continuation: 12 more primop
# error-message divergences aligned to TW byte-for-byte.
#
# After #692, blanket-greped remaining "v3 primop X:" / "v3 OP_X:"
# strings and probed each.  This batch covers:
#
#   - substring/genList negative-length error wording
#   - removeAttrs/attrValues/unsafeGetAttrPos/parseDrvName/scopedImport
#     wrong-type errors → use new `expectedTypeButFound` helper that
#     emits TW's `expected a X but found a Y: <value>`
#   - toPath relative + non-string error
#   - convertHash unknown-format hint
#   - genericClosure error phrasings (set + list + attribute-missing)
#   - hashFile / readFileType missing-path → "path 'X' does not exist"
#   - toJSON of function → "cannot convert a function to JSON"
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
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
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
# Negative-length / negative-index
run_err_case "substring-neg-start" 'builtins.substring (-1) 5 "h"' || fail=$((fail+1))
run_err_case "genList-neg-length"  'builtins.genList (i: i) (-1)'  || fail=$((fail+1))

# Wrong-type errors via expectedTypeButFound helper
run_err_case "removeAttrs-int"     'builtins.removeAttrs 42 []'                  || fail=$((fail+1))
run_err_case "removeAttrs-str-list" 'builtins.removeAttrs { a = 1; } "not-list"' || fail=$((fail+1))
run_err_case "attrValues-null"     'builtins.attrValues null'                    || fail=$((fail+1))
run_err_case "attrValues-int"      'builtins.attrValues 42'                      || fail=$((fail+1))
run_err_case "unsafeGetAttrPos-int" 'builtins.unsafeGetAttrPos 42 { a = 1; }'    || fail=$((fail+1))
run_err_case "parseDrvName-int"    'builtins.parseDrvName 42'                    || fail=$((fail+1))
run_err_case "scopedImport-int"    'builtins.scopedImport 42 ./.'                || fail=$((fail+1))

# toPath specifics
run_err_case "toPath-relative"     'builtins.toPath "relative"' || fail=$((fail+1))
run_err_case "toPath-int"          'builtins.toPath 42'          || fail=$((fail+1))

# convertHash hint
run_err_case "convertHash-unknown" 'builtins.convertHash { hash = "abc"; toHashFormat = "unknown"; }' || fail=$((fail+1))

# genericClosure shape errors
run_err_case "genericClosure-not-list" \
  'builtins.genericClosure { startSet = [{key=1;}]; operator = i: 42; }' || fail=$((fail+1))
run_err_case "genericClosure-not-set" \
  'builtins.genericClosure { startSet = [42]; operator = i: []; }' || fail=$((fail+1))
run_err_case "genericClosure-no-key" \
  'builtins.genericClosure { startSet = [{ value = 1; }]; operator = i: []; }' || fail=$((fail+1))

# Missing paths
run_err_case "hashFile-missing"    'builtins.hashFile "sha256" "/nonexistent"' || fail=$((fail+1))
run_err_case "readFileType-missing" 'builtins.readFileType /nonexistent'        || fail=$((fail+1))

# toJSON of function
run_err_case "toJSON-function"     'builtins.toJSON (x: x)'                     || fail=$((fail+1))
run_err_case "toJSON-functor"      'builtins.toJSON {__functor = self: x: x;}'  || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-693: PASS (19/19 error-message shapes match TW)"
  exit 0
else
  echo "run-693: FAIL ($fail divergence(s))"
  exit 1
fi
