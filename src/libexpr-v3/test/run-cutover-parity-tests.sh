#!/usr/bin/env bash
# v3 cutover output-parity tests.
#
# run-cutover-tests.sh checks only exit code (does the cutover crash
# or diverge with an error?).  This script is the strictness gate:
# for every lang/eval-okay-*.nix test, run TW and v3-cutover, compare
# stdout byte-for-byte.  A divergence catches silent wrong-output
# regressions that exit-code-only tests miss.
#
# Usage:
#   ./run-cutover-parity-tests.sh                 # summary
#   V3_PARITY_VERBOSE=1 ./run-cutover-parity-tests.sh
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

verbose="${V3_PARITY_VERBOSE:-0}"

cd "$ROOT/tests/functional"
export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"
PWD_REWRITE="$(pwd)"

ok=0; fail=0; total=0; tw_err=0; v3_err=0
fail_names=()

for f in lang/eval-okay-*.nix; do
  base="${f%.nix}"
  # Skip upstream-disabled tests.
  if [[ -e "${base}.exp-disabled" && ! -e "${base}.exp" ]]; then
    continue
  fi
  total=$((total + 1))
  name=$(basename "$f" .nix)

  # Read per-test flags.
  declare -a flag_arr=()
  if [[ -e "${base}.flags" ]]; then
    read -ra flag_arr < "${base}.flags"
  fi

  # Run TW first as ground-truth.
  tw_out=$(timeout -s KILL 10 "$NIX_INST" "${flag_arr[@]}" --eval --strict "$f" 2>/dev/null \
           | sed "s!$PWD_REWRITE!/pwd!g") || tw_out="<tw-error>"
  if [[ "$tw_out" == "<tw-error>" ]]; then
    tw_err=$((tw_err + 1))
    [[ "$verbose" == "1" ]] && echo "TW-ERROR $name (skipping parity)"
    continue
  fi

  # Run v3 cutover.
  v3_out=$(NIX_USE_V3=1 timeout -s KILL 10 "$NIX_INST" "${flag_arr[@]}" --eval --strict "$f" 2>/dev/null \
           | sed "s!$PWD_REWRITE!/pwd!g") || v3_out="<v3-error>"
  if [[ "$v3_out" == "<v3-error>" ]]; then
    v3_err=$((v3_err + 1))
    fail=$((fail + 1))
    fail_names+=("[v3-error] $name")
    [[ "$verbose" == "1" ]] && echo "V3-ERROR $name"
    continue
  fi

  if [[ "$tw_out" == "$v3_out" ]]; then
    ok=$((ok + 1))
    [[ "$verbose" == "1" ]] && echo "OK    $name"
  else
    fail=$((fail + 1))
    fail_names+=("[diff] $name")
    [[ "$verbose" == "1" ]] && echo "DIFF  $name"
  fi
done

echo "=== v3 cutover output-parity results ==="
echo "  total tests:  $total"
echo "  parity:       $ok"
echo "  diverged:     $fail"
echo "  tw-errors:    $tw_err (skipped, can't compare)"
echo "  v3-errors:    $v3_err (counted under diverged)"

if [[ ${#fail_names[@]} -gt 0 ]]; then
  echo
  echo "Divergent / errored cases:"
  printf '  %s\n' "${fail_names[@]}"
  exit 1
fi
