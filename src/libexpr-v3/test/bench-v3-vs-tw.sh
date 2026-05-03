#!/usr/bin/env bash
# v3 vs. tree-walker benchmark harness.
#
# Times a fixed set of workloads under tree-walker (default), v3 with
# the cutover hook (NIX_USE_V3=1), and v3 with the force hook also
# enabled (NIX_USE_V3=1 NIX_USE_V3_FORCE=1).  Outputs CSV so each
# landed optimization can be attributed to a measurable delta.
#
# Usage:
#   bench-v3-vs-tw.sh                    # default config; prints table + CSV
#   N=5 bench-v3-vs-tw.sh                # 5 runs per cell (default 3)
#   NPK=$HOME/src/nixpkgs ./...          # override nixpkgs path
#   ONLY=fib35,hello-name ./...          # run a subset by name
#   FORMAT=csv ./...                     # CSV-only output to stdout
#
# CSV columns:
#   workload,evaluator,run,real_seconds,rc
#
# Workloads:
#   fib35           -- compute-bound, no allocation
#   ackermann       -- compute-bound, deeper recursion
#   hello-name      -- nixpkgs cold path, dominant query
#   git-name        -- nixpkgs cold path, deeper attrset chain
#   drv3            -- batched derivation evaluation
#   attr-pkgs       -- nixpkgs attrNames at top level
#   attr-hask       -- nixpkgs attrNames at haskellPackages level
#   path-deep       -- synthetic deeply-nested attrset selection
#   letrec-fix      -- synthetic lib.fix-style let-rec
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix-instantiate}"
NPK="${NPK:-}"  # nixpkgs path; required for hello-name / git-name / drv3 / attr-*
SYSTEM="${SYSTEM:-aarch64-darwin}"
N="${N:-3}"
FORMAT="${FORMAT:-table}"
ONLY="${ONLY:-}"

if [[ ! -x "$NIX" ]]; then
    echo "nix-instantiate not found at $NIX" >&2
    exit 1
fi

# Workload table.  Synthetic workloads (no nixpkgs) are always
# available; nixpkgs-dependent ones are skipped if NPK is empty.
declare -a WORKLOADS=(
    "fib35:synth:let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 35"
    "ackermann:synth:let a=m: n: if m==0 then n+1 else if n==0 then a (m - 1) 1 else a (m - 1) (a m (n - 1)); in a 3 8"
    "path-deep:synth:let mk=n: if n==0 then { v=42; } else { x=mk (n - 1); }; r=mk 30; in r.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.v"
    "letrec-fix:synth:let fix=f: let x=f x; in x; mk=self: { a=1; b=self.a + 1; c=self.b + self.a; d=self.c + self.b; e=self.d + self.c; }; r=fix mk; in [r.a r.b r.c r.d r.e]"
)
if [[ -n "$NPK" && -d "$NPK" ]]; then
    WORKLOADS+=(
        "hello-name:nixpkgs:(import $NPK { system = \"$SYSTEM\"; }).hello.name"
        "git-name:nixpkgs:(import $NPK { system = \"$SYSTEM\"; }).git.name"
        "drv3:nixpkgs:let pkgs=import $NPK { system = \"$SYSTEM\"; }; in [pkgs.hello.drvPath pkgs.git.drvPath pkgs.vim.drvPath]"
        "attr-pkgs:nixpkgs:builtins.length (builtins.attrNames (import $NPK { system = \"$SYSTEM\"; }))"
        "attr-hask:nixpkgs:builtins.length (builtins.attrNames (import $NPK { system = \"$SYSTEM\"; }).haskellPackages)"
    )
fi

# Filter by ONLY (comma-separated names).
if [[ -n "$ONLY" ]]; then
    declare -a FILTERED=()
    IFS=',' read -ra wanted <<<"$ONLY"
    for entry in "${WORKLOADS[@]}"; do
        name="${entry%%:*}"
        for w in "${wanted[@]}"; do
            if [[ "$name" == "$w" ]]; then
                FILTERED+=("$entry")
                break
            fi
        done
    done
    WORKLOADS=("${FILTERED[@]}")
fi

run_once() {
    local mode="$1" expr="$2"
    local -a envv=()
    case "$mode" in
        v3)       envv=(NIX_USE_V3=1) ;;
        v3-fhook) envv=(NIX_USE_V3=1 NIX_USE_V3_FORCE=1) ;;
        tw)       envv=() ;;
    esac
    local t_start t_end real rc
    t_start=$(date +%s.%N)
    env "${envv[@]}" "$NIX" --eval --strict --expr "$expr" >/dev/null 2>&1
    rc=$?
    t_end=$(date +%s.%N)
    real=$(awk -v a="$t_start" -v b="$t_end" 'BEGIN{printf "%.4f", b - a}')
    printf '%s,%d\n' "$real" "$rc"
}

# Statistics: print mean, min, p50, p95, stddev for a list of times.
stats() {
    awk '
        { times[NR] = $1; sum += $1 }
        END {
            n = NR
            if (n == 0) { print "0,0,0,0,0,0"; exit }
            mean = sum / n
            # Sort copy
            for (i = 1; i <= n; i++) sorted[i] = times[i]
            for (i = 1; i <= n; i++)
                for (j = i+1; j <= n; j++)
                    if (sorted[i] > sorted[j]) { t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t }
            min = sorted[1]
            p50 = sorted[int((n+1)/2)]
            p95idx = int(n * 0.95 + 0.5); if (p95idx < 1) p95idx = 1; if (p95idx > n) p95idx = n
            p95 = sorted[p95idx]
            sumsq = 0
            for (i = 1; i <= n; i++) { d = times[i] - mean; sumsq += d * d }
            stddev = (n > 1) ? sqrt(sumsq / (n - 1)) : 0
            printf "%d,%.4f,%.4f,%.4f,%.4f,%.4f\n", n, mean, min, p50, p95, stddev
        }
    '
}

# Emit raw CSV.  One row per individual run.
csv_raw() {
    local entry name kind expr mode r
    echo "workload,evaluator,run,real_seconds,rc"
    for entry in "${WORKLOADS[@]}"; do
        name="${entry%%:*}"
        rest="${entry#*:}"
        kind="${rest%%:*}"
        expr="${rest#*:}"
        for mode in tw v3 v3-fhook; do
            for run in $(seq 1 "$N"); do
                r=$(run_once "$mode" "$expr")
                real="${r%,*}"
                rc="${r#*,}"
                echo "$name,$mode,$run,$real,$rc"
            done
        done
    done
}

# Emit summary table.  One row per (workload, evaluator).
table() {
    printf "%-13s %-10s %-3s %8s %8s %8s %8s %8s\n" \
        "workload" "evaluator" "n" "mean" "min" "p50" "p95" "stddev"
    printf "%-13s %-10s %-3s %8s %8s %8s %8s %8s\n" \
        "--------" "---------" "---" "-----" "-----" "-----" "-----" "------"
    local entry name kind expr mode r runs
    for entry in "${WORKLOADS[@]}"; do
        name="${entry%%:*}"
        rest="${entry#*:}"
        kind="${rest%%:*}"
        expr="${rest#*:}"
        for mode in tw v3 v3-fhook; do
            runs=""
            local any_fail=0
            for run in $(seq 1 "$N"); do
                r=$(run_once "$mode" "$expr")
                real="${r%,*}"
                rc="${r#*,}"
                if [[ "$rc" -ne 0 ]]; then any_fail=1; fi
                runs+="$real"$'\n'
            done
            if [[ "$any_fail" -eq 1 ]]; then
                printf "%-13s %-10s %-3s %8s %8s %8s %8s %8s\n" \
                    "$name" "$mode" "$N" "FAIL" "" "" "" ""
            else
                stat=$(printf '%s' "$runs" | stats)
                IFS=, read -r n mean min p50 p95 sd <<<"$stat"
                printf "%-13s %-10s %-3s %8s %8s %8s %8s %8s\n" \
                    "$name" "$mode" "$n" "$mean" "$min" "$p50" "$p95" "$sd"
            fi
        done
    done
}

case "$FORMAT" in
    csv)   csv_raw ;;
    table) table ;;
    *)     echo "unknown FORMAT=$FORMAT (want: table, csv)" >&2; exit 1 ;;
esac
