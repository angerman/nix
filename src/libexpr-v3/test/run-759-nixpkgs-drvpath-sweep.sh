#!/usr/bin/env bash
# #759 — v3-direct vs TW drvPath byte-identical sweep, 60+ nixpkgs
# packages.  This is the Stage 2 empirical foundation for deleting
# Stage 2 binary-exit verification (post-#760, SKIP_PREEVAL retired)
# §"Stage 2 needs to land", item 4: "cross-eval verification against
# a representative sample of nixpkgs legacyPackages.<sys>").
#
# Selection: ~65 packages spanning stdenv, build tools, languages,
# crypto/network/compression, text utils, databases, editors, qt5/qt6
# (libsForQt5 #455-family marker), haskell (compiler), and heavy
# applications.  Avoids IFD-heavy packages where divergence would
# reflect store availability not eval semantics.
#
# Modes compared per package, `.drvPath` only (no .outPath — see
# rationale comment below):
#   tw      — pure tree-walker (no v3 env vars)
#   v3      — NIX_V3_DIRECT_EVAL=1
#             (post-#760: SKIP_PREEVAL retired — v3-direct is
#             lazy-thunk by default)
#
# Why drvPath, not outPath: drvPath is a pure function of the
# derivation's input attrs (name + builder + args + env + outputs +
# inputDrvs + inputSrcs).  Any eval-time divergence between TW and v3
# manifests as a different .drv file → different drvPath.  outPath
# would require building the .drv, which is out of scope here.
#
# Usage:
#   ./run-759-nixpkgs-drvpath-sweep.sh                  # full sweep
#   ./run-759-nixpkgs-drvpath-sweep.sh --quick          # 8 hot pkgs
#   V3_TEST_VERBOSE=1 ./run-759-nixpkgs-drvpath-sweep.sh
#
# Exit codes:
#   0 — all packages byte-identical
#   1 — one or more divergences (each printed inline)
#   2 — infrastructure error (binary missing, etc.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
    echo "run-759: nix binary not found at $NIX" >&2
    exit 2
fi

VERBOSE="${V3_TEST_VERBOSE:-0}"
MODE="${1:-full}"
SYSTEM="${SYSTEM:-aarch64-darwin}"

# 65 representative packages.  Each one is a top-level attr of
# `(import <nixpkgs> {}).<attr>`.  Selected to span the patterns most
# likely to surface v3-vs-TW divergence:
#
#   - core stdenv: stdenv/cc/bash/gcc (the foundation everything
#     transitively depends on; divergence here is catastrophic).
#   - mkDerivation variants: simple (hello), multi-output (openssl),
#     fixed-output (curl).
#   - language runtimes (python3/nodejs/rustc/ruby/perl/lua) —
#     heavy mkDerivation chains with custom overlays.
#   - haskell.compiler.* — historical v3 divergence battleground
#     (#670/#671 ROOT CAUSE FIXED; this provides regression coverage).
#   - libsForQt5 — #455-family canonical marker (per
#     `feedback_libsForQt5_deferred` memory: "v3 forces qt5-packages.nix's
#     `inherit (pkgs) lib` thunk while pkgs is BLACK").
PACKAGES_FULL=(
    # ── stdenv core (anything broken here breaks everything) ─────
    "hello"
    "stdenv.cc"
    "bash"
    "coreutils"
    "gcc"
    "clang"
    "binutils"
    "gnumake"
    "pkg-config"
    # ── build tooling ────────────────────────────────────────────
    "cmake"
    "ninja"
    "meson"
    "autoconf"
    "automake"
    "patchelf"
    # ── crypto ───────────────────────────────────────────────────
    "openssl"
    "libsodium"
    "gnupg"
    # ── compression ──────────────────────────────────────────────
    "zlib"
    "xz"
    "bzip2"
    "gzip"
    "zstd"
    # ── text / GNU utils ─────────────────────────────────────────
    "gnused"
    "gnugrep"
    "gnutar"
    "gawk"
    "findutils"
    "diffutils"
    # ── network ──────────────────────────────────────────────────
    "curl"
    "wget"
    "openssh"
    # ── search / dev tooling ─────────────────────────────────────
    "ripgrep"
    "fzf"
    "jq"
    "tree"
    "tmux"
    # ── version control ──────────────────────────────────────────
    "git"
    "mercurial"
    # ── editors ──────────────────────────────────────────────────
    "vim"
    # ── shells ───────────────────────────────────────────────────
    "zsh"
    "fish"
    # ── languages: heavy mkDerivation chains ─────────────────────
    "python3"
    "nodejs"
    "rustc"
    "ruby"
    "perl"
    "lua"
    "go"
    # ── databases ────────────────────────────────────────────────
    "sqlite"
    "postgresql"
    # ── multimedia ───────────────────────────────────────────────
    "ffmpeg"
    "imagemagick"
    # ── haskell — historical divergence battleground ─────────────
    "haskell.compiler.ghc94"
    "haskell.compiler.ghc96"
    "haskell.compiler.ghc98"
    "haskell.compiler.ghc910"
    "cabal-install"
    # ── libsForQt5 — #455-family marker ──────────────────────────
    "libsForQt5.qt5.qtbase"
    "libsForQt5.qt5.qtdeclarative"
    # ── heavy applications ───────────────────────────────────────
    "firefox"
    "vlc"
    # ── pkg-set fragments (stress sharing semantics) ─────────────
    "pkgsStatic.hello"
    "pkgsCross.aarch64-multiplatform.hello"
)

PACKAGES_QUICK=(
    "hello"
    "gcc"
    "python3"
    "openssl"
    "ripgrep"
    "git"
    "haskell.compiler.ghc98"
    "libsForQt5.qt5.qtbase"
)

case "$MODE" in
    --quick) PACKAGES=("${PACKAGES_QUICK[@]}") ;;
    --full|full) PACKAGES=("${PACKAGES_FULL[@]}") ;;
    *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac

PASS=0
FAIL=0
SKIP=0
FAILED=()
SKIPPED=()

eval_one() {
    local mode="$1" attr="$2"
    # Build the legacyPackages.<sys>.<attr>.drvPath expression.
    # `--raw` so drvPath comes back as plain text.
    local expr="(import (builtins.getFlake \"flake:nixpkgs\") { system = \"$SYSTEM\"; }).$attr.drvPath"

    case "$mode" in
        tw)
            env "$NIX" eval --impure --raw \
                --expr "$expr" 2>/dev/null
            ;;
        v3)
            env NIX_V3_DIRECT_EVAL=1 \
                NIX_V3_MAX_HEAP=4G NIX_V3_MAX_WALL_TIME=120s \
                "$NIX" eval --impure --raw \
                --expr "$expr" 2>/dev/null
            ;;
    esac
}

run_one() {
    local attr="$1"

    local out_tw
    out_tw=$(eval_one tw "$attr")
    local rc_tw=$?

    local out_v3
    out_v3=$(eval_one v3 "$attr")
    local rc_v3=$?

    # If TW itself fails, package isn't evaluable in this context
    # (e.g. broken/insecure/unfree on this system) — skip.
    if [[ -z "$out_tw" ]] || [[ "$rc_tw" -ne 0 ]]; then
        printf "  [SKIP] %-50s (TW could not evaluate)\n" "$attr"
        SKIP=$((SKIP+1))
        SKIPPED+=("$attr")
        return
    fi

    if [[ "$out_tw" = "$out_v3" ]] && [[ "$rc_tw" -eq "$rc_v3" ]]; then
        printf "  [PASS] %-50s %s\n" "$attr" "$out_tw"
        PASS=$((PASS+1))
    else
        printf "  [FAIL] %-50s\n" "$attr"
        printf "         TW (rc=%d): %s\n" "$rc_tw" "$out_tw"
        printf "         v3 (rc=%d): %s\n" "$rc_v3" "$out_v3"
        FAIL=$((FAIL+1))
        FAILED+=("$attr")
    fi
}

echo "=== #759 nixpkgs drvPath sweep (mode=$MODE, $SYSTEM, ${#PACKAGES[@]} packages) ==="
echo ""
START=$(date +%s)
for p in "${PACKAGES[@]}"; do
    run_one "$p"
done
ELAPSED=$(( $(date +%s) - START ))
echo ""
echo "==================== summary ===================="
echo "  total:     ${#PACKAGES[@]}"
echo "  pass:      $PASS"
echo "  fail:      $FAIL"
echo "  skip:      $SKIP"
echo "  elapsed:   ${ELAPSED}s"
if (( FAIL > 0 )); then
    echo "  failed:"
    for f in "${FAILED[@]}"; do
        echo "    - $f"
    done
    exit 1
fi
if (( SKIP > 0 )) && [[ "$VERBOSE" = "1" ]]; then
    echo "  skipped:"
    for s in "${SKIPPED[@]}"; do
        echo "    - $s"
    done
fi
exit 0
