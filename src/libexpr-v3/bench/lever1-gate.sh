#!/usr/bin/env bash
# LEVER-1 applied-import cache — darwin-4 double-eval gate (pre-committed thresholds).
#
# Measures, per NEXT_LEVERS_2026-07-04.md §LEVER-1 gates:
#   eval#2 marginal CPU = median(E2,cache-on) - median(E1,cache-on)
#     SHIP: <=0.30x eval#1 CPU     KILL: >0.60x
#   steady RSS = median maxRSS(E2,cache-on) vs median maxRSS(E1,cache-off)
#     SHIP: <=1.3x                 KILL: >2x
#
# E1 = single hello.drvPath eval; E2 = the same eval twice via two SEPARATE
# `import <nixpkgs> {}` applications in one process (the cache's target shape).
# WARM shape: CU disk cache stays ON and is pre-warmed — the user's binding
# scenario is compile-once/eval-many, and cold parse+lower in eval#1 would
# flatter the marginal ratio. Runs each cell N times (default 5), reports
# medians. Deterministic pin via test/nixpkgs-pin.sh. Run on darwin-4 (quiet
# host) — laptop numbers are noise.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/.."   # src/libexpr-v3

N=${N:-5}
NIXBIN=${NIXBIN:-../../build/src/nix/nix}
source test/nixpkgs-pin.sh

E1='builtins.seq ((import <nixpkgs> {}).hello.drvPath) "done"'
E2='builtins.seq ((import <nixpkgs> {}).hello.drvPath) (builtins.seq ((import <nixpkgs> {}).hello.drvPath) "done")'

# one timed run: prints "<user+sys seconds> <maxrss-bytes>"
# macOS /usr/bin/time -l line: "  X.XX real  Y.YY user  Z.ZZ sys" -> $3=user $5=sys
run_one() { # $1=expr $2=cache(0|1)
    local out
    out=$(NIX_V3_APPLIED_CACHE=$2 NIX_V3_DIRECT_EVAL=1 \
          NIX_V3_MAX_WALL_TIME=120s NIX_V3_MAX_HEAP=4G \
          /usr/bin/time -l "$NIXBIN" eval --no-eval-cache --impure --expr "$1" 2>&1 >/dev/null) || {
        echo "EVAL FAILED:" >&2; echo "$out" >&2; exit 1; }
    local cpu rss
    cpu=$(echo "$out" | awk '/ real .* user .* sys/{printf "%.3f", $3+$5}')
    rss=$(echo "$out" | awk '/maximum resident set size/{print $1}')
    echo "$cpu $rss"
}

median() { sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

cell() { # $1=label $2=expr $3=cache
    local cpus="" rsss="" i r
    for i in $(seq "$N"); do
        r=$(run_one "$2" "$3")
        cpus+="${r%% *}"$'\n'; rsss+="${r##* }"$'\n'
    done
    local mc mr
    mc=$(echo "$cpus" | grep . | median)
    mr=$(echo "$rsss" | grep . | median)
    echo "$1 cpu_median=${mc}s rss_median=$((mr/1024/1024))MB"
    eval "${1}_CPU=$mc; ${1}_RSS=$mr"
}

echo "== LEVER-1 double-eval gate (N=$N, $(hostname)) =="
echo "-- warming CU disk cache (1 discarded E2 run) --"
run_one "$E2" 0 >/dev/null
cell E1_OFF "$E1" 0
cell E2_OFF "$E2" 0
cell E1_ON  "$E1" 1
cell E2_ON  "$E2" 1

awk -v e1on="$E1_ON_CPU" -v e2on="$E2_ON_CPU" -v e1off="$E1_OFF_CPU" -v e2off="$E2_OFF_CPU" \
    -v r2on="$E2_ON_RSS" -v r1off="$E1_OFF_RSS" 'BEGIN{
    marg = e2on - e1on; ratio = marg / e1on; rss = r2on / r1off;
    printf "eval#2 marginal CPU = %.3fs = %.2fx eval#1 (OFF marginal %.3fs)\n", marg, ratio, e2off - e1off;
    printf "steady RSS E2-on/E1-off = %.2fx\n", rss;
    printf "CPU verdict: %s (SHIP<=0.30 KILL>0.60)\n", ratio<=0.30?"SHIP":(ratio>0.60?"KILL":"GRAY");
    printf "RSS verdict: %s (SHIP<=1.3 KILL>2.0)\n",  rss<=1.3?"SHIP":(rss>2.0?"KILL":"GRAY");
}'
