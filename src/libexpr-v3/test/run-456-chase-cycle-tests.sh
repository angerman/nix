#!/usr/bin/env bash
# #456 — Bridge→Bridge chase cycle regression test.
#
# The bug: forceValue's chase loop encountered a Tag::Thunk in
# Evaluated state whose evaluated.tag was Tag::Thunk again -- because
# forceBridgeThunk -> treeWalkerToV3 wraps an nFunction TW Value as
# ANOTHER Bridge thunk for round-trip identity preservation.  Each
# chase iteration allocated a fresh Thunk, the chain extended forever,
# kMaxChaseIters fired.
#
# Fix (vm.cc forceValue): treat a Bridge-thunk-wrapping-nFunction as
# WHNF.  After forceBridgeThunk produces another Bridge thunk, break
# out of the chase.
#
# Test shapes:
#   p1 import-then-call: import a file that returns a function value,
#                        then call it -- exercises the v3 -> TW ->
#                        v3 round-trip on a Function tag.
#   p2 (nixpkgs): real-world hello.name -- the canary that exposed
#                 the bug.  Skipped if nixpkgs not in NIX_PATH.
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

# p1 — synthetic: a function value passes through let-binding then is
#       called.  Doesn't strictly exercise Bridge→Bridge but covers
#       the v3 -> TW boundary.
cat > "$TMP/inner.nix" <<'EOF'
x: x + 1
EOF
cat > "$TMP/p1.nix" <<EOF
let f = import $TMP/inner.nix; in f 41
EOF
EXP_P1='42'

# p2 — function-of-function: calling a function-returning-function via
#       let-bindings exercises the value-bridge round-trip path.
cat > "$TMP/p2.nix" <<'EOF'
let
  mkFn = base: x: base + x;
  add5 = mkFn 5;
in [(add5 10) (add5 20) (add5 30)]
EOF
EXP_P2='[ 15 25 35 ]'

# p3 — list of function values, mapped: forces v3 to handle Function
#       tags as elements.
cat > "$TMP/p3.nix" <<'EOF'
let
  mkFn = n: x: x * n;
  fs = [ (mkFn 2) (mkFn 3) (mkFn 4) ];
in builtins.map (f: f 10) fs
EOF
EXP_P3='[ 20 30 40 ]'

# p4 — nixpkgs hello.name: real-world canary that exposed the bug.
#       Only run if nixpkgs is available in NIX_PATH.

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

# Modes: TW baseline, then v3-direct.  NIX_V3_REQUIRE=1 makes a silent
# tree-walker fallback FAIL the test rather than masquerade as a v3 pass
# (NIX_USE_V3 was the retired cutover hook; NIX_V3_DIRECT_EVAL is current).
for spec in "tw::" "v3::NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1"; do
  IFS=:: read -r tag _ envspec <<< "$spec"
  IFS=' ' read -ra envarr <<< "$envspec"
  run_one "$tag/p1" "$TMP/p1.nix" "$EXP_P1" "${envarr[@]}"
  run_one "$tag/p2" "$TMP/p2.nix" "$EXP_P2" "${envarr[@]}"
  run_one "$tag/p3" "$TMP/p3.nix" "$EXP_P3" "${envarr[@]}"
done

# p4: nixpkgs hello.name -- the real-world canary that surfaced the
#     cycle.  Uses --expr: v3-direct handles --expr / -f, but NOT flake
#     installables (`nixpkgs#hello.name`), which silently fall back to the
#     tree-walker.  NIX_V3_REQUIRE=1 forces v3 (no silent fallback).
#     Best-effort -- skip if <nixpkgs> is not in NIX_PATH.  The
#     callPackage-cycle failure this originally tripped is resolved in
#     default v3; the old STG-mode workaround (NIX_V3_STG=1) was retired.
hello_expr='(import <nixpkgs> {}).hello.name'
hello_tw=$("$NIX_BIN" eval --raw --impure --expr "$hello_expr" 2>/dev/null) || true
if [[ -n "$hello_tw" ]]; then
  hello_v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_REQUIRE=1 NIX_V3_MAX_WALL_TIME=180s \
    NIX_V3_MAX_HEAP=8G "$NIX_BIN" eval --raw --impure --expr "$hello_expr" 2>/dev/null) \
    || hello_v3="<error>"
  if [[ "$hello_v3" == "$hello_tw" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("v3/p4(hello.name)  expected=$hello_tw  got=$hello_v3")
  fi
else
  skipped=$((skipped + 1))
fi

echo "=== #456 chase-cycle tests: ok=$ok fail=$fail skipped=$skipped ==="
for n in "${fail_names[@]}"; do
  echo "  FAIL $n"
done
[[ $fail -eq 0 ]] || exit 1
