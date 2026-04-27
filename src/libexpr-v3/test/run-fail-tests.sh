#!/usr/bin/env bash
# v3 evaluator eval-fail test runner.
#
# For each tests/functional/lang/eval-fail-*.nix expression, run through
# v3-eval and confirm it exits non-zero with output on stderr.  We don't
# strictly validate the error message text — just that an evaluation
# error is raised, so this guards against silent successes where the
# tree-walker would have failed.
#
# Usage:
#   ./run-fail-tests.sh                # summary
#   V3_FAIL_VERBOSE=1 ./run-fail-tests.sh
#   V3_FAIL_PATTERN='abort*' ./run-fail-tests.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

pattern="${V3_FAIL_PATTERN:-*}"
verbose="${V3_FAIL_VERBOSE:-0}"

TESTS_FUNCTIONAL="$ROOT/tests/functional"
cd "$TESTS_FUNCTIONAL"

export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"

correct=0
silent=0
crash=0
total=0
silent_cases=()
crash_cases=()

for f in lang/eval-fail-${pattern}.nix; do
  [[ -e "$f" ]] || continue
  name=$(basename "$f" .nix)
  total=$((total + 1))

  # 1-second per-test timeout, SIGKILL on miss — v3-eval has its own
  # signal-handler thread that swallows SIGTERM, so we go straight to
  # SIGKILL.  Tests that hang past 1s are counted as crashes.
  #
  # Capture stdout+stderr separately from the exit code: piping through
  # `tr` (to drop NULs from path payloads) destroys $? unless we either
  # use pipefail or run twice.  Running twice is the simpler / more
  # portable option and the tests are tiny.
  # `set +m` prevents bash from printing "Killed: 9" status messages
  # for the SIGKILL the timeout sends.
  set +m
  # Match upstream lang.sh: eval-fail tests run with `--eval --strict
  # --show-trace`.  Strict deep-forces every attrset value so
  # readDir/throw/etc are reached.
  out=$(timeout -s KILL 1 "$V3" --strict --file "$f" 2>&1 | tr -d '\000' || true)
  { timeout -s KILL 1 "$V3" --strict --file "$f" >/dev/null 2>&1; } 2>/dev/null
  ec=$?

  if [[ $ec -eq 0 ]]; then
    silent=$((silent + 1))
    silent_cases+=("$name")
    [[ "$verbose" == "1" ]] && echo "SILENT  $name (v3 returned 0 but expected error)"
  elif echo "$out" | grep -qiE "error|aborted|throw|assert|fail"; then
    correct=$((correct + 1))
    [[ "$verbose" == "1" ]] && echo "OK      $name"
  else
    crash=$((crash + 1))
    crash_cases+=("$name")
    [[ "$verbose" == "1" ]] && echo "CRASH   $name (ec=$ec, no error msg)"
  fi
done

echo "=== v3 eval-fail test results ==="
echo "  total tests:   $total"
echo "  raised error:  $correct"
echo "  silently passed (BUG): $silent"
echo "  crashed/other:         $crash"

if [[ ${#silent_cases[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Silent passes:"
  printf '  %s\n' "${silent_cases[@]}"
fi
if [[ ${#crash_cases[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Crash/other:"
  printf '  %s\n' "${crash_cases[@]}"
fi
