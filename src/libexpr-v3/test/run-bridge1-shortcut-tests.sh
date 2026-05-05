#!/usr/bin/env bash
# #458 step 2 — bridge1 short-circuit positive/negative/regression tests.
#
# The short-circuit (NIX_V3_NO_BRIDGE1_SHORTCIRCUIT=1 to disable)
# intercepts TW-side `callFunction(__v3_call_bridge_1, arg)` dispatches
# at the v3CallFunctionHook entry and routes them DIRECTLY through
# v3's callClosure, skipping TW's primop dispatch + bridge1's eager
# arg-force (the cardano-node #455 cycle source).
#
# Tests cover:
#   regression: parity with shortcut on/off across realistic shapes
#                that DO produce bridge1 PrimOpApps (NIX_V3_BRIDGE_CLOSURE
#                forces the v3->TW closure bridge regardless of Phase E).
#   positive:    bridge1 PrimOpApp evaluates correctly under shortcut.
#   negative:    arg-force-throw shape behaves identically to with
#                shortcut off (both either raise or absorb the throw,
#                depending on closure body — what matters is parity).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# A: simple v3 closure round-tripped through TW via mkPrimOpApp(bridge1).
cat > "$TMP/a.nix" <<'EOF'
let
  add = n: x: x + n;
  add5 = add 5;
in [(add5 10) (add5 20) (add5 30)]
EOF
EXPECT_A='[ 15 25 35 ]'

# B: arity-2 closure exercised (covers both `mkAdd 5` partial and final apply).
cat > "$TMP/b.nix" <<'EOF'
let
  comp = f: g: x: f (g x);
  inc = x: x + 1;
  dbl = x: x * 2;
  incThenDbl = comp dbl inc;
in incThenDbl 7
EOF
EXPECT_B='16'

# C: closure that does NOT force its arg — laziness preserved by the
#    Bridge-thunk wrap path.  Exercises the same code as LAZY_BRIDGE_ARG
#    tests but specifically through the bridge1 dispatch.
cat > "$TMP/c.nix" <<'EOF'
let
  ignore = x: 42;
in ignore (throw "should never force")
EOF
EXPECT_C='42'

# D: closure that DOES force its arg — Bridge thunk forces on demand
#    via the v3-side cycle through the TW arg-Value.
cat > "$TMP/d.nix" <<'EOF'
let
  use = x: x + 10;
in use (1 + 2)
EOF
EXPECT_D='13'

ok=0
fail=0
fail_names=()

run_one() {
  local label="$1"; shift
  local nix_file="$1"; shift
  local expected="$1"; shift
  local extra_env=("$@")

  local got
  got=$(env "${extra_env[@]}" "$NIX_BIN" eval --no-eval-cache -f "$nix_file" 2>/dev/null) \
    || got="<error>"
  if [[ "$got" == "$expected" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$label  expected=$expected  got=$got")
  fi
}

# Modes:
#   v3-default     : phase E + bridge1 shortcut on (default).
#   v3-no-shortcut : phase E + shortcut off (NIX_V3_NO_BRIDGE1_SHORTCIRCUIT=1)
#   v3-bridge      : NIX_V3_BRIDGE_CLOSURE=1 (force bridge regardless) + shortcut on
#   v3-bridge-noshc: NIX_V3_BRIDGE_CLOSURE=1 + shortcut off
modes=(
  "v3-default::NIX_USE_V3=1"
  "v3-no-shortcut::NIX_USE_V3=1 NIX_V3_NO_BRIDGE1_SHORTCIRCUIT=1"
  "v3-bridge::NIX_USE_V3=1 NIX_V3_BRIDGE_CLOSURE=1"
  "v3-bridge-noshc::NIX_USE_V3=1 NIX_V3_BRIDGE_CLOSURE=1 NIX_V3_NO_BRIDGE1_SHORTCIRCUIT=1"
)

for spec in "${modes[@]}"; do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/A" "$TMP/a.nix" "$EXPECT_A" "${envarr[@]}"
  run_one "$tag/B" "$TMP/b.nix" "$EXPECT_B" "${envarr[@]}"
  run_one "$tag/C" "$TMP/c.nix" "$EXPECT_C" "${envarr[@]}"
  run_one "$tag/D" "$TMP/d.nix" "$EXPECT_D" "${envarr[@]}"
done

# Compare TW baseline for parity across modes.
TW=$("$NIX_BIN" eval --no-eval-cache -f "$TMP/a.nix" 2>/dev/null)
if [[ "$TW" != "$EXPECT_A" ]]; then
  fail=$((fail + 1))
  fail_names+=("tw/A  expected=$EXPECT_A  got=$TW")
else
  ok=$((ok + 1))
fi

echo "=== bridge1 shortcut tests: ok=$ok fail=$fail (total=$((ok+fail))) ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
