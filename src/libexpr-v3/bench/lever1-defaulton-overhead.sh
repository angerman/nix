#!/usr/bin/env bash
# LEVER-1 #1.0 — pre-flip gate for applied-cache default-on.
#
# (A) SINGLE-eval overhead: cache-on vs off on hello/firefox/M5, N=5 medians.
#     A default-on cache pays arming/lookup/insert + GC-root walk on EVERY eval,
#     even single evals with no reuse.  GATE: <=2% CPU AND <=noise RSS; the
#     flip is KILLED if >5% CPU.  (The #16c WHNF pre-check should make this ~0.)
# (B) RSS-under-accumulation: eval K >> LRU-cap distinct (import f) arg_i in ONE
#     process; confirm RSS plateaus at the cap, not grows unbounded.  Uses a
#     generated set of K distinct single-attr args so each is a DISTINCT key.
#
# Run on darwin-4 (quiet host).  Cache default is still OFF at this commit, so
# "on" = NIX_V3_APPLIED_CACHE=1, "off" = unset.  After the #1.2 flip re-run to
# confirm the polarity inverted cleanly.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
N="${N:-5}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"

med() { sort -n | awk '{a[NR]=$0} END{ if(!NR){print "NA";exit} m=int((NR+1)/2); if(NR%2)print a[m]; else printf "%.2f",(a[m]+a[m+1])/2 }'; }

# one timed run → "cpu rssMB"  (user+sys; macOS /usr/bin/time -l -> $3 user $5 sys)
run_one() { # $1=cache(0|1) $2=expr $3=opts
    local out
    out=$(NIX_V3_APPLIED_CACHE=$1 NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=180s NIX_V3_MAX_HEAP=6G \
          /usr/bin/time -l "$NIX" eval --impure $3 --raw --expr "$2" 2>&1 >/dev/null) || {
        echo "FAIL" >&2; echo "$out" | tail -3 >&2; echo "NA NA"; return; }
    local cpu rss
    cpu=$(echo "$out" | awk '/ real .* user .* sys/{printf "%.3f",$3+$5}')
    rss=$(echo "$out" | awk '/maximum resident/{printf "%.0f",$1/1048576}')
    echo "${cpu:-NA} ${rss:-NA}"
}

cell() { # $1=label $2=cache $3=expr $4=opts
    local cs="" ms="" i r
    for i in $(seq "$N"); do r=$(run_one "$2" "$3" "$4"); cs+="${r%% *}"$'\n'; ms+="${r##* }"$'\n'; done
    echo "$(echo "$cs"|grep .|med) $(echo "$ms"|grep .|med)"
}

echo "== #1.0(A) single-eval overhead (N=$N, $(hostname)) =="
declare -A EXPR OPTS
EXPR[hello]='(import <nixpkgs> {}).hello.drvPath';   OPTS[hello]=''
EXPR[firefox]='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'; OPTS[firefox]=''
EXPR[M5]="(builtins.getFlake \"path:$CN\").packages.aarch64-darwin.cardano-node.name"
OPTS[M5]='--no-eval-cache --option allow-import-from-derivation true'
for w in hello firefox M5; do
  # warm the CU disk cache (compile-once workload model)
  run_one 0 "${EXPR[$w]}" "${OPTS[$w]}" >/dev/null
  read -r coff roff <<< "$(cell $w 0 "${EXPR[$w]}" "${OPTS[$w]}")"
  read -r con  ron  <<< "$(cell $w 1 "${EXPR[$w]}" "${OPTS[$w]}")"
  awk -v w="$w" -v a="$coff" -v b="$con" -v ra="$roff" -v rb="$ron" 'BEGIN{
    dc=(a>0)?100*(b-a)/a:0; dr=(ra>0)?100*(rb-ra)/ra:0;
    printf "%s  OFF %ss/%sMB  ON %ss/%sMB  Δcpu=%+.1f%% Δrss=%+.1f%%  %s\n",
      w,a,ra,b,rb,dc,dr, (dc<=2.0?"PASS":(dc>5.0?"KILL-FLIP":"GRAY")); }'
done

echo
echo "== #1.0(B) RSS-under-accumulation (K distinct args, one process) =="
# K distinct single-attr args → K distinct cache keys.  heavy-ish import so each
# result graph is non-trivial; LRU cap default 64.  Compare cap=8 vs cap=1024.
FIX="$ROOT/src/libexpr-v3/test/fixtures-applied-cache"
K=${K:-400}
gen() { # build `[ (import heavy { k0=0; }) (import heavy { k1=1; }) ... ]` length K
  local expr="[ " i
  for i in $(seq 0 $((K-1))); do expr+="(import $FIX/heavy.nix { k$i = $i; }) "; done
  echo "builtins.length $expr"
}
ACC="$(gen)"
for cap in 8 64 1024; do
  r=$(NIX_V3_APPLIED_CACHE=1 NIX_V3_APPLIED_CACHE_MAX_ENTRIES=$cap NIX_V3_DIRECT_EVAL=1 \
      NIX_V3_MAX_WALL_TIME=180s /usr/bin/time -l "$NIX" eval --impure --expr "$ACC" 2>&1 >/dev/null \
      | awk '/maximum resident/{printf "%.0f",$1/1048576}')
  echo "K=$K cap=$cap peakRSS=${r}MB"
done
echo "(RSS should be ~flat across caps if entries are LRU-bounded; a steep cap=1024 vs cap=8 rise = unbounded retention → lower default cap before flip)"
