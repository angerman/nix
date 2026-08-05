#!/usr/bin/env bash
# P0 (REPRESENTATION_REWRITE_PLAN): per-type LIVE-vs-DEAD arena split at peak.
# Resolves the A1/A3 contradiction (A1 ~592MB live thunks vs A3 ~71MB).
# Method: NIX_V3_LIVE_TRACE_PERIODIC=<K MB> writes an L(t) CSV with per-type
# live bytes (live_closures/thunks/bindings_mb) + alloc_offset_mb (resident
# arena in the never-releasing bump arena).  The PEAK row (max alloc_offset)
# gives live-at-peak; DEAD-at-peak = alloc_offset - live.  Plus MEM_BUCKETS for
# the arena-vs-CU-vs-elsewhere resident split.  darwin-4 (deterministic counters
# are host-independent; run here for the real workloads' store availability).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$HOME/Projects/iohk/nix"; NIX="$ROOT/build/src/nix/nix"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"

run() { # $1=label $2=expr $3=opts $4=v3extra
  local csv="/tmp/v3-live-$1.csv"
  echo "===== $1 ====="
  env $4 NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_MEM_BUCKETS=1 \
      NIX_V3_LIVE_TRACE=1 NIX_V3_LIVE_TRACE_PERIODIC=200 \
      NIX_V3_LIVE_TRACE_PERIODIC_OUT="$csv" \
      NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=6G \
      "$NIX" eval --impure $3 --raw --expr "$2" 2>&1 >/dev/null \
    | grep -iE "Reachable:|RSS-decomp|arena=|CU-bytecode|major-mark-sweep|per-tag" | head -12
  echo "--- L(t) periodic CSV (peak row = max alloc_offset): $csv ---"
  # header + the last (peak) few rows
  head -1 "$csv" 2>/dev/null
  tail -3 "$csv" 2>/dev/null
  echo "--- live-vs-dead at peak (awk) ---"
  # CSV cols: 1 alloc_offset 2 resident 3 live_mb 4 L_resident 5 L_cum 6 wall
  # 7 live_closures 8 live_thunks 9 live_bindings 10 live_lists 11 live_pairs.
  awk -F, 'NR>1 && $2+0>0 { if($2+0>maxres){maxres=$2; live=$3; c=$7; t=$8; b=$9; l=$10; p=$11} }
    END{ if(maxres>0){ printf "  peak arena=%.0fMB  LIVE=%.0fMB (%.0f%%)  DEAD=%.0fMB (%.0f%%)  [live closures=%.0f thunks=%.0f bindings=%.0f lists=%.0f pairs=%.0f]\n",
      maxres, live, 100*live/maxres, maxres-live, 100*(maxres-live)/maxres, c, t, b, l, p } }' "$csv" 2>/dev/null
}

run firefox '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' '' ''
run M5 "(builtins.getFlake \"path:$CN\").packages.aarch64-darwin.cardano-node.name" '--no-eval-cache --option allow-import-from-derivation true' ''
