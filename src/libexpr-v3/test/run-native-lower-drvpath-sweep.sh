#!/usr/bin/env bash
# Native-lower drvPath SHIP sweep (PARSER_PROJECT_PLAN §2.2 / §5.3).
#
# Differential test: `<pkg>.drvPath` under the v3-NATIVE parser+lowerer
# (NIX_V3_NATIVE_PARSER=1 NIX_V3_NATIVE_LOWER=1) must be BYTE-IDENTICAL to
# the tree-walker's, across a diverse package set.  drvPath is a pure
# function of the derivation's input attrs, so ANY native-lower eval
# divergence (name resolution, laziness, attrset shape, context) surfaces
# as a different .drv hash.  This is the oracle that caught the
# bare-`fetchurl`→builtins resolution bug (firefox, /v3-fake-store) and
# the stale-CU disk-cache hazard (qtbase warm).
#
# Cold cache (NIX_V3_NO_DISK_CACHE=1): exercises the lowerer, not the
# cache.  (The CU disk cache is disabled under native-lower anyway — it
# doesn't encode the lowerer version — so warm == cold here.)
#
# Needs nixpkgs on the search path: pass -I via $NIXPKGS or set NIX_PATH.
# Skips (exit 0) when nixpkgs is unavailable, so CI without a pinned
# nixpkgs doesn't spuriously fail.
#
# Usage:
#   NIXPKGS=/nix/store/<hash>-source ./run-native-lower-drvpath-sweep.sh
#   ./run-native-lower-drvpath-sweep.sh --quick      # 6 hot pkgs
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-eval}"
TW="${TW:-$ROOT/builddir/src/nix/nix-instantiate}"
SYS="${SYS:-aarch64-darwin}"
WALL="${WALL:-200s}"

if [[ ! -x "$V3" || ! -x "$TW" ]]; then echo "v3-eval / nix-instantiate not found" >&2; exit 2; fi

# Resolve a nixpkgs: explicit $NIXPKGS, else the flake registry's system entry.
NPKGS="${NIXPKGS:-}"
if [[ -z "$NPKGS" ]]; then
    NPKGS=$(nix registry list 2>/dev/null | awk '/system flake:nixpkgs/ {print $3}' | sed 's#^path:##' | head -1)
fi
if [[ -z "$NPKGS" || ! -e "$NPKGS" ]]; then
    echo "run-native-lower-drvpath-sweep: no nixpkgs (set \$NIXPKGS) — SKIP"; exit 0
fi
echo "nixpkgs = $NPKGS  (system = $SYS)"

quick=( hello coreutils python3 perl openssl git )
full=( "${quick[@]}" bash gnumake gnused gnugrep gawk gnutar gzip xz zlib bzip2
       zstd curl ruby lua jq sqlite ripgrep vim openssh patchelf cmake ninja
       meson pkg-config diffutils findutils go rustc nodejs postgresql
       imagemagick ffmpeg mercurial gnupg haskell.compiler.ghc96
       pkgsStatic.hello pkgsCross.aarch64-multiplatform.hello
       libsForQt5.qt5.qtbase libsForQt5.qt5.qtdeclarative )

pkgs=( "${full[@]}" )
[[ "${1:-}" == "--quick" ]] && pkgs=( "${quick[@]}" )

pass=0; fail=0; skip=0; failed=""
for pkg in "${pkgs[@]}"; do
    expr="(import <nixpkgs> { system = \"$SYS\"; }).$pkg.drvPath"
    tw=$("$TW" --eval -I nixpkgs="$NPKGS" --expr "$expr" 2>/dev/null)
    if [[ -z "$tw" ]]; then echo "  skip $pkg (TW yields nothing on $SYS)"; skip=$((skip+1)); continue; fi
    v3=$(NIX_V3_NO_DISK_CACHE=1 NIX_V3_MAX_WALL_TIME="$WALL" NIX_V3_NATIVE_PARSER=1 NIX_V3_NATIVE_LOWER=1 \
         "$V3" -I nixpkgs="$NPKGS" --expr "$expr" --strict 2>/dev/null)
    if [[ "$tw" == "$v3" ]]; then echo "  ok   $pkg"; pass=$((pass+1));
    else echo "  FAIL $pkg"; echo "    tw=$tw"; echo "    v3=$v3"; fail=$((fail+1)); failed="$failed $pkg"; fi
done

echo ""
echo "=== native-lower drvPath sweep: $pass byte-identical, $fail divergent, $skip skipped ==="
[[ -n "$failed" ]] && echo "failed:$failed"
[[ "$fail" -eq 0 ]]
