#!/usr/bin/env bash
# v3 iterative-force regression test (action plan Phase 1.3).
#
# Generates a deep let-chain and a deep curried-apply expression,
# runs each through v3-direct, asserts completion (exit 0 + correct
# answer) within a tight wall-time budget.
#
# A C-stack overflow would manifest as:
#   - Process aborts with SIGABRT / SIGSEGV (rc != 0)
#   - Or hangs past the timeout (rc 124 from `timeout`)
#
# A successful run proves the OP_FORCE chase + Tag::App spine walk +
# Tag::Thunk path-compression machinery handles deep dependency
# chains iteratively.  Regressions in the A8 iterative-force work
# would be caught by this test failing.
#
# Defaults to depth 5000 per action plan; override via DEPTH=N.
#
# Exit codes:
#   0  all depths pass
#   1  any depth fails (rc != 0, wrong answer, or timeout)
#   2  preflight failed (v3-eval / nix missing)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
DEPTH="${DEPTH:-5000}"
TIMEOUT="${TIMEOUT:-15}"
VERBOSE="${V3_FORCE_DEPTH_VERBOSE:-0}"

if [[ ! -x "$NIX" ]]; then
  echo "iterative-force-depth: nix not found at $NIX" >&2
  exit 2
fi

failures=0

run_test() {
  local name="$1" expr="$2" expected="$3"
  local out rc
  out=$(NIX_V3_DIRECT_EVAL=1 timeout -s KILL "$TIMEOUT" "$NIX" eval --impure --expr "$expr" 2>&1)
  rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "FAIL  $name (rc=$rc — likely C-stack overflow or timeout)" >&2
    [[ "$VERBOSE" == "1" ]] && echo "$out" >&2
    failures=$((failures + 1))
    return
  fi
  if [[ "$out" != "$expected" ]]; then
    echo "FAIL  $name (got '$out', expected '$expected')" >&2
    failures=$((failures + 1))
    return
  fi
  echo "OK    $name"
}

# Test 1: deep let-chain
#   let x0 = x1; x1 = x2; ...; xN = 0; in x0
# Stresses OP_FORCE chase / path-compression.
gen_chain() {
  local n="$1"
  echo "let"
  local i
  for ((i = 0; i < n; i++)); do printf '  x%d = x%d;\n' "$i" "$((i + 1))"; done
  printf '  x%d = 0;\nin x0\n' "$n"
}

# Test 2: deep curried-apply
#   (a0: a1: ... aN-1: aN-1) 0 1 ... N-1
# Stresses OP_CALL frame push / pop in a tight loop.
gen_curry() {
  local n="$1"
  printf '('
  local i
  for ((i = 0; i < n; i++)); do printf 'a%d: ' "$i"; done
  printf 'a%d) ' "$((n - 1))"
  for ((i = 0; i < n; i++)); do printf '%d ' "$i"; done
  printf '\n'
}

# Test 3: deep App-spine
#   let id = x: x; in id (id (id ... (id 0)))
# Stresses Tag::App walk through forceValue.
gen_appspine() {
  local n="$1"
  printf 'let id = x: x; in '
  local i
  for ((i = 0; i < n; i++)); do printf 'id ('; done
  printf '0'
  for ((i = 0; i < n; i++)); do printf ')'; done
  printf '\n'
}

# Hard assertions — must pass on current HEAD.
# The action plan's explicit 5000-deep let-chain target is included
# here, plus curry at the same depth (proves OP_CALL frame push/pop
# is also iterative).  App-spine capped at 3000 with margin — see
# the informational probe below for the higher threshold.
for D in 100 1000 "$DEPTH"; do
  run_test "let-chain-${D}"       "$(gen_chain "$D")"      "0"
  run_test "curry-apply-${D}"     "$(gen_curry "$D")"      "$((D - 1))"
done
# App-spine: capped at 3000.  v3 currently C-stack-overflows around
# 5000; that's a Phase 1.2 follow-up.  3000 is a regression-
# prevention guard with margin.
for D in 100 1000 3000; do
  run_test "app-spine-${D}"       "$(gen_appspine "$D")"   "0"
done

# Informational probe — NOT counted toward failures.  Documents the
# current C-stack ceiling on app-spine; expected to move up as
# Phase 1.2 converts callClosure / OP_CALL primop-arg force loops.
# If this case starts passing, the test's hard-assertion app-spine
# cap can be bumped to 5000.
probe_app_spine_5000() {
  local out rc
  out=$(NIX_V3_DIRECT_EVAL=1 timeout -s KILL "$TIMEOUT" "$NIX" eval --impure --expr "$(gen_appspine 5000)" 2>&1)
  rc=$?
  if [[ $rc -eq 0 && "$out" == "0" ]]; then
    echo "PROBE app-spine-5000: PASS (consider bumping hard-assertion cap)"
  else
    echo "PROBE app-spine-5000: fails (rc=$rc) — Phase 1.2 candidate"
  fi
}
probe_app_spine_5000

echo "=== iterative-force-depth results ==="
echo "  total failures: $failures"
if [[ $failures -gt 0 ]]; then
  exit 1
fi
exit 0
