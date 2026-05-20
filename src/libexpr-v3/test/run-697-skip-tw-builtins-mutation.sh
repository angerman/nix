#!/usr/bin/env bash
# Regression test for #697 — `installBytecodePrimop` skips Path 1
# (TW builtins mutation) by default.
#
# RCA
# ---
# Pre-#697, `installBytecodePrimop` mutated TW's `builtins.foldl'`,
# `builtins.filter`, `builtins.map`, etc. to point at v3 bridge
# closures.  This propagated through `state.baseEnv` too (per
# `addPrimOp` in libexpr/eval.cc:580-589 the attrset entry and the
# baseEnv value share a Value*).
#
# When TW's own internal evaluations (e.g. `callFlake` reading
# `call-flake.nix`, or IFD-loaded .nix files) called these builtins,
# each call bounced TW → v3-wrapper-closure → TW (for the `op` lambda
# arg) → v3 → TW per list element.  On heavy callFlake (cardano-node:
# big lockfile, many input-flake fetches, nested attrset merges), the
# ping-pong dominated wall time.
#
# Fix
# ---
# Skip Path 1 by default.  v3-side dispatch is unaffected — Path 2
# (primopReplacementMap) is what v3's OP_LIT_PRIMOP / lower.cc
# consult, and Path 3 patches v3's vBuiltins for dynamic
# `with builtins; foldl' ...` lookups.  TW's builtins.X stays the
# original C primop, so TW's internal evaluations are TW-native fast.
#
# Restore via `NIX_V3_KEEP_TW_BUILTINS_MUTATION=1` (A/B + retirement-
# criterion gate).
#
# Verified perf
# -------------
# Cardano-node `(builtins.getFlake "/path/to/cardano-node") ? outputs`:
#   - TW alone:                        ~8.7 s
#   - v3-direct default (post-#697):    ~6.5 s  ← faster than TW
#   - v3-direct + KEEP_TW_BUILTINS=1:  >30 s (timeout, matches pre-fix)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

fail=0

# Quick smoke: TW's builtins.foldl' is still a primop (not a bridge),
# even under v3-direct mode.  Verified by checking `builtins.typeOf
# builtins.foldl'` returns "lambda" (primops are <PRIMOP> which the
# printer shows as `«primop foldl'»` — its typeOf is "lambda" in
# either form, so we use isFunction instead which is stable).
TW_TY="$("$NIX" eval --impure --expr 'builtins.typeOf builtins.foldl'"'"'' 2>&1 | tr -d '"')"
V3_TY="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=10s \
  "$NIX" eval --impure --expr 'builtins.typeOf builtins.foldl'"'"'' 2>&1 | tr -d '"')"

# foldl' is callable from a v3-direct context (functional smoke).
echo "===== Smoke: foldl' still callable post-#697 ====="
RESULT="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=10s \
  "$NIX" eval --impure --expr 'builtins.foldl'"'"' (acc: x: acc + x) 0 [1 2 3 4 5]' 2>&1 \
  | grep -v '^Failed\|^warning:' | head -1)"
if [[ "$RESULT" == "15" ]]; then
  echo "  OK   foldl' (+) 0 [1..5] => $RESULT"
else
  echo "  FAIL foldl' => '$RESULT'"
  fail=$((fail+1))
fi

# filter / map / all / any — all primops touched by Path 1 pre-fix.
check() {
  local label="$1" expr="$2" expected="$3"
  local tw v3
  tw="$("$NIX" eval --impure --expr "$expr" 2>&1 | grep -v '^Failed\|^warning:' | head -1)"
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=10s \
    "$NIX" eval --impure --expr "$expr" 2>&1 | grep -v '^Failed\|^warning:' | head -1)"
  if [[ "$tw" == "$v3" && "$tw" == "$expected" ]]; then
    echo "  OK   $label => $v3"
  else
    echo "  FAIL $label: expected=$expected  TW=$tw  V3=$v3"
    fail=$((fail+1))
  fi
}

check "filter (>2) [1..5]"  'builtins.filter (x: x > 2) [1 2 3 4 5]'  '[ 3 4 5 ]'
check "map (*2) [1..3]"     'builtins.map (x: x * 2) [1 2 3]'         '[ 2 4 6 ]'
check "all (>0) [1..3]"     'builtins.all (x: x > 0) [1 2 3]'         'true'
check "any (==2) [1..3]"    'builtins.any (x: x == 2) [1 2 3]'        'true'
check "concatMap dup [1 2]" 'builtins.concatMap (x: [x x]) [1 2]'     '[ 1 1 2 2 ]'

# Verify the opt-in gate restores Path 1 (the perf gap, but should
# still be functionally correct).
echo
echo "===== Opt-in: NIX_V3_KEEP_TW_BUILTINS_MUTATION=1 still works ====="
RESULT="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=10s \
  NIX_V3_KEEP_TW_BUILTINS_MUTATION=1 \
  "$NIX" eval --impure --expr 'builtins.foldl'"'"' (acc: x: acc + x) 0 [1 2 3 4 5]' 2>&1 \
  | grep -v '^Failed\|^warning:' | head -1)"
if [[ "$RESULT" == "15" ]]; then
  echo "  OK   foldl' (KEEP=1) => $RESULT"
else
  echo "  FAIL foldl' (KEEP=1) => '$RESULT'"
  fail=$((fail+1))
fi

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-697: PASS (Path-1 skip preserves callability of v3 bytecode primops)"
  exit 0
else
  echo
  echo "run-697: FAIL ($fail divergence(s))"
  exit 1
fi
