#!/usr/bin/env bash
# WS-3 W3 (2026-07-13) — build + verify an AOT cache file for a CI image.
#
# The AOT cache (aot_cache.cc, NIX_V3_AOT_CACHE_FILE) is a read-only mmap'd
# flat file consulted BEFORE SQLite for both CompilationUnits and IFD
# EvalResults.  Shipping it in the CI image makes the warm-cache immutable +
# distributable, and every hit is an O(log N) memcmp + zero-copy view instead
# of a SQLite point lookup.  N parallel evals share the file via the page
# cache.
#
# Flow (per AOT_DISTRIBUTION §7.2):
#   1. CANARY run — warm the SQLite disk cache AND record the insert manifest
#      (NIX_V3_AOT_BUILD_MODE=<manifest>): every disk_cache insert appends its
#      (table,key) so the manifest enumerates exactly what the workload touches.
#   2. build-aot-cache.py <manifest> -o <output> — pack those blobs from the
#      SQLite DB into the flat file.
#   3. VERIFY run — re-run with NIX_V3_AOT_CACHE_FILE=<output> + NIX_VM_STATS;
#      require disk_cache hit-rate >= 95% (pre-committed gate).
#
# Usage:
#   WORKLOAD='(import <nixpkgs> {}).hello.name' bench/build-aot-cache-ci.sh -o /path/aot.bin
#   (WORKLOAD defaults to a pinned <nixpkgs> eval; pass your CI's real eval.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
PY_BUILD="$ROOT/src/libexpr-v3/bench/build-aot-cache.py"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh" 2>/dev/null || true

OUTPUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output) OUTPUT="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -z "$OUTPUT" ]] && OUTPUT="$ROOT/build/v3-aot-cache-ci.bin"
# NOTE: do NOT use ${WORKLOAD:-<default>} — the default contains `}`, which
# terminates the parameter expansion early and mangles the expression.
if [[ -z "${WORKLOAD:-}" ]]; then
  WORKLOAD='(import <nixpkgs> { config.allowUnfree = true; }).hello.name'
fi

# Workdir under build/ (always writable + visible to child procs) rather than
# $TMPDIR — under `nix develop -c` $TMPDIR is an ephemeral /tmp/nix-shell.XXX
# the eval child cannot use, which silently produced an empty manifest.
mkdir -p "$ROOT/build"
WORKDIR="$(mktemp -d "$ROOT/build/aot-ci.XXXXXX")"
MANIFEST="$WORKDIR/manifest.txt"
CACHEDIR="$WORKDIR/cache"     # isolated SQLite DB so the canary starts clean
mkdir -p "$CACHEDIR"

echo "== [1/3] canary run: cold cache → insert + record manifest =="
# The manifest records on every disk_cache INSERT.  So the recording run must
# be COLD (fresh cache dir): it inserts every CU/IFD-result into SQLite AND
# appends each to the manifest.  A warm re-run would only do lookups (hits) and
# record nothing — the earlier bug here.
NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$CACHEDIR" NIX_V3_AOT_BUILD_MODE="$MANIFEST" \
  "$NIX" eval --impure --expr "$WORKLOAD" >/dev/null 2>&1
mlines=$(wc -l < "$MANIFEST" 2>/dev/null | tr -d ' ')
echo "   manifest: ${mlines:-0} (table,key) entries → $MANIFEST"
if [[ "${mlines:-0}" == "0" ]]; then
  echo "ERROR: empty manifest — workload inserted nothing into the disk cache." >&2
  exit 1
fi

echo "== [2/3] pack the flat AOT file =="
DB="$CACHEDIR/nix/v3-bytecode-v3.sqlite"
[[ -f "$DB" ]] || DB="$(find "$CACHEDIR" -name '*.sqlite' | head -1)"
nix develop -c python3 "$PY_BUILD" "$MANIFEST" "$DB" -o "$OUTPUT" || { echo "pack failed" >&2; exit 1; }
nix develop -c python3 "$PY_BUILD" --verify "$OUTPUT" || { echo "verify structure failed" >&2; exit 1; }
echo "   wrote $OUTPUT ($(wc -c < "$OUTPUT" | tr -d ' ') bytes)"

echo "== [3/3] verify hit-rate with the AOT file (fresh SQLite, so hits come from AOT) =="
FRESHCACHE="$WORKDIR/cache2"; mkdir -p "$FRESHCACHE"
stats=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$FRESHCACHE" NIX_V3_AOT_CACHE_FILE="$OUTPUT" \
        NIX_VM_STATS=1 "$NIX" eval --impure --expr "$WORKLOAD" 2>&1 >/dev/null \
        | grep 'disk_cache: lookups=' | head -1)
echo "   $stats"
hits=$(sed -n 's/.*hits=\([0-9]*\).*/\1/p' <<<"$stats")
misses=$(sed -n 's/.*misses=\([0-9]*\).*/\1/p' <<<"$stats")
: "${hits:=0}"; : "${misses:=0}"
total=$((hits + misses))
if [[ "$total" -eq 0 ]]; then
  echo "RESULT: no lookups observed — cannot verify (workload may cache nothing)." >&2
  rm -rf "$WORKDIR"; exit 1
fi
rate=$(awk "BEGIN{printf \"%.1f\", 100.0*$hits/$total}")
echo "   AOT hit-rate: $hits/$total = ${rate}%"
rm -rf "$WORKDIR"
awk "BEGIN{exit !($hits/$total >= 0.95)}" \
  && { echo "GATE PASS (>= 95%): $OUTPUT is ready to ship in the CI image (NIX_V3_AOT_CACHE_FILE=$OUTPUT)"; exit 0; } \
  || { echo "GATE FAIL (< 95%) — manifest/DB out of sync or workload not cache-stable"; exit 1; }
