#!/usr/bin/env bash
# A1 coverage measurement: for a nixpkgs .drvPath corpus, eval under two fake
# clocks (NIX_V3_FAKE_CURRENTTIME) and byte-diff. A pkg whose drvPath is
# IDENTICAL across the two clocks is currentTime-STABLE → empirically pure →
# cacheable under policy P. coverage% = stable/total. Tests the hypothesis that
# nixpkgs calls currentTime only in result-irrelevant branches.
set -u
cd ~/Projects/iohk/nix || cd /Users/angerman/Projects/iohk/nix || exit 2
NIX=build/src/nix/nix
source src/libexpr-v3/test/nixpkgs-pin.sh 2>/dev/null || true
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
C=(--impure --raw --option allow-import-from-derivation true)
E=(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=180s NIX_V3_MAX_HEAP=4G)
echo "A1 coverage: 50-pkg .drvPath, FAKE_CURRENTTIME=1000 vs =999999999 (currentTime-stability)"
env "${E[@]}" NIX_V3_FAKE_CURRENTTIME=1000      "$NIX" eval "${C[@]}" --expr "$EXPR" >/tmp/a1-t1.txt 2>/dev/null
env "${E[@]}" NIX_V3_FAKE_CURRENTTIME=999999999 "$NIX" eval "${C[@]}" --expr "$EXPR" >/tmp/a1-t2.txt 2>/dev/null
total=$(grep -vc SKIP /tmp/a1-t1.txt)
# per-line diff → count stable (identical) vs currentTime-dependent
stable=0; unstable=0; unstable_names=""
while IFS= read -r l1 && IFS= read -r l2 <&3; do
  n="${l1%% *}"; [[ "$l1" == *SKIP ]] && continue
  if [[ "$l1" == "$l2" ]]; then stable=$((stable+1)); else unstable=$((unstable+1)); unstable_names+="$n "; fi
done < /tmp/a1-t1.txt 3< /tmp/a1-t2.txt
echo "total(non-SKIP)=$total  currentTime-STABLE=$stable  UNSTABLE=$unstable"
[[ -n "$unstable_names" ]] && echo "  unstable: $unstable_names"
if [[ $total -gt 0 ]]; then echo "  COVERAGE = $(( 100 * stable / total ))% of the corpus is cacheable under policy P"; fi