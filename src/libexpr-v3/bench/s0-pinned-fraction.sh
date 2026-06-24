#!/usr/bin/env bash
# bench/s0-pinned-fraction.sh — Safepoint Foundation S0.2 (dynamic half).
# Bartlett pinned-fraction: the % of mid-eval live cells reachable ONLY via the
# conservative C-stack scan (= cells a mostly-copying collector cannot move).
# Reads the existing "v3 evac-movability: ... (N% pinned by C-stack)" line emitted
# by the mid-eval mark-sweep under NIX_V3_MIDEVAL_GC=1 NIX_VM_STATS=1.  The
# pinned fraction SHRINKS as the precise live set grows; the LAST/largest sweep is
# the peak-relevant value (firefox ~12.5%, M5 ~3.6% at HEAD d12d11e5e).
# (Static site census lives in lode/SAFEPOINT_FOUNDATION_S0_FINDINGS — git-noted.)
# darwin-4 only.  Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; cd "$ROOT"
source src/libexpr-v3/test/nixpkgs-pin.sh
NIX="${NIX:-build/src/nix/nix}"
run() { local name="$1" envv="$2" cli="$3" expr="$4"
  echo "#### $name ####"
  env $envv NIX_V3_MIDEVAL_GC=1 NIX_VM_STATS=1 \
    $NIX eval $cli --impure --raw --expr "$expr" >/dev/null 2>/tmp/s0pin-$name.err
  grep -E "evac-movability" /tmp/s0pin-$name.err; echo; }
CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
run firefox "NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 NIX_V3_MAX_HEAP=4G" "" \
  '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'
run M5 "NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_NATIVE_CALL_FLAKE=1 NIX_V3_MAX_HEAP=8G" "--no-eval-cache" \
  "(builtins.getFlake \"path:$CN\").outputs.packages.aarch64-darwin.cardano-node.name"
