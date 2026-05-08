#!/usr/bin/env bash
# Comprehensive v3 vs TW benchmark with VM/FFI breakdown.
#
# For each workload:
#   - TW wall (baseline)
#   - v3-direct wall (eval bypasses TW eval hook)
#   - v3-direct phase split (lower, compile, run, bridge)
#   - v3 hook stats (evalEntries=top-level entries; forceEntries=
#     re-entries from TW into v3 force hook; bridge ms = time
#     spent in TW callbacks)
#   - Ratio v3.run / TW.wall  (apples-to-apples eval comparison;
#     compile is amortizable across re-evals.)
#
# Compile is reported separately; for production the v3 disk-cache
# amortizes it across re-evals of the same source.
set -u

ROOT="${ROOT:-/Users/angerman/Projects/iohk/nix}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
N="${N:-3}"

declare -a WORKLOADS=(
    "fib30|let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 30"
    "fib33|let f=n: if n<2 then n else f(n - 1) + f(n - 2); in f 33"
    "ackermann-3-7|let a=m: n: if m==0 then n+1 else if n==0 then a (m - 1) 1 else a (m - 1) (a m (n - 1)); in a 3 7"
    "path-deep-30|let mk=n: if n==0 then { v=42; } else { x=mk (n - 1); }; r=mk 30; in r.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.v"
    "letrec-fix|let fix=f: let x=f x; in x; mk=self: { a=1; b=self.a + 1; c=self.b + self.a; d=self.c + self.b; e=self.d + self.c; }; r=fix mk; in [r.a r.b r.c r.d r.e]"
    "list-build-1k|let mk=n: if n==0 then [] else [n] ++ mk (n - 1); in builtins.length (mk 1000)"
    "fold-add-10k|builtins.foldl' (a: b: a + b) 0 (builtins.genList (x: x) 10000)"
    "with-deep|let mk=n: if n==0 then 0 else with { x = n; }; (x + mk (n - 1)); in mk 200"
)

# Time a single eval.
time_run() {
    local mode="$1" expr="$2"
    local t0 t1
    t0=$(date +%s.%N)
    case "$mode" in
        tw)        env "$NIX" eval --impure --expr "$expr" >/dev/null 2>&1 ;;
        v3-direct) env NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" >/dev/null 2>&1 ;;
    esac
    t1=$(date +%s.%N)
    awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.4f", b - a}'
}

# Run with V3_TIMING + NIX_VM_STATS.  Returns: lower,compile,run,bridge,evalEntries,forceEntries
v3_diag() {
    local expr="$1"
    local out
    out=$(env NIX_USE_V3=1 V3_TIMING=1 NIX_VM_STATS=1 \
        "$NIX" eval --impure --expr "$expr" 2>&1)
    local lower compile run bridge evalE forceE
    lower=$(echo "$out" | grep -oE 'lower=[0-9.]+' | head -1 | cut -d= -f2)
    compile=$(echo "$out" | grep -oE 'compile=[0-9.]+' | head -1 | cut -d= -f2)
    run=$(echo "$out" | grep -oE 'run=[0-9.]+' | head -1 | cut -d= -f2)
    bridge=$(echo "$out" | grep -oE 'bridge=[0-9.]+' | head -1 | cut -d= -f2)
    evalE=$(echo "$out" | grep "v3 hook stats" | grep -oE 'evalEntries=[0-9]+' | head -1 | cut -d= -f2)
    forceE=$(echo "$out" | grep "v3 force stats" | grep -oE 'forceEntries=[0-9]+' | head -1 | cut -d= -f2)
    printf '%s,%s,%s,%s,%s,%s' \
        "${lower:-0}" "${compile:-0}" "${run:-0}" "${bridge:-0}" \
        "${evalE:-0}" "${forceE:-0}"
}

mmin() { awk 'NR==1 || $1 < m { m = $1 } END { printf "%.4f", m }'; }

printf "%-16s %8s %8s %8s %10s %10s %8s %8s %6s %6s\n" \
    "workload" "TW(s)" "v3-d(s)" "ratio" "v3.run(ms)" "ms.run/TW" "comp(ms)" "br(ms)" "evHk" "fcHk"
printf "%-16s %8s %8s %8s %10s %10s %8s %8s %6s %6s\n" \
    "--------" "-----" "------" "-----" "----------" "----------" "--------" "------" "----" "----"

for entry in "${WORKLOADS[@]}"; do
    name="${entry%%|*}"
    expr="${entry#*|}"
    tw_runs=()
    v3_runs=()
    for ((i=0; i<N; i++)); do
        tw_runs+=("$(time_run tw "$expr")")
        v3_runs+=("$(time_run v3-direct "$expr")")
    done
    tw_min=$(printf '%s\n' "${tw_runs[@]}" | mmin)
    v3_min=$(printf '%s\n' "${v3_runs[@]}" | mmin)
    ratio=$(awk -v a="$v3_min" -v b="$tw_min" 'BEGIN { if (b>0) printf "%.2fx", a/b; else print "n/a" }')
    diag=$(v3_diag "$expr")
    IFS=, read -r lower compile run bridge evalE forceE <<<"$diag"
    tw_ms=$(awk -v t="$tw_min" 'BEGIN { printf "%.1f", t * 1000 }')
    run_vs_tw=$(awk -v r="$run" -v t="$tw_ms" 'BEGIN { if (t>0) printf "%.2fx", r/t; else print "n/a" }')
    printf "%-16s %8s %8s %8s %10s %10s %8s %8s %6s %6s\n" \
        "$name" "$tw_min" "$v3_min" "$ratio" "$run" "$run_vs_tw" "$compile" "$bridge" "$evalE" "$forceE"
done
