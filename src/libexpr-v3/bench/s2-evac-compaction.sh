#!/usr/bin/env bash
# bench/s2-evac-compaction.sh — Safepoint Foundation S2.1b driver.
#
# Drives the mid-eval MOVING compactor experiment: run a workload with the
# mid-eval mark-sweep + the conservative C-stack scan OFF (NIX_V3_NO_CONSERV_SCAN,
# proven safe for the live set — see lode/CONSERV_PIN_PROVENANCE_2026-06-25.md) +
# the evacuator driven to FULL compaction (no cell-pin — there are no conservative
# referents to pin once the scan is off).  Reports, per workload:
#   - byte-identity of the result vs TW (correctness gate)
#   - the evac stats line (movedCells / blocksFreed / freedRSS) from NIX_VM_STATS
#   - the post-sweep evac-opportunity density (did blocks actually empty?)
#
# The win condition for S2.1b: blocksFreed > 0 AND freedRSS > 0 (whole blocks
# emptied by compaction + munmap'd) WHILE staying byte-identical.  The S2.3 ceiling
# (git-note on abd17037d): firefox peak ~128MB arena reclaimable by perfect packing.
#
# EVAC_ENV is the set of evac gates — finalized from the subsystem map
# (mark_sweep.cc runEvacuation / EvacVisitor / cell-pin / EVAC_PCT).  Until then it
# is a placeholder; do NOT trust results until EVAC_ENV is confirmed against the
# actual gate names + the g_majorGcEnabled dependency is resolved.
#
# Correctness (byte-id) can run anywhere; the RSS ship-gate (freedRSS magnitude)
# MUST be re-measured on darwin-4 (peak RSS is perf — laptop noise floor too high).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"; cd "$ROOT"
source src/libexpr-v3/test/nixpkgs-pin.sh
NIX="${NIX:-build/src/nix/nix}"

# Base env: mid-eval GC on, conservative scan off, stats on.  EVAC_ENV (the
# evacuator gates) is injected from the environment / finalized post-map.
BASE_ENV="NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 NIX_V3_MAX_HEAP=4G NIX_V3_MAX_WALL_TIME=120s NIX_V3_MIDEVAL_GC=1 NIX_V3_NO_CONSERV_SCAN=1 NIX_VM_STATS=1"
# Full-compaction mode confirmed from the subsystem map (Explore 2026-06-26):
# EVAC + PRECISE_ONLY (no conservative block-exclusion) + PCT=1.0 (all blocks are
# candidates).  This DOES run end-to-end — moves ~1.18M cells, frees blocks, munmaps
# (firefox: blocksFreed=1 freedRSS=16.8MB, evac-brute TYPED dangle=0) — BUT currently
# ABORTS with "cannot stringify type tag=14" (Blackhole): a relocation bug moving an
# in-force (blackholed) thunk so its forcing-frame writeback target dangles.  Affects
# BOTH precise-only AND cell-pin modes under firefox PCT=1.0 (a config #174's synthetic
# brute never hit).  THIS SCRIPT IS THE REPRODUCER for that S2.1b bug.
EVAC_ENV="${EVAC_ENV:-NIX_V3_EVAC=1 NIX_V3_EVAC_PRECISE_ONLY=1 NIX_V3_EVAC_PCT=1.0 NIX_V3_EVAC_BRUTE=1}"

run() { # name expr [cli]
  local name="$1" expr="$2" cli="${3:-}"
  # Oracle: TW result (byte-id target).
  $NIX eval $cli --impure --raw --expr "$expr" 1>/tmp/s2-tw-$name.out 2>/dev/null
  set +e
  env $BASE_ENV $EVAC_ENV \
    $NIX eval $cli --impure --raw --expr "$expr" 1>/tmp/s2-v3-$name.out 2>/tmp/s2-v3-$name.err
  local rc=$?
  set -e
  echo "#### $name (exit=$rc) ####"
  if [ $rc -ne 0 ]; then
    echo "  ✗ ERROR/CRASH — $(tail -1 /tmp/s2-v3-$name.err)"
  elif diff -q /tmp/s2-v3-$name.out /tmp/s2-tw-$name.out >/dev/null; then
    echo "  ✓ byte-id vs TW ($(cat /tmp/s2-v3-$name.out))"
  else
    echo "  ✗ DIVERGENCE vs TW"; diff /tmp/s2-tw-$name.out /tmp/s2-v3-$name.out | head
  fi
  echo "  --- evac stats (last 3) ---"
  grep -E "v3 evac:|movedCells|blocksFreed|freedRSS|evac-opportunity:" /tmp/s2-v3-$name.err | tail -3
  echo
}

echo "BASE_ENV=$BASE_ENV"
echo "EVAC_ENV=$EVAC_ENV"
echo "(if EVAC_ENV is empty the evacuator is NOT engaged — set it from the subsystem map)"
echo
run hello   '(import <nixpkgs> {}).hello.drvPath'
run git     '(import <nixpkgs> {}).git.drvPath'
run firefox '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'
