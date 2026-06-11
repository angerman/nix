#!/usr/bin/env bash
# POSITIVE parity regression — full-nixpkgs hello.name under v3-direct.
#
# This is the promotion of the long-standing known-fail-callpackage-with.sh
# tripwire (a known-fail since 2026-05-09).  The blocker — full nixpkgs
# instantiation `(import nixpkgs { system = "aarch64-darwin"; }).hello.name`
# failing under NIX_V3_DIRECT_EVAL=1 through the callPackage / texlive /
# libsForQt5 with-scope graph — was UNBLOCKED by C-8/C-10's App3 PAP-callee
# saturation (0fc563570, CODEBASE_REVIEW_2026-06-11): the with-scope path
# produces App3 PAP callees (from mapAttrs etc.) that the prior Tag::App-only
# call machinery sent to iter-force and looped forever.  See
# project_libsForQt5_deferred.md.
#
# Asserts:
#   1. v3-direct hello.name == TW hello.name (byte-identical), AND
#   2. v3 actually ENGAGED (NIX_VM_STATS lines present — TW emits none), so we
#      are not measuring a silent TW fallback (measurement gate / T-2).
#
# Exit 0 = parity + engaged; 1 = regression; 2 = harness/skip error.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
WALL="${NIX_V3_MAX_WALL_TIME:-120s}"
[ -x "$NIX" ] || { echo "run-callpackage-with-parity: $NIX not found, build first" >&2; exit 2; }

NIXPKGS="$("$NIX" --extra-experimental-features 'nix-command flakes' flake archive --json 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("inputs",{}).get("nixpkgs",{}).get("path",""))' 2>/dev/null)"
[ -n "$NIXPKGS" ] && [ -d "$NIXPKGS" ] || { echo "run-callpackage-with-parity: SKIP (no nixpkgs available)"; exit 0; }

EXPR="(import $NIXPKGS { system = \"aarch64-darwin\"; }).hello.name"
filt() { grep -vE 'search path|stack size|does not exist, ignoring'; }

tw=$(NIX_V3_MAX_WALL_TIME="$WALL" "$NIX" --extra-experimental-features nix-command \
        eval --impure --expr "$EXPR" 2>/dev/null | filt | tail -1)
statsf="$(mktemp)"
v3=$(NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME="$WALL" "$NIX" \
        --extra-experimental-features nix-command eval --impure --expr "$EXPR" 2>"$statsf" | filt | tail -1)
engaged=$(grep -c "v3-direct" "$statsf"); rm -f "$statsf"

fail=0
if [ -z "$tw" ]; then echo "FAIL  TW produced no result"; fail=1; fi
if [ "$v3" != "$tw" ]; then
  echo "FAIL  v3-direct ($v3) != TW ($tw)"; fail=1
else
  echo "PASS  parity      hello.name = $v3"
fi
if [ "${engaged:-0}" -lt 1 ]; then
  echo "FAIL  v3 NOT engaged (no NIX_VM_STATS lines — silent TW fallback?)"; fail=1
else
  echo "PASS  engaged     ($engaged v3-direct stats lines)"
fi

[ "$fail" -eq 0 ] && echo "--- callpackage-with parity: OK ---" || echo "--- callpackage-with parity: FAILED ---"
exit "$fail"
