#!/usr/bin/env bash
# bench/profile-workloads.sh — eval-time DECOMPOSITION for real (IFD) workloads.
#
# WHY: the v3-vs-TW CPU ratio only matters to the extent eval CPU is a large share
# of the WALL clock.  For IFD-heavy haskell.nix workloads (cardano-node, simplex-chat,
# haskell-nix-example) a big chunk of wall is the IFD-build subprocess + store I/O —
# work NO eval engine (and no JIT) can speed up.  This harness decomposes wall into:
#
#   wall (real)
#     ├─ run-phase eval CPU   ≈ WARM user      ← the ONLY part a JIT can address
#     ├─ parse+lower CPU      ≈ COLD user − WARM user   (cache-amortizable, not JIT)
#     └─ wait (IFD + store IO)≈ real − user − sys       (engine-independent)
#
# and reports, per workload, the JIT-ADDRESSABLE fraction = WARM run-eval / WARM wall.
# That is the ceiling on what beating-TW-on-eval-CPU buys for that workload.
#
# WARM = all caches populated + IFD built (production steady state).
# COLD = eval cache off / v3 disk cache off (re-parse+lower); IFD store still built.
# The one-time IFD BUILD itself is not in steady state — measure it separately with
#   `time nix build <the plan-to-nix drv>` if needed; here we focus on re-eval cost.
#
# MUST run on a QUIET host (darwin-4) — wall/wait are contention-sensitive (the laptop
# inflates `wait` badly).  Deterministic v3 counters (arena, thunks) are host-independent.
#
# Usage:
#   bench/profile-workloads.sh                         # firefox cardano-node HNE, N=3
#   N=5 WORKLOADS="cardano-node HNE" bench/profile-workloads.sh
#   SIMPLEX_EXPR='(builtins.getFlake "path:/p").packages.aarch64-darwin.X.name' \
#     WORKLOADS="simplex-chat" bench/profile-workloads.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"
N="${N:-3}"
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNE="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
SIMPLEX="${SIMPLEX_PATH:-/Users/angerman/Projects/zw3rk/simplex-chat}"
WORKLOADS="${WORKLOADS:-firefox cardano-node HNE}"

# workload → eval expression.  haskell.nix `.name` / `.drvPath` forces the full
# package-set eval + any IFD.  Override simplex-chat via SIMPLEX_EXPR.
wl_expr() { case "$1" in
  firefox)      echo '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
  cardano-node) echo "(builtins.getFlake \"path:$CN\").packages.aarch64-darwin.cardano-node.name" ;;
  HNE)          echo "(builtins.getFlake \"path:$HNE\").packages.aarch64-darwin.hello.drvPath" ;;
  simplex-chat) echo "${SIMPLEX_EXPR:-}" ;;
  *) echo "" ;;
esac; }
# IFD workloads need IFD + flake eval-cache off (re-eval each run).
wl_opts() { case "$1" in
  firefox) echo "" ;;
  *) echo "--no-eval-cache --option allow-import-from-derivation true" ;;
esac; }
# v3 native-flake bridge off on the flake workloads (matches beat-tw-compare).
wl_v3xtra() { case "$1" in firefox) echo "" ;; *) echo "NIX_V3_NO_NATIVE_CALL_FLAKE=1" ;; esac; }

med() { sort -n | awk '{a[NR]=$0} END{ if(!NR){print "NA";exit} m=int((NR+1)/2); if(NR%2)print a[m]; else printf "%.2f",(a[m]+a[m+1])/2 }'; }

# measure ENGINE_ENV EXPR OPTS WARMFLAG → "medReal medUser medSys medRSSmb"
measure() {
  local env="$1" expr="$2" opts="$3" warm="$4"
  # one discarded warmup: builds IFD + populates store/fs/disk-cache
  env $env "$NIX" eval --impure $opts --raw --expr "$expr" >/dev/null 2>&1
  local rs=() us=() ss=() ms=()
  local i
  for i in $(seq "$N"); do
    /usr/bin/time -l env $env "$NIX" eval --impure $opts --raw --expr "$expr" >/dev/null 2>/tmp/pw.$$
    rs+=( "$(grep -oE '^[ ]*[0-9.]+ real' /tmp/pw.$$ | grep -oE '[0-9.]+' | head -1)" )
    us+=( "$(grep -oE '[0-9.]+ user' /tmp/pw.$$ | grep -oE '[0-9.]+' | head -1)" )
    ss+=( "$(grep -oE '[0-9.]+ sys'  /tmp/pw.$$ | grep -oE '[0-9.]+' | head -1)" )
    ms+=( "$(grep 'maximum resident' /tmp/pw.$$ | awk '{printf "%.0f",$1/1048576}')" )
  done
  echo "$(printf '%s\n' "${rs[@]}"|med) $(printf '%s\n' "${us[@]}"|med) $(printf '%s\n' "${ss[@]}"|med) $(printf '%s\n' "${ms[@]}"|med)"
}

echo "================================================================================"
echo "profile-workloads — $(hostname -s) — $(date '+%Y-%m-%d %H:%M') — N=$N — $(uptime|sed 's/.*load/load/')"
echo "  WARM=caches+IFD built (production); COLD=eval/disk-cache off (re-parse+lower)"
echo "  user≈eval CPU | real-user-sys≈IFD+store wait | JIT can only touch WARM run-eval"
echo "================================================================================"

V3WARM="NIX_V3_DIRECT_EVAL=1"
V3COLD="NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1"

for w in $WORKLOADS; do
  expr="$(wl_expr "$w")"; opts="$(wl_opts "$w")"; x="$(wl_v3xtra "$w")"
  echo; echo "#### $w ####"
  if [ -z "$expr" ]; then echo "  (no eval expression configured — set SIMPLEX_EXPR / wire wl_expr)"; continue; fi

  read twR twU twS twM < <(measure ""                       "$expr" "$opts" warm)
  read vwR vwU vwS vwM < <(measure "$V3WARM $x NIX_V3_MAX_HEAP=8G" "$expr" "$opts" warm)
  read vcR vcU vcS vcM < <(measure "$V3COLD $x NIX_V3_MAX_HEAP=8G" "$expr" "$opts" cold)

  # all derived math in one awk per line, with safe defaults (empty→0).
  twWait=$(awk "BEGIN{d=(${twR:-0})-(${twU:-0})-(${twS:-0}); printf \"%.2f\",(d>0?d:0)}")
  vwWait=$(awk "BEGIN{d=(${vwR:-0})-(${vwU:-0})-(${vwS:-0}); printf \"%.2f\",(d>0?d:0)}")
  jit=$(awk "BEGIN{printf \"%.0f\",((${vwR:-0})>0?100*(${vwU:-0})/(${vwR:-0}):0)}")
  pl=$(awk "BEGIN{d=(${vcU:-0})-(${vwU:-0}); printf \"%.2f\",(d>0?d:0)}")
  cpux=$(awk "BEGIN{printf \"%.2f\",((${twU:-0})>0?(${vwU:-0})/(${twU:-0}):0)}")
  rssx=$(awk "BEGIN{printf \"%.2f\",((${twM:-0})>0?(${vwM:-0})/(${twM:-0}):0)}")

  printf "  %-12s WARM wall=%7ss evalCPU=%7ss wait=%7ss RSS=%7sMB\n" "TW" "$twR" "$twU" "$twWait" "$twM"
  printf "  %-12s WARM wall=%7ss evalCPU=%7ss wait=%7ss RSS=%7sMB\n" "v3" "$vwR" "$vwU" "$vwWait" "$vwM"
  printf "  %-12s COLD wall=%7ss evalCPU=%7ss (parse+lower≈%ss, amortized warm)\n" "v3" "$vcR" "$vcU" "$pl"
  printf "  → v3 WARM: JIT-addressable (run-eval/wall) = %s%%   | CPU %s× TW | RSS %s× TW\n" \
    "$jit" "$cpux" "$rssx"
done
rm -f /tmp/pw.$$
echo
echo "Read: JIT-addressable% = the share of WARM wall a perfect eval JIT could attack."
echo "Low % ⇒ the workload is IFD/store-bound; eval-engine speed (and a JIT) barely move wall."
