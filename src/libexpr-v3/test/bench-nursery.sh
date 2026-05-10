#!/usr/bin/env bash
# Compare nursery scavenge OFF (Phase A only) vs ON across the
# existing benchmark workloads.  Wall time only; RSS is measured
# separately because /usr/bin/time -l overhead biases the wall.
#
# Goal: confirm that NIX_V3_NURSERY_SCAVENGE=1 either matches or
# beats the no-nursery baseline at the default 32 MB nursery size,
# and stays correct (no regressions) at an aggressive 1 MB size.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u

ROOT="${ROOT:-/Users/angerman/Projects/iohk/nix}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
N="${N:-3}"

declare -a WORKLOADS=(
    "fib30|let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 30"
    "fib33|let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 33"
    "ackermann-3-7|let a=m: n: if m==0 then n+1 else if n==0 then a (m - 1) 1 else a (m - 1) (a m (n - 1)); in a 3 7"
    "letrec-fix|let fix=f: let x=f x; in x; mk=self: { a=1; b=self.a + 1; c=self.b + self.a; d=self.c + self.b; e=self.d + self.c; }; r=fix mk; in [r.a r.b r.c r.d r.e]"
    "list-build-1k|let mk=n: if n==0 then [] else [n] ++ mk (n - 1); in builtins.length (mk 1000)"
    "fold-add-50k|builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 50000)"
    "deep-letrec-2k|let rec1=self: { a=1; b=2; c=self.a + self.b; d=self.c * 2; e=self.d + self.a; }; fix=f: let x=f x; in x; deep=n: if n==0 then 0 else (fix rec1).e + deep (n - 1); in deep 2000"
)

time_run() {
    local mode="$1" expr="$2"
    local t0 t1
    t0=$(date +%s.%N)
    case "$mode" in
        off)        NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=0 NIX_V3_NURSERY_SCAVENGE=0 \
                        "$NIX" --extra-experimental-features nix-command eval --impure --expr "$expr" >/dev/null 2>&1 ;;
        scav32)     NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 \
                        "$NIX" --extra-experimental-features nix-command eval --impure --expr "$expr" >/dev/null 2>&1 ;;
        scav1)      NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 NIX_V3_NURSERY_SIZE=1 \
                        "$NIX" --extra-experimental-features nix-command eval --impure --expr "$expr" >/dev/null 2>&1 ;;
    esac
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.4f", b - a}'
}

mmin() { awk 'NR==1 || $1 < m { m = $1 } END { printf "%.4f", m }'; }

printf "%-18s %10s %10s %10s %8s %8s\n" \
    "workload" "off(s)" "scav32(s)" "scav1(s)" "32/off" "1/off"
printf "%-18s %10s %10s %10s %8s %8s\n" \
    "------------------" "------" "---------" "--------" "------" "-----"

for entry in "${WORKLOADS[@]}"; do
    name="${entry%%|*}"
    expr="${entry#*|}"
    off_runs=()
    s32_runs=()
    s1_runs=()
    for ((i=0; i<N; i++)); do
        off_runs+=("$(time_run off "$expr")")
        s32_runs+=("$(time_run scav32 "$expr")")
        s1_runs+=("$(time_run scav1 "$expr")")
    done
    off_min=$(printf '%s\n' "${off_runs[@]}" | mmin)
    s32_min=$(printf '%s\n' "${s32_runs[@]}" | mmin)
    s1_min=$(printf '%s\n' "${s1_runs[@]}" | mmin)
    r32=$(awk -v a="$s32_min" -v b="$off_min" 'BEGIN { if (b>0) printf "%.2fx", a/b; else print "n/a" }')
    r1=$(awk -v a="$s1_min" -v b="$off_min" 'BEGIN { if (b>0) printf "%.2fx", a/b; else print "n/a" }')
    printf "%-18s %10s %10s %10s %8s %8s\n" \
        "$name" "$off_min" "$s32_min" "$s1_min" "$r32" "$r1"
done
