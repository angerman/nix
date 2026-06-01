#!/usr/bin/env bash
# v3 FFI-consolidation lint (FFI_CONSOLIDATION_AUDIT_2026-06-01 §5.1 /
# Phase 6) — mechanically enforces the V3-NATIVE constraint at the HEADER
# surface: no v3 source file may gain a direct `#include "nix/...` unless
# it is already on the baseline allowlist (test/tw-include-baseline.txt).
#
# This is a RATCHET, not the strict end-state lint: the consolidation is
# multi-phase, so legitimate FFI leaves (store / fetchers / flake) + the
# not-yet-migrated files are baselined.  The lint catches:
#   (1) REGRESSION — a non-baselined file gains a TW include  → FAIL.
#   (2) PROGRESS  — a baselined file dropped all TW includes   → warn
#                   (informational: prune it from the baseline to tighten).
#
# End state (audit §5): the baseline shrinks to ffi.hh / ffi.cc /
# disk_cache.cc / parser/ + the lower_v3.hh translation boundary.
#
# Models test/lint-no-inline-getenv.sh.  Exit 0 = no regression; 1 =
# regression (new offender); 2 = preflight error.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3DIR="$ROOT/src/libexpr-v3"
BASELINE="$V3DIR/test/tw-include-baseline.txt"

if [[ ! -d "$V3DIR" ]]; then echo "lint-no-direct-tw-include: $V3DIR missing" >&2; exit 2; fi
if [[ ! -f "$BASELINE" ]]; then echo "lint-no-direct-tw-include: baseline $BASELINE missing" >&2; exit 2; fi

# Allowlist (baseline) — strip comments/blanks, sort.
allow=$(grep -vE '^\s*#|^\s*$' "$BASELINE" | sed 's/[[:space:]]*$//' | sort -u)

# Current offenders: v3 .cc/.hh files (excluding generated / build / the
# verbatim upstream parser snapshots) with a direct `#include "nix/`.
current=$(grep -rln '#include "nix/' "$V3DIR" 2>/dev/null \
    | grep -E '\.(cc|hh)$' \
    | grep -vE '\.gen\.hh|/builddir/|\.upstream$|v3-parser-tab|v3-parser-lex' \
    | sed "s#^$V3DIR/##" | sort -u)

# (1) Regression: in `current` but not in `allow`.
new_offenders=$(comm -23 <(echo "$current") <(echo "$allow"))
# (2) Progress: in `allow` but no longer in `current`.
cleaned=$(comm -13 <(echo "$current") <(echo "$allow"))

rc=0
if [[ -n "$new_offenders" ]]; then
    echo "lint-no-direct-tw-include: NEW direct TW includes (V3-NATIVE regression):" >&2
    echo "$new_offenders" | sed 's/^/  + /' >&2
    echo "" >&2
    echo "Route the dependency through include/v3/ffi.hh, or — if it is a" >&2
    echo "legitimate FFI leaf — add the file to test/tw-include-baseline.txt" >&2
    echo "with a one-line justification." >&2
    rc=1
fi

if [[ -n "$cleaned" ]]; then
    echo "lint-no-direct-tw-include: PROGRESS — these baselined files no longer" >&2
    echo "include any nix/ header; prune them from test/tw-include-baseline.txt" >&2
    echo "to tighten the ratchet:" >&2
    echo "$cleaned" | sed 's/^/  - /' >&2
fi

if [[ "$rc" -eq 0 && -z "$cleaned" ]]; then
    n=$(echo "$current" | grep -c . )
    echo "lint-no-direct-tw-include: clean ($n baselined files with TW includes; no regression)"
fi
exit "$rc"
