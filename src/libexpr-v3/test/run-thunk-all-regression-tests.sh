#!/usr/bin/env bash
# #558 Phase 2 — regression-test wrapper for NIX_V3_INHERIT_FROM_THUNK_ALL=1.
#
# This is the functionally-correct STG inherit-from semantics (matching
# TW's `from->maybeThunk` unconditional thunkify).  It eliminates the
# eager-force cycles that the partial-Bindings registry currently
# masks.  Activating it default-on requires the per-force perf cost
# to be brought in line with TW (currently ~100x slower on full nixpkgs).
#
# This script ensures Phase 2 doesn't REGRESS any functional behavior
# while we optimize the perf side.  It runs the same v3 regression
# suites we run unconditionally, under NIX_V3_INHERIT_FROM_THUNK_ALL=1.
#
# If a test fails here that passes in default mode, that's a Phase 2
# correctness regression that must be fixed before flipping the flag
# default-on.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
    echo "nix not found at $NIX_BIN" >&2
    exit 1
fi

PASS=0; FAIL=0
fail_names=()

run_suite() {
    local name="$1"
    local script="$ROOT/src/libexpr-v3/test/$name"
    if [[ ! -x "$script" ]]; then
        echo "  SKIP $name (not found)" >&2
        return
    fi
    local out rc
    out=$(NIX_V3_INHERIT_FROM_THUNK_ALL=1 timeout 60 bash "$script" 2>&1)
    rc=$?
    if [[ $rc -eq 0 ]]; then
        PASS=$((PASS + 1))
        printf "  PASS %s\n" "$name"
    else
        FAIL=$((FAIL + 1))
        local last
        last=$(printf '%s\n' "$out" | tail -1)
        fail_names+=("$name: $last")
        printf "  FAIL %-40s %s\n" "$name" "$last"
    fi
}

echo "=== THUNK_ALL regression: confirm all v3 suites pass with"
echo "    NIX_V3_INHERIT_FROM_THUNK_ALL=1 (Phase 2 functional gate) ==="
echo

run_suite "run-558-emit-order-tests.sh"
run_suite "run-inherit-from-laziness-tests.sh"
run_suite "run-fix-inherit-from-self-tests.sh"
run_suite "run-cell-update-protocol-tests.sh"
run_suite "run-direct-eval-tests.sh"
run_suite "run-self-dot-thunkify-tests.sh"
run_suite "run-lexical-withs-tests.sh"
run_suite "run-mutual-circular-formals-tests.sh"
run_suite "run-bridge-attr-lookup-tests.sh"
run_suite "run-evalscope-tests.sh"

echo
echo "=== THUNK_ALL regression summary: $PASS pass, $FAIL fail ==="
if (( FAIL > 0 )); then
    for n in "${fail_names[@]}"; do
        echo "  FAIL: $n"
    done
    exit 1
fi
exit 0
