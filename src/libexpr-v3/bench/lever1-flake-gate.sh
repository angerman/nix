#!/usr/bin/env bash
# LEVER-1 flake-workload double-eval characterization (task #16b).
#
# Same construct as bench/lever1-gate.sh but on the real haskell.nix flake
# workloads (M5 = cardano-node, HNE).  (The flake LAYER used to be TW-evaluated
# via NIX_V3_NO_NATIVE_CALL_FLAKE=1; that gate was retired — v3-native callFlake
# is now the sole path — so it is no longer set.)  The applied-import cache
# addresses the INNER v3 imports (nixpkgs + haskell.nix modules).  Reports, per
# workload,
# the eval#2 marginal CPU + steady RSS with the cache OFF vs ON.  These are
# CHARACTERIZATION rows (how much of each workload's re-eval the v1 cache
# already collapses), not a SHIP/KILL gate — the pre-committed gate ran on
# hello (lever1-gate.sh, SHIP 0.00x/1.00x, git note on e156a9874).
# haskell.nix module args are largely COMPUTED (unhashable by design in v1),
# so partial collapse is the expected outcome; the row quantifies it.
#
# Run on darwin-4 (quiet host).  N=3 default (flake evals are 4-12s each).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNE="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
N="${N:-3}"
WL=("$@"); [ ${#WL[@]} -eq 0 ] && WL=(M5 HNE)

wl_expr() { case "$1" in
  M5)  echo "(builtins.getFlake \"path:$CN\").packages.aarch64-darwin.cardano-node.name" ;;
  HNE) echo "(builtins.getFlake \"path:$HNE\").packages.aarch64-darwin.hello.drvPath" ;;
esac; }
OPTS="--no-eval-cache --option allow-import-from-derivation true"
V3ENV="NIX_V3_DIRECT_EVAL=1"

med() { sort -n | awk '{a[NR]=$0} END{ if(!NR){print "NA";exit} m=int((NR+1)/2); if(NR%2)print a[m]; else printf "%.2f",(a[m]+a[m+1])/2 }'; }

# measure CACHE(0|1) EXPR → "medCPU medRSSmb"  (user+sys; macOS time -l)
measure() {
  local cache="$1" expr="$2" cs=() ms=() i
  for i in $(seq "$N"); do
    /usr/bin/time -l env $V3ENV NIX_V3_APPLIED_CACHE=$cache \
      "$NIX" eval --impure $OPTS --raw --expr "$expr" >/dev/null 2>/tmp/l1fg.$$
    cs+=( "$(awk '/ real .* user .* sys/{printf "%.2f", $3+$5}' /tmp/l1fg.$$)" )
    ms+=( "$(awk '/maximum resident/{printf "%.0f",$1/1048576}' /tmp/l1fg.$$)" )
  done
  rm -f /tmp/l1fg.$$
  echo "$(printf '%s\n' "${cs[@]}"|med) $(printf '%s\n' "${ms[@]}"|med)"
}

echo "== LEVER-1 flake double-eval characterization (N=$N, $(hostname)) =="
for w in "${WL[@]}"; do
  E1="$(wl_expr "$w")"
  E2="builtins.seq ($E1) ($E1)"
  # one discarded warmup: IFD built + store/fs/CU-disk-cache warm
  env $V3ENV "$NIX" eval --impure $OPTS --raw --expr "$E2" >/dev/null 2>&1
  read -r c1off r1off <<< "$(measure 0 "$E1")"
  read -r c2off r2off <<< "$(measure 0 "$E2")"
  read -r c1on  r1on  <<< "$(measure 1 "$E1")"
  read -r c2on  r2on  <<< "$(measure 1 "$E2")"
  echo "$w E1_OFF cpu=${c1off}s rss=${r1off}MB | E2_OFF cpu=${c2off}s rss=${r2off}MB"
  echo "$w E1_ON  cpu=${c1on}s rss=${r1on}MB | E2_ON  cpu=${c2on}s rss=${r2on}MB"
  awk -v a="$c1off" -v b="$c2off" -v c="$c1on" -v d="$c2on" -v r1="$r1off" -v r2="$r2on" 'BEGIN{
    mo=b-a; mn=d-c;
    printf "%s eval#2 marginal: OFF %.2fs ON %.2fs (%.2fx of eval#1; collapse %.0f%%) | RSS E2on/E1off %.2fx\n",
      w, mo, mn, (c>0? mn/c : 0), (mo>0? 100*(mo-mn)/mo : 0), (r1>0? r2/r1 : 0) }' w="$w"
done
