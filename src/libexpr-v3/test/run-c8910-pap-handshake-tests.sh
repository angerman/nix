#!/usr/bin/env bash
# C-8/C-9/C-10 (CODEBASE_REVIEW_2026-06-11) regression driver — under-applied
# closure-PAPs vs the force handshake.
#
# Bug class: the rewind force handshake `if (isAppLike()||Thunk||Slot) { ip--;
# flags|=CFF_FORCE_RETRY; goto op_force_slow; }` re-tests the operand after the
# chase, but op_force_slow returns an under-applied closure-PAP UNCHANGED — so a
# PAP operand re-tests the same App/App3 tag and rewinds FOREVER (an infinite
# loop where TW raises a type error or applies the PAP).
#
#   C-9 NEGATIVE  a PAP where a list/string/attrset is expected must ERROR
#                 (head/tail/length/elemAt/++/string-concat///), not hang.
#   C-8 NEGATIVE  a PAP element in a deepForceList primop (foldl' etc.) and a
#                 PAP primop arg must be handled, not hang.
#   C-10 POSITIVE an App3 PAP callee (3+ args applied at once via mapAttrs /
#                 multi-arg application) must saturate correctly, not hang.
#
# The shared needsForce() predicate excludes PAPs at every handshake; OP_CALL/
# OP_TAIL_CALL gather App3 spine args (two per App3 link) so an App3 PAP callee
# saturates. Before the fix, the C-10 cases HUNG (repro-a12b-op-call-iter-force
# / POS-4 was a pre-existing wall-time timeout at HEAD).
#
# Usage: [V3=path] run-c8910-pap-handshake-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-8s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

# errors_no_hang label expr  — must produce an ERROR (not a value, not a hang)
errors_no_hang() {
  local label="$1" expr="$2" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --strict --expr "$expr" 2>&1 | strip | tail -1)
  if echo "$out" | grep -qi 'WallTimeExceeded'; then
    echo "FAIL  $label  (HUNG — regression!)"; fail=$((fail+1))
  elif echo "$out" | grep -qi 'error'; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  else
    echo "FAIL  $label  (expected an error, got: $out)"; fail=$((fail+1))
  fi
}

# value label expr expected  — must compute expected (not hang)
value() {
  local label="$1" expr="$2" expected="$3" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --strict --expr "$expr" 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  elif echo "$out" | grep -qi 'WallTimeExceeded'; then
    echo "FAIL  $label  (HUNG — regression! expected: $expected)"; fail=$((fail+1))
  else
    echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1))
  fi
}

# C-9: PAP (under-applied 3-arity closure) where a list/string/attrset op expects a concrete type.
PAP='((a: b: c: a) 1)'
errors_no_hang "C-9 head on PAP   " "builtins.head $PAP"
errors_no_hang "C-9 tail on PAP   " "builtins.tail $PAP"
errors_no_hang "C-9 length on PAP " "builtins.length $PAP"
errors_no_hang "C-9 elemAt on PAP " "builtins.elemAt $PAP 0"
errors_no_hang "C-9 ++ on PAP     " "$PAP ++ [1]"
errors_no_hang "C-9 str+ on PAP   " "\"x\" + $PAP"
errors_no_hang "C-9 // on PAP     " "$PAP // {x=1;}"

# C-8: PAP element in a deepForceList primop, and PAP primop arg.
value "C-8 foldl over PAP list" \
  'builtins.foldl'"'"' (acc: f: f acc) 0 [ (builtins.add 1) (builtins.add 2) ]' '3'

# C-10: App3 PAP callee saturation (arg order matters).
value "C-10 App3 saturate ints  " 'let f = a: b: c: a - b - c; in (f 100 10) 1' '89'
value "C-10 App3 saturate str   " 'let f = a: b: c: d: "${a}-${b}-${c}-${d}"; in (f "w" "x" "y") "z"' '"w-x-y-z"'
value "C-10 App3 via mapAttrs    " '(builtins.mapAttrs (n: v: x: "${n}=${toString v}+${toString x}") { k = 5; }).k 9' '"k=5+9"'

echo "--- C-8/9/10 PAP-handshake: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
