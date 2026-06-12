#!/usr/bin/env bash
#
# phase0-falsifiers.sh — Phase 0 of PLAN_BEAT_TW_2026-06-12.
#
# Zero/near-zero-code falsifiers that BOUND every later investment (measure-twice).
# Each probe prints the metric (min user-CPU over RUNS, max peak-RSS via
# /usr/bin/time -l) for each arm + byte-identity vs TW, so a human can read the
# keep/kill decision directly.
#
#   0.1  wrapper share        : v3 default (BC hybrid) vs NIX_V3_NO_BC_DERIVATION_HYBRID=1 (all-C)
#   0.2  RSS decomposition    : NIX_V3_MEM_BUCKETS=1 arena/boehm/elsewhere at HEAD
#   0.3  GC-threshold curve   : default (256 MB / 2.0) vs low (32 MB / 1.5)
#   (0.4 context-parse memo + 0.5 freeListBins need code; run after rebuild)
#
# Usage:  nix develop /Users/angerman/Projects/iohk/nix -c bash phase0-falsifiers.sh [hello|git|firefox|all]
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT=/Users/angerman/Projects/iohk/nix
NIX="${NIX:-$ROOT/build/src/nix/nix}"
RUNS="${RUNS:-3}"
WALL="${WALL:-300s}"
HEAP="${HEAP:-8G}"
WHICH="${1:-all}"

# workload expressions (drvPath; byte-identity is the correctness gate)
expr_hello='(import <nixpkgs> {}).hello.drvPath'
expr_git='(import <nixpkgs> {}).git.drvPath'
expr_firefox='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'

cpuof() { awk '/ user$/{print $1; exit}' "$1"; }   # first "N.NN user"
# /usr/bin/time -l line: "        N.NN user  ..."  -> grab the user field
usercpu() { grep -oE '[0-9]+\.[0-9]+ user' "$1" | grep -oE '^[0-9.]+' | head -1; }
rssmb()   { awk '/maximum resident/{printf "%.0f", $1/1048576; exit}' "$1"; }

# run_arm <tag> <expr> <env...> : RUNS reps, echo "minCPU maxRSS lastOut"
run_arm() {
  local tag="$1"; shift
  local expr="$1"; shift
  local min="" max=0 out="" i u r
  for ((i=0;i<RUNS;i++)); do
    /usr/bin/env "$@" /usr/bin/time -l "$NIX" eval --impure --expr "$expr" \
        >"/tmp/p0_$tag.out" 2>"/tmp/p0_$tag.err"
    u="$(usercpu /tmp/p0_$tag.err)"; r="$(rssmb /tmp/p0_$tag.err)"
    [[ -n "$u" ]] && { [[ -z "$min" ]] || awk "BEGIN{exit !($u<$min)}" && min="$u"; }
    [[ -n "$r" && "$r" -gt "$max" ]] 2>/dev/null && max="$r"
    out="$(cat /tmp/p0_$tag.out)"
  done
  echo "$min $max $out"
}

probe() {
  local name="$1" expr="$2" unfree="$3"
  printf '\n══════════ workload: %s ══════════\n' "$name"

  # TW reference (correctness oracle + CPU/RSS baseline)
  read tw_cpu tw_rss tw_out < <(run_arm "tw_$name" "$expr")
  printf 'TW                         : CPU=%-7s RSS=%-5sMB  %s\n' "${tw_cpu:-NA}" "${tw_rss:-NA}" "$tw_out"

  # v3 default arm (BC hybrid wrapper)
  read d_cpu d_rss d_out < <(run_arm "v3def_$name" "$expr" \
      NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME="$WALL" NIX_V3_MAX_HEAP="$HEAP")
  local d_ok="DIVERGE"; [[ "$d_out" == "$tw_out" ]] && d_ok="byte-identical"
  printf 'v3 default (BC hybrid)     : CPU=%-7s RSS=%-5sMB  ratio CPU=%sx RSS=%sx  [%s]\n' \
      "${d_cpu:-NA}" "${d_rss:-NA}" "$(awk "BEGIN{if(\"$tw_cpu\"!=\"\"&&$tw_cpu>0)printf \"%.2f\",$d_cpu/$tw_cpu;else print \"NA\"}")" \
      "$(awk "BEGIN{if($tw_rss>0)printf \"%.2f\",$d_rss/$tw_rss;else print \"NA\"}")" "$d_ok"

  # 0.1 — v3 NO_BC (all-C primDerivationStrictNative)
  read c_cpu c_rss c_out < <(run_arm "v3noBC_$name" "$expr" \
      NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_BC_DERIVATION_HYBRID=1 \
      NIX_V3_MAX_WALL_TIME="$WALL" NIX_V3_MAX_HEAP="$HEAP")
  local c_ok="DIVERGE"; [[ "$c_out" == "$tw_out" ]] && c_ok="byte-identical"
  printf '0.1 v3 NO_BC (all-C path)   : CPU=%-7s RSS=%-5sMB  ratio CPU=%sx RSS=%sx  [%s]\n' \
      "${c_cpu:-NA}" "${c_rss:-NA}" "$(awk "BEGIN{if(\"$tw_cpu\"!=\"\"&&$tw_cpu>0)printf \"%.2f\",$c_cpu/$tw_cpu;else print \"NA\"}")" \
      "$(awk "BEGIN{if($tw_rss>0)printf \"%.2f\",$c_rss/$tw_rss;else print \"NA\"}")" "$c_ok"
  printf '    -> wrapper share: CPU %s  (default %s vs C %s)\n' \
      "$(awk "BEGIN{if(\"$c_cpu\"!=\"\"&&$c_cpu>0)printf \"%.2fx\",$d_cpu/$c_cpu;else print \"NA\"}")" "${d_cpu:-NA}" "${c_cpu:-NA}"

  # 0.3 — low GC threshold (32 MB / growth 1.5)
  read g_cpu g_rss g_out < <(run_arm "v3lowgc_$name" "$expr" \
      NIX_V3_DIRECT_EVAL=1 NIX_V3_MAJOR_GC_THRESHOLD_MB=32 NIX_V3_MAJOR_GC_GROWTH=1.5 \
      NIX_V3_MAX_WALL_TIME="$WALL" NIX_V3_MAX_HEAP="$HEAP")
  local g_ok="DIVERGE"; [[ "$g_out" == "$tw_out" ]] && g_ok="byte-identical"
  printf '0.3 v3 lowGC (32MB/1.5)     : CPU=%-7s RSS=%-5sMB  ratio CPU=%sx RSS=%sx  [%s]\n' \
      "${g_cpu:-NA}" "${g_rss:-NA}" "$(awk "BEGIN{if(\"$tw_cpu\"!=\"\"&&$tw_cpu>0)printf \"%.2f\",$g_cpu/$tw_cpu;else print \"NA\"}")" \
      "$(awk "BEGIN{if($tw_rss>0)printf \"%.2f\",$g_rss/$tw_rss;else print \"NA\"}")" "$g_ok"
  printf '    -> GC trade vs default: RSS %+dMB  CPU %s\n' \
      "$(( ${g_rss:-0} - ${d_rss:-0} ))" \
      "$(awk "BEGIN{if(\"$d_cpu\"!=\"\"&&$d_cpu>0)printf \"%+.1f%%\",100*($g_cpu-$d_cpu)/$d_cpu;else print \"NA\"}")"

  # 0.2 — MEM_BUCKETS decomposition (default arm; stderr carries the report)
  printf '0.2 MEM_BUCKETS decomp (default arm):\n'
  NIX_V3_DIRECT_EVAL=1 NIX_V3_MEM_BUCKETS=1 NIX_V3_MAX_WALL_TIME="$WALL" NIX_V3_MAX_HEAP="$HEAP" \
      "$NIX" eval --impure --expr "$expr" >/dev/null 2>/tmp/p0_buckets_$name.err
  grep -iE 'bucket|arena|boehm|elsewhere|MB|peak|live' /tmp/p0_buckets_$name.err | sed 's/^/      /' | head -30
}

echo "phase0-falsifiers — host=$(hostname) ncpu=$(sysctl -n hw.ncpu) RUNS=$RUNS"
echo "nix=$NIX"
case "$WHICH" in
  hello)   probe hello "$expr_hello" no ;;
  git)     probe git "$expr_git" no ;;
  firefox) probe firefox "$expr_firefox" yes ;;
  all)     probe hello "$expr_hello" no
           probe git "$expr_git" no
           probe firefox "$expr_firefox" yes ;;
  *) echo "unknown workload: $WHICH"; exit 2 ;;
esac
