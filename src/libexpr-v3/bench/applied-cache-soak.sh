#!/usr/bin/env bash
# A2 applied-import cache byte-eq soak: eval a nixpkgs package corpus' drvPaths
# with the applied cache ON (default) vs OFF (NIX_V3_APPLIED_CACHE=0), byte-diff.
# GATE: 0 divergence. Correctness is host-independent (pinned nixpkgs).
set -u
cd ~/Projects/iohk/nix || cd /Users/angerman/Projects/iohk/nix || exit 2
NIX=build/src/nix/nix
source src/libexpr-v3/test/nixpkgs-pin.sh 2>/dev/null || true
# Curated corpus (~50 well-known, cleanly-evaluating packages). tryEval-guarded
# so a broken/unfree/platform attr becomes "" instead of aborting the sweep.
EXPR='let p = import <nixpkgs> { config.allowUnfree = true; };
  names = [ "hello" "git" "gcc" "curl" "openssl" "zlib" "bash" "coreutils"
    "python3" "perl" "ruby" "nodejs" "go" "rustc" "cmake" "ninja" "pkg-config"
    "gnumake" "gawk" "gnused" "gnugrep" "findutils" "which" "file" "jq" "wget"
    "vim" "emacs" "tmux" "htop" "ripgrep" "fd" "bat" "fzf" "gnutar" "gzip" "xz"
    "zstd" "bzip2" "sqlite" "libxml2" "libpng" "libjpeg" "freetype" "cairo"
    "pango" "glib" "gtk3" "boost" "icu" ];
  in builtins.concatStringsSep "\n" (map (n:
    let ok = builtins.hasAttr n p;
        r = if ok then builtins.tryEval ((builtins.getAttr n p).drvPath)
                  else { success = false; value = ""; };
    in n + " " + (if r.success then r.value else "SKIP")) names)'
COMMON=(--impure --raw --option allow-import-from-derivation true)
ENVV=(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=180s NIX_V3_MAX_HEAP=4G)
echo "A2 sweep: 50-pkg drvPath corpus, applied-cache ON vs OFF (byte-diff)"
env "${ENVV[@]}" "$NIX" eval "${COMMON[@]}" --expr "$EXPR" >/tmp/a2-on.txt 2>/tmp/a2-on.err
env "${ENVV[@]}" NIX_V3_APPLIED_CACHE=0 "$NIX" eval "${COMMON[@]}" --expr "$EXPR" >/tmp/a2-off.txt 2>/tmp/a2-off.err
echo "on:  $(wc -l </tmp/a2-on.txt) lines, $(grep -c SKIP /tmp/a2-on.txt) skipped"
echo "off: $(wc -l </tmp/a2-off.txt) lines, $(grep -c SKIP /tmp/a2-off.txt) skipped"
if diff -q /tmp/a2-on.txt /tmp/a2-off.txt >/dev/null; then
  echo "A2 RESULT: BYTE-IDENTICAL across the corpus (0 divergence) — GATE PASS"
else
  echo "A2 RESULT: DIVERGENCE — GATE FAIL:"; diff /tmp/a2-on.txt /tmp/a2-off.txt | head -40
fi
echo "--- non-SKIP package count ---"; grep -vc SKIP /tmp/a2-on.txt