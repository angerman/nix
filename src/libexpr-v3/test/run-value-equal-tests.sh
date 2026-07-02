#!/usr/bin/env bash
# v3 valueEqual regression tests (P3.4 + general equality-engine guard).
#
# valueEqual drives OP_EQ / OP_NEQ and primops (elem/all/…).  P3.4 added a
# scalar fast path (audit §3.5) that returns before the per-compare
# work-stack malloc for the common single-scalar shape; this battery guards
# that the equality RESULT stays byte-identical to the tree-walker across
# scalars, int/float coercion, strings, paths, mixed-type mismatches, lists,
# attrsets, nesting, and `!=`.  There was NO dedicated equality test before;
# this closes that gap.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"   # tree-walker oracle
if [[ ! -x "$V3" ]];  then echo "value-equal: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -x "$NIX" ]]; then echo "value-equal: nix not at $NIX" >&2; exit 2; fi

# Each row: "<expr>@@@<expected>".  Checked v3==expected AND v3==TW (parity).
cases=(
  '1 == 1@@@true'
  '1 == 2@@@false'
  '"a" == "a"@@@true'
  '"a" == "b"@@@false'
  '"" == ""@@@true'
  '1 == 1.0@@@true'
  '1.0 == 1@@@true'
  '2 == 2.5@@@false'
  '1.5 == 1.5@@@true'
  '1.5 == 2.5@@@false'
  'true == true@@@true'
  'true == false@@@false'
  'false == false@@@true'
  'null == null@@@true'
  '1 == "a"@@@false'
  '"x" == null@@@false'
  '1 == null@@@false'
  'null == 1@@@false'
  'true == 1@@@false'
  '[1 2 3] == [1 2 3]@@@true'
  '[1 2] == [1 3]@@@false'
  '[1 2] == [1 2 3]@@@false'
  '[] == []@@@true'
  '{ a = 1; b = 2; } == { a = 1; b = 2; }@@@true'
  '{ a = 1; } == { a = 2; }@@@false'
  '{ a = 1; } == { b = 1; }@@@false'
  '{} == {}@@@true'
  '[ 1 [ 2 3 ] ] == [ 1 [ 2 3 ] ]@@@true'
  '[ 1 [ 2 3 ] ] == [ 1 [ 2 4 ] ]@@@false'
  '{ a = [ 1 { c = 2; } ]; } == { a = [ 1 { c = 2; } ]; }@@@true'
  '1 != 2@@@true'
  '"a" != "a"@@@false'
  '[1 2] != [1 3]@@@true'
  '("ab" + "c") == "abc"@@@true'
  '(let x = 1; in x) == 1@@@true'
  'builtins.elem 3 [ 1 2 3 ]@@@true'
  'builtins.elem 5 [ 1 2 3 ]@@@false'
  'builtins.elem "b" [ "a" "b" ]@@@true'
)

pass=0; fail=0; failed=()
for row in "${cases[@]}"; do
  expr="${row%%@@@*}"; want="${row##*@@@}"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$expr" 2>/dev/null | tail -1)
  tw=$(env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$expr" 2>/dev/null)
  if [[ "$v3" == "$want" && "$tw" == "$want" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("[$expr] want=$want v3=$v3 tw=$tw")
  fi
done

echo "value-equal: pass=$pass fail=$fail"
if (( fail > 0 )); then printf '  FAIL %s\n' "${failed[@]}" >&2; exit 1; fi
exit 0
