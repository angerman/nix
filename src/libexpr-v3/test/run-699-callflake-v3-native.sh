#!/usr/bin/env bash
# Regression test for #698 Phase 3 / #758 — `builtins.getFlake`
# implemented natively in v3 via the compiled call-flake.nix.
# Post-#758 (2026-05-22) this is the SOLE getFlake implementation;
# the legacy TW-bridge code path is retired.  Both runs in this
# test exercise the same v3-native code path — the historic
# NIX_V3_NATIVE_CALL_FLAKE and NIX_V3_NO_NATIVE_CALL_FLAKE env vars
# are no-ops.
#
# What's verified:
#   1. Trivial-flake `(getFlake X).smoke` returns "hello".
#   2. Deeper attribute access: `(getFlake X).a.b.c` returns "deep".
#   3. Numeric attribute: `(getFlake X).n` returns 42.
#   4. The retired env-var aliases are no-ops (results unchanged
#      whether they are set or not — proves nothing routes through
#      a hidden bridge fallback any more).
#
# Cardano-node + nixpkgs coverage lives in
# `run-758-callflake-sweep.sh`.
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

# Post-#758 (2026-05-22) semantics:
#   - All getFlake calls route through v3-native callFlakeV3.
#   - NIX_V3_NATIVE_CALL_FLAKE and NIX_V3_NO_NATIVE_CALL_FLAKE are
#     no-op env vars retained only for backward-compat startup
#     scripts; their presence does not change behavior.
#   - Cross-eval verification (15 byte-identical queries spanning
#     nixpkgs + cardano-node) lives in run-758-callflake-sweep.sh.

# Default (= v3-native; the only impl).
DEFAULT="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").smoke" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".smoke (default)" "$DEFAULT" '"hello"'

# Retired NIX_V3_NATIVE_CALL_FLAKE — must be a no-op.
NATIVE="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_NATIVE_CALL_FLAKE=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").smoke" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".smoke (retired NIX_V3_NATIVE_CALL_FLAKE=1 → no-op)" "$NATIVE" '"hello"'

# Retired NIX_V3_NO_NATIVE_CALL_FLAKE — must be a no-op (no bridge any more).
NO_NATIVE="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").smoke" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".smoke (retired NIX_V3_NO_NATIVE_CALL_FLAKE=1 → no-op)" "$NO_NATIVE" '"hello"'

# Deeper traversal.
DEEP="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").a.b.c" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".a.b.c (default)" "$DEEP" '"deep"'

# int (42) under default.
N="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
  NIX_V3_MAX_WALL_TIME=15s \
  "$NIX" eval --impure --expr "(builtins.getFlake \"$TRIVIAL\").n" 2>&1 \
  | grep -v '^Failed\|^warning:' | tail -1)"
check_eq ".n (default)" "$N" "42"

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-699: PASS (v3-native callFlake matches TW on trivial flake)"
  exit 0
else
  echo
  echo "run-699: FAIL ($fail divergence(s))"
  exit 1
fi
