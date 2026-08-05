#!/usr/bin/env bash
# WC-38 nixpkgs regression probe.
#
# This is a SEPARATE test from run-wc-laziness-tests.sh because the bug
# is currently UNFIXED — running it will produce "FAIL: still broken"
# until v3 can evaluate `(import <nixpkgs>{}).callPackages`.  Once the
# fix lands, this script becomes a regular regression test.
#
# Why this file exists:
#   The bisection in WC-38 (2026-05-01) tried 23+ synthetic Nix
#   patterns; NONE reproduced.  The trigger requires the FULL nixpkgs
#   `lib.foldl' (flip lib.extends)` chain over 10+ overlays + splice's
#   `lib.callPackagesWith self`.  Smaller patterns work because their
#   evaluation order doesn't reach the recoverMetadata / mirrorArgs /
#   makeOverridable chain that lib.makeOverridable triggers during
#   pkgs construction in the actual nixpkgs.
#
# Probe expressions (all should produce a string in tree-walker; v3
# currently fails with "OP_WITH_LOOKUP: name 'callPackages' not found
# in with-scope"):
#
#   1. (import <nixpkgs>{}).callPackages   →  "lambda"   (smallest probe)
#   2. (import <nixpkgs>{}).system         →  "aarch64-darwin"
#   3. (import <nixpkgs>{}).lib.id 1       →  1
#
# Usage:
#   ./run-wc38-nixpkgs-probe.sh             # all probes
#   NIX_PATH="..." ./run-wc38-nixpkgs-probe.sh
#
# Diagnostic env vars (see project_wc38_with_blackhole.md memory):
#   V3_DBG_FORCE_SITE=1  V3_DBG_WITH=1  V3_DBG_FORCE_TRACE=1

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX_INSTANTIATE="${NIX_INSTANTIATE:-nix-instantiate}"

if [[ ! -x "$V3" ]]; then
  echo "v3-eval not found at $V3" >&2
  exit 1
fi

# Default to the bisected nixpkgs source if NIX_PATH isn't set.
NIX_PATH="${NIX_PATH:-nixpkgs=/nix/store/yb2s3slqfb45942ln5z7m3ssmn7mnr4s-source}"
export NIX_PATH

# #820 (2026-05-26): the hardcoded bisected store path is environment-
# specific and may have been GC-collected on developer machines.  Probe
# its availability and SKIP cleanly if missing — this test is opt-in
# (it documents a known-still-broken WC-38 surface and is gated by the
# operator), so a missing nixpkgs source is a setup mismatch, not a
# regression.  Setting NIX_PATH explicitly to a valid path overrides
# the default and re-enables the probe.
configured_nixpkgs="${NIX_PATH#nixpkgs=}"
configured_nixpkgs="${configured_nixpkgs%%:*}"
if [[ ! -e "$configured_nixpkgs" ]]; then
  echo "SKIP: configured NIX_PATH='$NIX_PATH' resolves to non-existent" >&2
  echo "       '$configured_nixpkgs' — set NIX_PATH to an extant nixpkgs source" >&2
  echo "       to re-enable this probe.  WC-38 status remains DEFERRED." >&2
  exit 0
fi

probes=(
  # Smallest failing case (no attr select; just forcing pkgs to WHNF):
  'builtins.isAttrs (import <nixpkgs>{})'
  # Type-of callPackages (the original WC-38 surface):
  'builtins.typeOf (import <nixpkgs>{}).callPackages'
  # Selecting .system from pkgs:
  '(import <nixpkgs>{}).system'
  # Calling lib.id (stresses the lib.callPackagesWith chain):
  'builtins.toString ((import <nixpkgs>{}).lib.id 1)'
)

expected=(
  'true'
  '"lambda"'
  '"aarch64-darwin"'
  '"1"'
)

pass=0
fail=0

for i in "${!probes[@]}"; do
  expr="${probes[$i]}"
  want="${expected[$i]}"

  # Tree-walker: should always succeed (reference impl).
  tw_out=$(timeout 60 "$NIX_INSTANTIATE" --eval -E "$expr" 2>&1 | tail -1)
  if [[ "$tw_out" != "$want" ]]; then
    echo "WARN: tree-walker baseline mismatch for: $expr"
    echo "  expected: $want"
    echo "  got:      $tw_out"
  fi

  # v3: this is the regression probe.
  v3_out=$(timeout 60 "$V3" --expr "$expr" 2>&1 | tail -1)
  if [[ "$v3_out" == "$want" ]]; then
    echo "PASS  WC-38-nixpkgs-$i: $expr"
    pass=$((pass + 1))
  else
    echo "FAIL  WC-38-nixpkgs-$i: $expr"
    echo "      expected: $want"
    echo "      v3 got:   $v3_out"
    fail=$((fail + 1))
  fi
done

echo ""
echo "WC-38 nixpkgs probe results: $pass passing, $fail still failing"
[[ $fail -eq 0 ]] && exit 0
exit 1
