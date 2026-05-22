#!/usr/bin/env bash
# A12b regression: iterative valueEqual must handle depth-10000
# nested list/attrset comparison without C-stack overflow.
# Pre-fix this would hit kMaxCallDepth=5000 (or worse, segfault on
# C-stack overflow) — `valueEqual` recursed C-style through itself
# at every nested level.
#
# Post-fix (vm.cc valueEqual converted to explicit work-stack):
# depth=10000 evaluates in <1 s with a few KB of heap, no C-stack
# pressure.
#
# Usage: ./run-A12b-deep-nested-eq.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
FIXTURE="$ROOT/src/libexpr-v3/test/repro-A12b-deep-nested-eq.nix"

if [[ ! -x "$NIX" ]]; then
    echo "run-A12b: nix binary not found at $NIX" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "run-A12b: fixture not found at $FIXTURE" >&2
    exit 2
fi

OUT=$(env NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_HEAP=2G NIX_V3_MAX_WALL_TIME=30s \
        "$NIX" eval --impure --file "$FIXTURE" 2>&1 \
       | grep -v '^Failed\|^v3 limits\|^warning')
EXPECTED='{ attrsEq = true; differingDepth = false; listEq = true; }'
if [[ "$OUT" = "$EXPECTED" ]]; then
    echo "run-A12b: PASS (depth=10000 nested == iterative; no C-stack overflow)"
    exit 0
else
    echo "run-A12b: FAIL"
    echo "  expected: $EXPECTED"
    echo "  got:      $OUT"
    exit 1
fi
