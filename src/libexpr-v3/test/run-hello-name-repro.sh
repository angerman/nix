#!/usr/bin/env bash
# Driver for the hello.name v3-direct hot-loop regression repro.
#
# Per action plan "Bisect nixpkgs to find the unit you can falsify
# against" (2026-05-15) — when a v3 failure surfaces on nixpkgs,
# capture the minimal repro + driver and keep it forever as a
# regression guardrail.
#
# This test PASSES when:
#   - The TW reference completes (proves env is sane)
#   - v3-direct evaluates the repro in under V3_MAX_SECONDS (default
#     20).  Today v3 times out — the test is INTENTIONALLY a
#     known-fail that flips to passing when Phase 2 cycle-handling
#     work closes the hot loop.
#
# Exit codes:
#   0  — both TW and v3-direct completed under threshold
#   1  — v3-direct timed out / exceeded threshold (expected today)
#   2  — TW also failed (env issue, NOT a v3 regression)
#
# Usage:
#   ./run-hello-name-repro.sh               # 20 s threshold
#   V3_MAX_SECONDS=60 ./run-hello-name-repro.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
TW_NIX="${TW_NIX:-nix-instantiate}"
REPRO="$(dirname "$0")/repro-hello-name.nix"
THRESHOLD="${V3_MAX_SECONDS:-20}"

if [[ ! -f "$REPRO" ]]; then
  echo "hello-name-repro: missing $REPRO" >&2
  exit 2
fi
if [[ ! -x "$NIX" ]]; then
  echo "hello-name-repro: $NIX not executable" >&2
  exit 2
fi
if ! command -v "$TW_NIX" >/dev/null 2>&1; then
  echo "hello-name-repro: $TW_NIX not on PATH (need TW for sanity)" >&2
  exit 2
fi

START=$(date +%s.%N)
TW_OUT=$($TW_NIX --eval --strict --impure "$REPRO" 2>&1)
TW_RC=$?
TW_ELAPSED=$(awk "BEGIN{print $(date +%s.%N) - $START}")

if [[ $TW_RC -ne 0 ]] || ! echo "$TW_OUT" | grep -q "^true$"; then
  echo "hello-name-repro: TW failed (rc=$TW_RC, out='$TW_OUT')" >&2
  echo "  this is an environment problem, NOT a v3 regression" >&2
  exit 2
fi
printf "TW: rc=%d elapsed=%.2fs\n" "$TW_RC" "$TW_ELAPSED"

START=$(date +%s.%N)
V3_OUT=$(NIX_V3_DIRECT_EVAL=1 timeout -s KILL "$THRESHOLD" "$NIX" eval --impure --expr "import $REPRO" 2>&1)
V3_RC=$?
V3_ELAPSED=$(awk "BEGIN{print $(date +%s.%N) - $START}")
printf "v3: rc=%d elapsed=%.2fs\n" "$V3_RC" "$V3_ELAPSED"

if [[ $V3_RC -eq 0 ]] && echo "$V3_OUT" | grep -q "^true$"; then
  echo "PASS — v3-direct completes $REPRO under ${THRESHOLD}s.  The"
  echo "       hello.name hot-loop has been closed.  Consider bumping"
  echo "       the test to its full-fidelity form: '(import <nixpkgs>{}).hello.name'."
  exit 0
fi

# Known-fail today — exit 1 documents the gap.
echo "EXPECTED-FAIL — v3-direct does not yet complete $REPRO under ${THRESHOLD}s."
echo "                Per project_583_memoization_loop.md, the failure"
echo "                is the parsedPlatform.check / flip / setTypes cycle"
echo "                inside nixpkgs's stage construction.  Phase 2 work"
echo "                target.  When this flips to PASS, the action plan's"
echo "                Phase 1 exit criterion is met."
exit 1
