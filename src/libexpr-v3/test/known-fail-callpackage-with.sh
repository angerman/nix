#!/usr/bin/env bash
# Known-fail regression marker for the v3-direct callPackage with-scope bug.
#
# As of 2026-05-09, full nixpkgs eval under NIX_V3_DIRECT_EVAL=1 fails
# with:
#
#   error: v3 OP_WITH_LOOKUP: name 'callPackage' not found in with-scope
#
# This script:
#   - exits 0 if the bug is STILL PRESENT (expected)
#   - exits 1 if the bug appears FIXED (cardano-node + nixpkgs unblocked!)
#
# When the bug is fixed, flip this script to a positive test by replacing
# its body with the matching positive assertion, then enable the
# `hello-name`, `attrnames-pkgs` etc. workloads in
# `src/libexpr-v3/bench/workloads.toml` (remove their `skip-by-default`
# tag).
#
# Investigation notes: see
# `src/libexpr-v3/lode/CALLPACKAGE_BUG_2026-05-09.md`.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
    echo "known-fail-callpackage-with: $NIX not found, build first" >&2
    exit 2
fi

# Resolve nixpkgs via the flake archive.
NIXPKGS="$(
    "$NIX" --extra-experimental-features 'nix-command flakes' \
        flake archive --json 2>/dev/null \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("inputs",{}).get("nixpkgs",{}).get("path",""))' \
        2>/dev/null
)"

if [[ -z "$NIXPKGS" || ! -d "$NIXPKGS" ]]; then
    echo "known-fail-callpackage-with: SKIP (no nixpkgs available)"
    exit 0
fi

# Run the failing eval.
out=$(NIX_V3_DIRECT_EVAL=1 "$NIX" --extra-experimental-features nix-command \
    eval --impure --expr "(import $NIXPKGS { system = \"aarch64-darwin\"; }).hello.name" 2>&1)
rc=$?

if [[ $rc -ne 0 ]]; then
    if [[ "$out" == *"OP_WITH_LOOKUP: name 'callPackage' not found in with-scope"* ]]; then
        echo "known-fail-callpackage-with: KNOWN-FAIL still present (this is expected)"
        exit 0
    else
        echo "known-fail-callpackage-with: failed but with a NEW symptom:" >&2
        echo "$out" | tail -5 >&2
        exit 2  # different error — investigate
    fi
fi

# Success means the bug is fixed.  Promote this to a positive test
# (replace this script's body) and unblock the bench workloads.
echo "known-fail-callpackage-with: bug appears FIXED — promote me!"
echo "  output: $out"
exit 1
