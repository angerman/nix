#!/usr/bin/env bash
# C-17/C-18/C-20/C-21/C-23 (CODEBASE_REVIEW_2026-06-11) parity regression driver.
#
#   C-17  zipAttrsWith over a //-composed (ChainBindings) input must include the
#         parent layer's attrs (the C fallback iterated entries[] raw → dropped
#         them). Also exercises the bytecode default path.
#   C-18  toXML of a chain attrset includes parent-layer attrs (materialise).
#   C-20  currentSystem returns a non-empty system string (FFI leaf honours
#         --system; full --system override parity needs the nix binary).
#   C-21  builtins.sort is STABLE (std::stable_sort): equal-key elements keep
#         their input order.
#   C-23  lessThan / `<` compare two paths lexically (TW does; v3 rejected them).
#
# (C-19 getEnv pure-eval parity needs `nix eval --pure-eval`; covered there.)
#
# Usage: [V3=path] run-c17-23-parity-tests.sh
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
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --strict --expr "$expr" 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then echo "PASS  $label  ($out)"; pass=$((pass+1));
  else echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1)); fi
}
check_nonempty() {
  local label="$1" expr="$2" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --expr "$expr" 2>&1 | strip | tail -1)
  if [ -n "$out" ] && [ "$out" != '""' ] && ! echo "$out" | grep -qi error; then
    echo "PASS  $label  ($out)"; pass=$((pass+1));
  else echo "FAIL  $label  (got: $out)"; fail=$((fail+1)); fi
}

# C-17 — chain input keeps parent attrs.
check "C-17 zipAttrsWith-chain " 'builtins.zipAttrsWith (n: vs: vs) [ ({a=1;} // {b=2;}) {a=3;} ]' '{ a = [ 1 3 ]; b = [ 2 ]; }'
# C-18 — toXML of a chain includes both layers (just assert both attr names appear).
check "C-18 toXML-chain-has-a   " 'builtins.match ".*name=\"a\".*" (builtins.toXML ({a=1;} // {b=2;})) != null' 'true'
check "C-18 toXML-chain-has-b   " 'builtins.match ".*name=\"b\".*" (builtins.toXML ({a=1;} // {b=2;})) != null' 'true'
# C-20 — currentSystem non-empty.
check_nonempty "C-20 currentSystem      " 'builtins.currentSystem'
# C-21 — stable sort keeps equal-key order.
check "C-21 sort-stable         " 'map (x: x.t) (builtins.sort (a: b: a.k < b.k) [ {k=1;t="a";} {k=1;t="b";} {k=0;t="c";} ])' '[ "c" "a" "b" ]'
# C-23 — path comparison.
check "C-23 lessThan-paths-lt   " 'builtins.lessThan ./a ./b' 'true'
check "C-23 lessThan-paths-gt   " 'builtins.lessThan ./b ./a' 'false'
check "C-23 path-op-<           " './aaa < ./bbb'              'true'

echo "--- C-17..23 parity: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
