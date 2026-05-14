#!/usr/bin/env bash
# eval-trace-diff.sh — run the same Nix expression through both the
# tree-walker and v3-direct evaluators with NIX_TRACE_EVAL enabled,
# then show a unified diff of the canonical force-order traces.
#
# Usage:
#   maintainers/eval-trace-diff.sh [-x EXPR | -f FILE] [-a ATTR]
#                                  [-l LIMIT] [-o OUTDIR]
#                                  [-- nix-eval-args...]
#
#   -x EXPR     Pass --expr EXPR to nix-eval
#   -f FILE     Pass --file FILE  to nix-eval
#   -a ATTR     Append an attribute path to select after the expr/file
#   -l LIMIT    Truncate the diff after LIMIT context lines.  Default
#               1000.  Pass 0 for no limit (warning: traces grow into
#               the hundreds of MB on real-nixpkgs queries).
#   -o OUTDIR   Where to write the raw traces.  Default $(mktemp -d).
#   -t SECS     Timeout per evaluator (default: 120).
#   --no-impure Omit --impure (default is to pass it).
#   --          End of options; remaining args are forwarded verbatim
#               to both nix-eval invocations.
#
# After running, two files exist in OUTDIR:
#
#   tw.trace    Tree-walker eval trace.
#   v3.trace    v3-direct eval trace.
#
# The diff is the *first* place the two traces deviate — by
# construction this is the v3-vs-TW eval-order divergence point.  When
# the diff is empty the two evaluators forced thunks in identical order.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

EXPR=""
FILE=""
ATTR=""
LIMIT=1000
OUTDIR=""
TIMEOUT=120
IMPURE=1
EXTRA=()

usage() {
    sed -n 's/^# \{0,1\}//;3,33p' "$0"
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        -x) EXPR="$2"; shift 2 ;;
        -f) FILE="$2"; shift 2 ;;
        -a) ATTR="$2"; shift 2 ;;
        -l) LIMIT="$2"; shift 2 ;;
        -o) OUTDIR="$2"; shift 2 ;;
        -t) TIMEOUT="$2"; shift 2 ;;
        --no-impure) IMPURE=0; shift ;;
        --) shift; EXTRA=("$@"); break ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

if [ -z "$EXPR" ] && [ -z "$FILE" ]; then
    echo "error: pass -x EXPR or -f FILE" >&2
    usage
fi

if [ -z "$OUTDIR" ]; then
    OUTDIR=$(mktemp -d)
    echo "outdir: $OUTDIR" >&2
fi
mkdir -p "$OUTDIR"

# Pick the nix binary.  Prefer build/src/nix/nix (developer build),
# fall back to PATH.
if [ -x ./build/src/nix/nix ]; then
    NIX=./build/src/nix/nix
elif [ -x ./result/bin/nix ]; then
    NIX=./result/bin/nix
else
    NIX=$(command -v nix)
fi
echo "nix: $NIX" >&2

ARGS=()
[ "$IMPURE" = 1 ] && ARGS+=(--impure)
[ -n "$EXPR" ] && ARGS+=(--expr "$EXPR")
[ -n "$FILE" ] && ARGS+=(--file "$FILE")
[ -n "$ATTR" ] && ARGS+=("$ATTR")
ARGS+=("${EXTRA[@]+${EXTRA[@]}}")

echo "=== running tree-walker (NIX_TRACE_EVAL=$OUTDIR/tw.trace)" >&2
NIX_TRACE_EVAL="$OUTDIR/tw.trace" timeout "$TIMEOUT" "$NIX" eval "${ARGS[@]}" > "$OUTDIR/tw.out" 2> "$OUTDIR/tw.err" || true
echo "  TW trace lines: $(wc -l < "$OUTDIR/tw.trace" 2>/dev/null || echo 0)" >&2
echo "  TW stdout: $(head -c 200 "$OUTDIR/tw.out")" >&2

echo "=== running v3-direct (NIX_V3_DIRECT_EVAL=1)" >&2
NIX_V3_DIRECT_EVAL=1 NIX_TRACE_EVAL="$OUTDIR/v3.trace" timeout "$TIMEOUT" "$NIX" eval "${ARGS[@]}" > "$OUTDIR/v3.out" 2> "$OUTDIR/v3.err" || true
echo "  v3 trace lines: $(wc -l < "$OUTDIR/v3.trace" 2>/dev/null || echo 0)" >&2
echo "  v3 stdout: $(head -c 200 "$OUTDIR/v3.out")" >&2

echo "=== diff (TW vs v3, first $LIMIT lines)"
# Show the first divergence point: walk through both traces in lockstep
# and stop after N lines past the first mismatch.
awk -v lim="$LIMIT" '
    FNR==NR { tw[FNR]=$0; ntw=FNR; next }
    { v3[FNR]=$0; nv3=FNR }
    END {
        n = (ntw > nv3) ? ntw : nv3
        first_diff = 0
        for (i = 1; i <= n; i++) {
            if (tw[i] != v3[i]) { first_diff = i; break }
        }
        if (first_diff == 0) {
            print "(traces are byte-identical for " ntw " lines)"
            exit 0
        }
        start = (first_diff > 5) ? first_diff - 5 : 1
        end = first_diff + (lim > 0 ? lim : n)
        if (end > n) end = n
        printf "(first divergence at line %d; context lines %d..%d of %d/%d)\n",
            first_diff, start, end, ntw, nv3
        for (i = start; i <= end; i++) {
            mark = (tw[i] == v3[i]) ? "  " : "! "
            twi = (i <= ntw) ? tw[i] : "<eof>"
            v3i = (i <= nv3) ? v3[i] : "<eof>"
            if (twi == v3i)
                printf "  %s\n", twi
            else {
                printf "- %s\n", twi
                printf "+ %s\n", v3i
            }
        }
    }
' "$OUTDIR/tw.trace" "$OUTDIR/v3.trace"

echo ""
echo "raw traces:  $OUTDIR/tw.trace  $OUTDIR/v3.trace"
echo "raw stdouts: $OUTDIR/tw.out    $OUTDIR/v3.out"
echo "raw stderrs: $OUTDIR/tw.err    $OUTDIR/v3.err"
