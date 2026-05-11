#!/usr/bin/env bash
# #558 cell-update protocol semantic tests (Phase 1.5b).
#
# Verifies the per-thunk `shapeCell` mechanism that replaces the
# partial-Bindings registry's cross-thunk pollution.
#
# Background: v3's STG WHNF recovery used to return values from
# OTHER thunks' chains (via publishToAllThunkFrames + registry-wide
# chain peek).  This caused `pkg.passthru.isFromBootstrapFiles or
# false` to incorrectly return TRUE for nixpkgs pkgs whose passthru
# didn't actually have that attr — see project_libsForQt5_deferred.md.
#
# The fix (`NIX_V3_CELL_EVERYWHERE=1`):
#   - Every Thunk gets a `shapeCell: Value*` allocated at MAKE_THUNK
#   - `*shapeCell` starts as Tag::Thunk(t) (sentinel "no progress")
#   - OP_ATTRS_REC_INIT_TAIL / OP_ATTRS_UPDATE_TAIL update all
#     outer THUNK_RETURN frames' shapeCells (tail-position semantics)
#   - forceValue's Black branch reads *shapeCell instead of
#     consulting the partial-Bindings registry
#   - OP_RETURN writes the final value and clears the cell
#
# This file tests:
#   p1 — basic let-rec works in both modes (positive, baseline)
#   p2 — lib.fix with extends works (positive, the canonical pattern)
#   n1 — true cycle still throws (negative, STG `<<loop>>` semantics)
#   c1 — cross-thunk passthru lookup doesn't leak (the bug we fixed)
#   m1 — cell-everywhere mode toggle: both produce same observable result
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

assert_contains() {
    local name="$1" needle="$2" haystack="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        fail_names+=("$name (needle='$needle' not in: '${haystack:0:200}')")
    fi
}

run_v3() {
    local cell_mode="$1"; shift
    local file="$1"
    local env=""
    if [[ "$cell_mode" == "on" ]]; then env="NIX_V3_CELL_EVERYWHERE=1"; fi
    eval "$env" NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" \
        --extra-experimental-features nix-command \
        eval --impure --file "$file" 2>&1
}

# ----------------------------------------------------------------------
# p1 — basic let-rec (positive baseline).  Both modes must succeed and
# produce identical results.
cat > "$TMP/p1.nix" <<'EOF'
let
  x = { a = 1; b = y.a + 10; };
  y = { a = x.a + 100; b = 2; };
in [ x.a x.b y.a y.b ]
EOF
p1_off=$(run_v3 off "$TMP/p1.nix")
p1_on=$(run_v3 on  "$TMP/p1.nix")
assert_eq "p1 cell-everywhere off"  "[ 1 111 101 2 ]" "$p1_off"
assert_eq "p1 cell-everywhere on"   "[ 1 111 101 2 ]" "$p1_on"
assert_eq "p1 mode-toggle parity"   "$p1_off" "$p1_on"

# ----------------------------------------------------------------------
# p2 — lib.fix with extends (the canonical libsForQt5-style fix-point).
cat > "$TMP/p2.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  extends = overlay: f: final:
    let prev = f final;
    in prev // overlay final prev;
  base = self: {
    a = 1;
    b = self.a + 10;
    c = self.b + 100;
  };
  ov = final: prev: { d = final.c + 1000; };
in (fix (extends ov base)).d
EOF
p2_off=$(run_v3 off "$TMP/p2.nix")
p2_on=$(run_v3 on  "$TMP/p2.nix")
assert_eq "p2 cell-everywhere off"  "1111" "$p2_off"
assert_eq "p2 cell-everywhere on"   "1111" "$p2_on"

# ----------------------------------------------------------------------
# n1 — true cycle.  STG semantics: must throw infinite-recursion / cycle.
# v3 currently throws "infinite recursion" via BlackholeError under both
# modes (cell-everywhere doesn't recover from genuine cycles because
# the shapeCell stays at the sentinel for non-AttrSet bodies).
cat > "$TMP/n1.nix" <<'EOF'
let x = x; in x
EOF
n1_off=$(run_v3 off "$TMP/n1.nix")
n1_on=$(run_v3 on  "$TMP/n1.nix")
# Both modes should produce SOME error (not a value).  Don't assert the
# exact message — TW says "infinite recursion encountered", v3 default
# says "v3 forceValue: infinite recursion (chase cycle ...)" or similar.
assert_contains "n1 off — error reported"  "rror" "$n1_off"
assert_contains "n1 on  — error reported"  "rror" "$n1_on"

# ----------------------------------------------------------------------
# c1 — cross-thunk passthru pollution scenario.
#
# Two pkgs A and B.  A's passthru is `{ isFromBootstrapFiles = true; }`
# (size-1 attrset).  B's passthru is `{ /* empty */ }` (size-0).
# Looking up `B.passthru.isFromBootstrapFiles or false` must return
# `false` — even if A is forced first and A's passthru is somewhere
# on the call stack.
#
# The pre-fix bug returned TRUE for B because partial-Bindings recovery
# from B's outer-thunk chain found A's Bindings via registry-wide peek.
# Cell-everywhere should prevent this because B's shapeCell is per-B
# and not populated with A's data.
cat > "$TMP/c1.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  isFromBootstrapFiles = pkg: pkg.passthru.isFromBootstrapFiles or false;
  base = self: {
    # A: a "bootstrap" pkg with isFromBootstrapFiles = true
    pkgA = { passthru = { isFromBootstrapFiles = true; }; };
    # B: a "nixpkgs" pkg with NO isFromBootstrapFiles attr
    pkgB = { passthru = { /* deliberately empty */ }; };
    # The check: B should be FALSE despite A being on the stack
    check = isFromBootstrapFiles self.pkgA && (! isFromBootstrapFiles self.pkgB);
  };
in (fix base).check
EOF
c1_off=$(run_v3 off "$TMP/c1.nix")
c1_on=$(run_v3 on  "$TMP/c1.nix")
assert_eq "c1 cross-thunk pollution off" "true" "$c1_off"
assert_eq "c1 cross-thunk pollution on"  "true" "$c1_on"

# ----------------------------------------------------------------------
# m1 — cell-everywhere mode toggle: produces identical observable
# result on a substantial Nix program that exercises let-rec + extends
# + // operations.
cat > "$TMP/m1.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  extends = overlay: f: final:
    let prev = f final;
    in prev // overlay final prev;
  base = self: {
    a = 1; b = self.a + 10; c = self.b + 100;
    nested = { inherit (self) a b; };
    fn = x: self.a + x;
  };
  ov1 = final: prev: { d = final.c + 1000; };
  ov2 = final: prev: { e = prev.d + final.b; };
in let
  pkgs = fix (extends ov2 (extends ov1 base));
in {
  inherit (pkgs) a b c d e;
  nested_a = pkgs.nested.a;
  nested_b = pkgs.nested.b;
  fn_5 = pkgs.fn 5;
}
EOF
m1_off=$(run_v3 off "$TMP/m1.nix")
m1_on=$(run_v3 on  "$TMP/m1.nix")
assert_eq "m1 mode-toggle parity (substantial)" "$m1_off" "$m1_on"

# ----------------------------------------------------------------------
# s1 — STG cell semantics: a thunk's value is observable AFTER the
# body completes (OP_RETURN updates cell+shapeCell with the retVal).
# A re-force should NOT re-enter the body — the cached value is returned.
cat > "$TMP/s1.nix" <<'EOF'
let
  counter = let
    state = builtins.tryEval (throw "side-effect");
  in 42;
in [ counter counter counter ]
EOF
s1_off=$(run_v3 off "$TMP/s1.nix")
s1_on=$(run_v3 on  "$TMP/s1.nix")
# A normal lang behavior: counter is bound once.  Reading three times
# yields three identical values without re-evaluation.  Sanity check.
assert_eq "s1 cell-everywhere off — memoization"  "[ 42 42 42 ]" "$s1_off"
assert_eq "s1 cell-everywhere on  — memoization"  "[ 42 42 42 ]" "$s1_on"

# ----------------------------------------------------------------------
echo
echo "=== cell-update protocol tests: $PASS pass, $FAIL fail ==="
if (( FAIL > 0 )); then
    for n in "${fail_names[@]}"; do
        echo "  FAIL: $n"
    done
    exit 1
fi
exit 0
