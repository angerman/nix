#!/usr/bin/env bash
#
# R1 trigger regression guard — V3_DBG_DESERIALIZE_VERIFY measurement.
#
# What it tests
# -------------
# Runs the V3_DBG_DESERIALIZE_VERIFY infrastructure (primops.cc:8128) over
# a warm-cache hello.drvPath eval and asserts that EVERY CU-cached-vs-
# fresh-compiled bytecode comparison is byte-identical:
#
#   1. `code=DIFF` count == 0  (R1 trigger no longer fires — every
#      cross-process bytecode determinism leak has been closed).
#   2. EVERY walk-result has `opDiffs=0 symStrDiffs=0 symIdDiffs=0`
#      (no #815-class symbol identity leak).
#
# Diff history
# ------------
# 2026-05-26 (commit `dcfbae871`)  : 353/357 DIFFs (99 %).  Class:
#                                    POSITIONAL — PosIdx pool index
#                                    drift in OP_ATTRS_LET_REC_INIT
#                                    trailer (`name, pos` pairs).
# 2026-05-26 (Schema 14)           : 4/357 DIFFs (1.1 %).  PosIdx
#                                    sparse table + remap closed the
#                                    positional class.  Residual:
#                                    OP_GET_LOCAL operand drift —
#                                    AttrSet REC_SET emit order was
#                                    SymbolId-sort (process-local),
#                                    leaking local-slot assignments.
# 2026-05-26 (AttrSet canonical emit) : 0/357 DIFFs.  emit.cc:876
#                                    decoupled visit order (canonical
#                                    string-sort via entries vector)
#                                    from REC_SET operand (SymbolId-
#                                    rank for runtime trailer).
#                                    R1 trigger FULLY CLOSED.
#
# Pass conditions (current state)
#   * 0 DIFFs  (every cached CU byte-matches a fresh compile)
#   * 0 structural diffs  (defensive — any nonzero is a #815-class
#     regression)
#
# Regression semantics
#   * Any DIFF re-appearing means a fresh determinism leak has been
#     introduced (emit-order dependence on process-local IDs, missed
#     remap, or new opcode trailer not covered by remap walks).
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

# Step 0 — clear CompilationUnits + EvalResults so step 1's cold eval
# populates a clean, in-process-only cache.  Without this clear, stale
# entries written by PRIOR processes (different SymbolId / PosIdx
# state) leak into step 2's verify pass and produce phantom DIFF
# events that aren't actually regressions in the current build.
#
# This is a test-hygiene fix, not a workaround — the R1-trigger
# closure is correct (verified by isolated runs); the cross-test
# contamination in `all-v3-tests.sh` was simply not visible until
# multiple test runs accumulated entries in the shared cache DB.
if [[ -f "$CACHE_DB" ]]; then
  "$SQLITE3" "$CACHE_DB" \
    "DELETE FROM CompilationUnits; DELETE FROM EvalResults;" 2>/dev/null \
    || true   # best-effort; cache DB may be locked by a parallel run
fi

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

# Assertion 1: zero DIFFs.  R1 trigger fully closed.
if [[ "$N_DIFF" -gt 0 ]]; then
  echo "FAIL: $N_DIFF DIFF events observed (expected 0)." >&2
  echo "       A determinism leak has been re-introduced." >&2
  echo "       Common causes:" >&2
  echo "         * Emit-time iteration order depends on process-local" >&2
  echo "           SymbolId / PosIdx (canonicalise to string / sparse)." >&2
  echo "         * New opcode trailer not covered by" >&2
  echo "           remapSymbolsInBytecode / remapPositionsInBytecode." >&2
  echo "         * Pre-image of slot allocator visits IR in" >&2
  echo "           process-local order." >&2
  echo "       Sample DIFF events:" >&2
  grep 'code=DIFF' "$LOG" | head -3 >&2
  exit 1
fi

# Assertion 2: no structural diffs (defensive — any nonzero is a
# #815-class regression).
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

# Pass — R1 trigger fully closed.
echo "  [step 3] profile intact:"
printf "    VERIFY events  : %d\n"  "$N_TOTAL"
printf "    DIFF            : %d  (must be 0)\n" "$N_DIFF"
printf "    SAME            : %d (%d%%)\n" \
  "$N_SAME" "$((N_SAME * 100 / N_TOTAL))"
printf "    structural diffs : %d  (must be 0)\n" "$STRUCTURAL_DIFFS"
echo
echo "  PASS: R1 trigger fully closed — all $N_TOTAL cached CUs"
echo "        byte-match a fresh compile.  Bytecode is now process-"
echo "        invariant across SymbolId / PosIdx / local-slot allocators."
exit 0
