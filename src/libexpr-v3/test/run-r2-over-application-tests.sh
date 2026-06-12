#!/usr/bin/env bash
# R2 over-application test driver — positive / negative / regression.
#
# Bug (FIXED 2026-06-12): an unforced arity-2 mapAttrs/zipAttrsWith App3 PAP,
# applied to a further argument (the callback's body returns a function), threw
# `v3 OP_TAIL_CALL: PAP over-applied` (twin: `v3 OP_CALL: PAP over-applied`)
# instead of saturate-then-reapply.  This was the cardano-node v3-direct
# `attrNames .packages.aarch64-darwin` failure.  See
# lode RCA + memory project_codebase_review_impl_2026-06-12.
#
#   POSITIVE   repro-r2-over-application-pos.nix   over-application by 1 and 2,
#              tail (OP_TAIL_CALL) and non-tail (OP_CALL), match TW exactly.
#   NEGATIVE   repro-r2-over-application-neg.nix   saturated + under-applied
#              App3 shapes are unchanged (no false over-applied throw).
#
# Both expected values are the tree-walker's.  Exit nonzero on any mismatch
# (wrong value, or a regression that throws "PAP over-applied" again).
# Usage: [V3=path] run-r2-over-application-tests.sh
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
  elif echo "$out" | grep -qi 'over-applied'; then
    echo "FAIL  $label  (PAP over-applied — regression! expected: $expected)"; fail=$((fail+1))
  else
    echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1))
  fi
}

check "positive (over-apply ×1/×2, tail+call)" repro-r2-over-application-pos.nix '[ [ 9 9 ] [ 30 ] "int" ]'
check "negative (saturated / under-applied)  " repro-r2-over-application-neg.nix '[ "a=1" "a-1-X" 7 ]'

echo "--- R2 over-application: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
