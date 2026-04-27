#!/usr/bin/env bash
# Run the official `tests/functional/lang/eval-okay-*.nix` suite through
# the v3 cutover (NIX_USE_V3=1).  Honors per-test `.flags` files like
# the upstream `tests/functional/lang.sh` runner.
#
# A test is counted as passing if it exits 0 — we don't compare to
# .exp files here (that's the job of run-lang-tests.sh, which uses
# v3-eval directly).  This runner is purely about confirming the
# cutover path doesn't crash or diverge.
#
# Usage:
#   ./run-cutover-tests.sh                 # summary
#   V3_CUTOVER_VERBOSE=1 ./run-cutover-tests.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/build/src/nix/nix-instantiate}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"

if [[ ! -x "$NIX_INST" ]]; then
  echo "nix-instantiate not found at $NIX_INST" >&2
  exit 1
fi

verbose="${V3_CUTOVER_VERBOSE:-0}"

TESTS_FUNCTIONAL="$ROOT/tests/functional"
cd "$TESTS_FUNCTIONAL"

export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"

ok=0; fail=0; total=0
fail_names=()

for f in lang/eval-okay-*.nix; do
  # Skip tests that upstream marks .exp-disabled.
  base="${f%.nix}"
  if [[ -e "${base}.exp-disabled" && ! -e "${base}.exp" ]]; then
    continue
  fi

  total=$((total + 1))
  name=$(basename "$f" .nix)
  flagfile="${base}.flags"

  # Match upstream lang.sh: per-test flags from .flags are *prepended*
  # to the standard `--eval --strict` invocation, not used in place of
  # them.  See tests/functional/lang.sh's eval-okay loop.
  declare -a flag_arr=()
  if [[ -e "$flagfile" ]]; then
    read -ra flag_arr < "$flagfile"
  fi

  if NIX_USE_V3=1 timeout -s KILL 10 "$NIX_INST" "${flag_arr[@]}" --eval --strict "$f" >/dev/null 2>&1; then
    ok=$((ok + 1))
    [[ "$verbose" == "1" ]] && echo "OK    $name"
  else
    fail=$((fail + 1))
    fail_names+=("$name")
    [[ "$verbose" == "1" ]] && echo "FAIL  $name"
  fi
done

echo "=== v3 cutover lang test results ==="
echo "  total tests:  $total"
echo "  passing:      $ok"
echo "  failing:      $fail"
if [[ ${#fail_names[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Failures:"
  printf '  %s\n' "${fail_names[@]}"
fi
