#!/usr/bin/env bash
# v3 evaluator eval-fail test runner.
#
# For each tests/functional/lang/eval-fail-*.nix expression:
#   1. Run v3-eval; confirm exit is non-zero.
#   2. If a matching .err.exp exists, sanitize stderr ($(pwd) → /pwd
#      to mirror lang.sh's transform) and `diff -u` exactly against it.
#
# Tests with no .err.exp fall back to the old "stderr contains
# error|aborted|throw|assert|fail" heuristic — that path stays in
# place for any future eval-fail-*.nix that hasn't grown a fixture
# yet.
#
# The byte-exact diff path was added 2026-05-15 per action plan
# Phase 0.5.  Previous loose-regex match silently accepted C1-C8
# semantic gaps where v3 raised the wrong error.  This script's
# exit code now reflects fixture mismatches in addition to silent
# passes and crashes.
#
# Usage:
#   ./run-fail-tests.sh                    # summary
#   V3_FAIL_VERBOSE=1 ./run-fail-tests.sh
#   V3_FAIL_PATTERN='abort*' ./run-fail-tests.sh
#   V3_FAIL_STRICT_DIFF=0 ./run-fail-tests.sh   # opt-out of byte-diff
#
# Exit codes:
#   0  all tests passed (no silents, no crashes, no diff mismatches)
#   1  one or more failures (silent/crash/mismatch)
#   2  preflight failed (v3-eval missing, etc.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 2
fi

pattern="${V3_FAIL_PATTERN:-*}"
verbose="${V3_FAIL_VERBOSE:-0}"
strict_diff="${V3_FAIL_STRICT_DIFF:-1}"

TESTS_FUNCTIONAL="$ROOT/tests/functional"
cd "$TESTS_FUNCTIONAL"

export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"

# pwd-as-seen-by-tests for path sanitization (matches lang.sh's
# `sed "s!$(pwd)!/pwd!g"` on the .err.exp side).
PWD_TOKEN="$(pwd)"

correct=0
silent=0
crash=0
mismatch=0
total=0
silent_cases=()
crash_cases=()
mismatch_cases=()

# sanitize_stderr: stdin → stdout with absolute paths normalized.
# Mirrors tests/functional/lang.sh's sanitization so v3's stderr is
# directly comparable to the existing .err.exp golden files.
sanitize_stderr() {
  # The $(pwd) → /pwd transform is the only transform lang.sh does
  # for eval-fail tests.  Replicate it here.  Use awk-style sub via
  # sed -e with a literal pattern (PWD_TOKEN may contain `/` which
  # is the default sed delimiter; use `!` as delimiter instead).
  sed -e "s!${PWD_TOKEN}!/pwd!g"
}

for f in lang/eval-fail-${pattern}.nix; do
  [[ -e "$f" ]] || continue
  name=$(basename "$f" .nix)
  total=$((total + 1))

  # Match upstream lang.sh: eval-fail tests run with `--eval --strict
  # --show-trace`.  Strict deep-forces every attrset value so
  # readDir/throw/etc are reached.
  #
  # Subshell with `set +m` (Monitor mode off) suppresses bash's
  # job-control "Killed: 9" status messages when the SIGKILL fires.
  # Capture stderr to a temp file; read exit code + body separately.
  err_tmp=$(mktemp -t v3failXXXXXX) || { echo "mktemp failed" >&2; exit 2; }
  trap 'rm -f "$err_tmp"' EXIT
  (set +m; timeout -s KILL 1 "$V3" --strict --file "$f" >/dev/null 2>"$err_tmp") 2>/dev/null
  ec=$?
  # tr -d strips NULs from path payloads BEFORE the pipeline so the
  # subsequent sed gets clean input.
  out=$(tr -d '\000' < "$err_tmp" | sanitize_stderr)

  err_exp="lang/${name}.err.exp"
  classify_with_diff=$([[ "$strict_diff" == "1" && -f "$err_exp" ]] && echo 1 || echo 0)

  if [[ $ec -eq 0 ]]; then
    silent=$((silent + 1))
    silent_cases+=("$name")
    [[ "$verbose" == "1" ]] && echo "SILENT   $name (v3 returned 0 but expected error)"
  elif [[ "$classify_with_diff" == "1" ]]; then
    # Byte-exact diff path.  `tr -d '\000'` keeps bash's command
    # substitution from emitting "ignored null byte" warnings if the
    # golden file ever ends up with NULs (defensive — none currently do).
    expected=$(tr -d '\000' < "$err_exp")
    if [[ "$out" == "$expected" ]]; then
      correct=$((correct + 1))
      [[ "$verbose" == "1" ]] && echo "OK       $name"
    else
      mismatch=$((mismatch + 1))
      mismatch_cases+=("$name")
      if [[ "$verbose" == "1" ]]; then
        echo "MISMATCH $name"
        diff -u <(printf '%s' "$expected") <(printf '%s' "$out") | head -20 | sed 's/^/    /'
      fi
    fi
  elif echo "$out" | grep -qiE "error|aborted|throw|assert|fail"; then
    # Loose-match fallback (only when .err.exp is missing or
    # V3_FAIL_STRICT_DIFF=0 explicitly opts out).
    correct=$((correct + 1))
    [[ "$verbose" == "1" ]] && echo "OK-LOOSE $name"
  else
    crash=$((crash + 1))
    crash_cases+=("$name")
    [[ "$verbose" == "1" ]] && echo "CRASH    $name (ec=$ec, no error msg)"
  fi
done

echo "=== v3 eval-fail test results ==="
echo "  total tests:           $total"
echo "  raised matching error: $correct"
echo "  fixture mismatch:      $mismatch"
echo "  silently passed (BUG): $silent"
echo "  crashed/other:         $crash"

if [[ ${#silent_cases[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Silent passes:"
  printf '  %s\n' "${silent_cases[@]}"
fi
if [[ ${#mismatch_cases[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Fixture mismatches:"
  printf '  %s\n' "${mismatch_cases[@]}"
fi
if [[ ${#crash_cases[@]} -gt 0 && "$verbose" == "1" ]]; then
  echo
  echo "Crash/other:"
  printf '  %s\n' "${crash_cases[@]}"
fi

# Exit non-zero on any failure category.  silent_cases is the most
# severe (BUG); mismatch + crash both indicate v3 is divergent from
# tree-walker.  Previous version only ever exited 0 from this loop
# (it had no exit at the end at all) — the harness's signal was
# `correct/total` ratio printed to stdout and consumed by a wrapper.
if [[ $silent -gt 0 || $mismatch -gt 0 || $crash -gt 0 ]]; then
  exit 1
fi
exit 0
