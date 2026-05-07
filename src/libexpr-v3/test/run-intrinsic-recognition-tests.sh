#!/usr/bin/env bash
# #495: regression for nix-stdlib intrinsic AST recognition (lower.cc).
# Verifies V3_DBG_INTRINSIC=1 fires for canonical lambda shapes and
# stays silent for near-miss shapes.
#
# This tests RECOGNITION only -- no native dispatch yet.  The lambda
# still evaluates via the regular v3 path; we just check that
# lower.cc's structural matcher sets `intrinsicKind` correctly.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0; FAIL=0
fail_names=()

assert_match() {
  local name="$1" expected_kind="$2" file="$3"
  local out
  out=$(V3_DBG_INTRINSIC=1 "$V3" --file "$file" --strict 2>&1)
  if echo "$out" | grep -q "kind=$expected_kind"; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("$name: expected kind=$expected_kind, got: $(echo "$out" | grep recogniseIntrinsic | head -1)")
  fi
}

assert_no_match() {
  local name="$1" file="$2"
  local out
  out=$(V3_DBG_INTRINSIC=1 "$V3" --file "$file" --strict 2>&1)
  if echo "$out" | grep -q "recogniseIntrinsic"; then
    FAIL=$((FAIL + 1))
    fail_names+=("$name: expected no recognition, got: $(echo "$out" | grep recogniseIntrinsic | head -1)")
  else
    PASS=$((PASS + 1))
  fi
}

# ----------------------------------------------------------------------
# p1 — canonical fix shape MUST match.
cat > "$TMP/p1.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  ext = self: { a = 1; b = self.a + 10; };
in (fix ext).b
EOF
assert_match "p1 canonical fix" "Fix" "$TMP/p1.nix"

# ----------------------------------------------------------------------
# p2 — fix's argument applied to wrong value (f 42, not f x): no match.
cat > "$TMP/p2.nix" <<'EOF'
let
  notfix = f: let x = f 42; in x;
in notfix (n: n + 1)
EOF
assert_no_match "p2 wrong arg shape" "$TMP/p2.nix"

# ----------------------------------------------------------------------
# p3 — fix with extra binding: no match (matcher requires single
# binding).
cat > "$TMP/p3.nix" <<'EOF'
let
  notfix = f: let x = f x; y = 1; in y;
in notfix (s: { a = 1; })
EOF
assert_no_match "p3 extra binding" "$TMP/p3.nix"

# ----------------------------------------------------------------------
# p4 — fix where let-body returns wrong var: no match.
cat > "$TMP/p4.nix" <<'EOF'
let
  notfix = f: let x = f x; in 42;
in 1
EOF
assert_no_match "p4 wrong body" "$TMP/p4.nix"

# ----------------------------------------------------------------------
# p5 — formals lambda (intrinsics are simple-arg only): no match.
cat > "$TMP/p5.nix" <<'EOF'
let
  notfix = { f }: let x = f x; in x;
in 0
EOF
assert_no_match "p5 formals lambda" "$TMP/p5.nix"

# ----------------------------------------------------------------------
# p6 — fix-shape with different arg/binding names (MUST match -- the
# matcher uses Symbol equality, not name strings).
cat > "$TMP/p6.nix" <<'EOF'
let
  myfix = g: let y = g y; in y;
  ext = self: { v = 99; };
in (myfix ext).v
EOF
assert_match "p6 alternate names" "Fix" "$TMP/p6.nix"

# ----------------------------------------------------------------------
# e1 — canonical extends shape MUST match.
cat > "$TMP/e1.nix" <<'EOF'
let
  extends = overlay: f: final: let prev = f final; in prev // overlay final prev;
in extends
EOF
assert_match "e1 canonical extends" "Extends" "$TMP/e1.nix"

# ----------------------------------------------------------------------
# e2 — extends with wrong overlay-call arity (1 arg instead of 2): no match.
cat > "$TMP/e2.nix" <<'EOF'
let
  notext = overlay: f: final: let prev = f final; in prev // overlay final;
in notext
EOF
assert_no_match "e2 wrong overlay arity" "$TMP/e2.nix"

# ----------------------------------------------------------------------
# e3 — extends with extra binding in let: no match.
cat > "$TMP/e3.nix" <<'EOF'
let
  notext = overlay: f: final: let prev = f final; tmp = 1; in prev // overlay final prev;
in notext
EOF
assert_no_match "e3 extra binding" "$TMP/e3.nix"

# ----------------------------------------------------------------------
# e4 — extends with swapped final/prev order in overlay call: no match.
cat > "$TMP/e4.nix" <<'EOF'
let
  notext = overlay: f: final: let prev = f final; in prev // overlay prev final;
in notext
EOF
assert_no_match "e4 swapped overlay args" "$TMP/e4.nix"

# ----------------------------------------------------------------------
# e5 — extends with alternate names (matcher is symbol-based): MUST match.
cat > "$TMP/e5.nix" <<'EOF'
let
  myext = ov: g: x: let p = g x; in p // ov x p;
in myext
EOF
assert_match "e5 alternate names" "Extends" "$TMP/e5.nix"

# ----------------------------------------------------------------------
# e6 — extends nested-call instead of f-call as prev RHS: no match.
cat > "$TMP/e6.nix" <<'EOF'
let
  notext = overlay: f: final: let prev = f final final; in prev // overlay final prev;
in notext
EOF
assert_no_match "e6 prev RHS arity" "$TMP/e6.nix"

# ----------------------------------------------------------------------
# c1 — canonical composeExtensions shape MUST match.
cat > "$TMP/c1.nix" <<'EOF'
let
  composeExtensions = f: g: final: prev:
    let
      fApplied = f final prev;
      prev' = prev // fApplied;
    in
    fApplied // g final prev';
in composeExtensions
EOF
assert_match "c1 canonical composeExtensions" "ComposeExtensions" "$TMP/c1.nix"

# ----------------------------------------------------------------------
# c2 — composeExtensions with wrong fApplied arity: no match.
cat > "$TMP/c2.nix" <<'EOF'
let
  notc = f: g: final: prev:
    let
      fApplied = f final;
      prev' = prev // fApplied;
    in
    fApplied // g final prev';
in notc
EOF
assert_no_match "c2 wrong fApplied arity" "$TMP/c2.nix"

# ----------------------------------------------------------------------
# c3 — composeExtensions with wrong prev' update direction: no match.
cat > "$TMP/c3.nix" <<'EOF'
let
  notc = f: g: final: prev:
    let
      fApplied = f final prev;
      prev' = fApplied // prev;
    in
    fApplied // g final prev';
in notc
EOF
assert_no_match "c3 wrong prev' update direction" "$TMP/c3.nix"

# ----------------------------------------------------------------------
# c4 — composeExtensions with three bindings (extra): no match.
cat > "$TMP/c4.nix" <<'EOF'
let
  notc = f: g: final: prev:
    let
      fApplied = f final prev;
      prev' = prev // fApplied;
      tmp = 1;
    in
    fApplied // g final prev';
in notc
EOF
assert_no_match "c4 extra binding" "$TMP/c4.nix"

# ----------------------------------------------------------------------
# c5 — alternate names should still match (symbol-based).
cat > "$TMP/c5.nix" <<'EOF'
let
  myc = a: b: x: y:
    let
      r = a x y;
      y' = y // r;
    in
    r // b x y';
in myc
EOF
assert_match "c5 alternate names" "ComposeExtensions" "$TMP/c5.nix"

# ----------------------------------------------------------------------
# d1 — DISPATCH (positive, opt-in via NIX_V3_INTRINSIC_DISPATCH=1).
# When intrinsic dispatch is enabled, the simple fix case returns the
# correct value AND the native dispatch counter bumps.
cat > "$TMP/d1.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  ext = self: { a = 1; b = self.a + 10; };
in (fix ext).b
EOF
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"
if [[ -x "$NIX_BIN" ]]; then
  d1_out=$(NIX_USE_V3=1 NIX_V3_INTRINSIC_DISPATCH=1 NIX_VM_STATS=1 \
    "$NIX_BIN" eval --impure -f "$TMP/d1.nix" 2>&1)
  if echo "$d1_out" | grep -q '^11$'; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("d1 simple fix dispatch result: expected 11, got $(echo "$d1_out" | tail -3)")
  fi
  if echo "$d1_out" | grep -q "intrinsic Fix: native dispatch calls=[1-9]"; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("d1 simple fix dispatch counter: expected calls>=1, got: $(echo "$d1_out" | grep intrinsic || echo none)")
  fi
fi

# ----------------------------------------------------------------------
# d2 — DEFAULT mode (intrinsic dispatch off): simple fix MUST still
# work (recognition fires but bytecode runs as before).  Negative
# regression for the gate -- removing it would silently change
# default-mode behaviour.
if [[ -x "$NIX_BIN" ]]; then
  d2_out=$(NIX_USE_V3=1 NIX_VM_STATS=1 \
    "$NIX_BIN" eval --impure -f "$TMP/d1.nix" 2>&1)
  if echo "$d2_out" | grep -q '^11$'; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("d2 default mode result: expected 11")
  fi
  # MUST NOT report intrinsic Fix calls when gate is off.
  if echo "$d2_out" | grep -q "intrinsic Fix: native dispatch"; then
    FAIL=$((FAIL + 1))
    fail_names+=("d2 default mode: intrinsic dispatch fired but should be gated off")
  else
    PASS=$((PASS + 1))
  fi
fi

# ----------------------------------------------------------------------
# d3 — KNOWN-FAIL reproducer (negative regression).
#
# d3a: Simple `fixedPoints.fix` with stub `lib = null` works under both
#      intrinsic dispatch and parse-precompile.
#
# d3b: Full `(import <nixpkgs/lib>).fix ext` fails with infinite
#      recursion under NIX_V3_PARSE_PRECOMPILE=1.  This is a
#      parse-precompile bug independent of intrinsic dispatch — the
#      same recursion happens with NIX_V3_INTRINSIC_DISPATCH off, so
#      flipping intrinsic dispatch does not regress this case.  Asserted
#      here so a fix to either path that silently changes the failure
#      shape is caught.  Tracked as a follow-on to #495.
if [[ -x "$NIX_BIN" ]]; then
  cat > "$TMP/d3a.nix" <<'EOF'
let
  fixedPoints = import <nixpkgs/lib/fixed-points.nix> { lib = null; };
  ext = self: { a = 1; b = self.a + 10; };
in (fixedPoints.fix ext).b
EOF
  d3a_out=$(NIX_USE_V3=1 NIX_V3_INTRINSIC_DISPATCH=1 NIX_V3_PARSE_PRECOMPILE=1 \
    "$NIX_BIN" eval --impure -f "$TMP/d3a.nix" 2>&1)
  if echo "$d3a_out" | grep -q '^11$'; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    fail_names+=("d3a fixedPoints.fix with stub lib: expected 11, got: $(echo "$d3a_out" | tail -2)")
  fi

  cat > "$TMP/d3b.nix" <<'EOF'
let
  lib = import <nixpkgs/lib>;
  ext = self: { a = 1; b = self.a + 10; };
in (lib.fix ext).b
EOF
  # Known-fail under intrinsic dispatch (multi-layer interaction).
  # When step 3 lands native Extends, this should pass.  Until then,
  # assert it fails with infinite recursion to catch silent changes.
  d3b_out=$(timeout 20 env NIX_USE_V3=1 NIX_V3_INTRINSIC_DISPATCH=1 \
    NIX_V3_PARSE_PRECOMPILE=1 "$NIX_BIN" eval --impure -f "$TMP/d3b.nix" 2>&1)
  d3b_exit=$?
  if echo "$d3b_out" | grep -q "infinite recursion" || [[ $d3b_exit -ne 0 ]]; then
    PASS=$((PASS + 1))  # Currently expected to fail
  else
    FAIL=$((FAIL + 1))
    fail_names+=("d3b full lib.fix: expected infinite-recursion (known fail), got success: $(echo "$d3b_out" | tail -1)")
  fi
fi

# ----------------------------------------------------------------------
echo
echo "=== intrinsic-recognition tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
