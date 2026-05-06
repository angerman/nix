#!/usr/bin/env bash
# #458 — slot-capture regression test for the lower.cc closure-capture
# redesign (steps 1-5/6 of project_v3_primary_inversion blueprint).
#
# Verifies that rec-attrset entry references captured by inner
# closures resolve through Tag::Slot pointing at heap-stable storage,
# rather than the wrap thunk's recAttrsVar.  The slot path sidesteps
# the BlackholeError that previously fired when a closure was called
# from a context where the wrap thunk was mid-construction.
#
# Test shapes (per memory blueprint lines 141-145):
#   p1  basic let-rec capture: `let r = rec { a = 1; b = a + 1; }; in r.b`
#   p2  cross-thunk capture: nested closures referencing outer rec
#   p3  mutual recursion: `let r = rec { a = b; b = 1; }; in r.a`
#   p4  fix-point under lib.fix: `lib.fix (self: { a = 1; b = self.a + 1; })`
#   p5  let-binding wrapping rec: `let pkgs = rec { ... }; in pkgs.x`
#   p6  closure escapes the let-rec scope and is called later
#   p7  hello.name: real-world canary (skipped if nixpkgs not in NIX_PATH).
#
# Each shape is run in three modes: default (slot-capture on),
# slot-capture off (legacy wrap-thunk), and TW reference.  All three
# must produce identical output.
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

# p1 — basic let-rec capture.
cat > "$TMP/p1.nix" <<'EOF'
let r = rec { a = 1; b = a + 1; }; in r.b
EOF
EXP_P1='2'

# p2 — cross-thunk capture: a closure references siblings of the rec
# attrset.  The closure escapes the rec-attrset scope (returned from
# the let body) and is called later.
cat > "$TMP/p2.nix" <<'EOF'
let
  r = rec {
    base = 100;
    bump = x: x + base;
  };
in r.bump 5
EOF
EXP_P2='105'

# p3 — mutual recursion within the rec-attrset.
cat > "$TMP/p3.nix" <<'EOF'
let r = rec { a = b; b = 1; }; in r.a
EOF
EXP_P3='1'

# p4 — fix-point pattern (the cardano-node lib.fix shape).
cat > "$TMP/p4.nix" <<'EOF'
let
  fix = f: let x = f x; in x;
  result = fix (self: { a = 1; b = self.a + 1; c = self.b * 10; });
in result.c
EOF
EXP_P4='20'

# p5 — let-binding wrapping rec.  pkgs references siblings via rec
# scope, then the body uses pkgs through the outer let.
cat > "$TMP/p5.nix" <<'EOF'
let
  pkgs = rec {
    base = "hello";
    greet = name: base + " " + name;
  };
in pkgs.greet "world"
EOF
EXP_P5='"hello world"'

# p6 — closure escape: a function value captured from the rec-attrset
# returns a Tag::Closure that the consumer later calls.  Exercises
# the path where the wrap thunk would otherwise be forced from a
# different context.
cat > "$TMP/p6.nix" <<'EOF'
let
  r = rec {
    factor = 7;
    multiplier = n: x: x * n * factor;
  };
  triple = r.multiplier 3;
in triple 5
EOF
EXP_P6='105'

ok=0
fail=0
fail_names=()
skipped=0

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

# Three modes: TW (no v3), v3 default (slot-capture on), v3 with slot
# capture forcibly disabled (legacy wrap-thunk path) — all three must
# produce identical output.
for spec in \
    "tw::" \
    "v3-slot::NIX_USE_V3=1" \
    "v3-legacy::NIX_USE_V3=1 NIX_V3_NO_REC_SLOT_CAPTURE=1"
do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/p1" "$TMP/p1.nix" "$EXP_P1" "${envarr[@]}"
  run_one "$tag/p2" "$TMP/p2.nix" "$EXP_P2" "${envarr[@]}"
  run_one "$tag/p3" "$TMP/p3.nix" "$EXP_P3" "${envarr[@]}"
  run_one "$tag/p4" "$TMP/p4.nix" "$EXP_P4" "${envarr[@]}"
  run_one "$tag/p5" "$TMP/p5.nix" "$EXP_P5" "${envarr[@]}"
  run_one "$tag/p6" "$TMP/p6.nix" "$EXP_P6" "${envarr[@]}"
done

# p7: nixpkgs#hello.name canary.  The slot-capture redesign was
# motivated in part by closure-capture across rec-attrset / fix-point
# boundaries inside nixpkgs's stdenv stack; this is the integration
# canary that the redesign doesn't break the real-world reference.
hello_tw=$("$NIX_BIN" eval --raw nixpkgs#hello.name 2>/dev/null) || true
if [[ -n "$hello_tw" ]]; then
  hello_v3_slot=$(NIX_USE_V3=1 "$NIX_BIN" eval --raw nixpkgs#hello.name 2>/dev/null) \
    || hello_v3_slot="<error>"
  hello_v3_legacy=$(NIX_USE_V3=1 NIX_V3_NO_REC_SLOT_CAPTURE=1 \
    "$NIX_BIN" eval --raw nixpkgs#hello.name 2>/dev/null) \
    || hello_v3_legacy="<error>"
  if [[ "$hello_v3_slot" == "$hello_tw" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("v3-slot/p7(nixpkgs#hello.name)  expected=$hello_tw  got=$hello_v3_slot")
  fi
  if [[ "$hello_v3_legacy" == "$hello_tw" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("v3-legacy/p7(nixpkgs#hello.name)  expected=$hello_tw  got=$hello_v3_legacy")
  fi
else
  skipped=$((skipped + 2))
fi

echo "=== #458 rec-slot-capture tests: ok=$ok fail=$fail skipped=$skipped ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
