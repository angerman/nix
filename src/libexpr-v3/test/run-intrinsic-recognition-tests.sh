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
echo
echo "=== intrinsic-recognition tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
