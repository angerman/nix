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

# 2. v3 default: KNOWN-FAIL infinite recursion.  Asserted so a silent
#    behaviour change is caught.  Flip when fixed.
v3_out=$(timeout 15 env NIX_USE_V3=1 \
  "$NIX_BIN" eval --impure -f "$TMP/repro.nix" 2>&1)
v3_exit=$?
if echo "$v3_out" | grep -q "infinite recursion" || [[ $v3_exit -ne 0 ]]; then
  PASS=$((PASS + 1))   # documented failure
else
  FAIL=$((FAIL + 1))
  fail_names+=("v3 fix-inherit-from-self: bug fixed but assertion not flipped (got success: $(echo "$v3_out" | tail -1))")
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

echo
echo "=== fix-inherit-from-self tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
