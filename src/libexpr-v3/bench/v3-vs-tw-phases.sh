#!/usr/bin/env bash
# v3 VM vs stock-nix tree-walker (TW): CPU + memory, with the
# compilation/lowering phase measured SEPARATELY from eval (hot).
# See BENCH_V3_VS_TW_2026-06-04.md for the methodology + a recorded run.
#
# Same binary for both evaluators (controls for build differences):
#   TW : ./build/src/nix/nix eval ...            (default)
#   v3 : NIX_V3_DIRECT_EVAL=1 ... eval ...       (bytecode VM owns eval)
#
# Reliable in-process instruments (NOT hand-rolled wall — see the wall note):
#   TW  cpuTime + GC : NIX_SHOW_STATS=1  -> "cpuTime", gc.heapSize, nrThunks
#   v3  phase split  : V3_TIMING=1
#       - eval (hot)        = max(run=) over the "v3-direct timing" lines
#                             (the dominant top-level run; install/wrapper
#                              lines are sub-ms).  Measure WARM so nested
#                              import work is deserialize, not compile.
#       - compile/lowering  = the "v3-direct import timing" line's
#                             (parse + lower + optimise + compile), measured
#                             COLD (NIX_V3_NO_DISK_CACHE=1); this is the
#                             per-CU miss-path total across all imports.
#   memory (both)    : /usr/bin/time -l "maximum resident set size" is the
#                      FAIR peak-RSS metric.  Boehm RESERVES ~400 MB of
#                      mostly-non-resident address space (heapSize / boehm_heap
#                      are NOT comparable RSS).  v3 arena/elsewhere split:
#                      NIX_VM_STATS=1 "v3-direct memory:" line.
#
# WALL note: for wall-clock ratios use `hyperfine` (warm host, n>=15) — a
# hand-rolled timer cannot resolve the variance and mis-ranked cold-vs-warm
# in an earlier draft.  This harness reports the in-process CPU/phase/RSS
# instruments, which are low-variance and directly comparable; pair it with
# a hyperfine wall run for the headline wall ratio.
#
# WARM cache must be bytecode-cached but RESULT-cache-cold: use a fresh
# NIX_V3_CACHE_DIR per workload, pre-warmed once.  (A fully result-cached
# run does ~no work and reports a misleadingly tiny arena.)
#
# Usage:  nix develop -c bash src/libexpr-v3/bench/v3-vs-tw-phases.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="${ROOT:-/Users/angerman/Projects/iohk/nix}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
TIME=/usr/bin/time
LIM="NIX_V3_MAX_WALL_TIME=600s NIX_V3_MAX_HEAP=6G"

rss(){ grep -i "maximum resident set size" | awk '{printf "%.0f", $1/1048576}'; }

# tw NAME EXPR
tw(){
  local e="$2" err; err=$(mktemp)
  $TIME -l env NIX_SHOW_STATS=1 "$NIX" eval --impure --expr "$e" 2>"$err" >/dev/null
  local cpu rssv heap
  cpu=$(grep -oE '"cpuTime": [0-9.]+' "$err" | awk '{printf "%.3f",$2}')
  rssv=$(rss <"$err"); heap=$(grep -oE '"heapSize": [0-9]+' "$err" | awk '{printf "%.0f",$2/1048576}')
  printf '%-18s %-8s eval=%6ss  peakRSS=%5sMB  (boehm heapSize=%sMB)\n' "$1" TW "$cpu" "$rssv" "$heap"
  rm -f "$err"
}

# v3eval NAME EXPR CACHEDIR  (WARM: eval hot)
v3eval(){
  local e="$2" cd="$3" err; err=$(mktemp)
  $TIME -l env NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$cd" $LIM V3_TIMING=1 NIX_VM_STATS=1 \
    "$NIX" eval --impure --expr "$e" 2>"$err" >/dev/null
  local run rssv mem
  run=$(grep -oE 'run=[0-9.]+' "$err" | cut -d= -f2 | sort -n | tail -1 | awk '{printf "%.3f",$1/1000}')
  rssv=$(rss <"$err"); mem=$(grep "v3-direct memory:" "$err" | tail -1 | grep -oE 'v3_arena=[0-9.]+MB|elsewhere=[0-9.]+MB' | tr '\n' ' ')
  printf '%-18s %-8s eval=%6ss  peakRSS=%5sMB  (%s)\n' "" v3-warm "$run" "$rssv" "$mem"
  rm -f "$err"
}

# v3compile NAME EXPR  (COLD: compilation phase, from import-timing line)
v3compile(){
  local e="$2" err; err=$(mktemp)
  env NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 $LIM V3_TIMING=1 \
    "$NIX" eval --impure --expr "$e" 2>"$err" >/dev/null
  local it
  it=$(grep "v3-direct import timing" "$err" | tail -1)
  local cus comp
  cus=$(echo "$it" | grep -oE 'calls=[0-9]+' | cut -d= -f2)
  comp=$(echo "$it" | grep -oE 'parse=[0-9.]+|lower=[0-9.]+|optimise=[0-9.]+|compile=[0-9.]+' \
        | cut -d= -f2 | awk '{s+=$1}END{printf "%.0f",s}')
  printf '%-18s %-8s compile(%s CUs)=%sms  [parse+lower+optimise+compile, amortized by disk cache]\n' \
    "" v3-cold "${cus:-0}" "${comp:-0}"
  rm -f "$err"
}

run_real(){ # NAME EXPR
  local cd; cd=$(mktemp -d)
  env NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$cd" $LIM "$NIX" eval --impure --expr "$2" >/dev/null 2>&1
  tw "$1" "$2"; v3eval "$1" "$2" "$cd"; v3compile "$1" "$2"; echo; rm -rf "$cd"
}
run_pure(){ # NAME EXPR  (compile negligible; cache irrelevant)
  local cd; cd=$(mktemp -d)
  tw "$1" "$2"; v3eval "$1" "$2" "$cd"; echo; rm -rf "$cd"
}

echo "### Pure eval (no store/FFI/nixpkgs — isolates the VM-vs-TW eval gap)"
run_pure "fib33"         'let f=n: if n<2 then n else f(n - 1)+f(n - 2); in f 33'
run_pure "ackermann-3-7" 'let a=m: n: if m==0 then n+1 else if n==0 then a (m - 1) 1 else a (m - 1) (a m (n - 1)); in a 3 7'
run_pure "fold-add-1M"   "builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 1000000)"
echo "### Real workloads (store + FFI + nixpkgs; compile phase is heavy, disk-cache-amortized)"
run_real "hello.drvPath" '(import <nixpkgs> {}).hello.drvPath'
run_real "HNE.drvPath"   '(builtins.getFlake "/Users/angerman/Projects/iohk/haskell-nix-example").packages.x86_64-linux.hello.drvPath'
