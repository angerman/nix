#!/usr/bin/env bash
# bench/s0-reclaimable-ceiling.sh — Safepoint Foundation S0.1.
# Warm peak-time reclaimable-ceiling measurement: how much of the v3 arena is
# DEAD at peak RSS (= the RSS a compacting mid-eval GC could reclaim).  Uses the
# existing NIX_V3_LIVE_TRACE_PERIODIC sampler (mark-only, no move).  Because the
# arena grows monotonically without mid-eval GC, the LAST/max-arena CSV sample is
# peak; its L_resident (live/arena) gives the reclaimable fraction (1-L).
#
# Caveat: live_mb sums {closures,thunks,bindings,lists,pairs} only — it omits
# chars/envs/values, so (1-L) is an UPPER bound on reclaimable.  Back-correct via
# L_cumulative (cumulative 5-type allocated) vs arena total.
#
# MUST run on the quiet host (darwin-4).  WARM = a discarded warmup run populates
# the v3 bytecode disk cache; flake workloads pass --no-eval-cache as a CLI FLAG
# (NOT in the env — `env VAR=v --no-eval-cache nix` makes env treat the flag as
# the command).  Result git-noted to the measured commit.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; cd "$ROOT"
source src/libexpr-v3/test/nixpkgs-pin.sh
NIX="${NIX:-build/src/nix/nix}"; K="${K:-32}"
peakrow() { local c="$1"; [ -s "$c" ] || { echo "  (no rows — cache-served?)"; return; }; head -1 "$c"; tail -n +2 "$c" | sort -t, -k1 -n | tail -1; }
run() { local name="$1" envv="$2" cli="$3" expr="$4"; local csv="/tmp/s0-$name.csv"; rm -f "$csv"
  env $envv $NIX eval $cli --impure --raw --expr "$expr" >/dev/null 2>/dev/null
  env $envv NIX_V3_LIVE_TRACE_PERIODIC="$K" NIX_V3_LIVE_TRACE_PERIODIC_OUT="$csv" \
    $NIX eval $cli --impure --raw --expr "$expr" >/dev/null 2>/dev/null
  echo "#### $name ####"; peakrow "$csv"; echo; }
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNEP="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
run firefox "NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=4G" "" \
  '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'
run M5  "NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 NIX_V3_MAX_HEAP=8G" "--no-eval-cache" \
  "(builtins.getFlake \"path:$CN\").outputs.packages.aarch64-darwin.cardano-node.name"
run HNE "NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 NIX_V3_MAX_HEAP=6G" "--no-eval-cache" \
  "(builtins.getFlake \"path:$HNEP\").packages.aarch64-darwin.hello.drvPath"
