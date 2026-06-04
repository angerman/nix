#!/usr/bin/env bash
# Regression test for opt #3: eval/apply (arity-aware uncurried calling).
#
# A curried lambda chain `x: y: … : body` is collapsed to one arity-N Function;
# a saturated N-arg application enters it once with the args in slots 0..N-1 —
# NO per-step partial-application closure.  This is a RUNTIME property (the
# bytecode is similar binary OP_CALLs; the win is the eliminated MAKE_CLOSURE),
# so it is guarded behaviorally here via NIX_VM_OPCOUNTS rather than FileCheck.
#
# Asserts, on fold-add over 1M elements (op = `a: b: a + b`, arity-2):
#   1. byte-identical result with eval/apply ON vs OFF (NIX_V3_EVAL_APPLY).
#   2. far fewer OP_MAKE_CLOSURE executed with eval/apply ON (the per-element
#      partial-application closures for `op acc` and `go (i+1)` are gone).
# Plus correctness of partial-application shapes (stored / inline / isFunction).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

NIX="${NIX:-./build/src/nix/nix}"
V3EVAL="${V3EVAL:-./build/src/libexpr-v3/v3-eval}"
EXPR="builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)"
ENV=(NIX_V3_DIRECT_EVAL=1 NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME=120s)

run() { env "$@" "${ENV[@]}" "$NIX" eval --impure --expr "$EXPR" 2>/tmp/eatest.$$.err; }
makeclosures() { grep -E "OP_MAKE_CLOSURE +[0-9]+" "/tmp/eatest.$$.err" | awk '{print $2}' | sort -rn | head -1; }

echo "== opt #3 eval/apply regression =="

# eval/apply is DEFAULT-ON; opt out with NIX_V3_NO_EVAL_APPLY=1.
res_on=$(run)                       ; mc_on=$(makeclosures)
res_off=$(run NIX_V3_NO_EVAL_APPLY=1) ; mc_off=$(makeclosures)
rm -f "/tmp/eatest.$$.err"

# 1. correctness — identical result
if [ "$res_on" != "$res_off" ] || [ -z "$res_on" ]; then
    echo "FAIL: result mismatch ON=$res_on OFF=$res_off"; exit 1
fi
echo "  fold-add result identical ON/OFF: $res_on  [OK]"

# 2. far fewer MAKE_CLOSURE with eval/apply on (partial-app closures gone)
if [ -z "$mc_on" ] || [ -z "$mc_off" ]; then
    echo "FAIL: could not read OP_MAKE_CLOSURE counts (ON=$mc_on OFF=$mc_off)"; exit 1
fi
if [ "$mc_on" -ge "$mc_off" ]; then
    echo "FAIL: eval/apply did not reduce MAKE_CLOSURE (ON=$mc_on OFF=$mc_off)"; exit 1
fi
echo "  OP_MAKE_CLOSURE ON=$mc_on << OFF=$mc_off  [OK]"

# 3. partial-application correctness (stored / inline / isFunction)
check() { # expr expected
    local got
    got=$(env NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --expr "$1" 2>/dev/null | tail -1)
    if [ "$got" != "$2" ]; then echo "FAIL: '$1' = '$got' (expected '$2')"; exit 1; fi
    echo "  '$1' = $got  [OK]"
}
check 'let f = (a: b: a + b) 10; in f 5'            '15'
check '(a: b: a + b) 3 4'                           '7'
check 'let f = (a: b: c: a+b+c) 1 2; in f 3'        '6'
check 'builtins.isFunction ((a: b: b) 10)'          'true'

echo "PASS"
