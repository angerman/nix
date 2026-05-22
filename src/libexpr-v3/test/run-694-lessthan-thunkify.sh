#!/usr/bin/env bash
# Regression test for #694 — lower.cc `isTrivialForLazy(forArg=true)` MUST
# thunkify `__lessThan` (i.e. `<`, `>`, `<=`, `>=` ExprCalls) when passed
# as function arguments.  Pre-fix v3 eager-lowered them, which forced
# their args at call site rather than at consumer-demand time.
#
# Repro pattern: `lib.mkIf (length attrNames cfg > 0) {}` inside
# lib.evalModules.  Pre-fix, this tripped the `_module.freeformType`
# cycle (v3 force-cfg → merge → mkIf cond → force-cfg).  Post-fix the
# cond is a thunk passed lazily into mkIf, which mkIf stores in
# `condition` without forcing — matching TW.
#
# Operator-bisection covered:
#   - `length X > 0` / `0 < length X` / `length X < 1` / `1 > length X`
#     (parse to bare `Less{...}`; pre-fix FAILED, post-fix WORKS)
#   - `length X >= 1` / `length X != 0` / `length X == 0`
#     (parse to Not(Less{...}) / Eq / NEq; never failed, double-checked)
#   - `(builtins.length [...]) > 0` w/o cfg cycle (works regardless)
#
# Also verifies the synthetic minimal repro file matches TW.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

fail=0

# Run TW vs v3 on the same expression; PASS if both produce the same
# first non-warning line.
run_pair() {
  local label="$1" expr="$2"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 \
        | grep -v '^Failed\|^warning:' | head -1)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=15s \
        "$NIX" eval --impure --expr "$expr" 2>&1 \
        | grep -v '^Failed\|^warning:' | head -1)"
  if [[ "$tw" == "$v3" ]]; then
    printf "  OK   %-50s => %s\n" "$label" "${tw:0:60}"
  else
    printf "  FAIL %-50s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    fail=$((fail+1))
  fi
}

PRE='let pkgs=import <nixpkgs> {}; lib=pkgs.lib; in '
SUF=').config.services.foo'
OPT='options.services.foo = lib.mkOption { type=lib.types.attrsOf lib.types.int; default={}; }; '
EVALMODULES_TEMPLATE() {
  local cond="$1"
  echo "${PRE}(lib.evalModules { modules = [({lib,config,...}: let cfg = config.services.foo; in { ${OPT}config = lib.mkIf (${cond}) {}; })]; }${SUF}"
}

echo "===== Operator-bisection: lessThan-arg in evalModules-mkIf ====="
# Failing cases pre-#694 (all parse to bare Less{...}):
run_pair "length X > 0"          "$(EVALMODULES_TEMPLATE 'builtins.length (builtins.attrNames cfg) > 0')"
run_pair "0 < length X"          "$(EVALMODULES_TEMPLATE '0 < builtins.length (builtins.attrNames cfg)')"
run_pair "length X < 1"          "$(EVALMODULES_TEMPLATE 'builtins.length (builtins.attrNames cfg) < 1')"
run_pair "1 > length X"          "$(EVALMODULES_TEMPLATE '1 > builtins.length (builtins.attrNames cfg)')"
# Working cases (regression-guardrails — should never have failed):
run_pair "length X >= 1"         "$(EVALMODULES_TEMPLATE 'builtins.length (builtins.attrNames cfg) >= 1')"
run_pair "length X != 0"         "$(EVALMODULES_TEMPLATE 'builtins.length (builtins.attrNames cfg) != 0')"
run_pair "length X == 0"         "$(EVALMODULES_TEMPLATE 'builtins.length (builtins.attrNames cfg) == 0')"
run_pair "0 <= length X"         "$(EVALMODULES_TEMPLATE '0 <= builtins.length (builtins.attrNames cfg)')"

echo
echo "===== Non-cycle smoke: lessThan-arg still works at runtime ====="
# Don't accidentally break arithmetic / comparison FAST paths.  fib's
# `if k < 2 then ... else ...` uses `<` as an If cond (not a func arg),
# so it's unaffected.  But `map (x: x > 0) [-1 0 1]` uses `>` in a
# closure body — that should still produce [false false true].
run_pair "map (x: x > 0) [-1 0 1]" 'builtins.map (x: x > 0) [(-1) 0 1]'
run_pair "filter (x: x > 5) ..."  'builtins.filter (x: x > 5) [1 5 10]'
run_pair "fib(10) (n < 2 cond)"   'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 10'
run_pair "n - 1 still eager"      '(let f = n: n - 1; in f 100)'
run_pair "n * 2 still eager"      '(let f = n: n * 2; in f 21)'
run_pair "n / 7 still eager"      '(let f = n: n / 7; in f 35)'

echo
echo "===== Full synthetic repro (was KNOWN-FAIL) ====="
REPRO_PATH="$ROOT/src/libexpr-v3/test/repro-nylon-derivation-hybrid.nix"
if [[ -f "$REPRO_PATH" ]]; then
  REPRO_EXPR="$(cat "$REPRO_PATH")"
  run_pair "repro-nylon-derivation-hybrid.nix" "$REPRO_EXPR"
fi

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-694: PASS (operator-bisection + smoke + nylon-repro all match TW)"
  exit 0
else
  echo
  echo "run-694: FAIL ($fail divergence(s))"
  exit 1
fi
