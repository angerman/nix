#!/usr/bin/env bash
# v3 primop / opcode EDGE-CASE TW-parity regression net (production hardening).
#
# A curated positive+negative net over the highest-risk *dual-path* primops
# (each has BOTH an OP_* opcode fast path AND a builtins.* primop path — the
# class that produced the div INT64_MIN/-1, head/tail, and readFile/hashFile
# divergences) exercised on boundary inputs.  Every case was empirically
# confirmed byte-identical across all three engines on 2026-08-08:
#
#   P = v3-eval          (the primop path)
#   O = nix + NIX_V3_DIRECT_EVAL=1   (the OP_* opcode path — the split-catcher)
#   T = nix (TW)         (the tree-walker oracle)
#
# POSITIVE rows assert the value is byte-identical on P, O, AND T (so a value
# split between the opcode and primop paths can't slip through — a strengthening
# over run-arith-compare-parity's P-vs-T-only positives).  NEGATIVE rows assert
# the error fragment appears on P, O, AND T.
#
# Boundary inputs covered: INT64_MIN bit/mul, truncating div sign, negative /
# out-of-range substring, empty-pattern + overlapping replaceStrings, UTF-8
# stringLength, split group count, dup-key listToAttrs (first wins), attrValues
# key-order, forced genList/mapAttrs, compareVersions, toJSON int-vs-float,
# list concat; and negative: missing attr (primop + opcode select), elemAt OOB,
# negative genList, negative substring, seq error propagation, replaceStrings
# arity mismatch.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$V3" ]];  then echo "primop-edge-parity: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -x "$NIX" ]]; then echo "primop-edge-parity: nix not at $NIX" >&2; exit 2; fi
export _NIX_TEST_NO_ENVIRONMENT_WARNINGS=1

pass=0; fail=0; failed=()

v3eval()  { NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$1" 2>/dev/null | tail -1; }
nixv3val(){ NIX_V3_DIRECT_EVAL=1 env -u NIX_V3_REQUIRE "$NIX" eval --expr "$1" 2>/dev/null; }
tweval()  { env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$1" 2>/dev/null; }
v3err()   { NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$1" 2>&1 >/dev/null || true; }
nixv3err(){ NIX_V3_DIRECT_EVAL=1 env -u NIX_V3_REQUIRE "$NIX" eval --expr "$1" 2>&1 >/dev/null || true; }
twerr()   { env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$1" 2>&1 >/dev/null || true; }

# ---- POSITIVE: value byte-identical on P (primop), O (opcode), and T (TW) ----
pos=(
  # INT64_MIN + overflow-wrap bit/mul (must match TW's wrap, not clamp/UB).
  'builtins.bitAnd (-1) 255@@@255'
  'builtins.bitOr 0 (-1)@@@-1'
  'builtins.bitXor 5 3@@@6'
  'builtins.bitAnd (-9223372036854775807 - 1) (-1)@@@-9223372036854775808'
  '2147483648 * 2147483648@@@4611686018427387904'
  # div truncates toward zero (sign cases).
  'builtins.div 7 (-3)@@@-2'
  'builtins.div (-7) 3@@@-2'
  # substring: negative length = "to end"; out-of-range start = "".
  'builtins.substring 2 (-1) "hello"@@@"llo"'
  'builtins.substring 10 5 "hi"@@@""'
  # replaceStrings: empty pattern inserts between every char; first-match wins.
  'builtins.replaceStrings [""] ["X"] "ab"@@@"XaXbX"'
  'builtins.replaceStrings ["ana"] ["X"] "banana"@@@"bXna"'
  # UTF-8 byte length; split group count.
  'builtins.stringLength "café"@@@5'
  'builtins.length (builtins.split "(a)" "aba")@@@5'
  # listToAttrs dup key: FIRST wins; attrValues in key order.
  '(builtins.listToAttrs [{name="a";value=1;}{name="a";value=2;}]).a@@@1'
  'builtins.elemAt (builtins.attrValues {b=2;a=1;c=3;}) 0@@@1'
  # forced genList / mapAttrs elements (avoid the lazy-thunk printer).
  'builtins.elemAt (builtins.genList (x: x*x) 5) 3@@@9'
  'builtins.concatStringsSep "," (builtins.attrValues (builtins.mapAttrs (n: v: n) {b=1;a=2;c=3;}))@@@"a,b,c"'
  # zipAttrsWith over valid sets (incl a valid EMPTY set {} which contributes nothing).
  'builtins.length (builtins.attrNames (builtins.zipAttrsWith (n: vs: vs) [{a=1;} {a=2;b=3;}]))@@@2'
  'builtins.length (builtins.attrNames (builtins.zipAttrsWith (n: vs: vs) [{a=1;} {}]))@@@1'
  'builtins.concatStringsSep "," (map toString (builtins.attrValues {b=2;a=1;c=3;}))@@@"1,2,3"'
  # compareVersions; toJSON int-vs-float; strict foldl; list concat length.
  'builtins.compareVersions "1.0" "1.0.1"@@@-1'
  'builtins.toJSON 1.5@@@"1.5"'
  'builtins.toJSON (6 / 3)@@@"2"'
  'builtins.foldl'"'"' (a: b: a - b) 0 [1 2 3]@@@-6'
  'builtins.length ([1 2 3] ++ [4 5])@@@5'
)
for row in "${pos[@]}"; do
  expr="${row%%@@@*}"; want="${row##*@@@}"
  v3=$(v3eval "$expr"); o=$(nixv3val "$expr"); tw=$(tweval "$expr")
  if [[ "$v3" == "$want" && "$o" == "$want" && "$tw" == "$want" ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("POS [$expr] want=$want P=$v3 O=$o T=$tw"); fi
done

# ---- NEGATIVE: error fragment on P (primop), O (opcode), and T (TW) ---------
neg=(
  # missing attr — primop getAttr AND opcode OP_ATTRS_SELECT (dot) paths.
  'builtins.getAttr "b" {a=1;}@@@attribute '"'"'b'"'"' missing'
  '({a=1;}).b@@@attribute '"'"'b'"'"' missing'
  'builtins.elemAt [1 2 3] 5@@@called with index 5 on a list of size 3'
  'builtins.genList (x:x) (-1)@@@cannot create list of size -1'
  'builtins.substring (-1) 3 "hello"@@@negative start position'
  'builtins.seq (throw "boom") 2@@@boom'
  'builtins.replaceStrings ["a"] [] "abc"@@@arguments passed to builtins.replaceStrings have different lengths'
  # zipAttrsWith must forceAttrs each element — non-attrset throws (WS-D fuzzer 2026-08-09),
  # not fail-open by skipping (v3 pre-fix returned {} / dropped the element).
  'builtins.zipAttrsWith (n: vs: vs) [1]@@@expected a set but found an integer: 1'
  'builtins.zipAttrsWith (n: vs: vs) [{a=1;} 2]@@@expected a set but found an integer: 2'
)
for row in "${neg[@]}"; do
  expr="${row%%@@@*}"; frag="${row##*@@@}"
  v3=$(v3err "$expr"); nv3=$(nixv3err "$expr"); tw=$(twerr "$expr")
  if [[ "$v3" == *"$frag"* && "$nv3" == *"$frag"* && "$tw" == *"$frag"* ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("NEG [$expr] want[$frag] P[$(printf '%s' "$v3"|head -c 80)] O[$(printf '%s' "$nv3"|head -c 80)] T[$(printf '%s' "$tw"|head -c 80)]"); fi
done

echo "primop-edge-parity: pass=$pass fail=$fail"
if (( fail > 0 )); then printf '  FAIL %s\n' "${failed[@]}" >&2; exit 1; fi
exit 0
