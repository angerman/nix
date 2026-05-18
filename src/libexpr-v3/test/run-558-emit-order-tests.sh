#!/usr/bin/env bash
# #558 emit-order restructure regression test.
#
# Validates that v3-direct can evaluate `inherit (E) y;` clauses where
# the from-expr `E` references a sibling regular entry through a
# `with self;` chain — the canonical libsForQt5-style cycle pattern
# from nixpkgs all-packages.nix:
#
#   self: with self; {
#     libsForQt5 = { callPackage = ...; };
#     inherit (libsForQt5.callPackage path {}) wt4;
#   }
#
# Pre-558: v3-direct threw "OP_WITH_LOOKUP: cycle while resolving
# 'libsForQt5'" because the from-expr's eager OP_WITH_LOOKUP fired
# BEFORE the surrounding attrset's regular entries were SET in the
# partial Bindings.  withLookup's partial-Bindings peek (via the
# nearest Black thunk's registry entry) couldn't see libsForQt5.
#
# Post-558: lower.cc reorders the bindings within the parent block
# such that the AttrSet's REC_INIT + regular SETs run BEFORE the
# from-expr cache binding.  vm.cc's publishToNearestBlackThunkFrame
# additionally registers with all thunk frames (Suspended + Black)
# under largest-wins semantics so deeper fix-point chains find the
# right Bindings via the with-source slot deref.
#
# Tests:
#   t1 simple `with self;` + sibling-via-callPackage (canonical pattern)
#   t2 lib.fix + lib.extends overlay chain (3 layers)
#
# The full nixpkgs `(import <nixpkgs> {}).hello.name` reproducer is
# tracked separately via known-fail-callpackage-with.sh — this test
# guards against regressions of the closed cases.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
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
        fail_names+=("$name (expected='$expected' got='$got')")
    fi
}

# t1: canonical libsForQt5-style pattern (single fix layer).
cat > "$TMP/t1.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  stage = self: with self; {
    libsForQt5 = {
      callPackage = path: args: { wt4 = "wt4-evaluated"; };
    };
    inherit (libsForQt5.callPackage "/path" {}) wt4;
  };
in (fix stage).wt4
EOF

t1_tw=$("$NIX_BIN" --extra-experimental-features 'nix-command flakes' \
    eval --impure --file "$TMP/t1.nix" 2>/dev/null)
t1_v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" --extra-experimental-features \
    'nix-command flakes' eval --impure --file "$TMP/t1.nix" 2>/dev/null)
assert_eq "t1-tw"     '"wt4-evaluated"' "$t1_tw"
assert_eq "t1-v3"     '"wt4-evaluated"' "$t1_v3"

# t2: lib.extends-style overlay chain (3 layers above the base).
cat > "$TMP/t2.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  extends = overlay: f: final: let prev = f final; in prev // overlay final prev;
  base = self: with self; {
    libsForQt5 = {
      callPackage = path: args: { wt4 = "wt4-evaluated"; };
    };
    inherit (libsForQt5.callPackage "/path" {}) wt4;
  };
  identity = final: prev: {};
  pkgs = fix (extends identity (extends identity (extends identity base)));
in pkgs.wt4
EOF

t2_tw=$("$NIX_BIN" --extra-experimental-features 'nix-command flakes' \
    eval --impure --file "$TMP/t2.nix" 2>/dev/null)
t2_v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" --extra-experimental-features \
    'nix-command flakes' eval --impure --file "$TMP/t2.nix" 2>/dev/null)
assert_eq "t2-tw"     '"wt4-evaluated"' "$t2_tw"
assert_eq "t2-v3"     '"wt4-evaluated"' "$t2_v3"

echo
echo "#558 emit-order tests: $PASS pass, $FAIL fail"
if (( FAIL > 0 )); then
    for n in "${fail_names[@]}"; do
        echo "  FAIL: $n"
    done
    exit 1
fi
