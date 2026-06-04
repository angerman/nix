#!/usr/bin/env bash
# Regression test for opt #2(B): eager-forced-let.
#
# A `let x = e; in builtins.seq x (… x)` (x forced by seq, also captured by a
# later thunk — the bytecode foldl''s `let next = op acc elem; in seq next
# (go (i+1) next)` shape) should bind x EAGERLY: the per-iteration MkThunk for
# x is eliminated.  This fires in the eval-path strictness pass
# (applyStrictnessAtCallSites), which runs AFTER the `--emit-ir` dump stage —
# so it is NOT visible to the IR-CHECK fixtures and is guarded behaviorally
# here via NIX_VM_OPCOUNTS instead.
#
# Asserts, on fold-add over 1M elements:
#   1. byte-identical result with the opt ON vs OFF (NIX_V3_NO_EAGER_FORCED_LET=1)
#   2. fewer OP_MAKE_THUNK executed with the opt ON (the `next` thunk is gone)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

NIX="${NIX:-./build/src/nix/nix}"
EXPR="builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)"
COMMON=(--impure --expr "$EXPR")
ENV=(NIX_V3_DIRECT_EVAL=1 NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME=120s)

run() { env "$@" "${ENV[@]}" "$NIX" eval "${COMMON[@]}" 2>/tmp/eflt.$$.err; }

makethunks() { grep -E "OP_MAKE_THUNK +[0-9]{6,}" "/tmp/eflt.$$.err" | awk '{print $2}' | sort -rn | head -1; }

echo "== opt #2(B) eager-forced-let regression =="

res_on=$(run)                                ; mt_on=$(makethunks)
res_off=$(run NIX_V3_NO_EAGER_FORCED_LET=1)  ; mt_off=$(makethunks)
rm -f "/tmp/eflt.$$.err"

# 1. correctness — identical result
if [ "$res_on" != "$res_off" ] || [ -z "$res_on" ]; then
    echo "FAIL: result mismatch ON=$res_on OFF=$res_off"; exit 1
fi
echo "  result identical ON/OFF: $res_on  [OK]"

# 2. fewer MAKE_THUNK with the opt on
if [ -z "$mt_on" ] || [ -z "$mt_off" ]; then
    echo "FAIL: could not read OP_MAKE_THUNK counts (ON=$mt_on OFF=$mt_off)"; exit 1
fi
if [ "$mt_on" -ge "$mt_off" ]; then
    echo "FAIL: opt did not reduce MAKE_THUNK (ON=$mt_on OFF=$mt_off)"; exit 1
fi
echo "  OP_MAKE_THUNK ON=$mt_on < OFF=$mt_off  [OK]"

echo "PASS"
