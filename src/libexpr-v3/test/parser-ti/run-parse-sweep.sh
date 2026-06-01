#!/usr/bin/env bash
# v3 parser TI.5 — parse-sweep harness.
#
# Per PARSER_PROJECT_PLAN_2026-06-01.md §4 (TI.5):
#   "Full-nixpkgs parse-sweep harness (extends 72-pkg sweep to ≥10K)."
#
# Parses every .nix file in a corpus via BOTH parsers and byte-compares
# the AST output (`--parse`).  This is the Stage 1 SHIP-gate check that
# scales beyond the curated fixture batteries.
#
# Corpora (cumulative; each gated by availability):
#   1. tests/functional/lang/*.nix    — always available (263 parse-okay)
#   2. $NIXPKGS/**/*.nix               — when NIXPKGS=/path is set
#                                        (the ≥10K-package Stage-1 gate)
#
# Modes:
#   (default) tw-self: TW --parse vs TW --parse  (harness self-test;
#             trivially equal — proves the sweep mechanism works)
#   --v3:     v3-eval --parse vs TW --parse  (the REAL gate once the
#             v3-native parser lands behind NIX_V3_NATIVE_PARSER=1)
#
# Files that TW itself rejects at --parse (free variables, etc.) are
# SKIPPED — we only compare files both parsers should accept.
#
# Per [[falsification-rule]]: SHIP if v3 AST == TW AST on every
# parseable file; FALSIFY (isolate) on the first divergence.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/builddir/src/nix/nix-instantiate}"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-eval}"
LANG_DIR="${LANG_DIR:-$ROOT/tests/functional/lang}"
# Optional: a nixpkgs checkout to sweep (the ≥10K-file Stage-1 gate).
NIXPKGS="${NIXPKGS:-}"
# Cap files swept (0 = unlimited).  Useful to sample a huge corpus.
LIMIT="${LIMIT:-0}"

USE_V3=0
case "${1:-}" in
    --v3) USE_V3=1 ;;
    --tw-self|"") USE_V3=0 ;;
    *) echo "Unknown arg: $1 (use --v3 or --tw-self)" >&2; exit 2 ;;
esac

if [[ ! -x "$NIX_INST" ]]; then
    echo "run-parse-sweep: nix-instantiate not found at $NIX_INST" >&2
    exit 2
fi
if [[ "$USE_V3" -eq 1 ]] && [[ ! -x "$V3" ]]; then
    echo "run-parse-sweep: v3-eval not found at $V3" >&2
    exit 2
fi

# Collect candidate files.
declare -a files=()
for f in "$LANG_DIR"/*.nix; do
    [[ -f "$f" ]] && files+=("$f")
done
if [[ -n "$NIXPKGS" ]] && [[ -d "$NIXPKGS" ]]; then
    while IFS= read -r -d '' f; do files+=("$f"); done \
        < <(find "$NIXPKGS" -name '*.nix' -type f -print0 2>/dev/null)
fi

swept=0
skipped=0
equal=0
diverged=0
declare -a divergences=()

for f in "${files[@]}"; do
    if (( LIMIT > 0 && swept >= LIMIT )); then break; fi

    # TW reference parse.  Skip files TW itself rejects (free vars etc.).
    tw_out=$("$NIX_INST" --parse "$f" 2>/dev/null) || { skipped=$((skipped+1)); continue; }
    swept=$((swept+1))

    if [[ "$USE_V3" -eq 1 ]]; then
        cmp_out=$("$V3" --parse --file "$f" 2>/dev/null) || {
            diverged=$((diverged+1))
            divergences+=("$f (v3 parse failed)")
            continue
        }
    else
        cmp_out="$tw_out"  # self-test
    fi

    if [[ "$cmp_out" == "$tw_out" ]]; then
        equal=$((equal+1))
    else
        diverged=$((diverged+1))
        divergences+=("$f")
        if [[ "${V3_TI_VERBOSE:-0}" == "1" ]]; then
            echo "  DIFF $f"
            echo "    tw: $tw_out"
            echo "    v3: $cmp_out"
        fi
    fi
done

echo ""
echo "=== TI.5 parse-sweep (mode=$([[ $USE_V3 -eq 1 ]] && echo v3-vs-tw || echo tw-self)) ==="
echo "  candidate files: ${#files[@]}"
echo "  swept (TW-parseable): $swept"
echo "  skipped (TW rejected): $skipped"
echo "  equal:    $equal"
echo "  diverged: $diverged"
if (( ${#divergences[@]} > 0 )); then
    echo "  divergences (first 20):"
    n=0
    for d in "${divergences[@]}"; do
        echo "    - $d"; n=$((n+1)); (( n >= 20 )) && break
    done
fi

(( diverged == 0 )) && exit 0 || exit 1
