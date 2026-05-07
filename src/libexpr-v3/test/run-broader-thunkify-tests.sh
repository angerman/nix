#!/usr/bin/env bash
# #495 follow-on: regression for the broader-thunkify upvalue bug.
#
# Asserts the documented states (post-#496 + #497 fixes):
#
#   1. TW: returns "x86_64"
#   2. v3 default (NIX_V3_SELF_DOT_MAX_LEVEL implicitly 0): returns "x86_64"
#   3. v3 + NIX_V3_SELF_DOT_MAX_LEVEL=1: returns "x86_64"
#      (was KNOWN-FAIL pre-#497; root cause was eager from-expr
#      lowering for ExprOpUpdate shapes that capture rec/let
#      siblings.  Fix: thunkify ExprOpUpdate from-exprs so the
#      `inherit ({...} // platforms.select final) ...` clause stays
#      lazy across construction, matching TW's `from->maybeThunk`.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"
REPRO="$ROOT/src/libexpr-v3/test/repro-495-broader-thunkify-bug.nix"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi
if [[ ! -e "$REPRO" ]]; then
  echo "reproducer not found at $REPRO" >&2
  exit 1
fi

PASS=0; FAIL=0; fail_names=()

# 1. TW baseline.
tw_out=$(timeout 30 "$NIX_BIN" eval --impure --raw -f "$REPRO" 2>&1)
if echo "$tw_out" | grep -q '^x86_64$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("TW baseline: expected x86_64, got: $(echo "$tw_out" | tail -2)")
fi

# 2. v3 default mode.
v3_out=$(timeout 30 env NIX_USE_V3=1 \
  "$NIX_BIN" eval --impure --raw -f "$REPRO" 2>&1)
if echo "$v3_out" | grep -q '^x86_64$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("v3 default: expected x86_64, got: $(echo "$v3_out" | tail -2)")
fi

# 3. v3 + MAX_LEVEL=1: returns "x86_64" post-#497 (was KNOWN-FAIL).
v3_l1_out=$(timeout 30 env NIX_USE_V3=1 NIX_V3_SELF_DOT_MAX_LEVEL=1 \
  "$NIX_BIN" eval --impure --raw -f "$REPRO" 2>&1)
if echo "$v3_l1_out" | grep -q '^x86_64$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("MAX_LEVEL=1: expected x86_64 (post-#497), got: $(echo "$v3_l1_out" | tail -2)")
fi

# 4. v3 + NIX_V3_NO_COMPLEX_FROM_THUNK=1 + MAX_LEVEL=1: pins the
#    pre-#497 KNOWN-FAIL shape.  Asserts a silent regression to the
#    OPTIONAL no-thunkify path of OpUpdate from-exprs is caught.
v3_l1_no_complex_out=$(timeout 30 env NIX_USE_V3=1 NIX_V3_SELF_DOT_MAX_LEVEL=1 \
  NIX_V3_NO_COMPLEX_FROM_THUNK=1 \
  "$NIX_BIN" eval --impure --raw -f "$REPRO" 2>&1)
v3_l1_no_complex_exit=$?
if echo "$v3_l1_no_complex_out" | grep -q "infinite recursion (blackhole)" \
   || echo "$v3_l1_no_complex_out" | grep -q "OP_ATTRS_SELECT: attribute not found" \
   || [[ $v3_l1_no_complex_exit -ne 0 ]]; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("MAX_LEVEL=1 + NO_COMPLEX_FROM_THUNK=1: expected the pre-#497 failure, got: $(echo "$v3_l1_no_complex_out" | tail -1)")
fi

echo
echo "=== broader-thunkify tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
