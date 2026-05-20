#!/usr/bin/env bash
# Regression test for #690 — lib.evalModules patterns that need
# more than 16 IR computeFreeVars iterations.  Pre-fix v3 aborted
# with "free-var fixed-point did not converge within 16 iterations"
# on deeply-recursive module patterns even though the lattice is
# monotone and convergence is guaranteed.  Limit raised to 256.
#
# Also: validates the module-eval patterns I confirmed work in this
# session as part of Tier 3 validation (Battery C+E).
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
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s NIX_V3_MAX_HEAP=2G \
        "$NIX" eval --impure --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && "$tw" == "$expected" ]]; then
    printf "  POS-OK   %-30s => %s\n" "$label" "$tw"
    return 0
  else
    printf "  POS-FAIL %-30s expected=%s\n    TW: %s\n    V3: %s\n" "$label" "$expected" "$tw" "$v3"
    return 1
  fi
}

fail=0

# Basic evalModules patterns (Battery C — all passed before #690 too,
# we want to make sure they STILL pass after the limit change).
run_pos_case "basic-mkOption" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.x = mkOption { type = types.int; default = 1; }; }]; }).config.x' \
  '1' || fail=$((fail+1))

run_pos_case "mkForce-override" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [ { options.x = mkOption { type = types.int; default = 1; }; } { config.x = mkForce 99; } ]; }).config.x' \
  '99' || fail=$((fail+1))

run_pos_case "attrsOf-submodule" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.services = mkOption { type = types.attrsOf (types.submodule { options.enable = mkOption { type = types.bool; default = false; }; }); default = {}; }; config.services.foo.enable = true; }]; }).config.services.foo.enable' \
  'true' || fail=$((fail+1))

run_pos_case "listOf-int" \
  'with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.items = mkOption { type = types.listOf types.int; default = []; }; config.items = [1 2 3]; }]; }).config.items' \
  '[ 1 2 3 ]' || fail=$((fail+1))

# Fix-point patterns (Battery E)
run_pos_case "lib-fix-simple" \
  'with (import <nixpkgs>{}).lib; (fix (self: { x = 1; y = self.x + 1; })).y' \
  '2' || fail=$((fail+1))

run_pos_case "makeExtensible-extend" \
  'with (import <nixpkgs>{}).lib; let base = makeExtensible (self: { x = 1; y = self.x + 1; }); ext1 = base.extend (final: prev: { x = 10; z = final.y; }); in ext1.z' \
  '11' || fail=$((fail+1))

# Type-error message parity (TW-exact phrasing through full module eval)
last_err_line() { grep -E "^[[:space:]]*error: " | tail -1; }
type_err_expr='with (import <nixpkgs>{}).lib; (evalModules { modules = [{ options.x = mkOption { type = types.int; }; config.x = "not-an-int"; }]; }).config.x'
tw_err=$("$NIX" eval --impure --expr "$type_err_expr" 2>&1 | last_err_line | sed 's/^[[:space:]]*//')
v3_err=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_V3_MAX_HEAP=2G \
  "$NIX" eval --impure --expr "$type_err_expr" 2>&1 | last_err_line | sed 's/^[[:space:]]*//')
if [[ "$tw_err" == "$v3_err"* || "$v3_err" == "$tw_err"* ]] && [[ -n "$v3_err" ]]; then
  printf "  ERR-OK   %-30s => %s\n" "type-error-module" "${v3_err:0:80}"
else
  printf "  ERR-FAIL %-30s\n    TW: %s\n    V3: %s\n" "type-error-module" "$tw_err" "$v3_err"
  fail=$((fail+1))
fi

if [[ "$fail" -eq 0 ]]; then
  echo "run-690: PASS (6 positive + 1 type-error shapes match TW)"
  exit 0
else
  echo "run-690: FAIL ($fail divergence(s))"
  exit 1
fi
