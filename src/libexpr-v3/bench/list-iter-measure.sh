#!/usr/bin/env bash
#
# list-iter-measure.sh — T0 yardstick for the list-iteration fix plan.
#
# Per-pass v3-vs-TW user-CPU decomposition (genList / map / foldl / mapfoldl /
# filter) + the FINAL-dump NIX_VM_STATS (pairs / lists / insns / peak-RSS) for
# map.foldl + filter. Every fix-plan task (T1–T7) measures its keep/revert delta
# against THIS, run the same way each time.
#   lode/LIST_ITERATION_FIX_PLAN_2026-06-08.md   — the tasks + keep/revert bars
#   lode/LIST_ITERATION_PERF_2026-06-08.md       — the RCA (why each number)
#   lode/MEASUREMENT_GATE_2026-06-07.md §8        — run on darwin-4 (the timing host)
#
# Standalone (shells out to the built nix; never touches the engine build).
# Run ON the timing host. genList MUST read ~1.00× — if not, the host is loaded
# or the binary is wrong; the numbers are not trustworthy.
#
# Usage:  [NIX_BIN=…] [RUNS=3] [N=2000000] [V3_WALL=180s] [V3_HEAP=6G] ./list-iter-measure.sh
set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-3}"
N="${N:-2000000}"
export NIX_V3_MAX_WALL_TIME="${V3_WALL:-180s}" NIX_V3_MAX_HEAP="${V3_HEAP:-6G}"
A="--extra-experimental-features"; F="nix-command flakes"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN (set NIX_BIN=)"; exit 2; }

gen="builtins.genList (i: i) $N"
declare -A WL
WL[genList]="builtins.length ($gen)"
WL[map]="builtins.length (builtins.map (x: x + 1) ($gen))"
WL[foldl]="builtins.foldl' (a: x: a + x) 0 ($gen)"
WL[mapfoldl]="builtins.foldl' (a: x: a + x) 0 (builtins.map (x: x + 1) ($gen))"
WL[filter]="builtins.length (builtins.filter (x: x > 1) ($gen))"

# run <outfile> <env/argv…> → echoes user-CPU seconds; eval stdout → outfile
run(){ local out="$1"; shift; /usr/bin/time -l env "$@" >"$out" 2>"$TMP/e"
       grep -oE '[0-9.]+ user' "$TMP/e" | grep -oE '^[0-9.]+' | head -1; }
min(){ awk "BEGIN{print ($1<$2)?$1:$2}"; }
field(){ sed -nE "s/.*[^a-z]$2=([0-9.]+).*/\1/p" <<<"$1" | head -1; }   # $2=NN from a stats line

echo "list-iter-measure — host=$(hostname -s)  load=$(uptime | sed 's/.*averages*: //')  N=$N  RUNS=$RUNS"
echo "nix=$NIX_BIN  ($("$NIX_BIN" --version 2>/dev/null | head -1))"
echo
echo "── per-pass user-CPU, v3 vs TW (best of $RUNS) ──   [genList MUST be ~1.00×]"
printf '  %-10s %8s %8s %7s   %s\n' pass TW v3 v3/TW result
for k in genList map foldl mapfoldl filter; do
  e="${WL[$k]}"; t=999; v=999
  for ((i=0;i<RUNS;i++)); do
    x=$(run "$TMP/ot" "$NIX_BIN" eval $A "$F" --expr "$e");                       t=$(min "${x:-999}" "$t")
    y=$(run "$TMP/ov" NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" eval $A "$F" --expr "$e");   v=$(min "${y:-999}" "$v")
  done
  [[ "$(cat "$TMP/ot")" == "$(cat "$TMP/ov")" ]] && id="identical ✓" || id="DIVERGENT ✗"
  printf '  %-10s %7ss %7ss %6sx   %s\n' "$k" "$t" "$v" "$(awk "BEGIN{printf \"%.2f\",$v/$t}")" "$id"
done

echo
echo "── v3 allocation + peak-RSS (FINAL NIX_VM_STATS dump — tail, not head) ──"
printf '  %-9s %10s %7s %10s %7s %11s %7s %4s\n' workload pairs pairsMB lists listsMB insns peakRSS eng
for w in mapfoldl filter; do
  /usr/bin/time -l env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 "$NIX_BIN" eval $A "$F" \
      --expr "${WL[$w]}" >/dev/null 2>"$TMP/s"
  al=$(grep 'v3-direct alloc:' "$TMP/s" | tail -1)
  pt=$(grep 'run-phase per-tag bytes' "$TMP/s" | tail -1)
  rssB=$(grep 'maximum resident' "$TMP/s" | grep -oE '[0-9]+' | head -1)
  eng=$(grep -q 'v3-direct' "$TMP/s" && echo yes || echo NO)
  printf '  %-9s %10s %6sM %10s %6sM %11s %6sM %4s\n' "$w" \
    "$(field "$al" pairs)" "$(field "$pt" pairs)" \
    "$(field "$al" lists)" "$(field "$pt" lists)" \
    "$(field "$al" insns)" "$(awk "BEGIN{printf \"%.0f\",${rssB:-0}/1048576}")" "$eng"
done

echo
echo "BASELINE pinned 2026-06-08 (darwin-4, post-fix 2.35.0):"
echo "  per-pass  genList 1.00× · map 3.6× · foldl 3.5× · mapfoldl 4.0× · filter 4.7×"
echo "  mapfoldl  pairs=6000002 pairsMB=384 lists=3 insns=26000033 peakRSS≈556M"
echo "  filter    pairs=4000000 lists=2000002 listsMB=144 insns=26000036 peakRSS≈515M"
echo "A fix WINS when its target metric clears the keep-bar in LIST_ITERATION_FIX_PLAN_2026-06-08.md,"
echo "results stay 'identical ✓', eng=yes, and 'make scaling' stays green."
