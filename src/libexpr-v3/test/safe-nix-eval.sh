#!/bin/bash
# Run a nix expression under strict memory + wall-clock limits.
# Polls combined RSS (target + children) once a second and SIGKILLs
# if it exceeds MAX_RSS_KB (default 4 GB).  Also enforces TIMEOUT
# (default 30 s).  Defends against the v3 hook cycle scenarios that
# allocate unboundedly while spinning.
#
# Usage:
#   safe-nix-eval.sh <expr> [VAR=val ...]
#
# Output (single-line summary, then up to 10 lines of stderr):
#   PASS peak_rss_kb=N | <last line of stdout>
#   FAIL_TIMEOUT peak_rss_kb=N
#   FAIL_OOM peak_rss_kb=N limit=L
#   FAIL_EXIT <exitcode> peak_rss_kb=N | <last line of stderr>
#
# Tunables: MAX_RSS_KB (kilobytes), TIMEOUT (seconds), NIX_BIN
# (default $REPO/build/src/nix/nix).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
MAX_RSS_KB=${MAX_RSS_KB:-4194304}
TIMEOUT=${TIMEOUT:-30}
NIX_BIN=${NIX_BIN:-$(cd "$(dirname "$0")/../../.." && pwd)/build/src/nix/nix}

if [[ ! -x "$NIX_BIN" ]]; then
    echo "FAIL_NO_NIX nix binary not found at $NIX_BIN" >&2
    exit 2
fi

EXPR="$1"; shift
ENV_ARGS=()
if (( $# > 0 )); then ENV_ARGS=("$@"); fi

OUT=$(mktemp); ERR=$(mktemp)
trap 'rm -f "$OUT" "$ERR"' EXIT

if (( ${#ENV_ARGS[@]} > 0 )); then
    env "${ENV_ARGS[@]}" "$NIX_BIN" eval --impure --expr "$EXPR" \
        >"$OUT" 2>"$ERR" &
else
    "$NIX_BIN" eval --impure --expr "$EXPR" \
        >"$OUT" 2>"$ERR" &
fi
PID=$!

PEAK_KB=0
START=$(date +%s)
KILLED_OOM=0

while kill -0 "$PID" 2>/dev/null; do
    NOW=$(date +%s)
    if (( NOW - START > TIMEOUT )); then
        kill -KILL "$PID" 2>/dev/null
        # Reap any descendant ps -ax with PPID == PID.
        for kid in $(ps -ax -o pid=,ppid= | awk -v p="$PID" '$2==p {print $1}'); do
            kill -KILL "$kid" 2>/dev/null
        done
        wait "$PID" 2>/dev/null
        echo "FAIL_TIMEOUT peak_rss_kb=$PEAK_KB"
        tail -10 "$ERR" | cut -c1-80
        exit 0
    fi
    # Sum RSS across the target + any direct children.  ps -o rss
    # returns kilobytes on macOS.
    RSS=$(ps -ax -o pid=,ppid=,rss= 2>/dev/null \
          | awk -v p="$PID" '$1==p || $2==p {sum+=$3} END {print sum+0}')
    if [[ "$RSS" =~ ^[0-9]+$ ]]; then
        if (( RSS > PEAK_KB )); then PEAK_KB=$RSS; fi
        if (( RSS > MAX_RSS_KB )); then
            kill -KILL "$PID" 2>/dev/null
            for kid in $(ps -ax -o pid=,ppid= | awk -v p="$PID" '$2==p {print $1}'); do
                kill -KILL "$kid" 2>/dev/null
            done
            wait "$PID" 2>/dev/null
            KILLED_OOM=1
            break
        fi
    fi
    sleep 1
done

wait "$PID" 2>/dev/null
EXIT=$?

if (( KILLED_OOM == 1 )); then
    echo "FAIL_OOM peak_rss_kb=$PEAK_KB limit=$MAX_RSS_KB"
    tail -10 "$ERR" | cut -c1-80
    exit 0
fi

LAST=$(tail -1 "$OUT" | head -c 200)
if (( EXIT == 0 )); then
    echo "PASS peak_rss_kb=$PEAK_KB | $LAST"
else
    LAST_ERR=$(tail -1 "$ERR" | head -c 200)
    echo "FAIL_EXIT $EXIT peak_rss_kb=$PEAK_KB | $LAST_ERR"
    tail -10 "$ERR" | cut -c1-80
fi
