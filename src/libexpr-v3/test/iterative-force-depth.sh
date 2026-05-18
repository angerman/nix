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
  # Capture stdout ONLY for value comparison; stderr captured separately
  # for diagnostics.  Without this split, the harmless macOS warning
  # "Failed to increase stack size ..." (which lands on stderr) was
  # leaking into the comparison via 2>&1.
  local stdout stderr
  stdout="$(NIX_V3_DIRECT_EVAL=1 timeout -s KILL "$TIMEOUT" "$NIX" eval --impure --expr "$expr" 2>/dev/null)"
  rc=$?
  if [[ $rc -ne 0 ]]; then
    echo "FAIL  $name (rc=$rc — likely C-stack overflow or timeout)" >&2
    [[ "$VERBOSE" == "1" ]] && echo "$stdout" >&2
    failures=$((failures + 1))
    return
  fi
  if [[ "$stdout" != "$expected" ]]; then
    echo "FAIL  $name (got '$stdout', expected '$expected')" >&2
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
# App-spine: capped at 10000.  Phase 1.2 identity-lambda specialisation
# (emit.cc + vm.cc; LambdaDescriptor::identityLambda) made the App-
# spine apply loop truly iterative: each `id` substitutes the arg
# directly with no frame push.  Prior to that fix, the cap was 3000
# (kMaxCallDepth=5000 fired at ~4000-5000).  Now bounded only by
# the parser's nesting limit (~15000 for our generator pattern).
for D in 100 1000 5000 10000; do
  run_test "app-spine-${D}"       "$(gen_appspine "$D")"   "0"
done

echo "=== iterative-force-depth results ==="
echo "  total failures: $failures"
if [[ $failures -gt 0 ]]; then
  exit 1
fi
exit 0
