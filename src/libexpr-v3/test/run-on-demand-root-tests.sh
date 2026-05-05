#!/usr/bin/env bash
# v3 #451 / #455: on-demand-root regression suite.
#
# Runs the lang test corpus through the call hook with on-demand-root
# enabled in each of its diagnostic modes.  This is a *regression
# test*: the existing lang tests are known to all pass under default
# v3, so any failure here is a regression introduced by on-demand-root
# infrastructure.
#
# The four modes covered:
#
#   default-off    NIX_V3_ON_DEMAND_ROOT unset      -- baseline
#   safe           NIX_V3_ON_DEMAND_ROOT=1          -- nUpvalues=0 only
#   unsafe         NIX_V3_ON_DEMAND_ROOT_UNSAFE=1   -- all lambdas
#   never-run-od   NIX_V3_NEVER_RUN_OD=1            -- bug isolation gate
#
# Cardano-node is the keystone real-world workload that exercises the
# pattern triggering #455's silent-wrong-output / infinite-recursion
# regression.  This script does NOT include it -- the lang tests are
# the regression baseline, the cardano-node check is a known-broken
# *negative* test until #455 is fixed.
#
# Usage:
#   ./run-on-demand-root-tests.sh
#   V3_OD_VERBOSE=1 ./run-on-demand-root-tests.sh   # show per-test PASS/FAIL
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/build/src/nix/nix-instantiate}"

if [[ ! -x "$NIX_INST" ]]; then
  echo "nix-instantiate not found at $NIX_INST" >&2
  exit 1
fi

verbose="${V3_OD_VERBOSE:-0}"

cd "$ROOT/tests/functional"
export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"

declare -a MODES=(
  "default-off::NIX_USE_V3=1"
  "safe:NIX_V3_ON_DEMAND_ROOT=1:NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1"
  "unsafe:NIX_V3_ON_DEMAND_ROOT_UNSAFE=1:NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_ON_DEMAND_ROOT_UNSAFE=1"
  "never-run-od:NIX_V3_NEVER_RUN_OD=1:NIX_USE_V3=1 NIX_V3_ON_DEMAND_ROOT=1 NIX_V3_NEVER_RUN_OD=1"
)

overall_fail=0

for mode_spec in "${MODES[@]}"; do
  IFS=':' read -r mode_name _flag mode_env <<< "$mode_spec"
  ok=0; fail=0; total=0
  fail_names=()
  for f in lang/eval-okay-*.nix; do
    base="${f%.nix}"
    [[ -e "${base}.exp-disabled" && ! -e "${base}.exp" ]] && continue
    total=$((total + 1))
    flagfile="${base}.flags"
    declare -a flag_arr=()
    [[ -e "$flagfile" ]] && read -ra flag_arr < "$flagfile"
    if env $mode_env timeout -s KILL 10 "$NIX_INST" "${flag_arr[@]}" --eval --strict "$f" >/dev/null 2>&1; then
      ok=$((ok + 1))
      [[ "$verbose" == "1" ]] && echo "OK    [$mode_name] $(basename "$f" .nix)"
    else
      fail=$((fail + 1))
      fail_names+=("$(basename "$f" .nix)")
      [[ "$verbose" == "1" ]] && echo "FAIL  [$mode_name] $(basename "$f" .nix)"
    fi
  done
  echo "=== $mode_name: total=$total passing=$ok failing=$fail ==="
  if [[ $fail -gt 0 ]]; then
    overall_fail=1
    if [[ "$verbose" == "1" || $fail -le 5 ]]; then
      echo "  Failures:"
      printf '    %s\n' "${fail_names[@]}"
    fi
  fi
done

if [[ $overall_fail -ne 0 ]]; then
  echo
  echo "=== SUITE FAILED: at least one mode regressed lang tests ==="
  exit 1
fi
echo
echo "=== SUITE PASSED: all modes pass lang tests ==="
exit 0
