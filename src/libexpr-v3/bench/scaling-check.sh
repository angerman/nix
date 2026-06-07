#!/usr/bin/env bash
#
# scaling-check.sh — COMPLEXITY-CLASS regression guard for v3.
#
# Why this and not a threshold benchmark: the listToAttrs O(n²) blowup was
# LATENT at small n (fine at 1k, 127× at 200k). An absolute "must run in < T ms"
# test passes at small n and never catches it. The only thing that does is a
# SCALING test: measure at n, 2n, 4n and assert the doubling RATIO stays in the
# expected class — ~2× for O(n)/O(n log n); ~4× ⇒ O(n²) ⇒ FAIL.
#
# Bonus: the ratio CANCELS host speed/load (both sizes scale together), so it is
# far more noise-robust than an absolute timing — but still run the paired sizes
# back-to-back on an idle host (MEASUREMENT_GATE §8 = darwin-4).
#
# Metric tiers (per-workload, in the manifest):
#   user   — wall-free user-CPU; catches "O(n) ops each O(n)" (the listToAttrs
#            class that counters MISS). Needs an idle host. min-of-RUNS.
#   insns  — bytecode insn count (deterministic, load-immune; catches O(n²) insns)
#   attrs  — attrsets allocated   (deterministic; catches O(n²) allocation)
#   rss    — peak RSS             (deterministic-ish; catches memory blowups)
#
# XFAIL: a known-failing entry keeps the suite GREEN and AUTO-DETECTS the fix:
# when it measures linear it reports **XPASS** ("fix landed — remove the xfail,
# promote to a hard guard"). So while the team fixes listToAttrs concurrently,
# this is green now and flags the exact moment it's fixed.
#
# Usage:  [NIX_BIN=…] [RUNS=3] ./scaling-check.sh
# Exit:   1 on a hard FAIL (a non-xfail entry went super-linear) or ERROR.

set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-3}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN (set NIX_BIN=)"; exit 2; }

# ── workload templates ({n} = size; quote-safe: no embedded " and no $ ) ──────
T_lta="let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) {n}); in builtins.seq s 0"
T_map="let xs = builtins.map (x: x + 1) (builtins.genList (i: i) {n}); in builtins.foldl' (a: x: a + x) 0 xs"
T_filter="let xs = builtins.filter (x: x > 1) (builtins.genList (i: i) {n}); in builtins.length xs"
T_fold="let xs = builtins.genList (i: i) {n}; in builtins.foldl' (a: x: a + x) 0 xs"
T_sort="let xs = builtins.sort (a: b: a < b) (builtins.genList (i: {n} - i) {n}); in builtins.length xs"

# ── manifest:  name | template-var | sizes | metric | expected-class | xfail ──
MANIFEST=(
  "listToAttrs | T_lta    | 40000 80000 160000      | user | nlogn  |"
  "map         | T_map    | 500000 1000000 2000000  | user | linear |"
  "filter      | T_filter | 500000 1000000 2000000  | user | linear |"
  "foldl'      | T_fold   | 500000 1000000 2000000  | user | linear |"
  # sort: XFAIL — builtins.sort is super-linear in v3 (5k→10k = 10.9× ≈ O(n³),
  # found 2026-06-07; a 200k sort runs for >25 min). Tiny sizes so the suite
  # survives it; xfail keeps the suite green + auto-detects a future fix.
  "sort        | T_sort   | 1000 2000 4000          | user | nlogn  | xfail"
)

# measure <expr> <metric> → echoes the metric value (min over RUNS), or NOTENGAGED
measure() {
  local expr="$1" metric="$2" best="" v i
  for ((i=0; i<RUNS; i++)); do
    /usr/bin/time -l env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 "$NIX_BIN" \
        eval --extra-experimental-features 'nix-command flakes' --impure \
        --expr "$expr" >/dev/null 2>"$TMP/m" || true
    grep -q 'v3-direct' "$TMP/m" || { echo NOTENGAGED; return; }
    case "$metric" in
      user)  v=$(grep -oE '[0-9.]+ user' "$TMP/m" | grep -oE '^[0-9.]+' | head -1) ;;
      insns) v=$(grep -oE 'insns=[0-9]+' "$TMP/m" | grep -oE '[0-9]+' | tail -1) ;;
      rss)   v=$(grep 'maximum resident set size' "$TMP/m" | grep -oE '[0-9]+' | head -1) ;;
      attrs) v=$(grep -oE 'attrsets=[0-9]+' "$TMP/m" | grep -oE '[0-9]+' | tail -1) ;;
    esac
    [[ -z "$v" ]] && { echo NODATA; return; }
    [[ -z "$best" ]] && best="$v" || best=$(awk "BEGIN{print ($v<$best)?$v:$best}")
  done
  echo "$best"
}

echo "scaling-check — host=$(hostname -s)  load=$(uptime | sed 's/.*averages*: //')  nix=$NIX_BIN  RUNS=$RUNS"
echo

RESULTS="$TMP/results"
for entry in "${MANIFEST[@]}"; do
  IFS='|' read -r name tvar sizes metric class xfail <<<"$entry"
  name="${name// /}"; tvar="${tvar// /}"; metric="${metric// /}"; class="${class// /}"; xfail="${xfail// /}"
  tmpl="${!tvar}"
  vals=""
  for s in $sizes; do
    vals="$vals $(measure "${tmpl//\{n\}/$s}" "$metric")"
  done
  echo "$name|$metric|$class|$xfail|$(echo $sizes)|$(echo $vals)" >>"$RESULTS"
done

python3 - "$RESULTS" <<'PY'
import sys, math
rows = [l.rstrip("\n") for l in open(sys.argv[1])]
ACC = {"linear": 2.5, "nlogn": 2.8}   # max acceptable geomean doubling ratio
hard_fail = 0; xpass = 0
print(f'{"workload":<12}{"metric":>7}{"expect":>8}   sizes→values (doubling ratios)            class      verdict')
print("─"*104)
for r in rows:
    name, metric, klass, xfail, sizes, vals = r.split("|")
    sl = sizes.split(); vl = vals.split()
    try:
        nums = [float(v) for v in vl]
        if any(n <= 0 for n in nums): raise ValueError
    except ValueError:
        print(f'{name:<12}{metric:>7}{klass:>8}   {vals.strip():<44} —          ERROR ({vals.strip()})')
        hard_fail += 1; continue
    ratios = [nums[i+1]/nums[i] for i in range(len(nums)-1)]
    g = math.exp(sum(math.log(x) for x in ratios)/len(ratios))   # geomean doubling ratio
    measured = "linear" if g <= 2.5 else ("nlogn" if g <= 2.8 else ("super" if g < 3.3 else "QUADRATIC"))
    ok = g <= ACC.get(klass, 2.8)
    if xfail == "xfail":
        verdict = "XPASS ⚠ promote!" if ok else "xfail (known)"
        if ok: xpass += 1
    else:
        verdict = "PASS" if ok else "FAIL ✗ REGRESSION"
        if not ok: hard_fail += 1
    sv = "  ".join(f"{int(float(s)/1000)}k:{n:.3g}" for s, n in zip(sl, nums))
    rr = "×".join(f"{x:.1f}" for x in ratios)
    print(f'{name:<12}{metric:>7}{klass:>8}   {sv:<32} r={g:.2f} [{rr}]   {measured:<10} {verdict}')
print("─"*104)
if xpass:     print(f"⚠  {xpass} XPASS — a known-O(n²) workload now scales linearly. The fix landed: remove its `xfail` and it becomes a hard guard.")
if hard_fail: print(f"✗  {hard_fail} hard failure(s) — a workload that should be linear went super-linear. REGRESSION."); sys.exit(1)
print("✓  no regressions (xfail entries are known/tracked).")
PY
