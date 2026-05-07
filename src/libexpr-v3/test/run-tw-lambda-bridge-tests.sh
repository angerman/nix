#!/usr/bin/env bash
# #493: regression for the TW-lambda Value bridge (NIX_V3_TW_LAMBDA_BRIDGE=1).
#
# Validates that under lambda-skip + TW-lambda bridge, the formals-closure
# bridge correctly:
#   - constructs a Tag::tLambda whose `lambda.fun` is the original
#     ExprLambda (autoCallFunction works)
#   - dispatches calls via tryDispatchFormalsLambdaBridge with the v3
#     closure's captured upvalues
#   - peeks through Bridge thunks for type predicates (isFunction,
#     primFunctionArgs)
#
# Pre-#493 behaviour: v3ToTreeWalker REFUSED formals closures and threw
# BlackholeError → fallback re-eval through TW.  The bridge replaces
# this with a real Tag::tLambda dispatch.
#
# Tests run in three modes:
#   - TW (no v3)
#   - v3 default
#   - v3 lambda-skip + TW-lambda bridge
# All three should produce the same result.
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

PASS=0; FAIL=0
fail_names=()

assert_eq() {
  local name="$1" expected="$2" got="$3"
  if [[ "$expected" == "$got" ]]; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("$name: expected=$expected got=$got")
  fi
}

run_eval() {
  "$NIX_BIN" eval --impure -f "$1" 2>&1 || true
}

run_three_modes() {
  local label="$1" file="$2"
  local tw v3 v3bridge
  tw=$(run_eval "$file")
  v3=$(NIX_USE_V3=1 run_eval "$file")
  v3bridge=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 NIX_V3_TW_LAMBDA_BRIDGE=1 \
              run_eval "$file")
  assert_eq "$label TW=v3-default" "$tw" "$v3"
  assert_eq "$label TW=v3-bridge"  "$tw" "$v3bridge"
}

# ----------------------------------------------------------------------
# p1 — basic formals-call.  The lambda has formals; without the bridge,
# v3ToTreeWalker refuses with BlackholeError.  With the bridge, it
# constructs a Tag::tLambda, autoCallFunction sees formals, callFunction
# dispatches to v3 via tryDispatchFormalsLambdaBridge.
cat > "$TMP/p1.nix" <<'EOF'
let
  cfg = { name, value ? 42 }: { ${name} = value; };
in (cfg { name = "x"; value = 5; }).x
EOF
run_three_modes "p1 formals-call" "$TMP/p1.nix"

# ----------------------------------------------------------------------
# p2 — formals-call with default value.  Exercises autoCallFunction's
# missing-formal default-fill path through the bridge.
cat > "$TMP/p2.nix" <<'EOF'
let
  cfg = { name, value ? 42 }: { ${name} = value; };
in (cfg { name = "x"; }).x
EOF
run_three_modes "p2 formals-default" "$TMP/p2.nix"

# ----------------------------------------------------------------------
# p3 — fix-point with overlay.  Each overlay is a formals lambda that
# must bridge correctly so the fix-point's recursive self-reference
# resolves.
cat > "$TMP/p3.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  ext = self: { a = 1; b = self.a + 10; };
in (fix ext).b
EOF
run_three_modes "p3 fix-point" "$TMP/p3.nix"

# ----------------------------------------------------------------------
# p4 — extends overlay chain.  Each layer's formals lambda is bridged.
# Tests bridge1 reentry counter (#493 step 3b): legitimate same-handle
# re-entries through the overlay chain must NOT trip the cycle detector.
cat > "$TMP/p4.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  extends = f: rattrs: self:
    let super = rattrs self;
    in super // f self super;
  base = self: { a = 1; b = self.a + 10; };
  overlay1 = self: super: { c = super.b * 2; };
  overlay2 = self: super: { d = self.c + super.a; };
  result = fix (extends overlay2 (extends overlay1 base));
in result.d
EOF
run_three_modes "p4 extends-chain" "$TMP/p4.nix"

# ----------------------------------------------------------------------
# p5 — formals lambda passed through builtins.import-style indirection.
# Exercises the OP_IS_FUNCTION Bridge-peek (#493 step 3a): the bridged
# lambda is checked via isFunction; the peek must answer true so the
# `if isFunction X then X args else import X` pattern routes correctly.
cat > "$TMP/p5_module.nix" <<'EOF'
{ pkg ? "default", count ? 1 }:
{ inherit pkg count; }
EOF
cat > "$TMP/p5.nix" <<'EOF'
let
  loadModule = m:
    if builtins.isFunction m
    then m { pkg = "explicit"; count = 3; }
    else throw "not a function";
in (loadModule (import ./p5_module.nix)).pkg
EOF
run_three_modes "p5 isFunction-bridge-peek" "$TMP/p5.nix"

# ----------------------------------------------------------------------
# p6 — builtins.functionArgs on a bridged formals lambda.  Exercises
# the primFunctionArgs Bridge-peek (#493 step 3a).
cat > "$TMP/p6.nix" <<'EOF'
let
  cfg = { a, b ? 10, c ? 20 }: a + b + c;
  args = builtins.functionArgs cfg;
in builtins.attrNames args
EOF
run_three_modes "p6 functionArgs-bridge-peek" "$TMP/p6.nix"

# ----------------------------------------------------------------------
echo
echo "=== tw-lambda-bridge tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
