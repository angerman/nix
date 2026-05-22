#!/usr/bin/env bash
# Regression test for #688 — `builtins.splitVersion` must split on
# letter↔digit transitions in ADDITION to '.' and '-' separators.
# TW's algorithm (libstore/names.cc:54 `nextComponent`):
#   - Skip '.' and '-' separators.
#   - Each component is ALL DIGITS or ALL NON-DIGIT-NON-SEP.
#
# Pre-fix v3 split ONLY on '.' and '-' so "1.0-rc1" became
# ["1","0","rc1"] instead of TW's ["1","0","rc","1"].  This in turn
# broke `builtins.compareVersions` for version strings with
# letter/digit boundaries: comparing "1.0-rc10" and "1.0-rc2" used
# string comparison on "rc10" vs "rc2" instead of numeric on
# 10 vs 2.  Real nixpkgs versioning bug surface.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

run_pos_case() {
  local label="$1" expr="$2" expected="$3"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=5s "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && "$tw" == "$expected" ]]; then
    printf "  POS-OK   %-40s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-40s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
    return 1
  fi
}

fail=0
# splitVersion shapes
run_pos_case "basic-1.0.0"        'builtins.splitVersion "1.0.0"'        '[ "1" "0" "0" ]'                    || fail=$((fail+1))
run_pos_case "letter-digit-split" 'builtins.splitVersion "1.0.0-rc1"'    '[ "1" "0" "0" "rc" "1" ]'           || fail=$((fail+1))
run_pos_case "rc-suffix"          'builtins.splitVersion "1.0-rc1"'      '[ "1" "0" "rc" "1" ]'               || fail=$((fail+1))
run_pos_case "alpha-suffix"       'builtins.splitVersion "1.2.3-alpha"'  '[ "1" "2" "3" "alpha" ]'            || fail=$((fail+1))
run_pos_case "trailing-letter"    'builtins.splitVersion "2.3a"'         '[ "2" "3" "a" ]'                    || fail=$((fail+1))
run_pos_case "empty-string"       'builtins.splitVersion ""'             '[ ]'                                || fail=$((fail+1))
run_pos_case "only-separators"    'builtins.splitVersion "---"'          '[ ]'                                || fail=$((fail+1))
run_pos_case "consecutive-sep"    'builtins.splitVersion "1..2"'         '[ "1" "2" ]'                        || fail=$((fail+1))
run_pos_case "all-letters"        'builtins.splitVersion "abc"'          '[ "abc" ]'                          || fail=$((fail+1))
run_pos_case "interleaved"        'builtins.splitVersion "abc123def456"' '[ "abc" "123" "def" "456" ]'        || fail=$((fail+1))
run_pos_case "multi-suffix"       'builtins.splitVersion "1.2.3-pre-alpha"' '[ "1" "2" "3" "pre" "alpha" ]'   || fail=$((fail+1))

# compareVersions consistency — this is where the splitVersion bug
# actually affects user code.
run_pos_case "compare-rc1-rc2"    'builtins.compareVersions "1.0-rc1" "1.0-rc2"'   '-1' || fail=$((fail+1))
run_pos_case "compare-rc10-rc2"   'builtins.compareVersions "1.0-rc10" "1.0-rc2"'  '1'  || fail=$((fail+1))
run_pos_case "compare-1.2.3a-b"   'builtins.compareVersions "1.2.3a" "1.2.3b"'     '-1' || fail=$((fail+1))
run_pos_case "compare-2.3a-2.3.1" 'builtins.compareVersions "2.3a" "2.3.1"'        '-1' || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-688: PASS (15/15 splitVersion + compareVersions shapes match TW)"
  exit 0
else
  echo "run-688: FAIL ($fail divergence(s))"
  exit 1
fi
