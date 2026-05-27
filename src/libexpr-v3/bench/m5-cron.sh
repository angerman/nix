#!/usr/bin/env bash
# v3 cardano-node M5 ritual measurement — drift detection.
#
# Per lode/DIRECTION_NOTE_2026-05-26.md §3.3 / §6 action #3:
# strategic-workload (cardano-node M5) measurement runs periodically
# to surface silent drift in wall + peak_rss + correctness.
#
# Per lode/CARDANO_NODE_M5_2026-05-26.md: the 2026-05-21 baseline
# was wrong at source (919 MB capability claim not reproducible);
# this script's PURPOSE is to keep the baseline LIVE and reproducible
# so a future "regression vs baseline" claim has data to cross-check.
#
# Usage
# -----
#   bench/m5-cron.sh                              # default — append to ledger
#   bench/m5-cron.sh --check-regression LIMIT     # exit 1 if delta > LIMIT% vs last entry
#   bench/m5-cron.sh --quick                      # 1 warm run only (~30s)
#   bench/m5-cron.sh --full                       # cold + warm + AOT (~2 min)
#   bench/m5-cron.sh --ledger PATH                # custom ledger location
#
# Default mode: --full.
#
# Outputs
# -------
#   bench/samples/m5-cron-ledger.tsv     — append-only, one line per run:
#     timestamp \t git_hash \t mode \t wall_s \t peak_rss_mb \t output_drvPath
#   stderr summary: human-readable comparison vs prior run
#
# Falsifier check (per [[same-host-bisect]] + [[head-5-counter-trap]])
# --------------------------------------------------------------------
# The ledger is the FIRST defense against phantom regressions.
# Any "M5 wall regressed" claim should reference the ledger; the
# ledger entry includes the git_hash so reproduction is trivial via
# `git checkout HASH -- src/libexpr-v3/ && rebuild && rerun this script`.
#
# Pre-committed thresholds (per [[threshold-recalibration-rule]])
# ---------------------------------------------------------------
# Default --check-regression compares vs the immediately-prior entry
# of the same mode in the ledger.  Suggested thresholds:
#   wall      ±15 %         (noise tolerance on a 30s workload)
#   peak_rss  ±20 %         (Boehm + workload state variance)
# Tighter thresholds need a quiescent-host bench-runner (out of scope).
#
# Skip rules
# ----------
#   * cardano-node checkout at /Users/angerman/Projects/iohk/cardano-node
#     missing → SKIP exit 0
#   * NIX missing → SKIP exit 0
#   * sqlite3 missing → SKIP exit 0 (need it for cache clear in --full)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
LEDGER="${LEDGER:-$ROOT/src/libexpr-v3/bench/samples/m5-cron-ledger.tsv}"
SQLITE3="${SQLITE3:-sqlite3}"
CACHE_DB="${HOME}/.cache/nix/v3-bytecode-v3.sqlite"

# Per CARDANO_NODE_M5_2026-05-21.md / -26.md: pin to aarch64-darwin
# `cardano-node.name` derivation.  Same target across all measurements.
SYSTEM="${SYSTEM:-aarch64-darwin}"
EXPR="(builtins.getFlake \"$CN_PATH\").outputs.packages.${SYSTEM}.cardano-node.name"
EXPECTED_OUTPUT_PREFIX="cardano-node-exe-cardano-node-"

# v3 resource limits.  16G heap headroom (M5 needs ≥ 6 GB peak post
# verdict-correction 2026-05-26).  WALL_TIME 180s (M5 cold ≈ 35s,
# generous margin for noisy hosts).
V3_LIMITS=(
    NIX_V3_DIRECT_EVAL=1
    NIX_V3_MAX_HEAP=16G
    NIX_V3_MAX_WALL_TIME=180s
    NIX_V3_NO_NATIVE_CALL_FLAKE=1
)

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
if [[ ! -x "$NIX" ]]; then
    echo "m5-cron: NIX not executable at $NIX; SKIP" >&2
    exit 0
fi
if [[ ! -d "$CN_PATH" ]]; then
    echo "m5-cron: cardano-node checkout missing at $CN_PATH; SKIP" >&2
    exit 0
fi
if ! command -v "$SQLITE3" >/dev/null 2>&1; then
    echo "m5-cron: sqlite3 not on PATH; SKIP (needed for cache clear)" >&2
    exit 0
fi
mkdir -p "$(dirname "$LEDGER")"

# Parse args.
MODE="full"
CHECK_REGRESSION=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --quick)   MODE="quick"; shift ;;
        --full)    MODE="full"; shift ;;
        --check-regression) CHECK_REGRESSION="${2:-15}"; shift 2 ;;
        --ledger)  LEDGER="$2"; shift 2 ;;
        -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
        *) echo "m5-cron: unknown arg $1; try --help" >&2; exit 2 ;;
    esac
done

# Capture environment context.
TS="$(date '+%Y-%m-%dT%H:%M:%S%z')"
GIT_HASH="$(cd "$ROOT" && git rev-parse --short=10 HEAD 2>/dev/null || echo unknown)"

# Format ledger header on first creation.
if [[ ! -f "$LEDGER" ]]; then
    cat > "$LEDGER" <<'HDR'
# v3 cardano-node M5 cron ledger
# tab-separated: timestamp git_hash mode wall_s peak_rss_mb output_drvPath
# Append-only; do not edit historical entries.
HDR
fi

# ---------------------------------------------------------------------------
# Run a single measurement.  Echoes one TSV line to stdout + the ledger.
# Args: $1=mode_label, $2=env-var-additions
# ---------------------------------------------------------------------------
run_one() {
    local label="$1"
    local extra_env="${2:-}"

    # /usr/bin/time -l on darwin prints "maximum resident set size" in bytes.
    # On linux GNU time it's in KB; we'd normalize, but cron is darwin-first.
    local tmp_err tmp_out
    tmp_err="$(mktemp -t m5-cron-XXXXXX.err)"
    tmp_out="$(mktemp -t m5-cron-XXXXXX.out)"
    # shellcheck disable=SC2064
    trap "rm -f '$tmp_err' '$tmp_out'" RETURN

    local start_ns end_ns
    start_ns="$(date +%s%N 2>/dev/null || echo 0)"
    env "${V3_LIMITS[@]}" $extra_env \
        /usr/bin/time -l "$NIX" eval --impure --expr "$EXPR" \
        >"$tmp_out" 2>"$tmp_err"
    local rc=$?
    end_ns="$(date +%s%N 2>/dev/null || echo 0)"

    local wall_s peak_rss_bytes peak_rss_mb output_path
    if [[ "$start_ns" != 0 && "$end_ns" != 0 ]]; then
        wall_s="$(awk -v s="$start_ns" -v e="$end_ns" \
            'BEGIN { printf "%.3f", (e-s)/1e9 }')"
    else
        # Fallback: parse /usr/bin/time's "real" / "real_s" line.
        wall_s="$(grep -E '^[[:space:]]*[0-9]+\.[0-9]+ real' "$tmp_err" \
            | awk '{print $1}' | head -1)"
        [[ -z "$wall_s" ]] && wall_s="?"
    fi
    peak_rss_bytes="$(grep 'maximum resident' "$tmp_err" \
        | awk '{print $1}' | head -1)"
    peak_rss_mb="?"
    [[ -n "$peak_rss_bytes" ]] && peak_rss_mb=$(( peak_rss_bytes / 1024 / 1024 ))
    output_path="$(cat "$tmp_out" | tr -d '"' | head -1)"

    # Correctness check.
    local status="ok"
    if [[ "$rc" != 0 ]]; then
        status="exit_$rc"
    elif [[ "$output_path" != "${EXPECTED_OUTPUT_PREFIX}"* ]]; then
        status="output_mismatch:$output_path"
    fi

    # Write to ledger + stdout.
    local line
    line="$(printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$TS" "$GIT_HASH" "$label" "$wall_s" "$peak_rss_mb" "$output_path")"
    echo "$line" >> "$LEDGER"
    printf '  %-18s wall=%-8s rss=%-7s status=%s\n' \
        "$label" "${wall_s}s" "${peak_rss_mb}MB" "$status"

    # Return non-zero if the run was broken (not for regression — that's
    # the caller's job via --check-regression).
    [[ "$status" == "ok" ]]
}

# ---------------------------------------------------------------------------
# Modes
# ---------------------------------------------------------------------------
echo "m5-cron: $TS  git=$GIT_HASH  mode=$MODE  ledger=$LEDGER"

failures=0
if [[ "$MODE" == "quick" ]]; then
    run_one "warm" "" || failures=$((failures+1))
elif [[ "$MODE" == "full" ]]; then
    # Clear cache for a true-cold measurement, then warm runs.
    "$SQLITE3" "$CACHE_DB" \
        "DELETE FROM CompilationUnits; DELETE FROM EvalResults;" 2>/dev/null \
        || echo "  (cache clear failed; cold measurement may be partial)" >&2
    run_one "cold"          ""                                || failures=$((failures+1))
    run_one "warm"          ""                                || failures=$((failures+1))
    run_one "warm+drvhash"  "NIX_V3_DRV_HASH_CACHE_DISK=1"    || failures=$((failures+1))
fi

# ---------------------------------------------------------------------------
# Regression check
# ---------------------------------------------------------------------------
if [[ -n "$CHECK_REGRESSION" ]]; then
    echo ""
    echo "m5-cron: regression check (limit ${CHECK_REGRESSION}% wall)"
    # Find the immediately-prior entry of each mode just added.
    # Naive impl: tail the ledger and compare.
    awk -v limit="$CHECK_REGRESSION" -v ts="$TS" '
        # Skip comments + empty lines.
        /^#/ { next } NF < 5 { next }
        # Collect ALL entries by mode.
        { mode=$3; wall[mode "@" NR]=$4; ts_of[mode "@" NR]=$1; n[mode]++ }
        END {
            # For each mode, find the last two entries (the just-added one
            # is the very last; the prior one is the comparand).
            for (m in n) {
                last_wall = ""
                prev_wall = ""
                for (i in wall) {
                    split(i, parts, "@")
                    if (parts[1] != m) continue
                    if (last_wall == "" || parts[2]+0 > last_idx+0) {
                        prev_wall = last_wall
                        last_wall = wall[i]
                        last_idx = parts[2]
                    }
                }
                if (prev_wall == "" || last_wall == "" || prev_wall == "?") continue
                if (prev_wall+0 == 0) continue
                delta = (last_wall - prev_wall) / prev_wall * 100
                abs = (delta < 0) ? -delta : delta
                marker = "ok"
                if (abs > limit+0) marker = "REGRESSION"
                printf "  %-18s prev=%s now=%s delta=%+.1f%%  %s\n",
                    m, prev_wall, last_wall, delta, marker
                if (marker == "REGRESSION") had_regression=1
            }
            exit (had_regression ? 1 : 0)
        }
    ' "$LEDGER" || failures=$((failures+1))
fi

if [[ "$failures" -gt 0 ]]; then
    echo ""
    echo "m5-cron: $failures failure(s) — see ledger $LEDGER" >&2
    exit 1
fi

exit 0
