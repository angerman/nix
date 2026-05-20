#!/usr/bin/env bash
# Regression test for #698 Phase 3 — `NIX_V3_NATIVE_CALL_FLAKE=1`
# routes `builtins.getFlake` through v3's own compiled
# call-flake.nix.
#
# What's verified:
#   1. Default (post-#697 bridge): trivial-flake `(getFlake X).smoke`
#      returns "hello".  Regression guard for the proven-fast path.
#   2. Opt-in v3-native: same expression returns "hello".  Verifies
#      end-to-end the v3-side parse + lower + compile + run dispatch
#      + TW args build + bridge + callClosure × 3 chain works on a
#      minimal real flake.
#   3. Deeper attribute access: `(getFlake X).a.b.c` returns "deep"
#      under v3-native, verifying attrset traversal post-bridge.
#   4. fallback semantics: with no flakeSettings wired (e.g. v3-eval
#      standalone) the path silently falls back to the bridge.  We
#      can't test this directly via the nix CLI (which always wires
#      flakeSettings), so we trust the in-code null-check.
#
# This file does NOT test cardano-node because the v3-native path
# currently has a perf gap there (~6× TW; see Phase 3 commit message).
# That gap is the retirement criterion for the opt-in gate.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

# Pre-flight: the trivial-flake fixture must exist.  It's a 5-line
# Nix file at /private/tmp/trivial-flake created earlier in the
# session.  Recreate if missing.
TRIVIAL=/private/tmp/trivial-flake
if [[ ! -f "$TRIVIAL/flake.nix" ]]; then
  mkdir -p "$TRIVIAL"
  cat > "$TRIVIAL/flake.nix" <<'EOF'
{
  description = "trivial";
  outputs = _: {
    smoke = "hello";
    n = 42;
    a = { b = { c = "deep"; }; };
  };
}
EOF
  # Auto-lock if needed.
  "$NIX" flake lock --extra-experimental-features 'flakes nix-command' "$TRIVIAL" 2>/dev/null || true
fi

fail=0

check_eq() {
  local label="$1" got="$2" want="$3"
  if [[ "$got" == "$want" ]]; then
    echo "  OK   $label => $got"
  else
    echo "  FAIL $label: want=$want got=$got"
    fail=$((fail+1))
  fi
}

# Post-#700-default-flip semantics:
#   - Default is now v3-native (no env var needed).
#   - Opt-OUT to the bridge via NIX_V3_NO_NATIVE_CALL_FLAKE=1.

# v3-direct default (NOW v3-native by default)
DEFAULT="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").smoke" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".smoke (default = v3-native)" "$DEFAULT" '"hello"'

# v3-direct + opt-out to bridge (emergency-rollback path)
BRIDGE="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").smoke" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".smoke (opt-out = bridge)" "$BRIDGE" '"hello"'

# Deeper traversal under v3-native (catches bridge / shallow-attrset issues)
DEEP="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").a.b.c" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".a.b.c (default = v3-native)" "$DEEP" '"deep"'

# Same query under opt-out — must agree.
DEEP_BRIDGE="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").a.b.c" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".a.b.c (v3-native vs bridge parity)" "$DEEP" "$DEEP_BRIDGE"

# int (42) under v3-native default
N="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").n" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".n (default = v3-native)" "$N" "42"

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-699: PASS (v3-native callFlake matches TW on trivial flake)"
  exit 0
else
  echo
  echo "run-699: FAIL ($fail divergence(s))"
  exit 1
fi
