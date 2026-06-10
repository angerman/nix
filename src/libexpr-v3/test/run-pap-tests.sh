#!/usr/bin/env bash
# PAP-recognition test driver — positive / negative / regression.
#
# Bug (FIXED 2026-06-10): builtins.typeOf / isFunction / functionArgs
# mis-handled an under-applied multi-arity closure (a "PAP" — Tag::App / App3
# chain whose leaf is a Closure with arity > applied-depth, produced by the
# default-ON eval/apply curry-collapse).  typeOf → "unknown", functionArgs →
# typeError, and isFunction *infinite-looped* (the OP_IS_FUNCTION force-retry
# macro treated the WHNF PAP as non-WHNF).  This was the haskell.nix / cardano
# v3-direct spin: `getFlake haskell-nix-example` hung at ~5612 closures inside
# nixpkgs lib.isFunction / makeOverridable.  See
# lode/RCA_HASKELLNIX_SPIN_2026-06-10.md.
#
#   POSITIVE   repro-pap-pos.nix                  typeOf/isFunction/functionArgs
#              of a PAP match TW exactly.
#   NEGATIVE   repro-pap-neg.nix                  saturated results / non-funcs
#              are NOT mis-classified as lambda.
#   REGRESSION repro-pap-functor-recursion.nix    the exact spin kernel
#              (lib.isFunction over a __functor-of-curried-lambda) terminates
#              = true (was: WallTimeExceeded).
#   REGRESSION repro-pap-functionargs-functor.nix lib.functionArgs over a
#              functor/PAP = { } (was: typeError).
#
# Exit nonzero on any failure (wrong value, or a regression that SPINS again).
# Usage: [V3=path] run-pap-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-8s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

# label  file  expected
check() {
  local label="$1" file="$2" expected="$3"
  local out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --file "$HERE/$file" --strict 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  elif echo "$out" | grep -qi 'WallTimeExceeded'; then
    echo "FAIL  $label  (SPUN — regression! expected: $expected)"; fail=$((fail+1))
  else
    echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1))
  fi
}

check "positive               " repro-pap-pos.nix                 '[ "lambda" true "lambda" "lambda" { } "int" 3 ]'
check "negative               " repro-pap-neg.nix                 '[ false false false false "int" ]'
check "regression functor-isFn" repro-pap-functor-recursion.nix   'true'
check "regression functor-fArg" repro-pap-functionargs-functor.nix '{ }'

echo "--- PAP: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
