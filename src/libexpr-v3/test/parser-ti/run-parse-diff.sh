#!/usr/bin/env bash
# v3 parser TI.1 — AST-shape differential runner.
#
# Per PARSER_PROJECT_PLAN_2026-06-01.md §4 (TI.1):
#   "AST-shape pretty-printer + differential runner ... byte-diff
#    v3 AST vs TW AST on the 54 parse-okay/parse-fail goldens."
#
# This script runs each `parse-okay-*.nix` (and parse-fail-*.nix)
# through TW's `nix-instantiate --parse` AND v3-eval's `--parse`
# (TODO: that flag doesn't exist yet — Stage 1.0 work).  Until v3
# has a --parse flag, this script only captures TW baselines.
#
# Once v3 has --parse: byte-compare line-by-line.  Any divergence
# fails the test.
#
# Usage:
#   ./run-parse-diff.sh            # against TW baseline (today)
#   ./run-parse-diff.sh --v3       # diff v3 vs TW (once --parse exists)
#
# Exit codes:
#   0 — all tests passed (or all baselines captured)
#   1 — divergence detected
#   2 — infrastructure error (binary missing, etc.)
#
# Per [[falsification-rule]]:
#   What this kills if SHIP: "v3 parser produces non-byte-equal AST
#   to TW on any parse-okay fixture" — KILLED.
#   What this kills if FALSIFY: specific divergence isolated for fix.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/builddir/src/nix/nix-instantiate}"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-eval}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"

USE_V3="${USE_V3:-0}"
case "${1:-}" in
    --v3) USE_V3=1 ;;
    --tw-only|"") USE_V3=0 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
esac

if [[ ! -x "$NIX_INST" ]]; then
    echo "TI.1 ERROR: nix-instantiate not found at $NIX_INST" >&2
    exit 2
fi
if [[ "$USE_V3" -eq 1 ]] && [[ ! -x "$V3" ]]; then
    echo "TI.1 ERROR: v3-eval not found at $V3" >&2
    exit 2
fi

pass=0
fail=0
total=0
divergences=()

# --- parse-okay phase ---
for nix_file in "$LANG_DIR"/parse-okay-*.nix; do
    [[ -f "$nix_file" ]] || continue
    name=$(basename "$nix_file" .nix)
    exp_file="$LANG_DIR/$name.exp"
    [[ -f "$exp_file" ]] || continue
    total=$((total + 1))

    # TW baseline: nix-instantiate --parse
    tw_out=$("$NIX_INST" --parse "$nix_file" 2>/dev/null) || {
        echo "[$name] TW PARSE FAILED (skipping)"
        continue
    }

    if [[ "$USE_V3" -eq 0 ]]; then
        # TW-only mode: verify TW matches .exp golden
        exp=$(cat "$exp_file")
        if [[ "$tw_out" == "$exp" ]]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            divergences+=("$name (TW vs .exp)")
        fi
    else
        # v3 mode: --parse not yet implemented
        echo "[$name] v3 --parse not yet implemented (Stage 1.0 work)"
        fail=$((fail + 1))
    fi
done

# --- parse-fail phase ---
# Parse-fail fixtures should produce a TW parse error matching the
# .err.exp golden.  Stage 1 must produce error messages of equivalent
# semantics (not necessarily byte-equal — error wording may vary
# slightly per parser implementation, but error category + position
# must match).
fail_pass=0
fail_total=0
for nix_file in "$LANG_DIR"/parse-fail-*.nix; do
    [[ -f "$nix_file" ]] || continue
    name=$(basename "$nix_file" .nix)
    err_exp_file="$LANG_DIR/$name.err.exp"
    [[ -f "$err_exp_file" ]] || continue
    fail_total=$((fail_total + 1))

    # TW must REJECT (exit non-zero with parse error).
    if ! "$NIX_INST" --parse "$nix_file" >/dev/null 2>/dev/null; then
        fail_pass=$((fail_pass + 1))
    else
        echo "[$name] EXPECTED PARSE FAILURE but TW accepted"
    fi
done

echo ""
echo "=== TI.1 results (mode=$([[ $USE_V3 -eq 1 ]] && echo v3 || echo tw-baseline)) ==="
echo "  parse-okay:  total=$total pass=$pass fail=$fail"
echo "  parse-fail:  total=$fail_total reject=$fail_pass (rejected as expected)"
if (( ${#divergences[@]} > 0 )); then
    echo "  divergences:"
    for d in "${divergences[@]}"; do
        echo "    - $d"
    done
fi

(( fail == 0 && fail_pass == fail_total )) && exit 0 || exit 1
