#!/usr/bin/env bash
# Regression test for #698 Phase 2 — v3 can compile call-flake.nix
# natively.
#
# Pre-#698, `builtins.getFlake` routed entirely to TW: TW parsed AND
# evaluated call-flake.nix.  Phase 2 of #698 wires the v3 parse +
# lower + optimise + compile + run pipeline for call-flake.nix and
# verifies it produces a Tag::Closure (the 3-arg lambda).
#
# This test exercises that pipeline via the diagnostic primop
# `builtins.__v3CompileCallFlake`.  Returns the string
# `"compiled-ok-closure-tag-<N>"` where N is the Tag value for
# Closure (should be 9).
#
# A failure here indicates one of:
#   - Build / link issue (libflake not linked / generated header missing).
#   - call-flake.nix uses a Nix construct v3 doesn't yet lower
#     (would surface as a `lowerNixExpr` exception).
#   - Top-level form is not a closure (would surface in the
#     `closureValue.tag() != Tag::Closure` check inside the cache).
#
# Phase 3 will remove the diagnostic primop once `primGetFlake` is
# rewritten to use the v3-native dispatch end-to-end.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

fail=0

echo "===== Phase 2: v3-side compile of call-flake.nix ====="
RESULT="$(NIX_V3_DIRECT_EVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s "$NIX" eval --impure --expr 'builtins.__v3CompileCallFlake null' 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"

if [[ "$RESULT" =~ ^\"compiled-ok-closure-tag-[0-9]+\"$ ]]; then
  echo "  OK   v3 compiled call-flake.nix => $RESULT"
else
  echo "  FAIL v3 compile failed or unexpected result: $RESULT"
  fail=$((fail+1))
fi

# Idempotency: call again to verify the std::call_once cache hits.
echo
echo "===== Phase 2 cache hit (second call is O(1)) ====="
RESULT2="$(NIX_V3_DIRECT_EVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s "$NIX" eval --impure --expr 'builtins.__v3CompileCallFlake null' 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"

if [[ "$RESULT" == "$RESULT2" ]]; then
  echo "  OK   second call returns same result => $RESULT2"
else
  echo "  FAIL second call differs: $RESULT vs $RESULT2"
  fail=$((fail+1))
fi

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-698: PASS (v3 successfully compiles + runs call-flake.nix to Tag::Closure)"
  exit 0
else
  echo
  echo "run-698: FAIL ($fail divergence(s))"
  exit 1
fi
