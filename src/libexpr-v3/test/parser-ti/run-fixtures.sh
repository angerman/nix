#!/usr/bin/env bash
# v3 parser TI — fixture runner.
#
# Validates every fixtures/**/*.nix against its committed .exp golden
# parenthesized-AST.  Two modes:
#
#   (default) tw-baseline: re-parse via nix-instantiate --parse; the
#             output must still match the committed golden.  Catches
#             upstream parser drift + confirms goldens are current.
#
#   --v3:     parse via `v3-eval --parse` (Stage 1.0+); byte-compare
#             v3 AST output vs the committed golden.  This is the
#             Stage 1 SHIP-gate check for the precedence + position
#             batteries.
#
# Fixture layout:
#   fixtures/<category>/<name>.nix   — source (vars bound for --parse)
#   fixtures/<category>/<name>.exp   — golden parenthesized AST
#
# Per PARSER_PROJECT_PLAN_2026-06-01.md §4 (TI.1/TI.3/TI.4).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/builddir/src/nix/nix-instantiate}"
V3="${V3:-$ROOT/builddir/src/libexpr-v3/v3-eval}"
FIXROOT="$(cd "$(dirname "$0")" && pwd)/fixtures"

USE_V3=0
case "${1:-}" in
    --v3) USE_V3=1 ;;
    --tw-only|"") USE_V3=0 ;;
    *) echo "Unknown arg: $1 (use --v3 or --tw-only)" >&2; exit 2 ;;
esac

if [[ "$USE_V3" -eq 0 ]] && [[ ! -x "$NIX_INST" ]]; then
    echo "run-fixtures: nix-instantiate not found at $NIX_INST" >&2
    exit 2
fi
if [[ "$USE_V3" -eq 1 ]] && [[ ! -x "$V3" ]]; then
    echo "run-fixtures: v3-eval not found at $V3" >&2
    exit 2
fi

pass=0
fail=0
total=0
declare -a failures=()

while IFS= read -r -d '' nix_file; do
    exp_file="${nix_file%.nix}.exp"
    [[ -f "$exp_file" ]] || continue
    rel="${nix_file#"$FIXROOT"/}"
    total=$((total + 1))
    golden="$(cat "$exp_file")"

    # Dispatch by category:
    #   position/  fixtures validate the parser's recorded {line,column}
    #              via eval (unsafeGetAttrPos) — use --eval --strict.
    #   everything else (precedence/, ...) validates AST shape — --parse.
    mode="parse"
    case "$rel" in
        position/*) mode="eval" ;;
    esac

    if [[ "$USE_V3" -eq 1 ]]; then
        if [[ "$mode" == "eval" ]]; then
            # v3-eval evaluates a file; --strict forces the {column,line}
            # thunks (matches TW's --eval --strict).
            out=$("$V3" --file "$nix_file" --strict 2>/dev/null)
        else
            # --file is required: v3-eval treats a bare positional arg
            # as --expr (so it would parse the PATH literal, not the
            # file contents).
            out=$("$V3" --parse --file "$nix_file" 2>/dev/null)
        fi
        rc=$?
    else
        if [[ "$mode" == "eval" ]]; then
            out=$("$NIX_INST" --eval --strict "$nix_file" 2>/dev/null)
        else
            out=$("$NIX_INST" --parse "$nix_file" 2>/dev/null)
        fi
        rc=$?
    fi

    if (( rc != 0 )); then
        fail=$((fail + 1))
        failures+=("$rel (parse rc=$rc)")
        continue
    fi
    if [[ "$out" == "$golden" ]]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        failures+=("$rel")
        if [[ "${V3_TI_VERBOSE:-0}" == "1" ]]; then
            echo "  DIFF $rel"
            echo "    golden: $golden"
            echo "    got:    $out"
        fi
    fi
done < <(find "$FIXROOT" -name '*.nix' -print0 | sort -z)

echo ""
echo "=== TI fixture results (mode=$([[ $USE_V3 -eq 1 ]] && echo v3 || echo tw-baseline)) ==="
echo "  total:  $total"
echo "  pass:   $pass"
echo "  fail:   $fail"
if (( ${#failures[@]} > 0 )); then
    echo "  failures:"
    for f in "${failures[@]}"; do echo "    - $f"; done
fi

(( fail == 0 )) && exit 0 || exit 1
