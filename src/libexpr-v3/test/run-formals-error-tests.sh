#!/usr/bin/env bash
# v3 formals-validation regression tests (P3.2 + general guard).
#
# OP_CALL / OP_TAIL_CALL formals-validation emits TW-parity errors for a
# non-attrset arg, an unexpected extra arg, and a missing required arg
# (libexpr/eval.cc:1434/1847/1849).  P3.2 (audit §3.1) made the
# `lambdaName` used in those messages LAZY (built only in the error
# branches) and added a needsForce guard on the arg force — both must keep
# the error text + valid-formals results byte-identical to TW.  There was
# no dedicated formals-error test before; this closes that gap.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$V3" ]];  then echo "formals-error: v3-eval not at $V3" >&2; exit 2; fi
if [[ ! -x "$NIX" ]]; then echo "formals-error: nix not at $NIX" >&2; exit 2; fi

pass=0; fail=0; failed=()

# NEGATIVE: error expr @@@ message fragment (must appear in BOTH v3 + TW stderr).
neg=(
  '({ a }: a) { a = 1; b = 2; }@@@called with unexpected argument '"'"'b'"'"''
  '({ a, c }: a) { a = 1; }@@@called without required argument '"'"'c'"'"''
  '({ a }: a) 42@@@expected a set but found an integer: 42'
  '({ a }: a) "x"@@@expected a set but found a string: "x"'
  '({ a, b, c }: a) { a = 1; }@@@called without required argument '"'"'b'"'"''
)
for row in "${neg[@]}"; do
  expr="${row%%@@@*}"; frag="${row##*@@@}"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$expr" 2>&1 >/dev/null || true)
  tw=$(env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$expr" 2>&1 >/dev/null || true)
  if [[ "$v3" == *"$frag"* && "$tw" == *"$frag"* ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("NEG [$expr] want[$frag] v3[$(printf '%s' "$v3"|head -c 90)] tw[$(printf '%s' "$tw"|head -c 90)]"); fi
done

# POSITIVE: valid formals must eval + equal TW (success path — needsForce guard).
pos=(
  '({ a, b ? 5 }: a + b) { a = 10; }@@@15'
  '({ a, b ? 5 }: a + b) { a = 10; b = 20; }@@@30'
  '({ a, ... }: a) { a = 1; b = 2; c = 3; }@@@1'
  '({ a ? 1, b ? 2 }: a + b) {}@@@3'
  '({ stdenv ? 1, lib ? 2, ... }: stdenv + lib) { stdenv = 100; }@@@102'
  '(let f = { x, y }: x * y; in f { x = 6; y = 7; })@@@42'
)
for row in "${pos[@]}"; do
  expr="${row%%@@@*}"; want="${row##*@@@}"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$V3" --expr "$expr" 2>/dev/null | tail -1)
  tw=$(env -u NIX_V3_DIRECT_EVAL "$NIX" eval --expr "$expr" 2>/dev/null)
  if [[ "$v3" == "$want" && "$tw" == "$want" ]]; then pass=$((pass+1))
  else fail=$((fail+1)); failed+=("POS [$expr] want=$want v3=$v3 tw=$tw"); fi
done

echo "formals-error: pass=$pass fail=$fail"
if (( fail > 0 )); then printf '  FAIL %s\n' "${failed[@]}" >&2; exit 1; fi
exit 0
