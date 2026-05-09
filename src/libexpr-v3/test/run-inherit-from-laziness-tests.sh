#!/usr/bin/env bash
# #466 / #482 — regression tests for `inherit (FROM) names` laziness.
#
# Tree-walker emits `from->maybeThunk(state, up)` for the FROM expression
# (eval.cc:1520, buildInheritFromEnv): each from-expr is thunkified so
# its evaluation is deferred until at least one of the inherited names
# is actually demanded.  v3's lower.cc had `pushInheritFromCache` calling
# `lowerExpr(...)` directly -- emitting the FROM evaluation EAGERLY in
# the enclosing block.  When the enclosing block is a lambda body that
# v3 owns (lambda-skip on, or any "v3 owns more" path), the eager
# evaluation runs DURING attrset construction.  If the FROM expression
# touches a self-referential rec/let-rec value that's mid-construction
# (e.g. `inherit (self.X) Y` inside `self: ...` passed to fix-point
# helpers like `makeExtensible'`), the eager force trips on a thunk
# that's currently Black on the outer frame -- surfaces as
# "OP_ATTRS_SELECT: attribute not found" because the outer let-bindings
# scope (size=1, just `callLibs`) is what ends up on the stack instead
# of the lambda's parameter.
#
# Fix: thunkify each from-expression in pushInheritFromCache, matching
# TW's maybeThunk semantics.  Sharing across the N inherited names is
# preserved (one Thunk Value referenced by N AttrDefs).
#
# This test exercises:
#   p1 simple inherit-from (positive): `let X = {a=1;}; in { inherit (X) a; }`.a
#       Should produce 1 in both modes (default + lambda-skip).
#   p2 inherit-from-self in fix-point (POSITIVE under #466 / #482 fix):
#       The lib/default.nix shape -- `makeExtensible'` + `self: { ...
#       inherit (self.trivial) ver; }`.  Reproduces the failure.
#   p3 NEGATIVE: `inherit (THROW) name` with name unused -- TW lazy
#       semantics keep it lazy and don't throw.  v3 must match.
#   p4 NEGATIVE: `inherit (THROW) name` with name USED -- TW evaluates
#       and throws.  v3 must match.
#   p5 sharing: `inherit (sideEffect) a b c` where sideEffect bumps a
#       counter -- TW evaluates ONCE for all N names.  Verify v3
#       behavior parity.
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
  local file="$1"; shift
  "$NIX_BIN" eval --impure -f "$file" 2>&1 || true
}

# Run with NIX_VM_STATS=1 and return number of v3 eval fallback "runThrew"
# events.  Any non-zero count means v3 attempted the eval, threw mid-way,
# and TW fallback masked the failure.  Used by the lambda-skip tests to
# assert v3 owns the evaluation cleanly (no silent fallback).
count_run_threw() {
  local file="$1"
  local out
  out=$(NIX_VM_STATS=1 "$NIX_BIN" eval --impure -f "$file" 2>&1 || true)
  local n
  n=$(printf '%s\n' "$out" | awk '/runThrew[ \t]+[0-9]+/ {print $2; exit}')
  echo "${n:-0}"
}

# ----------------------------------------------------------------------
# p1 — simple inherit-from positive
cat > "$TMP/p1.nix" <<'EOF'
let X = { a = 1; b = 2; };
in (rec { inherit (X) a b; }).a
EOF

p1_tw=$(run_eval "$TMP/p1.nix")
p1_v3=$(NIX_USE_V3=1 run_eval "$TMP/p1.nix")
p1_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p1.nix")
assert_eq "p1 TW=v3-default" "$p1_tw" "$p1_v3"
assert_eq "p1 TW=v3-skip"    "$p1_tw" "$p1_v3_skip"

# ----------------------------------------------------------------------
# p2 — fix-point inherit-from (the lib/default.nix shape) — POSITIVE
cat > "$TMP/p2-trivial.nix" <<'EOF'
{ lib }: { ver = "1.0"; }
EOF
cat > "$TMP/p2.nix" <<EOF
let
  makeExtensible' = rattrs:
    let self = rattrs self // { extend = f: null; };
    in self;
  lib = makeExtensible' (
    self:
    let
      callLibs = file: import file { lib = self; };
    in {
      trivial = callLibs $TMP/p2-trivial.nix;
      inherit (self.trivial) ver;
    });
in lib.ver
EOF

p2_tw=$(run_eval "$TMP/p2.nix")
p2_v3=$(NIX_USE_V3=1 run_eval "$TMP/p2.nix")
p2_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p2.nix")
assert_eq "p2 TW=v3-default" "$p2_tw" "$p2_v3"
assert_eq "p2 TW=v3-skip (lambda-skip + fix-point inherit-from)" \
    "$p2_tw" "$p2_v3_skip"

# p2 strict — v3 owns the evaluation without triggering the WC-1
# `run threw` TW fallback.  History:
#   #466/#482 fix: lambda-skip + thunkifyForAttr keeps from-exprs
#       lazy, eliminating eager mid-construction force.  At that
#       point the self-referential force was masked by the
#       partialBindings recovery returning the inner lambda body's
#       attrset — which happened to be the right shape for
#       self.trivial access, so v3 owned the evaluation cleanly.
#   #496 fix (2026-05-07): partialBindings registration restricted
#       to OP_ATTRS_REC_INIT.  Side-effect: this lambda-skip path
#       lost the partial-bindings safety net so the mid-construction
#       force surfaced as BlackholeError → TW fallback (runThrew=1).
#   #530 fix (2026-05-08): lexical-with chain materialises
#       capturedWiths statically at MAKE_THUNK time, so the inner
#       inherit-from thunk no longer trips the mid-construction
#       force.  v3 owns the evaluation cleanly: runThrew=0.
#
# Regression asserts runThrew=0 — going back to 1 means we regressed
# either the lexical chain or the from-expr lazy thunkify path.
p2_v3_skip_runthrew=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 count_run_threw "$TMP/p2.nix")
assert_eq "p2 v3-skip runThrew=0 (v3 owns eval, post-#530)" "0" "$p2_v3_skip_runthrew"

# ----------------------------------------------------------------------
# p3 — NEGATIVE: inherit-from with unused name (must stay lazy).  TW
# laziness keeps the throw side dormant; v3 must match.
cat > "$TMP/p3.nix" <<'EOF'
let X = { a = 1; b = throw "boom"; };
in (rec { inherit (X) a b; }).a
EOF

p3_tw=$(run_eval "$TMP/p3.nix")
p3_v3=$(NIX_USE_V3=1 run_eval "$TMP/p3.nix")
p3_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p3.nix")
assert_eq "p3 TW=v3-default (unused throw stays lazy)" "$p3_tw" "$p3_v3"
assert_eq "p3 TW=v3-skip (unused throw stays lazy)"    "$p3_tw" "$p3_v3_skip"

# ----------------------------------------------------------------------
# p4 — NEGATIVE: inherit-from with USED name from a throw expr.  Both
# engines should evaluate and throw.  We just check both produce a
# non-empty error matching "boom".
cat > "$TMP/p4.nix" <<'EOF'
let X = { a = throw "boom"; };
in (rec { inherit (X) a; }).a
EOF

p4_tw=$(run_eval "$TMP/p4.nix")
p4_v3=$(NIX_USE_V3=1 run_eval "$TMP/p4.nix")
p4_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p4.nix")
[[ "$p4_tw" == *boom* ]] && p4_tw_ok=1 || p4_tw_ok=0
[[ "$p4_v3" == *boom* ]] && p4_v3_ok=1 || p4_v3_ok=0
[[ "$p4_v3_skip" == *boom* ]] && p4_v3_skip_ok=1 || p4_v3_skip_ok=0
assert_eq "p4 TW throws" "1" "$p4_tw_ok"
assert_eq "p4 v3 throws" "1" "$p4_v3_ok"
assert_eq "p4 v3-skip throws" "1" "$p4_v3_skip_ok"

# ----------------------------------------------------------------------
# p5 — sharing: from-expression evaluated once for N inherited names.
# We can't directly count side effects in pure Nix, but we can verify
# the result matches across engines, and the from-expr produces a
# stable value (the cache returns the same VarId / thunk).
cat > "$TMP/p5.nix" <<'EOF'
let
  src = { a = 1; b = 2; c = 3; };
  s = rec { inherit (src) a b c; sum = a + b + c; };
in s.sum
EOF

p5_tw=$(run_eval "$TMP/p5.nix")
p5_v3=$(NIX_USE_V3=1 run_eval "$TMP/p5.nix")
p5_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p5.nix")
assert_eq "p5 TW=v3-default sharing" "$p5_tw" "$p5_v3"
assert_eq "p5 TW=v3-skip sharing"    "$p5_tw" "$p5_v3_skip"

# ----------------------------------------------------------------------
# p6 — #548 (2026-05-09) curried-call from-expr.  AST shape:
#   ExprCall {
#     fun = ExprCall { fun = ExprVar(f), arg = a },
#     arg = b
#   }
# i.e. `f a b` where f is a let-bound (or fromWith) function.  Prior
# to the fix, isComplexFromExpr only matched when c->fun->exprKind ==
# Var (the IMMEDIATE fun) — curried calls slipped through and were
# lowered eagerly.  Under v3-direct nixpkgs eval, this surfaced as
# `OP_WITH_LOOKUP: cycle while resolving 'callPackage'` because
# stage.nix's allPackages has `inherit (callPackagesWith pkgs ./top-
# level/...) Y Z;` and the eager call forced the with-source `pkgs`
# (= the lib.fix x_thunk being computed → Black) → cycle.
#
# Synthetic repro: a self-referential rec-attrset where one entry's
# inherit-from source is a curried call that references the rec
# binding mid-construction.
cat > "$TMP/p6.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
in fix (self:
  let
    callIt = a: b: { result = a; meta = b; };
  in
  rec {
    config = { foo = "bar"; };
    inherit (callIt config "x") result meta;
  }).result
EOF

p6_tw=$(run_eval "$TMP/p6.nix")
p6_v3=$(NIX_USE_V3=1 run_eval "$TMP/p6.nix")
p6_v3_direct=$(NIX_V3_DIRECT_EVAL=1 run_eval "$TMP/p6.nix")
assert_eq "p6 TW=v3-default (curried-call from-expr lazy)" "$p6_tw" "$p6_v3"
assert_eq "p6 TW=v3-direct (curried-call from-expr lazy)"  "$p6_tw" "$p6_v3_direct"

# p6b — NEGATIVE: same shape, unused inherited name + a throw inside
# the curried call.  Must stay lazy and produce a non-throw result.
cat > "$TMP/p6b.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
in fix (self:
  let
    callIt = a: b: { result = a; meta = b; bomb = throw "boom"; };
  in
  rec {
    config = { foo = "bar"; };
    inherit (callIt config "x") result;
  }).result.foo
EOF

p6b_tw=$(run_eval "$TMP/p6b.nix")
p6b_v3=$(NIX_USE_V3=1 run_eval "$TMP/p6b.nix")
p6b_v3_direct=$(NIX_V3_DIRECT_EVAL=1 run_eval "$TMP/p6b.nix")
assert_eq "p6b TW=v3-default (unused throw stays lazy)" "$p6b_tw" "$p6b_v3"
assert_eq "p6b TW=v3-direct (unused throw stays lazy)"  "$p6b_tw" "$p6b_v3_direct"

# ----------------------------------------------------------------------
echo
echo "=== inherit-from-laziness tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
