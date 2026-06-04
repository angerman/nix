#!/usr/bin/env bash
# Regression test for the LIVE MEMORY BUCKETS report
# (dumpV3MemoryBuckets, gated NIX_V3_MEM_BUCKETS=1).
#
# The report is the honest, GHC-style resident decomposition: it splits
# arena LIVE bytes (precise mark, eval-first first-touch) into EVAL vs
# CU-cache, then adds CU-cache bytecode, FFI/Boehm-live, and the BC-cache
# SQLite page cache — replacing the misleading "v3_arena = cumulative
# bump counter" headline.
#
# Positive: under NIX_VM_STATS=1 + NIX_V3_MEM_BUCKETS=1 the report fires
#   EXACTLY ONCE (the multi-pass guard must filter the bytecode-primop
#   install passes), contains every bucket label, reports >= 1 cached CU
#   (the import populated the CU cache), and the eval result is correct.
# Negative: WITHOUT NIX_V3_MEM_BUCKETS the report must NOT appear (gate).
#
# Usage: ./run-mem-buckets-tests.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
FIXTURE="$ROOT/src/libexpr-v3/test/repro-mem-buckets.nix"

if [[ ! -x "$NIX" ]]; then
    echo "run-mem-buckets: nix binary not found at $NIX" >&2
    exit 2
fi
if [[ ! -f "$FIXTURE" ]]; then
    echo "run-mem-buckets: fixture not found at $FIXTURE" >&2
    exit 2
fi

fail() { echo "run-mem-buckets: FAIL — $1"; rm -rf "$CACHE"; exit 1; }

# Hermetic, fresh bytecode cache per run: the report itself doesn't care,
# but it keeps the test deterministic and isolates it from any warm-cache
# state in the shared $XDG_CACHE_HOME/nix DB.
CACHE="$(mktemp -d "${TMPDIR:-/tmp}/v3-mem-buckets-cache.XXXXXX")"

# -- Positive: report fires once, structurally complete, CU cache hit ----
OUT=$(env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_MEM_BUCKETS=1 \
        NIX_V3_CACHE_DIR="$CACHE" \
        NIX_V3_MAX_HEAP=2G NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --file "$FIXTURE" 2>&1)

# Eval result must be exactly 20000 (length of the 20000-entry attrset).
echo "$OUT" | grep -qx '20000' \
    || fail "eval result was not 20000 (got: $(echo "$OUT" | grep -vE '^(warning|trace|v3|Failed)' | tail -1))"

# The report must fire EXACTLY ONCE — the install-pass guard must work.
HDR=$(echo "$OUT" | grep -c 'v3 LIVE MEMORY BUCKETS')
[[ "$HDR" -eq 1 ]] || fail "expected report exactly once, saw $HDR (multi-pass guard?)"

# Every bucket label must be present.  Use BRE (grep without -E) so the
# literal parens in the labels are matched literally; `.*` skips the
# non-ASCII em-dash in the "CU cache —" lines.
for label in \
    'eval working set (arena)' \
    'CU cache.*result graph (arena)' \
    'CU cache.*bytecode (libc)' \
    'FFI / Boehm-live' \
    'BC cache (SQLite page cache)' \
    'accounted live' \
    'resident RSS (now)' \
    'arena LIVE' \
    'arena RESERVED'
do
    echo "$OUT" | grep -q "$label" || fail "report missing bucket line: '$label'"
done

# The import must have populated the CU cache: >= 1 cached CU.
CUS=$(echo "$OUT" | sed -nE 's/.*bytecode \(libc\) *[0-9.]+ *[KMGB]+ *([0-9]+) CUs.*/\1/p' | head -1)
[[ -n "$CUS" && "$CUS" -ge 1 ]] || fail "CU cache bytecode bucket reports < 1 CU (got '$CUS'); import not accounted"

# -- Negative: no gate => no report --------------------------------------
OUT2=$(env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 \
        NIX_V3_CACHE_DIR="$CACHE" \
        NIX_V3_MAX_HEAP=2G NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --impure --file "$FIXTURE" 2>&1)
if echo "$OUT2" | grep -q 'v3 LIVE MEMORY BUCKETS'; then
    fail "report appeared WITHOUT NIX_V3_MEM_BUCKETS (gate broken)"
fi

rm -rf "$CACHE"
echo "run-mem-buckets: PASS (report fires once; all buckets present; ${CUS} CU(s) cached; gate honoured)"
exit 0
