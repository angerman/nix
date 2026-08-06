#!/usr/bin/env bash
# bench/beat-tw-compare.sh — reliable TW-vs-v3 CPU+RSS head-to-head (P0.1 of
# lode/BEAT_TW_PLAN_2026-06-23.md).
#
# WHY THIS EXISTS: single-run / min-of-N numbers on darwin-4 are unreliable —
# v3 absolutes drift across sessions (a "min-of-5" once read firefox 1.81s/586MB
# but the stable value is ~2.65s/675MB), and NIX_VM_STATS perturbs peak RSS.  This
# harness measures TW and every v3 config BACK-TO-BACK in one session, reports the
# MEDIAN of N runs (not min), captures the spread (so noise is visible), and prints
# the v3/TW RATIO (which cancels cross-session drift since TW is the stable anchor).
# It does NOT set NIX_VM_STATS (which forces a teardown GC that perturbs peak RSS).
#
# MUST run on a QUIET host (darwin-4).  Usage:
#   bench/beat-tw-compare.sh                 # firefox + M5, N=5
#   N=7 bench/beat-tw-compare.sh firefox     # one workload, N=7
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
N="${N:-5}"
WL=("$@"); [ ${#WL[@]} -eq 0 ] && WL=(firefox M5)

# workload → (expr, extra `nix eval` opts).  M5 is a flake → --no-eval-cache so
# both engines re-evaluate (no flake eval-cache shortcut); firefox is a plain
# import (no eval cache).  Both cache-off for v3 (NIX_V3_NO_DISK_CACHE).
wl_expr() { case "$1" in
  firefox) echo '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
  M5)  echo "(builtins.getFlake \"path:$CN\").outputs.packages.aarch64-darwin.cardano-node.name" ;;
esac; }
wl_opts() { case "$1" in
  firefox) echo "" ;;
  M5)  echo "--no-eval-cache --option allow-import-from-derivation true" ;;
esac; }

# median of stdin numbers (one per line)
median() { sort -n | awk '{a[NR]=$0} END{ if(NR==0){print "NA";exit} m=int((NR+1)/2); if(NR%2) print a[m]; else printf "%.2f", (a[m]+a[m+1])/2 }'; }
spread() { sort -n | awk '{a[NR]=$0} END{ if(NR){printf "%s-%s", a[1], a[NR]} }'; }

# run config N times → echo "medCPU medRSS cpuSpread rssSpread"
measure() { local env="$1" expr="$2" opts="$3"
  local us=() ms=()
  # WARM mode (P0.2): one discarded warmup run so the v3 disk cache (bytecode)
  # is populated + FS/store warm — measures the PRODUCTION steady state where
  # parse+lower is amortized.  Cold mode skips this (NIX_V3_NO_DISK_CACHE makes
  # it a no-op anyway) to preserve the committed cache-off baseline methodology.
  [ "${WARM:-0}" = 1 ] && env $env "$NIX" eval --impure $opts --raw --expr "$expr" >/dev/null 2>&1
  for i in $(seq "$N"); do
    /usr/bin/time -l env $env "$NIX" eval --impure $opts --raw --expr "$expr" >/dev/null 2>/tmp/btc.$$
    us+=( "$(grep -E ' real ' /tmp/btc.$$ | awk '{print $3}')" )
    ms+=( "$(grep 'maximum resident' /tmp/btc.$$ | awk '{printf "%.0f", $1/1048576}')" )
  done
  local mc; mc=$(printf '%s\n' "${us[@]}" | median)
  local mm; mm=$(printf '%s\n' "${ms[@]}" | median)
  local sc; sc=$(printf '%s\n' "${us[@]}" | spread)
  local sm; sm=$(printf '%s\n' "${ms[@]}" | spread)
  echo "$mc $mm $sc $sm"
}

MODE="cache-off"; [ "${WARM:-0}" = 1 ] && MODE="WARM cache-on (production steady-state)"
echo "================================================================"
echo "beat-tw-compare — $(hostname -s) — $(date '+%Y-%m-%d %H:%M') — N=$N — $(uptime | sed 's/.*load/load/')"
echo "  ($MODE; median-of-$N; back-to-back; ratios vs stable TW anchor)"
echo "================================================================"
# WARM (P0.2): disk cache ON (parse+lower amortized).  Cold: cache OFF.
if [ "${WARM:-0}" = 1 ]; then V3="NIX_V3_DIRECT_EVAL=1"; else V3="NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1"; fi
for w in "${WL[@]}"; do
  expr="$(wl_expr "$w")"; opts="$(wl_opts "$w")"
  [ -z "$expr" ] && { echo "unknown workload $w"; continue; }
  echo; echo "#### $w ####"
  read tc tm tcs tms < <(measure ""                                              "$expr" "$opts")
  read dc dm dcs dms < <(measure "$V3"                                            "$expr" "$opts")
  printf "  %-16s CPU=%6ss  RSS=%6sMB\n" "TW (stock)" "$tc" "$tm"
  printf "  %-16s CPU=%6ss  RSS=%6sMB   (%.2f× CPU, %.2f× RSS vs TW)\n" "v3 default"  "$dc" "$dm" \
    "$(awk -v a=$dc -v b=$tc 'BEGIN{print a/b}')" "$(awk -v a=$dm -v b=$tm 'BEGIN{print a/b}')"
  printf "    spreads: TW cpu=%s rss=%s | v3 cpu=%s rss=%s\n" "$tcs" "$tms" "$dcs" "$dms"
done
rm -f /tmp/btc.$$
