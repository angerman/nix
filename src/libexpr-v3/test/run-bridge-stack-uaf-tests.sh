#!/usr/bin/env bash
# #483 part 2: regression for use-after-free in treeWalkerToV3 Bridge wrap.
#
# Background: treeWalkerToV3 for nFunction / nExternal / nThunk wraps the
# TW Value as a v3 Bridge thunk by storing `&nv` in `bridgeSrc`.  Several
# callers passed STACK-allocated `nix::Value`:
#   - vm.cc OP_CALL Bridge handler's `nix::Value outTw`
#   - primops.cc derivationStrict cache result `nix::Value result`
#   - primops.cc builtins.path bridge result `nix::Value result`
#
# Once the caller's stack frame returned, the bridge thunk's bridgeSrc
# became a dangling stack pointer.  A subsequent force of the thunk read
# garbage from the reused stack slot.
#
# Symptom under lambda-skip + nixpkgs#hello.name (lldb confirmed):
#   - 2 successful OP_CALL Bridge invocations stash bridgeSrc=&outTw
#     (heap-looking address since stack was warm)
#   - The 3rd Bridge thunk force reads the SAME pointer; outTw's slot
#     has been recycled by a different caller's stack frame
#   - The reused memory contains an empty list `[ ]`
#   - TW's callFunction sees a list as the callee → throws "attempt to
#     call something which is not a function but a list: [ ]"
#
# Fix (primops.cc:3780-3835): treeWalkerToV3 now allocates a heap copy
# via `ns.allocValue()` and stashes its address.  bridgeSrc is stable
# for the lifetime of the bridge thunk regardless of the caller's frame
# discipline.
#
# This test exercises a pattern where TW returns a function value to v3,
# which then re-bridges back -- catching any future regression in
# bridgeSrc lifetime.
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
# p1 — TW function bridged to v3 then called multiple times.  Pre-fix,
# repeated invocations would surface the stack reuse: the second/third
# call's bridgeSrc would dangle.  This test calls the bridged function
# many times to bias toward catching the regression.
cat > "$TMP/p1.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  fns = [ (x: x + 10) (x: x * 2) (x: x - 1) (x: x + 100) ];
  imapped = lib.lists.imap1 (i: f: f i) fns;
  call = idx: builtins.elemAt imapped idx;
in builtins.map (i: call i) [ 0 1 2 3 ]
EOF

p1_tw=$(run_eval "$TMP/p1.nix")
p1_v3=$(NIX_USE_V3=1 run_eval "$TMP/p1.nix")
p1_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p1.nix")
assert_eq "p1 TW=v3-default" "$p1_tw" "$p1_v3"
assert_eq "p1 TW=v3-skip"    "$p1_tw" "$p1_v3_skip"

# ----------------------------------------------------------------------
# p2 — dfold-style cycle from pkgs/stdenv/booter.nix.  Pre-fix this
# triggered the use-after-free deep inside a TW callFunction loop;
# repeated iterations are required to surface the dangling bridgeSrc.
cat > "$TMP/p2.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  dfold = op: lnul: rnul: list:
    let
      len = builtins.length list;
      go = pred: n:
        if n == len then rnul pred
        else
          let
            cur = op pred (builtins.elemAt list n) succ;
            succ = go cur (n + 1);
          in cur;
      lapp = lnul cur;
      cur = go lapp 0;
    in cur;

  withDefaults = lib.lists.imap1 (
    index: stageFun: prevStage:
    { idx = index; } // (stageFun prevStage)
  ) [
    (prev: { x = 1; })
    (prev: { x = 2; })
    (prev: { x = 3; })
  ];

  folder = nextStage: stageFun: prevStage:
    let args = stageFun prevStage; in args;

  postStage = pkgs: { final = pkgs.x; };
  result = dfold folder postStage (_: {}) withDefaults;
in result.idx
EOF

p2_tw=$(run_eval "$TMP/p2.nix")
p2_v3=$(NIX_USE_V3=1 run_eval "$TMP/p2.nix")
p2_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p2.nix")
assert_eq "p2 TW=v3-default (dfold cycle)" "$p2_tw" "$p2_v3"
assert_eq "p2 TW=v3-skip (dfold cycle)"    "$p2_tw" "$p2_v3_skip"

# ----------------------------------------------------------------------
# p3 — repeated bridge round-trip.  Many lambda-skip invocations stress
# the bridgeSrc lifetime.
cat > "$TMP/p3.nix" <<'EOF'
let
  lib = (import <nixpkgs/lib>);
  inc = x: x + 1;
  applyN = n: f: x: if n == 0 then x else applyN (n - 1) f (f x);
  result = applyN 50 inc 0;
in result
EOF

p3_tw=$(run_eval "$TMP/p3.nix")
p3_v3=$(NIX_USE_V3=1 run_eval "$TMP/p3.nix")
p3_v3_skip=$(NIX_USE_V3=1 NIX_V3_LAMBDA_SKIP=1 run_eval "$TMP/p3.nix")
assert_eq "p3 TW=v3-default (50 iter)" "$p3_tw" "$p3_v3"
assert_eq "p3 TW=v3-skip (50 iter)"    "$p3_tw" "$p3_v3_skip"

# ----------------------------------------------------------------------
echo
echo "=== bridge-stack-uaf tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
  for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
  exit 1
fi
exit 0
