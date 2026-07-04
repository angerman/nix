#!/usr/bin/env bash
# v3 evaluator lang test runner — runs each tests/functional/lang/eval-okay-*.nix
# expression through v3-eval and compares against the recorded .exp output (or
# the tree-walker's output when no .exp exists).
#
# Usage:
#   ./run-lang-tests.sh                # summary
#   V3_LANG_VERBOSE=1 ./run-lang-tests.sh  # verbose (show every test)
#   V3_LANG_PATTERN='fib*' ./run-lang-tests.sh  # only run matching tests

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
TW="${TW:-$ROOT/build/src/nix/nix-instantiate}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

pattern="${V3_LANG_PATTERN:-*}"
verbose="${V3_LANG_VERBOSE:-0}"

# Mirror tests/functional/lang.sh — the upstream runner runs with cwd =
# tests/functional/, sets these env vars, and rewrites `$(pwd)` to
# `/pwd` in test output before diffing.
TESTS_FUNCTIONAL="$ROOT/tests/functional"
cd "$TESTS_FUNCTIONAL"
PWD_REWRITE="$(pwd)"

export TEST_VAR=foo
export HOME=/fake-home
export NIX_PATH="lang/dir3:lang/dir4"

pass=0
fail=0
errors=0
total=0
failed_cases=()
errored_cases=()

# Skip files known to require unsupported features in v3 today; track separately.
declare -A KNOWN_SKIP
# Currently no tests are skipped — failures show what to fix next.

for f in lang/eval-okay-${pattern}.nix; do
  [[ -e "$f" ]] || continue
  name=$(basename "$f" .nix)
  total=$((total + 1))

  # Skip listed cases.
  if [[ -n "${KNOWN_SKIP[$name]:-}" ]]; then
    continue
  fi

  # Upstream marks tests as disabled by adding `.exp-disabled` next to
  # the .nix file (e.g. eval-okay-tail-call-1).  Honor that.
  if [[ -e "lang/$name.exp-disabled" ]]; then
    total=$((total - 1))
    continue
  fi

  # Read per-test flags (.flags file alongside .nix), if any.
  flags=()
  if [[ -e "lang/$name.flags" ]]; then
    while IFS= read -r line; do
      [[ -z "$line" || "$line" == \#* ]] && continue
      # shellcheck disable=SC2206
      flags+=($line)
    done < "lang/$name.flags"
  fi

  # Run v3-eval.  Use --strict so we get fully-evaluated results, matching
  # what nix-instantiate --eval --strict would produce.  Discard stderr —
  # the lang test goldens compare against stdout only (warnings have a
  # separate .err.exp file).  Substitute $(pwd) -> /pwd in output to
  # match upstream test runner.
  v3_out=$("$V3" "${flags[@]}" --file "$f" --strict 2>/dev/null | sed "s!$PWD_REWRITE!/pwd!g") || {
    err_msg=$("$V3" "${flags[@]}" --file "$f" --strict 2>&1 >/dev/null | head -c 200)
    errors=$((errors + 1))
    errored_cases+=("$name: $err_msg")
    [[ "$verbose" -eq 1 ]] && echo "ERROR  $name: $err_msg"
    continue
  }

  # Compare with .exp if present, otherwise with tree-walker.
  exp_file="lang/$name.exp"
  if [[ -f "$exp_file" ]]; then
    expected=$(cat "$exp_file")
  else
    expected=$("$TW" --eval --strict "$f" 2>/dev/null) || expected="<tw-error>"
  fi

  if [[ "$v3_out" == "$expected" ]]; then
    pass=$((pass + 1))
    [[ "$verbose" -eq 1 ]] && echo "OK     $name"
  else
    fail=$((fail + 1))
    failed_cases+=("$name | v3=$(echo "$v3_out" | head -c 80) tw/exp=$(echo "$expected" | head -c 80)")
    [[ "$verbose" -eq 1 ]] && echo "DIFF   $name"
  fi
done

echo ""
echo "=== v3 lang test results ==="
echo "  total tests:  $total"
echo "  passing:      $pass"
echo "  failing:      $fail"
echo "  v3 errors:    $errors"

if (( fail > 0 || errors > 0 )); then
  if [[ "$verbose" -eq 1 ]]; then
    echo ""
    echo "Failed cases:"
    printf '  %s\n' "${failed_cases[@]}"
    echo ""
    echo "Errored cases:"
    printf '  %s\n' "${errored_cases[@]}"
  fi
fi
