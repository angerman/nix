#!/usr/bin/env bash
# Phase E v0.2 stress validation harness — Day-1 handoff mechanics.
#
# Per `lode/PHASE_E_V02_STRESS_DESIGN_2026-05-27.md` §4 Day 1:
# real-workload stress validation on the 3 anchor workloads under
# `NIX_V3_GC_STRESS=1000` is the AR7 ship criterion for flipping
# nursery default-on.  This script automates that Day-1 mechanical
# path so the future session executing the Phase E handoff has a
# reproducible PASS / FAIL output and reduces manual-eyeball error.
#
# Usage
# -----
#   bench/phase-e-stress-validate.sh
#   bench/phase-e-stress-validate.sh --workload hello   # single workload
#   bench/phase-e-stress-validate.sh --workload firefox
#   bench/phase-e-stress-validate.sh --workload hne
#   bench/phase-e-stress-validate.sh --no-stress        # baseline comparison
#
# Default mode: run all 3 workloads under stress mode (NIX_V3_GC_STRESS=1000),
# then compare each output to the TW baseline (same expr without
# NIX_V3_DIRECT_EVAL=1).
#
# Outputs
# -------
#   /tmp/phase-e-validate-LOGS/<workload>.{out,err,baseline.out}
#   stderr summary table: PASS / FAIL per workload + ship-gate verdict
#
# Exit codes
# ----------
#   0 — all SHIP gates met (output byte-equal + no crash + no BRUTE/AUDIT)
#   1 — at least one SHIP gate failed
#   2 — harness error (missing nix binary, etc.)
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
LOG_DIR="${LOG_DIR:-/tmp/phase-e-validate-$$}"

# Stress level (per NIX_V3_GC_STRESS env var; 1000 = aggressive
# per AR7 ship criterion).
STRESS_LEVEL="${STRESS_LEVEL:-1000}"

# HNE path — required for the HNE workload; checked at runtime
# below.  Honor caller's env or fall back to user's canonical
# path used in HNE_MEMORY_ATTRIBUTION_2026-05-26 + HNE_BUCKET_DECOMP.
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"

# Workload registry.  Each workload defines:
#   name           — short label for output/logs
#   expr           — Nix expression to eval
#   requires_path  — optional path that must exist (skipped if absent)
#   needs_flakes   — "yes" / "no"
WORKLOADS=(
    "hello|(import <nixpkgs> {}).hello.drvPath||no"
    "firefox|(import <nixpkgs> {}).firefox.drvPath||no"
    "hne|(builtins.getFlake \"$HNE_PATH\").packages.x86_64-linux.hello.drvPath|$HNE_PATH|yes"
)

USE_STRESS="yes"
SINGLE_WORKLOAD=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-stress)      USE_STRESS="no" ;;
        --workload)       shift; SINGLE_WORKLOAD="$1" ;;
        --workload=*)     SINGLE_WORKLOAD="${1#--workload=}" ;;
        --log-dir)        shift; LOG_DIR="$1" ;;
        --log-dir=*)      LOG_DIR="${1#--log-dir=}" ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "phase-e-stress-validate: unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done

# Preflight checks.
if [[ ! -x "$NIX_BIN" ]]; then
    echo "phase-e-stress-validate: nix binary not found at $NIX_BIN" >&2
    echo "  Build with: nix develop -c ninja -C build src/nix/nix" >&2
    exit 2
fi
mkdir -p "$LOG_DIR" || { echo "phase-e-stress-validate: cannot mkdir $LOG_DIR" >&2; exit 2; }

# Color output (gated to TTY).
if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_BOLD=$'\033[1m'; C_RESET=$'\033[0m'
else
    C_RED=""; C_GREEN=""; C_YELLOW=""; C_BOLD=""; C_RESET=""
fi

# Print harness banner.
{
    if [[ "$USE_STRESS" == "yes" ]]; then
        echo "${C_BOLD}Phase E v0.2 stress validation${C_RESET} — NIX_V3_GC_STRESS=$STRESS_LEVEL"
    else
        echo "${C_BOLD}Phase E v0.2 baseline${C_RESET} — no stress (--no-stress)"
    fi
    echo "  log_dir   = $LOG_DIR"
    echo "  nix       = $NIX_BIN"
    echo "  max_heap  = $MAX_HEAP, wall_cap = $WALL_TIME_S"
    echo
} >&2

# Run one (mode, expr) pair, capture all outputs, return result.
#
# Args:  $1=name  $2=expr  $3=mode (stress|baseline|tw)
#
# Globals updated:  WORKLOAD_RC, WORKLOAD_OUT_BYTES, WORKLOAD_BRUTE_HITS
run_one() {
    local name="$1"
    local expr="$2"
    local mode="$3"
    local needs_flakes="$4"
    local out_path="$LOG_DIR/$name.$mode.out"
    local err_path="$LOG_DIR/$name.$mode.err"

    local -a env_kv=()
    local -a flakes_args=()
    if [[ "$needs_flakes" == "yes" ]]; then
        flakes_args=("--extra-experimental-features" "nix-command flakes")
    else
        flakes_args=("--extra-experimental-features" "nix-command")
    fi

    case "$mode" in
        stress)
            env_kv=(
                "NIX_V3_DIRECT_EVAL=1"
                "NIX_V3_NURSERY=1"
                "NIX_V3_PHASE_E=1"
                "NIX_V3_GC_STRESS=$STRESS_LEVEL"
                "NIX_VM_STATS=1"
                "NIX_V3_MAX_WALL_TIME=$WALL_TIME_S"
                "NIX_V3_MAX_HEAP=$MAX_HEAP"
            )
            ;;
        baseline)
            # v3-direct with default nursery state (off) — what
            # the workload looks like without Phase E.
            env_kv=(
                "NIX_V3_DIRECT_EVAL=1"
                "NIX_VM_STATS=1"
                "NIX_V3_MAX_WALL_TIME=$WALL_TIME_S"
                "NIX_V3_MAX_HEAP=$MAX_HEAP"
            )
            ;;
        tw)
            # Tree-walker reference: no v3-direct.
            env_kv=(
                "NIX_VM_STATS=1"
                "NIX_V3_MAX_WALL_TIME=$WALL_TIME_S"
                "NIX_V3_MAX_HEAP=$MAX_HEAP"
            )
            ;;
        *) echo "internal: bad mode $mode" >&2; return 99 ;;
    esac

    # Build the env prefix.
    env "${env_kv[@]}" timeout "${TIMEOUT_S}s" \
        "$NIX_BIN" "${flakes_args[@]}" \
        eval --impure --expr "$expr" \
        > "$out_path" 2> "$err_path"
    WORKLOAD_RC=$?
    WORKLOAD_OUT_BYTES=$(wc -c < "$out_path" 2>/dev/null || echo "0")

    # Tally BRUTE / AUDIT lines in stderr (the canonical missed-
    # root signature per BRUTE/AUDIT in CI 2026-05-21 memo).
    # macOS BSD grep -c outputs "0" on no-match AND exits 1 — the
    # `|| echo 0` fallback then doubled the output.  Use grep + wc
    # instead so empty match → 0 cleanly.
    WORKLOAD_BRUTE_HITS=$(
        grep -E "v3 SCAVENGE BRUTE|v3 SCAVENGE AUDIT: nursery .* reachable via" \
            "$err_path" 2>/dev/null | wc -l | tr -d ' '
    )
    WORKLOAD_BRUTE_HITS="${WORKLOAD_BRUTE_HITS:-0}"
}

# Validate one workload.  Runs (stress + tw) and compares.
#
# Args: $1=name $2=expr $3=requires_path $4=needs_flakes
#
# Sets globals: WL_VERDICT (PASS|FAIL|SKIP), WL_NOTE (reason)
validate_workload() {
    local name="$1"
    local expr="$2"
    local requires_path="$3"
    local needs_flakes="$4"

    if [[ -n "$requires_path" && ! -e "$requires_path" ]]; then
        WL_VERDICT="SKIP"
        WL_NOTE="missing prerequisite: $requires_path"
        return
    fi

    local stress_mode="stress"
    [[ "$USE_STRESS" == "no" ]] && stress_mode="baseline"

    # Run v3 stress (or baseline if --no-stress) + TW reference in
    # parallel-ish (sequential here; could parallelise but bounded
    # by NIX_V3_MAX_HEAP cap).
    run_one "$name" "$expr" "$stress_mode" "$needs_flakes"
    local stress_rc=$WORKLOAD_RC
    local stress_out_bytes=$WORKLOAD_OUT_BYTES
    local stress_brute=$WORKLOAD_BRUTE_HITS

    run_one "$name" "$expr" "tw" "$needs_flakes"
    local tw_rc=$WORKLOAD_RC
    local tw_out_bytes=$WORKLOAD_OUT_BYTES

    # Compare.  All must hold for PASS:
    #   * stress_rc == 0 (no crash; eval completed under cap)
    #   * tw_rc == 0 (baseline completed)
    #   * stress_brute == 0 (no missed-root signature)
    #   * stress.out byte-identical to tw.out
    local stress_path="$LOG_DIR/$name.$stress_mode.out"
    local tw_path="$LOG_DIR/$name.tw.out"

    if [[ "$tw_rc" -ne 0 ]]; then
        WL_VERDICT="SKIP"
        WL_NOTE="TW baseline failed rc=$tw_rc (eval limit hit?  network?)"
        return
    fi
    if [[ "$stress_rc" -ne 0 ]]; then
        WL_VERDICT="FAIL"
        WL_NOTE="stress eval rc=$stress_rc (crash or wall-cap exceeded)"
        return
    fi
    if [[ "$stress_brute" -gt 0 ]]; then
        WL_VERDICT="FAIL"
        WL_NOTE="BRUTE/AUDIT missed-root signature: $stress_brute lines"
        return
    fi
    if ! cmp -s "$stress_path" "$tw_path"; then
        WL_VERDICT="FAIL"
        WL_NOTE="output divergence: stress=$stress_out_bytes B vs tw=$tw_out_bytes B (cmp differs)"
        return
    fi
    WL_VERDICT="PASS"
    WL_NOTE="byte-identical to TW; 0 BRUTE/AUDIT lines; rc=0"
}

# Workload loop.
declare -a RESULTS=()
overall_fail=0
overall_skip=0

for entry in "${WORKLOADS[@]}"; do
    name="${entry%%|*}"; rest="${entry#*|}"
    expr="${rest%%|*}"; rest="${rest#*|}"
    requires_path="${rest%%|*}"; rest="${rest#*|}"
    needs_flakes="${rest%%|*}"

    if [[ -n "$SINGLE_WORKLOAD" && "$name" != "$SINGLE_WORKLOAD" ]]; then
        continue
    fi

    echo "  [${C_BOLD}${name}${C_RESET}] running…" >&2
    validate_workload "$name" "$expr" "$requires_path" "$needs_flakes"

    case "$WL_VERDICT" in
        PASS) tag="${C_GREEN}PASS${C_RESET}";;
        FAIL) tag="${C_RED}FAIL${C_RESET}"; overall_fail=1 ;;
        SKIP) tag="${C_YELLOW}SKIP${C_RESET}"; overall_skip=$((overall_skip+1)) ;;
    esac
    echo "      $tag — $WL_NOTE" >&2
    RESULTS+=("$name|$WL_VERDICT|$WL_NOTE")
done

# Final summary.
echo >&2
echo "================ summary ================" >&2
echo >&2
printf "  %-10s  %-5s  %s\n" "Workload" "Verdict" "Note" >&2
for r in "${RESULTS[@]}"; do
    n="${r%%|*}"; rest="${r#*|}"
    v="${rest%%|*}"; note="${rest#*|}"
    case "$v" in
        PASS) col="${C_GREEN}";;
        FAIL) col="${C_RED}";;
        SKIP) col="${C_YELLOW}";;
    esac
    printf "  %-10s  ${col}%-5s${C_RESET}  %s\n" "$n" "$v" "$note" >&2
done
echo >&2

if [[ "$overall_fail" -eq 1 ]]; then
    echo "${C_RED}${C_BOLD}SHIP gate: NOT MET${C_RESET}" >&2
    echo "  At least one workload failed under stress.  Per Phase E v0.2" >&2
    echo "  handoff design §4 Day-3-if-Day-1-fails: diagnose remaining" >&2
    echo "  root-walk gap via V3_DBG_NURSERY_BRUTE + GC_AUDIT_ROUND_2" >&2
    echo "  methodology.  Logs: $LOG_DIR" >&2
    exit 1
fi
if [[ "$overall_skip" -gt 0 ]]; then
    echo "${C_YELLOW}${C_BOLD}SHIP gate: PARTIAL${C_RESET} ($overall_skip workload(s) skipped)" >&2
    echo "  Resolve skipped workloads before declaring AR7 closed." >&2
    exit 1
fi
echo "${C_GREEN}${C_BOLD}SHIP gate: MET${C_RESET}" >&2
echo "  All workloads PASS under stress.  Proceed to Day-2 mortality" >&2
echo "  measurement per Phase E v0.2 handoff design §4." >&2
exit 0
