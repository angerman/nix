#!/usr/bin/env bash
# Arena deregistration Day-1 audit harness.
#
# Per `lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md` §4.1 + §4.2:
# the Day-1 audit must verify that arena cells don't store Boehm-
# managed pointers via Tag::External, AND classify the address
# ranges of Tag::String / Tag::Path payloads to identify which are
# arena-managed (safe) vs Boehm-managed (need separate registration).
#
# This script automates the audit across a configurable workload set
# using the live-trace audit extension (commit `1efa7c886`).  Reports
# per-workload PASS / FAIL on the "Tag::External=0" criterion + the
# String/Path counts for follow-up classification.
#
# Usage
# -----
#   bench/arena-dereg-audit.sh
#   bench/arena-dereg-audit.sh --workload hello
#   bench/arena-dereg-audit.sh --workloads hello,firefox,hne,ackermann
#   bench/arena-dereg-audit.sh --threshold-external N   # FAIL if External >= N
#
# Default mode: run all 3 anchor workloads (hello / firefox / hne)
# + the synthetic ackermann test.
#
# Outputs
# -------
#   /tmp/arena-dereg-audit-LOGS/<workload>.{out,err}
#   stderr summary table: PASS / FAIL per workload + final verdict
#
# Exit codes
# ----------
#   0 — all workloads PASS (External=0 on every probed workload)
#   1 — at least one workload has External > 0 (needs investigation)
#   2 — harness error (missing nix binary, missing HNE, etc.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"
WALL_TIME_S="${WALL_TIME_S:-600s}"
TIMEOUT_S="${TIMEOUT_S:-700}"
MAX_HEAP="${MAX_HEAP:-8G}"
LOG_DIR="${LOG_DIR:-/tmp/arena-dereg-audit-$$}"
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"

# Threshold above which a workload FAILS the audit (default: 0 =
# any External reached fails the External-clean claim).
THRESHOLD_EXTERNAL="${THRESHOLD_EXTERNAL:-0}"

# Workload registry.  Each: name|expr|requires_path|needs_flakes
declare -a ALL_WORKLOADS=(
    "hello|(import <nixpkgs> {}).hello.drvPath||no"
    "firefox|(import <nixpkgs> {}).firefox.drvPath||no"
    "hne|(builtins.getFlake \"$HNE_PATH\").packages.x86_64-linux.hello.drvPath|$HNE_PATH|yes"
    "ackermann|let ack = m: n: if m == 0 then n + 1 else if n == 0 then ack (m - 1) 1 else ack (m - 1) (ack m (n - 1)); in ack 3 8||no"
)

SELECTED_WORKLOADS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --workload)        shift; SELECTED_WORKLOADS="$1" ;;
        --workload=*)      SELECTED_WORKLOADS="${1#--workload=}" ;;
        --workloads)       shift; SELECTED_WORKLOADS="$1" ;;
        --workloads=*)     SELECTED_WORKLOADS="${1#--workloads=}" ;;
        --threshold-external)   shift; THRESHOLD_EXTERNAL="$1" ;;
        --threshold-external=*) THRESHOLD_EXTERNAL="${1#--threshold-external=}" ;;
        --log-dir)         shift; LOG_DIR="$1" ;;
        --log-dir=*)       LOG_DIR="${1#--log-dir=}" ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "arena-dereg-audit: unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# Preflight checks.
if [[ ! -x "$NIX_BIN" ]]; then
    echo "arena-dereg-audit: nix binary not found at $NIX_BIN" >&2
    echo "  Build with: nix develop -c ninja -C build src/nix/nix" >&2
    exit 2
fi
mkdir -p "$LOG_DIR" || {
    echo "arena-dereg-audit: cannot mkdir $LOG_DIR" >&2
    exit 2
}

# Color output (gated to TTY).
if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BOLD=$'\033[1m'; C_RESET=$'\033[0m'
else
    C_RED=""; C_GREEN=""; C_YELLOW=""; C_BOLD=""; C_RESET=""
fi

{
    echo "${C_BOLD}Arena deregistration Day-1 audit${C_RESET}"
    echo "  Per lode/ARENA_DEREGISTRATION_DESIGN_2026-05-27.md §4.1+§4.2"
    echo "  log_dir   = $LOG_DIR"
    echo "  nix       = $NIX_BIN"
    echo "  threshold = External < $THRESHOLD_EXTERNAL (any breach = FAIL)"
    echo "  max_heap  = $MAX_HEAP, wall_cap = $WALL_TIME_S"
    echo
} >&2

# Run one workload under NIX_V3_LIVE_TRACE=1, then parse the audit
# section from stderr to extract Tag::External / String / Path
# counts.
#
# Sets globals: WL_RC, WL_EXT, WL_STR, WL_PATH, WL_VERDICT, WL_NOTE
run_one_workload() {
    local name="$1"
    local expr="$2"
    local requires_path="$3"
    local needs_flakes="$4"
    local out_path="$LOG_DIR/$name.out"
    local err_path="$LOG_DIR/$name.err"

    if [[ -n "$requires_path" && ! -e "$requires_path" ]]; then
        WL_RC=0; WL_EXT=0; WL_STR=0; WL_PATH=0
        WL_VERDICT="SKIP"
        WL_NOTE="missing prerequisite: $requires_path"
        return
    fi

    local -a flakes_args=()
    if [[ "$needs_flakes" == "yes" ]]; then
        flakes_args=("--extra-experimental-features" "nix-command flakes")
    else
        flakes_args=("--extra-experimental-features" "nix-command")
    fi

    NIX_V3_LIVE_TRACE=1 \
    NIX_VM_STATS=1 \
    NIX_V3_DIRECT_EVAL=1 \
    NIX_V3_MAX_WALL_TIME="$WALL_TIME_S" \
    NIX_V3_MAX_HEAP="$MAX_HEAP" \
    timeout "${TIMEOUT_S}s" \
        "$NIX_BIN" "${flakes_args[@]}" \
        eval --impure --expr "$expr" \
        > "$out_path" 2> "$err_path"
    WL_RC=$?

    # Parse from stderr.  Look for the "Arena-dereg audit" section
    # then the most recent (= last) "reached" line per Tag type.
    # The live-trace dumps fire on each primop-install pass + final;
    # `tail -1` per type gives the meaningful final value.
    WL_EXT=$(
        grep "Tag::External" "$err_path" 2>/dev/null \
            | tail -1 \
            | awk '{for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) { print $i; exit } }'
    )
    WL_EXT="${WL_EXT:-0}"

    WL_STR=$(
        grep "Tag::String" "$err_path" 2>/dev/null \
            | tail -1 \
            | awk '{for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) { print $i; exit } }'
    )
    WL_STR="${WL_STR:-0}"

    WL_PATH=$(
        grep "Tag::Path" "$err_path" 2>/dev/null \
            | tail -1 \
            | awk '{for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) { print $i; exit } }'
    )
    WL_PATH="${WL_PATH:-0}"

    if [[ "$WL_RC" -ne 0 ]]; then
        WL_VERDICT="FAIL"
        WL_NOTE="eval rc=$WL_RC (crash / wall-cap exceeded?)"
        return
    fi

    if [[ "$WL_EXT" -gt "$THRESHOLD_EXTERNAL" ]]; then
        WL_VERDICT="FAIL"
        WL_NOTE="Tag::External=$WL_EXT exceeds threshold $THRESHOLD_EXTERNAL"
        return
    fi

    WL_VERDICT="PASS"
    WL_NOTE="External=$WL_EXT, String=$WL_STR, Path=$WL_PATH"
}

# Filter the workload set if --workload(s) was passed.
declare -a TO_RUN=()
if [[ -n "$SELECTED_WORKLOADS" ]]; then
    IFS=',' read -ra WANTED <<< "$SELECTED_WORKLOADS"
    for entry in "${ALL_WORKLOADS[@]}"; do
        name="${entry%%|*}"
        for w in "${WANTED[@]}"; do
            if [[ "$name" == "$w" ]]; then
                TO_RUN+=("$entry")
                break
            fi
        done
    done
else
    TO_RUN=("${ALL_WORKLOADS[@]}")
fi

# Run loop.
declare -a RESULTS=()
overall_fail=0
overall_skip=0

for entry in "${TO_RUN[@]}"; do
    name="${entry%%|*}"; rest="${entry#*|}"
    expr="${rest%%|*}"; rest="${rest#*|}"
    requires_path="${rest%%|*}"; rest="${rest#*|}"
    needs_flakes="${rest%%|*}"

    echo "  [${C_BOLD}${name}${C_RESET}] running…" >&2
    run_one_workload "$name" "$expr" "$requires_path" "$needs_flakes"

    case "$WL_VERDICT" in
        PASS) tag="${C_GREEN}PASS${C_RESET}";;
        FAIL) tag="${C_RED}FAIL${C_RESET}"; overall_fail=1 ;;
        SKIP) tag="${C_YELLOW}SKIP${C_RESET}"; overall_skip=$((overall_skip+1)) ;;
    esac
    echo "      $tag — $WL_NOTE" >&2
    RESULTS+=("$name|$WL_VERDICT|$WL_EXT|$WL_STR|$WL_PATH|$WL_NOTE")
done

# Final summary.
echo >&2
echo "================ summary ================" >&2
echo >&2
printf "  %-10s  %-5s  %12s  %12s  %12s\n" "Workload" "Verdict" "External" "String" "Path" >&2
for r in "${RESULTS[@]}"; do
    n="${r%%|*}"; rest="${r#*|}"
    v="${rest%%|*}"; rest="${rest#*|}"
    ext="${rest%%|*}"; rest="${rest#*|}"
    str="${rest%%|*}"; rest="${rest#*|}"
    path="${rest%%|*}"
    case "$v" in
        PASS) col="${C_GREEN}";;
        FAIL) col="${C_RED}";;
        SKIP) col="${C_YELLOW}";;
    esac
    printf "  %-10s  ${col}%-5s${C_RESET}  %12s  %12s  %12s\n" \
        "$n" "$v" "$ext" "$str" "$path" >&2
done
echo >&2

if [[ "$overall_fail" -eq 1 ]]; then
    echo "${C_RED}${C_BOLD}AUDIT VERDICT: FAIL${C_RESET}" >&2
    echo "  At least one workload has Tag::External > $THRESHOLD_EXTERNAL." >&2
    echo "  Per arena dereg design §4.2, this means v3 cells DO hold" >&2
    echo "  External-tagged Values on the failing workload.  Investigate" >&2
    echo "  the sample addresses in $LOG_DIR/<workload>.err to localize" >&2
    echo "  which cells materialize Externals." >&2
    exit 1
fi
if [[ "$overall_skip" -gt 0 ]]; then
    echo "${C_YELLOW}${C_BOLD}AUDIT VERDICT: PARTIAL${C_RESET} ($overall_skip workload(s) skipped)" >&2
    echo "  Resolve skipped workloads (missing prerequisites) before" >&2
    echo "  declaring the External-clean claim conclusive." >&2
    exit 1
fi
echo "${C_GREEN}${C_BOLD}AUDIT VERDICT: PASS${C_RESET}" >&2
echo "  All probed workloads have Tag::External ≤ $THRESHOLD_EXTERNAL." >&2
echo "  Arena dereg can proceed wrt External tag for THIS workload set." >&2
echo "  Future workloads must re-run this audit before assuming the" >&2
echo "  External-clean claim holds.  Logs: $LOG_DIR" >&2
exit 0
