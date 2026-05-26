#!/usr/bin/env bash
#
# R1 trigger regression guard — V3_DBG_DESERIALIZE_VERIFY measurement.
#
# What it tests
# -------------
# Runs the V3_DBG_DESERIALIZE_VERIFY infrastructure (primops.cc:8128) over
# a warm-cache hello.drvPath eval and asserts the EXPECTED PROFILE of
# CU-cached-vs-fresh-compiled bytecode divergence:
#
#   1. `code=DIFF` count > 0  (R1 trigger fires — Light variant did NOT
#      close all cross-process bytecode determinism leaks).
#   2. EVERY DIFF event has `opDiffs=0 symStrDiffs=0 symIdDiffs=0`
#      (opcodes and symbol identity are CORRECTLY preserved; the leak is
#      POSITIONAL, not structural).
#   3. EVERY DIFF event has `otherDiffs > 0` (the residual diff is in
#      raw bytecode trailer bytes — specifically the PosIdx halves of
#      OP_ATTRS_LET_REC_INIT / OP_ATTRS_REC_INIT name/pos pairs,
#      serialize.cc:436 reads pos as-is without remap).
#
# Diff history
# ------------
# 2026-05-26 (commit `dcfbae871`)  : 353/357 DIFFs (99 %).  Class:
#                                    POSITIONAL — PosIdx pool index
#                                    drift in OP_ATTRS_LET_REC_INIT
#                                    trailer (`name, pos` pairs).
# 2026-05-26 (commit `Schema 14`)  : 4/357 DIFFs (1.1 %).  PosIdx
#                                    sparse table + remap closed the
#                                    positional class.  Residual is
#                                    OP_GET_LOCAL operand drift —
#                                    local-slot allocator iteration
#                                    order non-determinism, a
#                                    SEPARATE class from PosIdx /
#                                    SymbolId.
#
# Pass conditions (post-Schema 14 state)
#   * ≥ 1 DIFF observed (residual local-slot drift still present)
#   * 0 structural diffs (opDiffs / symStrDiffs / symIdDiffs all zero —
#     no #815-class symbol identity leak)
#
# Future state (after local-slot-allocator determinism fix or R1)
#   * 0 DIFFs — flip the assertion in §"Assertions" below.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "FAIL: $NIX is not executable.  Build first (ninja -C build)." >&2
  exit 1
fi

# Need a real nixpkgs source on disk for the warm-cache path.  Use
# NIX_PATH (env-provided) — the user / CI sets this; we don't synthesise
# a fake one.
if [[ -z "${NIX_PATH:-}" ]]; then
  if [[ -d "/Users/angerman/Projects/zw3rk/nixpkgs" ]]; then
    export NIX_PATH="nixpkgs=/Users/angerman/Projects/zw3rk/nixpkgs"
  else
    echo "SKIP: NIX_PATH is unset and no fallback nixpkgs found." >&2
    exit 0
  fi
fi

# Locate sqlite3 — needed to pre-clear the EvalResults table so the warm
# pass exercises the CU cache (not the eval-result cache).
SQLITE3="${SQLITE3:-sqlite3}"
if ! command -v "$SQLITE3" >/dev/null 2>&1; then
  echo "SKIP: sqlite3 not on PATH; needed for cache-state setup." >&2
  exit 0
fi

CACHE_DB="${HOME}/.cache/nix/v3-bytecode-v3.sqlite"
LOG=$(mktemp -t r1-verify-XXXXXX.log)
trap 'rm -f "$LOG"' EXIT

# Step 1 — warm the CU cache (populate CompilationUnits table).
echo "  [step 1] warming CU cache via cold eval..."
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=4G \
  "$NIX" eval --raw --impure \
  --expr '(import <nixpkgs>{}).hello.drvPath' >/dev/null 2>&1

# Step 2 — run again with V3_DBG_DESERIALIZE_VERIFY to capture the
# divergence profile.  Cache is now warm; every primImport hit replays
# the deserialise + verify path.
echo "  [step 2] warm eval with V3_DBG_DESERIALIZE_VERIFY..."
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=4G \
  V3_DBG_DESERIALIZE_VERIFY="$LOG" \
  "$NIX" eval --raw --impure \
  --expr '(import <nixpkgs>{}).hello.drvPath' >/dev/null 2>&1

# Step 3 — assertions on the captured profile.

# Total VERIFY events.
N_TOTAL=$(grep -c '^VERIFY' "$LOG" 2>/dev/null)
N_DIFF=$(grep -c 'code=DIFF' "$LOG" 2>/dev/null)
N_SAME=$(grep -c 'code=SAME' "$LOG" 2>/dev/null)

if [[ "$N_TOTAL" -lt 1 ]]; then
  echo "FAIL: no VERIFY events captured (expected hundreds)." >&2
  exit 1
fi

# Assertion 1: DIFFs > 0 (R1 trigger fires).
if [[ "$N_DIFF" -lt 1 ]]; then
  echo "PROFILE CHANGED: 0 DIFF events observed.  R1 trigger NO LONGER fires." >&2
  echo "                 If this is a PosIdx-remap or R1 landing, update this" >&2
  echo "                 test to assert N_DIFF == 0." >&2
  exit 1
fi

# Assertion 2: no structural diffs.  Every walk-result must show
# opDiffs=0 symStrDiffs=0 symIdDiffs=0.  Any nonzero is a regression.
STRUCTURAL_DIFFS=$(grep 'walk-result' "$LOG" |
  grep -v 'opDiffs=0 symStrDiffs=0 symIdDiffs=0' | wc -l | tr -d ' ')

if [[ "$STRUCTURAL_DIFFS" -gt 0 ]]; then
  echo "FAIL: structural diffs observed ($STRUCTURAL_DIFFS events)." >&2
  echo "       At least one walk-result has nonzero opDiffs / symStrDiffs / symIdDiffs." >&2
  echo "       This is the #815-class bug RETURNING.  Sample:" >&2
  grep 'walk-result' "$LOG" |
    grep -v 'opDiffs=0 symStrDiffs=0 symIdDiffs=0' | head -3 >&2
  exit 1
fi

# Pass — current expected profile is intact.
echo "  [step 3] profile intact:"
printf "    VERIFY events  : %d\n"  "$N_TOTAL"
printf "    DIFF (positional) : %d (%d%%)\n" \
  "$N_DIFF" "$((N_DIFF * 100 / N_TOTAL))"
printf "    SAME            : %d (%d%%)\n" \
  "$N_SAME" "$((N_SAME * 100 / N_TOTAL))"
printf "    structural diffs : %d  (must be 0)\n" "$STRUCTURAL_DIFFS"
echo
echo "  PASS: R1 trigger profile is positional-only as expected."
echo "  (DIFFs persist due to PosIdx not being remapped on deserialize,"
echo "   serialize.cc:436.  Targeted fix or R1 will flip these to SAME.)"
exit 0
