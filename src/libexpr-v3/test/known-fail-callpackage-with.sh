#!/usr/bin/env bash
# Known-fail regression marker for the v3-direct callPackage with-scope bug.
#
# PROMOTED 2026-06-12: the blocker is FIXED (full-nixpkgs hello.name under
# v3-direct is byte-identical to TW, v3 engaged) — unblocked by C-8/C-10's
# App3 PAP-callee saturation (0fc563570, CODEBASE_REVIEW_2026-06-11). The
# positive regression guard now lives in run-callpackage-with-parity.sh. This
# tripwire is retained for history; it now exits 1 ("FIXED — promote me!") by
# design.
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
    # Symptom history as the v3 lower.cc / STG mechanism evolved.  Each
    # row represents the failure mode at a point-in-time — the script
    # accepts ANY of them as a known-fail.  When all symptoms are gone,
    # promote this to a positive test.
    #
    # 2026-05-09 (#548): inherit-from thunkify widening exposed three
    # OP_WITH_LOOKUP cycle floors:
    #   - 'callPackage' — closed by curried-call walk (0931b77a3)
    #   - 'texlive'     — closed by ExprSelect-with-Var head (eae55d149)
    #   - 'libsForQt5'  — closed by 15-commit STG ladder (ff629384b..1214a40b0)
    # 2026-05-12 (#558 force-list diff): post-STG-ladder symptom is now
    # `OP_ATTRS_SELECT: attribute not found` looking up `qt5`.  Root
    # cause traced to darwin/default.nix:1058's
    # `assert allDeps isBuiltByNixpkgsCompiler [...]` — v3's STG WHNF
    # recovery returns a partial Bindings that flips
    # `pkg.passthru.isFromBootstrapFiles or false` evaluation, which
    # flips `lib.all checkFn` from TRUE (TW) to FALSE (v3) → fires
    # `lib.deepSeq` cascade through pkg.stdenv.cc.cc → libsForQt5's
    # `inherit (pkgs) lib` thunk → forces pkgs (= lib.fix's x, BLACK).
    # See project_libsForQt5_deferred.md memory.
    if [[ "$out" == *"OP_WITH_LOOKUP: name 'callPackage' not found in with-scope"* \
       || "$out" == *"OP_WITH_LOOKUP: cycle while resolving 'callPackage'"* \
       || "$out" == *"OP_WITH_LOOKUP: cycle while resolving 'texlive'"* \
       || "$out" == *"OP_WITH_LOOKUP: cycle while resolving 'libsForQt5'"* \
       || "$out" == *"OP_ATTRS_SELECT: attribute not found"* ]]; then
        # Extract the resolved symbol so the message tracks progress.
        sym=$(printf '%s\n' "$out" | sed -n "s/.*resolving '\\([^']*\\)'.*/\\1/p" | head -1)
        if [[ -z "$sym" ]]; then
            if [[ "$out" == *"OP_ATTRS_SELECT: attribute not found"* ]]; then
                sym="ATTRS_SELECT-attr-not-found (allDeps deepSeq → partial-Bindings recovery, see #558)"
            else
                sym="callPackage(legacy)"
            fi
        fi
        echo "known-fail-callpackage-with: KNOWN-FAIL still present at '$sym'"
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
