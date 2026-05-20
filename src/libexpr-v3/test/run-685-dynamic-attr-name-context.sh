#!/usr/bin/env bash
# Regression test for #685 — dynamic attr names must reject string
# context (TW's forceStringNoCtx mirror).  Pre-fix v3 silently accepted
# contexted strings as attr names at THREE opcode sites, discarding
# the context entirely.  Each site needed an explicit check:
#
#   OP_ATTRS_INIT_DYN   — `{ ${ctxedStr} = v; }`
#   OP_ATTRS_SELECT_DYN — `attrs.${ctxedStr}`
#   OP_ATTRS_HAS_DYN    — `attrs ? ${ctxedStr}`
#
# All three are gated by TW's `forceStringNoCtx` call inside
# `evalDynamicAttrs` (libexpr/eval.cc:2826).  Silent context loss
# could mask drvPath-affecting bugs (a derivation reference dropped
# at a dynamic-attr boundary won't carry through to inputDrvs).
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
        NIX_V3_MAX_WALL_TIME=10s NIX_V3_MAX_HEAP=2G \
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
# NEGATIVE — dynamic attr name with derivation context: TW errors.
run_err_case "init-dyn-with-drv-ctx" \
  'let s = "${(import <nixpkgs>{}).hello}"; in { "${s}" = 1; }' || fail=$((fail+1))
run_err_case "select-dyn-with-drv-ctx" \
  'let s = "${(import <nixpkgs>{}).hello}"; in { a = 1; }.${s}' || fail=$((fail+1))
run_err_case "has-dyn-with-drv-ctx" \
  'let s = "${(import <nixpkgs>{}).hello}"; in { a = 1; } ? ${s}' || fail=$((fail+1))
run_err_case "init-dyn-direct-drv" \
  '{ "${(import <nixpkgs>{}).hello}" = 1; }' || fail=$((fail+1))

# POSITIVE — context-free dynamic names work normally.
run_pos_case "init-dyn-no-ctx" \
  '{ "${"abc"}" = 1; }'                  '{ abc = 1; }'   || fail=$((fail+1))
run_pos_case "select-dyn-no-ctx" \
  '{ a = 42; }.${"a"}'                   '42'             || fail=$((fail+1))
run_pos_case "has-dyn-no-ctx-true" \
  '{ a = 1; } ? ${"a"}'                  'true'           || fail=$((fail+1))
run_pos_case "has-dyn-no-ctx-false" \
  '{ a = 1; } ? ${"b"}'                  'false'          || fail=$((fail+1))
run_pos_case "init-dyn-let-name" \
  'let n = "key"; in { ${n} = 42; }.key' '42'             || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-685: PASS (4 negative + 5 positive shapes match TW)"
  exit 0
else
  echo "run-685: FAIL ($fail divergence(s))"
  exit 1
fi
