#!/usr/bin/env bash
# v3 arithmetic / comparison TW-parity regression tests.
#
# Five tree-walker-parity bugs in the numeric + comparison surface, all
# empirically confirmed against the tree-walker (`src/libexpr/`) oracle:
#
#   1. builtins.div / the `/` operator on INT64_MIN / -1 must raise
#      `integer overflow in dividing <a> / <b>` (TW: prim_div checked
#      division, libexpr/primops.cc:4720) instead of silently wrapping to
#      INT64_MIN (and SIGFPE'ing on x86_64).  The primop (primops.cc
#      primDiv) and the operator (vm.cc OP_DIV) are SEPARATE code paths —
#      both are guarded + carry the full message.
#   2. builtins.floor / builtins.ceil on a float outside [INT64_MIN,
#      INT64_MAX) must raise `NixFloat argument <f> is not in the range of
#      NixInt` (TW: prim_floor/prim_ceil) instead of clamping via the cast.
#   3. builtins.catAttrs must raise `expected a set but found <T>: <v>` on a
#      non-attrset element (TW: forceAttrs per element) instead of failing
#      open by silently skipping it.
#   4. `<` / builtins.lessThan / sort over lists must skip value-EQUAL
#      elements (TW's CompareValues uses eqValues) and only ORDER the first
#      unequal pair, so `[ {} ] < [ {} 1 ]` is `true` (length tiebreak),
#      not a fail-closed "cannot compare" error.  Both v3 comparison
#      engines are fixed: vm.cc valueLess (OP_LESS) and primops.cc
#      valueLessHelper (primLessThan — the path `<` and sort take).
#   5. builtins.head / builtins.tail must type-check FIRST (TW: forceList →
#      `expected a list but found <T>: <v>`) and only then check emptiness,
#      instead of reporting a non-list as `called on an empty list`.
#
# Format mirrors run-formals-error-tests.sh: NEGATIVE rows assert an error
# fragment appears in BOTH the v3 and TW stderr; POSITIVE rows assert the
# value is byte-identical on both engines.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$V3" ]];  then echo "arith-compare-parity: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -x "$NIX" ]]; then echo "arith-compare-parity: nix not at $NIX" >&2; exit 2; fi
export _NIX_TEST_NO_ENVIRONMENT_WARNINGS=1

pass=0; fail=0; failed=()

v3eval() { NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$1" 2>/dev/null | tail -1; }
tweval()  { env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$1" 2>/dev/null; }
v3err()  { NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$1" 2>&1 >/dev/null || true; }
# nix (v3-direct) path — exercises the OP_* opcodes (head/tail/length/div), which
# can diverge from the primop path v3-eval takes (this caught OP_HEAD/OP_TAIL
# message drift on 2026-08-07 that the v3-eval-only checks missed).
nixv3err() { NIX_V3_DIRECT_EVAL=1 env -u NIX_V3_REQUIRE "$NIX" eval --expr "$1" 2>&1 >/dev/null || true; }
twerr()  { env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$1" 2>&1 >/dev/null || true; }

# ---- NEGATIVE: error fragment must appear in BOTH v3 + TW stderr ----------
neg=(
  # (1) div overflow — primop AND operator, both carry the full message.
  'builtins.div (-9223372036854775807 - 1) (-1)@@@integer overflow in dividing -9223372036854775808 / -1'
  '(-9223372036854775807 - 1) / (-1)@@@integer overflow in dividing -9223372036854775808 / -1'
  # (1) div-by-zero controls (must stay "division by zero").
  'builtins.div 1 0@@@division by zero'
  '(1 / 0)@@@division by zero'
  # (2) floor/ceil out-of-range float.
  'builtins.floor 1.0e300@@@NixFloat argument 1e+300 is not in the range of NixInt'
  'builtins.ceil 1.0e300@@@NixFloat argument 1e+300 is not in the range of NixInt'
  'builtins.floor (-1.0e300)@@@NixFloat argument -1e+300 is not in the range of NixInt'
  # (3) catAttrs non-attrset element.
  'builtins.catAttrs "a" [ {a=1;} 2 ]@@@expected a set but found an integer: 2'
  # (5) head/tail non-list — type error, NOT "empty list".
  'builtins.head 42@@@expected a list but found an integer: 42'
  'builtins.tail 42@@@expected a list but found an integer: 42'
  # (5) empty-list controls (must stay the empty-list message).
  'builtins.head []@@@'"'"'builtins.head'"'"' called on an empty list'
  'builtins.tail []@@@'"'"'builtins.tail'"'"' called on an empty list'
)
for row in "${neg[@]}"; do
  expr="${row%%@@@*}"; frag="${row##*@@@}"
  v3=$(v3err "$expr"); nv3=$(nixv3err "$expr"); tw=$(twerr "$expr")
  # require the fragment on BOTH v3 front-ends (v3-eval primop path AND the nix
  # OP_* opcode path) plus TW, so opcode-vs-primop message drift can't slip through.
  if [[ "$v3" == *"$frag"* && "$nv3" == *"$frag"* && "$tw" == *"$frag"* ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("NEG [$expr] want[$frag] v3[$(printf '%s' "$v3"|head -c 80)] nix[$(printf '%s' "$nv3"|head -c 80)] tw[$(printf '%s' "$tw"|head -c 80)]"); fi
done

# ---- POSITIVE: value byte-identical on v3 + TW ----------------------------
pos=(
  # (1) div controls — value/behaviour unchanged.
  'builtins.div 7 2@@@3'
  'builtins.div 7.0 2.0@@@3.5'
  '(6 / 3)@@@2'
  # (2) floor/ceil controls — in-range values still work; int passthrough.
  'builtins.floor 1.9@@@1'
  'builtins.ceil 1.1@@@2'
  'builtins.floor 5@@@5'
  'builtins.floor 1000000.5@@@1000000'
  'builtins.ceil (-1.5)@@@-1'
  # (3) catAttrs controls — all-attrs, and a valid EMPTY attrset element.
  'builtins.catAttrs "a" [ {a=1;} {b=2;} {a=3;} ]@@@[ 1 3 ]'
  'builtins.catAttrs "a" [ {a=1;} {} ]@@@[ 1 ]'
  # (4) equal-non-orderable prefix → length tiebreak (the headline repro).
  '[ {} ] < [ {} 1 ]@@@true'
  'builtins.lessThan [ {} ] [ {} 1 ]@@@true'
  '[ {} 1 ] < [ {} ]@@@false'
  # (4) sort over lists-of-records with equal leading elements no longer
  # throws (the real-world genericClosure/sort trigger).  Compare via
  # derived scalars — the human printer cosmetically dedups shared empty
  # attrsets (TW `«repeated»`), but the sorted VALUE + ordering are equal.
  'builtins.length (builtins.sort builtins.lessThan [ [ {} 1 ] [ {} ] ])@@@2'
  'builtins.length (builtins.head (builtins.sort builtins.lessThan [ [ {} 1 ] [ {} ] ]))@@@1'
  # (4) ordinary comparison controls — ints / strings / equal + unequal lists.
  '1 < 2@@@true'
  '"a" < "b"@@@true'
  '[1] < [2]@@@true'
  '[1 2] < [1 2]@@@false'
  '[1 2 3] < [1 2]@@@false'
  '[1 2] < [1 2 3]@@@true'
  # (5) head/tail controls — value unchanged.
  'builtins.head [1 2]@@@1'
  'builtins.tail [1 2 3]@@@[ 2 3 ]'
)
for row in "${pos[@]}"; do
  expr="${row%%@@@*}"; want="${row##*@@@}"
  v3=$(v3eval "$expr"); tw=$(tweval "$expr")
  if [[ "$v3" == "$want" && "$tw" == "$want" ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("POS [$expr] want=$want v3=$v3 tw=$tw"); fi
done

# ---- (4) guardrail: genuinely-UNEQUAL non-orderable elements still error
# on BOTH engines (the fix must not over-reach into returning a bool).  The
# error TEXT diverges (v3 primop lessThan emits the terse "expected
# comparable types"; TW emits the full "cannot compare a set with a set;
# values of that type are incomparable ...") — that is a PRE-EXISTING,
# orthogonal divergence, unchanged by this fix — so we only assert that
# neither engine yields a boolean.
for expr in '[ {a=1;} ] < [ {a=2;} ]' 'builtins.lessThan [ {a=1;} ] [ {a=2;} ]'; do
  v3=$(v3eval "$expr"); tw=$(tweval "$expr")
  if [[ "$v3" != "true" && "$v3" != "false" && "$tw" != "true" && "$tw" != "false" ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("ERRBOTH [$expr] v3=$v3 tw=$tw (expected both to error, not a bool)"); fi
done

echo "arith-compare-parity: pass=$pass fail=$fail"
if (( fail > 0 )); then printf '  FAIL %s\n' "${failed[@]}" >&2; exit 1; fi
exit 0
