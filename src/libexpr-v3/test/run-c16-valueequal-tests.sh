#!/usr/bin/env bash
# C-16 (CODEBASE_REVIEW_2026-06-11) regression driver — valueEqual short-circuit
# + PAP function-arm.
#
# Bug: valueEqual force-evaluated ALL container elements up front (in the push
# loop), so `[1 (throw "x")] == [2 3]` threw where TW short-circuits on the
# first (mismatching) element and returns false. Also under-applied closure-PAPs
# fell to the default raw-pointer-equality arm instead of being treated as
# function values.
#
# Fix: push element pairs unforced (with their source slots), force at pop time
# with writeback (preserves A12 memoization — 583 cache tests), and return on
# the first mismatch so later elements are never forced. App/App3 PAPs compare
# like closures (never equal by ==; pointer-identity inside a container).
#
# Usage: [V3=path] run-c16-valueequal-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-8s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0
check() {
  local label="$1" expr="$2" expected="$3" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --expr "$expr" 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then echo "PASS  $label  ($out)"; pass=$((pass+1));
  else echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1)); fi
}

# Short-circuit: mismatch in the first element returns false WITHOUT forcing a
# later throwing element.
check "list short-circuit (len) " '[1 (throw "x")] == [2 3]'     'false'
check "list short-circuit (val) " '[5 (throw "x")] == [9 9 9]'   'false'
check "attrs short-circuit      " '{ a = 1; b = throw "x"; } == { a = 2; b = 9; }' 'false'
# When the earlier elements MATCH, the later element is forced (TW does too).
check "list forces-on-match     " 'builtins.tryEval ([1 (throw "x")] == [1 2])' '{ success = false; value = false; }'
# Equality still correct.
check "list-equal               " '[1 2 3] == [1 2 3]'           'true'
check "attrs-equal              " '{ a = 1; b = 2; } == { a = 1; b = 2; }' 'true'
check "attrs-unequal            " '{ a = 1; b = 2; } == { a = 1; b = 3; }' 'false'
check "nested-equal             " '{ x = [1 {y=2;}]; } == { x = [1 {y=2;}]; }' 'true'
# PAP function-arm: distinct PAPs are never equal (functions); no crash.
check "pap-not-equal            " 'let f = a: b: c: a; in (f 1) == (f 1)' 'false'
check "pap-in-list-not-equal    " 'let f = a: b: c: a; in [ (f 1) ] == [ (f 1) ]' 'false'

echo "--- C-16 valueEqual: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
