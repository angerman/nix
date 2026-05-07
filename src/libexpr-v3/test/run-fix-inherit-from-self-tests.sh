#!/usr/bin/env bash
# #495 follow-on: documents a v3 evaluation bug independent of intrinsic
# dispatch -- `(fix0 (self: { fixedPoints = ...; inherit (self.fixedPoints) fix; })).fix`
# infinitely recurses when the inherited attr is a fix-shape lambda that
# is then called.  TW evaluates correctly.  Reproducer mirrors the actual
# nixpkgs lib structure where `lib.fix` is exposed via
# `inherit (self.fixedPoints) fix` from makeExtensible'.
#
# This test currently EXPECTS infinite recursion under v3 -- once the
# underlying bug is fixed, flip the assertion.
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
PASS=0; FAIL=0; fail_names=()

# Canonical reproducer: fix-point attrset re-exposing a fix-shape lambda
# via `inherit (self.X) Y`.
cat > "$TMP/repro.nix" <<'NIX'
let
  fix0 = f: let x = f x; in x;
  outer = self: {
    fixedPoints = { fn = g: let y = g y; in y; };
    inherit (self.fixedPoints) fn;
  };
  ext = self: { a = 1; b = self.a + 10; };
in (((fix0 outer).fn) ext).b
NIX

# 1. TW-only: must succeed (return 11).
tw_out=$(timeout 15 "$NIX_BIN" eval --impure -f "$TMP/repro.nix" 2>&1)
if echo "$tw_out" | grep -q '^11$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("TW-only repro: expected 11, got: $(echo "$tw_out" | tail -2)")
fi

# 2. v3 default: positive regression — MUST return 11.  Closed by the
#    always-thunkify fix in pushInheritFromCache (lower.cc); previously
#    failed with infinite recursion because eager from-expr lowering
#    forced `self` mid-construction.
v3_out=$(timeout 15 env NIX_USE_V3=1 \
  "$NIX_BIN" eval --impure -f "$TMP/repro.nix" 2>&1)
if echo "$v3_out" | grep -q '^11$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("v3 fix-inherit-from-self: expected 11, got: $(echo "$v3_out" | tail -3)")
fi

# 3. Same shape but inheriting a non-fix-shape lambda must still work
#    (cross-check that the bug specifically requires a recursive lambda).
cat > "$TMP/non-fix.nix" <<'NIX'
let
  fix0 = f: let x = f x; in x;
  outer = self: {
    fixedPoints = { fn = a: a + 1; };
    inherit (self.fixedPoints) fn;
  };
in (fix0 outer).fn 5
NIX
nf_out=$(timeout 15 env NIX_USE_V3=1 \
  "$NIX_BIN" eval --impure -f "$TMP/non-fix.nix" 2>&1)
if echo "$nf_out" | grep -q '^6$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("v3 non-fix lambda inherit: expected 6, got: $(echo "$nf_out" | tail -2)")
fi

# 4. Nested-let case: `outer = self: let X = ...; in { inherit (self.Y) Z }`.
#    `self`'s level is 1 (from-expr in attrset goes one scope up through
#    the let).  Was a documented KNOWN-FAIL under default mode (heuristic
#    at level=0 didn't reach the Lambda scope, so the eager from-expr
#    forced `self` mid-construction and looped).  Resolved 2026-05-07
#    via the partial-bindings recovery / WC-38 slot machinery -- default
#    mode now returns 11 (the correct value).  Pinned here so a
#    regression to the loop shape is caught.
cat > "$TMP/deep.nix" <<'NIX'
let
  fix0 = f: let x = f x; in x;
  outer = self:
    let helper = 5; in
    {
      fixedPoints = { fn = g: let y = g y; in y; };
      inherit (self.fixedPoints) fn;
    };
  ext = sf: { a = 1; b = sf.a + 10; };
in (((fix0 outer).fn) ext).b
NIX
deep_default_out=$(timeout 10 env NIX_USE_V3=1 \
  "$NIX_BIN" eval --impure -f "$TMP/deep.nix" 2>&1)
if echo "$deep_default_out" | grep -q '^11$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("deep default: expected 11, got: $(echo "$deep_default_out" | tail -1)")
fi

# 5. Same nested-let case under NIX_V3_SELF_DOT_MAX_LEVEL=2 -- the
#    opt-in heuristic walks level 1 to find the Lambda scope and
#    thunkifies.  MUST return 11 (same as default mode post-2026-05-07).
deep_optin_out=$(timeout 10 env NIX_USE_V3=1 NIX_V3_SELF_DOT_MAX_LEVEL=2 \
  "$NIX_BIN" eval --impure -f "$TMP/deep.nix" 2>&1)
if echo "$deep_optin_out" | grep -q '^11$'; then
  PASS=$((PASS + 1))
else
  FAIL=$((FAIL + 1))
  fail_names+=("deep MAX_LEVEL=2: expected 11, got: $(echo "$deep_optin_out" | tail -3)")
fi

echo
echo "=== fix-inherit-from-self tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
