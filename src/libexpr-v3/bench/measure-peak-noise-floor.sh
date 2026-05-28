#!/usr/bin/env bash
# Peak-RSS noise-floor measurement harness.
#
# Step 1 of the v3 GC post-Phase-3.8 plan (2026-05-29).  Runs N evals
# of a v3 workload under a specified gate configuration, parses the
# NIX_VM_STATS final-line output, drops outermost 2 samples (trimmed
# mean for N≥5), and reports mean ± σ for peak_rss / v3_arena /
# elsewhere / wall.  Emits a JSON sidecar consumable by downstream
# steps (Phase 4 SHIP-gate verdict, Step 14 honest re-measurement,
# Step 19 M5 post-MS state).
#
# Pre-warms cache with one untimed run so the timed runs hit warm
# nix-store + filesystem cache.  This is the hyperfine convention
# and is what production users actually see.
#
# Usage
# -----
#   bench/measure-peak-noise-floor.sh <workload> <config> [N] [out.json]
#
# Workloads: hello.name | hello.drvPath | firefox.name | HNE
# Configs:   gate-off | gate-on-reuse-off | gate-on-reuse-on |
#            gate-on-reuse-on-stress
#
# Defaults: N=10, out=bench/noise-floor/<workload>-<config>.json
#
# Re-execs itself under `nix develop -c` if not inside a nix shell
# (per project CLAUDE.md convention).
#
# Why noise floor first: prior session demonstrated peak_rss varies
# ~500 MB run-to-run on HNE.  Any RSS claim below 2σ is noise.  This
# script makes σ first-class so downstream SHIP-gate verdicts rest
# on stable envelopes rather than n=1 single shots.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u
set -o pipefail

# ----------------------------------------------------------------------
# Help.
# ----------------------------------------------------------------------
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
    exit 0
fi

# ----------------------------------------------------------------------
# Argument parsing.
# ----------------------------------------------------------------------
WORKLOAD="${1:-}"
CONFIG="${2:-}"
N="${3:-10}"
OUT="${4:-}"

if [[ -z "$WORKLOAD" || -z "$CONFIG" ]]; then
    {
        echo "Error: <workload> and <config> required."
        echo
        echo "Usage: $0 <workload> <config> [N=10] [out.json]"
        echo "Workloads: hello.name | hello.drvPath | firefox.name | HNE"
        echo "Configs:   gate-off | gate-on-reuse-off | gate-on-reuse-on | gate-on-reuse-on-stress"
    } >&2
    exit 2
fi

if ! [[ "$N" =~ ^[0-9]+$ ]] || (( N < 2 )); then
    echo "Error: N must be a positive integer ≥2 (got: $N)" >&2
    exit 2
fi

# ----------------------------------------------------------------------
# Re-exec under nix develop if not inside a nix shell.
# ----------------------------------------------------------------------
if [[ -z "${IN_NIX_SHELL:-}" ]]; then
    echo "[noise-floor] Re-executing under 'nix develop -c'..." >&2
    exec nix develop -c bash "$0" "$@"
fi

# ----------------------------------------------------------------------
# Repo root + nix binary discovery.
# ----------------------------------------------------------------------
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"

# Auto-pick the newer of build/ and builddir/ unless NIX_BIN is set
# explicitly.  Per `[[bench-binary-fingerprint]]` (2026-05-29): the
# original default `$ROOT/build/src/nix/nix` was stale relative to
# the active meson build at builddir/.  Pre-fix, Day 6-8 / Day 9-11
# / Day 12 bench measurements all ran against pre-Week-1 binary
# without detection.
if [[ -z "${NIX_BIN:-}" ]]; then
    declare -a CANDIDATES=()
    [[ -x "$ROOT/builddir/src/nix/nix" ]] && CANDIDATES+=("$ROOT/builddir/src/nix/nix")
    [[ -x "$ROOT/build/src/nix/nix"    ]] && CANDIDATES+=("$ROOT/build/src/nix/nix")
    if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
        echo "Error: no nix binary found at build/ or builddir/ — build first" >&2
        echo "Or set NIX_BIN=<path>" >&2
        exit 2
    fi
    # Pick the most-recently-modified candidate.
    NIX_BIN=""
    NIX_BIN_MTIME=0
    for c in "${CANDIDATES[@]}"; do
        local_mtime=$(python3 -c "import os; print(int(os.path.getmtime('$c')))")
        if [[ "$local_mtime" -gt "$NIX_BIN_MTIME" ]]; then
            NIX_BIN="$c"
            NIX_BIN_MTIME="$local_mtime"
        fi
    done
    echo "[noise-floor] auto-selected NIX_BIN=$NIX_BIN (mtime=$NIX_BIN_MTIME)" >&2
    # Warn if HEAD commit is newer than the chosen binary.
    HEAD_MTIME=$(git -C "$ROOT" log -1 --format=%ct HEAD 2>/dev/null || echo 0)
    if [[ "$HEAD_MTIME" -gt "$NIX_BIN_MTIME" ]]; then
        echo "[noise-floor] WARNING: HEAD commit (mtime $HEAD_MTIME) is NEWER than $NIX_BIN (mtime $NIX_BIN_MTIME) — rebuild before measuring" >&2
    fi
fi

if [[ ! -x "$NIX_BIN" ]]; then
    {
        echo "Error: nix binary not found or not executable: $NIX_BIN"
        echo "Build first via: ninja -C build src/nix/nix  (release build)"
        echo "             or: ninja -C builddir src/nix/nix  (debug build, common)"
        echo "Or set NIX_BIN=<path>"
    } >&2
    exit 2
fi

# Default output path.
if [[ -z "$OUT" ]]; then
    OUT="$ROOT/src/libexpr-v3/bench/noise-floor/${WORKLOAD}-${CONFIG}.json"
fi
mkdir -p "$(dirname "$OUT")"

# ----------------------------------------------------------------------
# Workload registry.
# ----------------------------------------------------------------------
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"

declare -a EXTRA_ENV=()
case "$WORKLOAD" in
    hello.name)
        EXPR='(import <nixpkgs> { }).hello.name'
        WALL_BUDGET=60
        HEAP_BUDGET=4G
        ;;
    hello.drvPath)
        EXPR='(import <nixpkgs> { }).hello.drvPath'
        WALL_BUDGET=120
        HEAP_BUDGET=4G
        ;;
    firefox.name)
        EXPR='(import <nixpkgs> { }).firefox.name'
        WALL_BUDGET=120
        HEAP_BUDGET=4G
        ;;
    HNE)
        EXPR='(builtins.getFlake "'"$HNE_PATH"'").packages.x86_64-linux.hello.drvPath'
        WALL_BUDGET=900
        HEAP_BUDGET=8G
        # Note (2026-05-29 Day 2 EXIT_GC_SPIRAL): previously this
        # auto-added NIX_V3_NO_DISK_CACHE=1.  Day 2 requires
        # measuring HNE WITHOUT that override to establish the
        # cache-on baseline; use the `gate-off-cache-off` config
        # for the explicit cache-disabled comparison.
        ;;
    M5)
        # cardano-node M5 (Pillar 2 strategic workload).  Per
        # bench/m5-cron.sh: cardano-node-exe-cardano-node-* drvPath
        # via `cardano-node.name`.
        EXPR='(builtins.getFlake "'"$CN_PATH"'").outputs.packages.x86_64-linux.cardano-node.name'
        WALL_BUDGET=1800
        HEAP_BUDGET=8G
        ;;
    *)
        echo "Error: unknown workload '$WORKLOAD'" >&2
        echo "Workloads: hello.name | hello.drvPath | firefox.name | HNE | M5" >&2
        exit 2
        ;;
esac

# ----------------------------------------------------------------------
# Config registry.
# ----------------------------------------------------------------------
declare -a GATE_ENV=()
case "$CONFIG" in
    gate-off)
        : # no extras
        ;;
    gate-on-reuse-off)
        GATE_ENV=(NIX_V3_MAJOR_GC=1)
        ;;
    gate-on-reuse-on)
        GATE_ENV=(NIX_V3_MAJOR_GC=1 V3_DBG_FREELIST_REUSE=1)
        ;;
    gate-on-reuse-on-stress)
        GATE_ENV=(NIX_V3_MAJOR_GC=1 V3_DBG_FREELIST_REUSE=1 V3_DBG_GC_STRESS=1000)
        ;;
    gate-off-cache-off)
        # Day 2 of EXIT_GC_SPIRAL_PLAN: cache-eviction PoC.
        # Disables the v3 disk cache (SQLite-backed bytecode shadow
        # per HNE_BUCKET_DECOMP §3); approximates Phase 4b LRU yield.
        GATE_ENV=(NIX_V3_NO_DISK_CACHE=1)
        ;;
    *)
        echo "Error: unknown config '$CONFIG'" >&2
        echo "Configs: gate-off | gate-on-reuse-off | gate-on-reuse-on | gate-on-reuse-on-stress | gate-off-cache-off" >&2
        exit 2
        ;;
esac

# Workload prereq: HNE flake must be checked out.
if [[ "$WORKLOAD" == "HNE" && ! -d "$HNE_PATH" ]]; then
    {
        echo "Error: HNE workload requires the haskell-nix-example flake at:"
        echo "  $HNE_PATH"
        echo "Set HNE_PATH=<path> or check out the flake first."
    } >&2
    exit 2
fi
# Workload prereq: M5 = cardano-node flake must be checked out.
if [[ "$WORKLOAD" == "M5" && ! -d "$CN_PATH" ]]; then
    {
        echo "Error: M5 workload requires the cardano-node flake at:"
        echo "  $CN_PATH"
        echo "Set CN_PATH=<path> or check out the flake first."
    } >&2
    exit 2
fi

# ----------------------------------------------------------------------
# Common eval environment.
# ----------------------------------------------------------------------
declare -a EVAL_ENV=(
    NIX_V3_DIRECT_EVAL=1
    NIX_VM_STATS=1
    "NIX_V3_MAX_WALL_TIME=${WALL_BUDGET}s"
    "NIX_V3_MAX_HEAP=${HEAP_BUDGET}"
)

# ----------------------------------------------------------------------
# Run helper — captures one timed run; emits "peak,arena,elsewhere,wall"
# on success, blank line on failure.
# ----------------------------------------------------------------------
run_one() {
    local log
    log="$(mktemp "${TMPDIR:-/tmp}/v3-noise.XXXXXX")"
    local start end
    start=$(python3 -c 'import time; print(time.time())')

    set +e
    env \
        "${EVAL_ENV[@]}" \
        "${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}" \
        "${GATE_ENV[@]+"${GATE_ENV[@]}"}" \
        "$NIX_BIN" \
            --extra-experimental-features nix-command \
            --extra-experimental-features flakes \
            eval --impure --expr "$EXPR" \
        >"$log" 2>&1
    local rc=$?
    set -e

    end=$(python3 -c 'import time; print(time.time())')
    local wall
    wall=$(python3 -c "print(${end} - ${start})")

    if (( rc != 0 )); then
        echo "[noise-floor]   RUN FAILED (exit=$rc)" >&2
        echo "[noise-floor]   Last log: $log (kept for inspection)" >&2
        echo ""
        return
    fi

    # Final v3-direct memory line — multiple appear during eval; use
    # the LAST one (post-eval summary).
    local mem_line
    mem_line=$(grep "v3-direct memory:" "$log" | tail -1 || true)
    if [[ -z "$mem_line" ]]; then
        echo "[noise-floor]   no v3-direct memory line found" >&2
        echo "[noise-floor]   Last log: $log (kept for inspection)" >&2
        echo ""
        return
    fi

    local peak arena elsewhere
    peak=$(echo "$mem_line" | sed -nE 's/.*peak_rss=([0-9.]+)MB.*/\1/p')
    arena=$(echo "$mem_line" | sed -nE 's/.*v3_arena=([0-9.]+)MB.*/\1/p')
    elsewhere=$(echo "$mem_line" | sed -nE 's/.*elsewhere=([0-9.]+)MB.*/\1/p')

    if [[ -z "$peak" || -z "$arena" || -z "$elsewhere" ]]; then
        echo "[noise-floor]   parse failure: '$mem_line'" >&2
        echo ""
        rm -f "$log"
        return
    fi

    rm -f "$log"
    echo "$peak,$arena,$elsewhere,$wall"
}

# ----------------------------------------------------------------------
# Main loop.
# ----------------------------------------------------------------------
{
    echo "[noise-floor] workload=$WORKLOAD config=$CONFIG N=$N"
    echo "[noise-floor] expr:    $EXPR"
    echo "[noise-floor] gate:    ${GATE_ENV[*]+"${GATE_ENV[*]}"}"
    echo "[noise-floor] extra:   ${EXTRA_ENV[*]+"${EXTRA_ENV[*]}"}"
    echo "[noise-floor] out:     $OUT"
} >&2

echo "[noise-floor] Pre-warming cache (untimed run)..." >&2
PREWARM_LOG="$(mktemp "${TMPDIR:-/tmp}/v3-noise-prewarm.XXXXXX")"
set +e
env \
    "${EVAL_ENV[@]}" \
    "${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}" \
    "${GATE_ENV[@]+"${GATE_ENV[@]}"}" \
    "$NIX_BIN" \
        --extra-experimental-features nix-command \
        --extra-experimental-features flakes \
        eval --impure --expr "$EXPR" \
    >"$PREWARM_LOG" 2>&1
PREWARM_RC=$?
set -e
if (( PREWARM_RC != 0 )); then
    echo "[noise-floor]   pre-warm FAILED (exit=$PREWARM_RC); continuing anyway" >&2
    echo "[noise-floor]   Pre-warm log: $PREWARM_LOG" >&2
else
    rm -f "$PREWARM_LOG"
fi

CSV="$(mktemp "${TMPDIR:-/tmp}/v3-noise-csv.XXXXXX")"
SUCCESSES=0
for (( i=1; i <= N; i++ )); do
    echo "[noise-floor] Run $i/$N..." >&2
    line=$(run_one)
    if [[ -n "$line" ]]; then
        echo "$line" >>"$CSV"
        SUCCESSES=$((SUCCESSES + 1))
        echo "[noise-floor]   $line" >&2
    fi
done

if (( SUCCESSES < 2 )); then
    {
        echo "[noise-floor] FATAL: fewer than 2 successful runs ($SUCCESSES/$N)."
        echo "[noise-floor] Cannot compute σ.  See /tmp/v3-noise-*.* for logs."
    } >&2
    rm -f "$CSV"
    exit 1
fi

# ----------------------------------------------------------------------
# Stats via python.
# ----------------------------------------------------------------------
python3 - "$CSV" "$WORKLOAD" "$CONFIG" "$EXPR" "$N" "$SUCCESSES" \
        "${GATE_ENV[*]+"${GATE_ENV[*]}"}" \
        "${EXTRA_ENV[*]+"${EXTRA_ENV[*]}"}" \
        >"$OUT" <<'PYEOF'
import json
import statistics
import sys
from datetime import datetime, timezone

csv_path     = sys.argv[1]
workload     = sys.argv[2]
config       = sys.argv[3]
expr         = sys.argv[4]
n_requested  = int(sys.argv[5])
n_succeeded  = int(sys.argv[6])
gate_env_str = sys.argv[7] if len(sys.argv) > 7 else ""
extra_env_str = sys.argv[8] if len(sys.argv) > 8 else ""

peak_rss, v3_arena, elsewhere, wall_s = [], [], [], []
with open(csv_path) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        parts = line.split(",")
        if len(parts) != 4:
            continue
        try:
            p, a, e, w = (float(x) for x in parts)
        except ValueError:
            continue
        peak_rss.append(p)
        v3_arena.append(a)
        elsewhere.append(e)
        wall_s.append(w)

def trim(xs):
    """Drop top + bottom (trimmed mean) for N>=5; else return all."""
    if len(xs) < 5:
        return sorted(xs)
    return sorted(xs)[1:-1]

def stats(xs):
    if not xs:
        return {
            "n_raw": 0, "n_used": 0,
            "mean": None, "sigma": None,
            "min": None, "max": None,
            "raw": [],
        }
    t = trim(xs)
    return {
        "n_raw": len(xs),
        "n_used": len(t),
        "mean":  round(statistics.fmean(t), 3),
        "sigma": round(statistics.stdev(t), 3) if len(t) > 1 else 0.0,
        "min":   round(min(t), 3),
        "max":   round(max(t), 3),
        "raw":   [round(x, 3) for x in xs],
    }

out = {
    "schema_version":  1,
    "generated_utc":   datetime.now(timezone.utc).isoformat(),
    "workload":        workload,
    "config":          config,
    "expr":            expr,
    "n_requested":     n_requested,
    "n_succeeded":     n_succeeded,
    "gate_env":        gate_env_str.strip(),
    "extra_env":       extra_env_str.strip(),
    "stats": {
        "peak_rss_mb":  stats(peak_rss),
        "v3_arena_mb":  stats(v3_arena),
        "elsewhere_mb": stats(elsewhere),
        "wall_s":       stats(wall_s),
    },
}
print(json.dumps(out, indent=2))
PYEOF

rm -f "$CSV"

# ----------------------------------------------------------------------
# Pretty-print summary to stderr.
# ----------------------------------------------------------------------
python3 - "$OUT" <<'PYEOF' >&2
import json
import sys

d = json.load(open(sys.argv[1]))
print()
print(f"[noise-floor] === summary ===")
print(f"[noise-floor] workload:  {d['workload']}")
print(f"[noise-floor] config:    {d['config']}")
print(f"[noise-floor] runs:      {d['n_succeeded']}/{d['n_requested']} succeeded")
print(f"[noise-floor] output:    {sys.argv[1]}")
print()
for k, v in d["stats"].items():
    if v["mean"] is None:
        print(f"[noise-floor]   {k:15s}: (no data)")
        continue
    print(f"[noise-floor]   {k:15s}: "
          f"{v['mean']:8.2f} ± {v['sigma']:6.2f} "
          f"(n={v['n_used']:>2}/{v['n_raw']:<2}, "
          f"range [{v['min']:.2f}, {v['max']:.2f}])")
PYEOF

echo "[noise-floor] Done." >&2
