#!/usr/bin/env bash
# Regression test for #682 — builtins.toFile must:
#   (a) reject contents carrying derivation context (TW-matching error)
#   (b) include path/store-path context entries as refs in the resulting
#       store path's content-address (so its drvPath matches TW)
#   (c) accept context-free strings (positive: produces same drvPath)
#
# Pre-fix v3 silently ignored the contents' string context.  For (a)
# this let derivation refs slip into a store file (semantic error TW
# guards against).  For (b) the CA hash was computed with EMPTY refs,
# producing a different drvPath than TW for any toFile that
# interpolated paths/drvs — a drvPath-AFFECTING bug.
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

run_pos_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=30s NIX_V3_MAX_HEAP=2G \
        "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && -n "$tw" ]]; then
    printf "  POS-OK   %-30s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

run_err_case() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=30s NIX_V3_MAX_HEAP=2G \
        "$NIX" eval --impure --expr "$expr" 2>&1 | last_error_line)"
  tw="$(strip_ws "$tw")"
  v3="$(strip_ws "$v3")"
  if [[ "$tw" == "$v3" || "$tw" == "$v3"* ]]; then
    printf "  ERR-OK   %-30s => %s\n" "$label" "${v3:0:80}"
    return 0
  else
    printf "  ERR-FAIL %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0
# NEGATIVE: derivation context refused with TW-matching error
run_err_case "ctx-drv-rejected" \
  'builtins.toFile "x" "${(import <nixpkgs>{}).hello}"' || fail=$((fail+1))

# POSITIVE: no context → matching drvPath
run_pos_case "no-context"  'builtins.toFile "x" "hello"'  || fail=$((fail+1))
run_pos_case "empty-name"  'builtins.toFile "empty" ""'   || fail=$((fail+1))

# POSITIVE: path context propagates → matching drvPath (this is the
# correctness-critical case: pre-fix v3 dropped refs → wrong hash).
run_pos_case "nested-toFile-context" \
  'let f1 = builtins.toFile "inner" "hello"; in builtins.toFile "outer" "ref: ${f1}"' || fail=$((fail+1))

# POSITIVE: TW must reject "name" with context (forceStringNoCtx).
run_err_case "name-has-context" \
  'let f1 = builtins.toFile "inner" "hi"; in builtins.toFile "x-${f1}" "y"' || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-682: PASS (3 positive + 2 negative shapes match TW)"
  exit 0
else
  echo "run-682: FAIL ($fail divergence(s))"
  exit 1
fi
