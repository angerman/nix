#!/usr/bin/env bash
# Regression test for opt #2(B): eager-forced-let.
#
# A `let x = e; in builtins.seq x (… x)` (x forced by seq, also captured by a
# later thunk — the bytecode foldl''s `let next = op acc elem; in seq next
# (go (i+1) next)` shape) binds x EAGERLY: the per-iteration MkThunk for x is
# eliminated.  This fires in the eval-path strictness pass
# (applyStrictnessAtCallSites).
#
# The optimisation is now UNCONDITIONAL (the NIX_V3_NO_EAGER_FORCED_LET A/B
# opt-out was retired once it shipped byte-identical).  This test is the
# enduring correctness guardrail: the fold-add over 1M elements must produce
# the exact arithmetic result under the eager-forced-let rewrite.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

NIX="${NIX:-./build/src/nix/nix}"
EXPR="builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)"
EXPECTED="499999500000"
ENV=(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=120s)

echo "== opt #2(B) eager-forced-let regression =="

res=$(env "${ENV[@]}" "$NIX" eval --impure --expr "$EXPR" 2>/dev/null)

if [ "$res" != "$EXPECTED" ]; then
    echo "FAIL: result mismatch got=$res expected=$EXPECTED"; exit 1
fi
echo "  fold-add 1M result correct: $res  [OK]"

echo "PASS"
