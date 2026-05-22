#!/usr/bin/env bash
# A12b regression: iterative `forceDeep` must walk a depth-5000
# attrset+list tree without C-stack overflow.  Pre-fix
# `print.cc::forceDeep` C-recursed at every nested level (with a
# DeepForceGuard pushed per level for GC safety) — depth 5000 hits
# kMaxCallDepth=5000 (or segfaults on C-stack overflow).  Post-fix,
# uses `tlDeepForceRoots` as the GC-protected work queue and
# processes in <1 s.
#
# Usage: ./run-A12b-deep-force.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
FIXTURE="$ROOT/src/libexpr-v3/test/repro-A12b-deep-force.nix"

if [[ ! -x "$NIX" ]]; then
    echo "run-A12b-force: nix binary not found at $NIX" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "run-A12b-force: fixture not found at $FIXTURE" >&2
    exit 2
fi

OUT=$(env NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_HEAP=2G NIX_V3_MAX_WALL_TIME=30s \
        "$NIX" eval --impure --file "$FIXTURE" 2>&1 \
       | grep -v '^Failed\|^v3 limits\|^warning')
EXPECTED='"ok"'
if [[ "$OUT" = "$EXPECTED" ]]; then
    echo "run-A12b-force: PASS (depth=5000 attrs+list deepSeq forced iteratively)"
    exit 0
else
    echo "run-A12b-force: FAIL"
    echo "  expected: $EXPECTED"
    echo "  got:      $OUT"
    exit 1
fi
