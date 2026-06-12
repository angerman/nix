#!/usr/bin/env bash
# darwin-4 measurement harness for the CODEBASE_REVIEW_2026-06-11 remaining
# items.  Invoke as:  nix develop /Users/angerman/Projects/iohk/nix -c bash THIS
# (so the dev-shell PATH is active).  Uses absolute paths + allowUnfree.
#
#   (1) firefox.drvPath v3 vs TW — byte-identity + peak RSS (validates the
#       shipped M-8/M-9/M-10 wins + all 38 commits on the heavy workload).
#   (2) M-7 Immix A/B (V3_DBG_IMMIX_ALLOC on vs off, same binary) — RSS delta
#       vs the pre-committed >=150 MB ship bar + correctness.
#   (3) C-4b: ghc98.drvPath v3 vs TW — is the open <<slot>> residual gone?
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
NIX=/Users/angerman/Projects/iohk/nix/build/src/nix/nix
FF='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'
GHC='(import <nixpkgs> {}).haskell.compiler.ghc98.drvPath'
rssof() { awk '/maximum resident/{printf "%.0f", $1/1048576}' "$1"; }

echo "##### (1) firefox.drvPath — correctness + RSS #####"
tw=$("$NIX" eval --impure --expr "$FF" 2>/dev/null)
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=8G \
  /usr/bin/time -l "$NIX" eval --impure --expr "$FF" >/tmp/ff.v3.out 2>/tmp/ff.v3.time
v3=$(cat /tmp/ff.v3.out)
/usr/bin/time -l "$NIX" eval --impure --expr "$FF" >/dev/null 2>/tmp/ff.tw.time
echo "TW = $tw"
echo "v3 = $v3"
[ -n "$tw" ] && [ "$tw" = "$v3" ] && echo "CORRECTNESS: byte-identical" || echo "CORRECTNESS: DIVERGE"
echo "peak RSS: TW=$(rssof /tmp/ff.tw.time)MB  v3=$(rssof /tmp/ff.v3.time)MB"

echo "##### (2) M-7 Immix A/B (firefox.drvPath, same binary) #####"
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=8G \
  /usr/bin/time -l "$NIX" eval --impure --expr "$FF" >/tmp/ff.off.out 2>/tmp/ff.off.time
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=8G V3_DBG_IMMIX_ALLOC=1 \
  /usr/bin/time -l "$NIX" eval --impure --expr "$FF" >/tmp/ff.im.out 2>/tmp/ff.im.time
off=$(cat /tmp/ff.off.out); im=$(cat /tmp/ff.im.out)
ro=$(rssof /tmp/ff.off.time); ri=$(rssof /tmp/ff.im.time)
echo "Immix OFF: RSS=${ro}MB drv=$off"
echo "Immix ON : RSS=${ri}MB drv=$im"
{ [ "$off" = "$im" ] && [ "$off" = "$tw" ]; } && echo "M-7 CORRECTNESS: byte-identical" || echo "M-7 CORRECTNESS: DIVERGE"
echo "M-7 RSS delta (OFF-ON) = $((ro - ri)) MB (ship bar >=150)"

echo "##### (3) C-4b — ghc98.drvPath divergence (open <<slot>> RCA) #####"
gtw=$("$NIX" eval --impure --expr "$GHC" 2>/dev/null)
gv3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=8G "$NIX" eval --impure --expr "$GHC" 2>/tmp/ghc.v3.err)
echo "TW  ghc98 = $gtw"
echo "v3  ghc98 = $gv3"
if [ -n "$gtw" ] && [ "$gtw" = "$gv3" ]; then
  echo "C-4b: ghc98 BYTE-IDENTICAL — the <<slot>> residual is GONE (likely fixed by C-1/C-8/9/10/C-4a-c)"
else
  echo "C-4b: ghc98 DIVERGES — residual live; v3 stderr tail:"; tail -3 /tmp/ghc.v3.err
fi
