#!/usr/bin/env bash
# Regression test for opt #3: eval/apply (arity-aware uncurried calling).
#
# A curried lambda chain `x: y: … : body` is collapsed to one arity-N Function;
# a saturated N-arg application enters it once with the args in slots 0..N-1 —
# NO per-step partial-application closure.  This is now UNCONDITIONAL (the
# NIX_V3_NO_EVAL_APPLY A/B opt-out was retired once it shipped byte-identical).
#
# The eliminated-MAKE_CLOSURE win was previously A/B-measured against the
# opt-out arm; with the opt-out gone this test is the enduring CORRECTNESS
# guardrail: the fold-add result and the partial-application shapes (stored /
# inline / isFunction) must all evaluate correctly under eval/apply.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

NIX="${NIX:-./build/src/nix/nix}"
V3EVAL="${V3EVAL:-./build/src/libexpr-v3/v3-eval}"
EXPR="builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)"
ENV=(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=120s)

echo "== opt #3 eval/apply regression =="

# 1. correctness — the arity-2 fold-add over 1M elements
res=$(env "${ENV[@]}" "$NIX" eval --impure --expr "$EXPR" 2>/dev/null)
if [ "$res" != "499999500000" ]; then
    echo "FAIL: fold-add result got=$res expected=499999500000"; exit 1
fi
echo "  fold-add 1M result correct: $res  [OK]"

# 2. partial-application correctness (stored / inline / isFunction)
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
