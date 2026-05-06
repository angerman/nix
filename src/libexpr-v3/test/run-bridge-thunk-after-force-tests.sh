#!/usr/bin/env bash
# #483 regression: v3ToTreeWalker must short-circuit Bridge thunks
# AFTER forceValue, not just before.
#
# Background: per the #456 fix at vm.cc:4549+, when forceBridgeThunk
# returns a result that is itself a Bridge thunk wrapping a TW
# nFunction/nList Value, the chase loop in forceValue breaks out
# instead of allocating another Bridge wrapper -- so forceValue can
# RETURN a Tag::Thunk (in Bridge state) as WHNF.
#
# v3ToTreeWalker had a pre-force short-circuit for Bridge thunks but
# no post-force one.  When forceValue returned a Bridge thunk, the
# switch fell through to the default branch and produced mkNull --
# silently losing the wrapped TW value.  Symptom: under lambda-skip,
# accessing a TW function bridged into a v3 list and then back out via
# `__v3_force_list_elem` produced an empty list `[ ]` where TW would
# call the function, surfacing as "attempt to call something which is
# not a function but a list" (e.g. inside dfold-style iteration in
# pkgs/stdenv/booter.nix during nixpkgs#hello.name evaluation).
#
# The fix re-checks for Bridge-thunk shape after the force and returns
# the original TW Value, mirroring the pre-force short-circuit.
#
# This test exercises the round-trip:
#   p1 v3 list of TW-bridged functions, accessed via TW elemAt then
#      called -- must not turn into [ ] silently.
#   p2 v3 attrset whose value is a TW-bridged function, accessed via
#      TW select then called.
#   p3 NEGATIVE: same shapes but with a TW-bridged list (not function)
#      -- must round-trip identity.
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

# ----------------------------------------------------------------------
# p1 — v3 list of functions, accessed via TW elemAt + called.
# Forcing the list element under lambda-skip exercises the
# forceValue → Bridge-thunk-as-WHNF → v3ToTreeWalker path that #483
# fixes.  Pre-fix: v3ToTreeWalker fell through Tag::Thunk to default
# → mkNull, so TW received `[ ]` and tried to call it as a function.
cat > "$TMP/p1.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  fns = [ (x: x + 10) (x: x * 2) (x: x - 1) ];
  imapped = lib.lists.imap1 (i: f: f i) fns;
in builtins.elemAt imapped 1
EOF

p1_tw=$(run_eval "$TMP/p1.nix")
p1_v3=$(NIX_USE_V3=1 run_eval "$TMP/p1.nix")
p1_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p1.nix")
assert_eq "p1 TW=v3-default"     "$p1_tw" "$p1_v3"
assert_eq "p1 TW=v3-skip"        "$p1_tw" "$p1_v3_skip"

# ----------------------------------------------------------------------
# p2 — v3 attrset bridged with a function value.
cat > "$TMP/p2.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  attrs = lib.attrsets.mapAttrs (n: v: { fn = x: "${n}-${toString x}-${toString v}"; }) { a = 1; b = 2; };
in (attrs.a.fn 5)
EOF

p2_tw=$(run_eval "$TMP/p2.nix")
p2_v3=$(NIX_USE_V3=1 run_eval "$TMP/p2.nix")
p2_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p2.nix")
assert_eq "p2 TW=v3-default"     "$p2_tw" "$p2_v3"
assert_eq "p2 TW=v3-skip"        "$p2_tw" "$p2_v3_skip"

# ----------------------------------------------------------------------
# p3 — round-trip identity for non-function values via the same path.
cat > "$TMP/p3.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  fns = [ [1 2] [3 4 5] [6] ];
  imapped = lib.lists.imap1 (i: l: l ++ [ i ]) fns;
in builtins.elemAt imapped 1
EOF

p3_tw=$(run_eval "$TMP/p3.nix")
p3_v3=$(NIX_USE_V3=1 run_eval "$TMP/p3.nix")
p3_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p3.nix")
assert_eq "p3 TW=v3-default"     "$p3_tw" "$p3_v3"
assert_eq "p3 TW=v3-skip"        "$p3_tw" "$p3_v3_skip"

# ----------------------------------------------------------------------
# p4 — fix-point + bridged-function through inherit-from path.
cat > "$TMP/p4.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  fix = f: let x = f x; in x;
  set = fix (self:
    let lst = lib.lists.imap1 (i: f: f i) [ (i: i + 100) ];
    in {
      first = builtins.elemAt lst 0;
      inherit (self) first;
    });
in set.first
EOF

p4_tw=$(run_eval "$TMP/p4.nix")
p4_v3=$(NIX_USE_V3=1 run_eval "$TMP/p4.nix")
p4_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p4.nix")
assert_eq "p4 TW=v3-default"     "$p4_tw" "$p4_v3"
assert_eq "p4 TW=v3-skip"        "$p4_tw" "$p4_v3_skip"

# ----------------------------------------------------------------------
echo
echo "=== bridge-thunk-after-force tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
