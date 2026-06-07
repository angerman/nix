#!/usr/bin/env bash
#
# bc-vs-cpp.sh — A/B v3 BYTECODE primops vs C++, in THREE regimes.
#
#   big   — one call on a large collection (per-element work). C++ wins (only map ties).
#   small — primop called N× on tiny inputs (per-CALL dispatch). C++ wins all 10.
#   chain — a fusable chain (map|>filter|>foldl') 3-way: default (bytecode, unfused)
#           vs STREAM_FUSION=1 (fused, no intermediate lists) vs NO_BYTECODE_PRIMOPS=1
#           (C++ + intermediates). This is the modest-6's last possible justification —
#           does fusion beat C++? (NB: opt_stream_fusion is default-OFF + "regresses".)
#
# Toggle (run.cc): NIX_V3_NO_BC_<NAME>=1 / NIX_V3_NO_BYTECODE_PRIMOPS=1 / NIX_V3_STREAM_FUSION=1.
# file-based workloads + per-eval timeout that reaps. Run on an IDLE host.
#
# Usage:  [NIX_BIN=…] [RUNS=2] [TIMEOUT_S=25] ./bc-vs-cpp.sh [big|small|chain|all]
set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-2}"; TIMEOUT_S="${TIMEOUT_S:-25}"; MODE="${1:-all}"
TMP="$(mktemp -d)"; WL="$TMP/wl.nix"
trap 'rm -rf "$TMP"; pkill -9 -f "$TMP/wl.nix" 2>/dev/null' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN"; exit 2; }

# ── BIG: __N__ = collection size, one call ───────────────────────────────────
big_map()          { echo 'let xs = builtins.map (x: x + 1) (builtins.genList (i: i) __N__); in builtins.length xs'; }
big_filter()       { echo 'let xs = builtins.filter (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length xs'; }
big_foldl()        { echo 'let xs = builtins.genList (i: i) __N__; in builtins.foldl'"'"' (a: x: a + x) 0 xs'; }
big_concatMap()    { echo 'let xs = builtins.concatMap (x: [ x x ]) (builtins.genList (i: i) __N__); in builtins.length xs'; }
big_sort()         { echo 'let xs = builtins.sort (a: b: a < b) (builtins.genList (i: __N__ - i) __N__); in builtins.length xs'; }
big_zipAttrsWith() { echo 'let xs = builtins.genList (i: builtins.listToAttrs [ { name = toString i; value = i; } ]) __N__; in builtins.length (builtins.attrNames (builtins.zipAttrsWith (k: vs: vs) xs))'; }
big_all()          { echo 'builtins.all (x: x > (0 - 1)) (builtins.genList (i: i) __N__)'; }
big_any()          { echo 'builtins.any (x: x > __N__) (builtins.genList (i: i) __N__)'; }
big_partition()    { echo 'let p = builtins.partition (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length p.right'; }
big_groupBy()      { echo 'let g = builtins.groupBy (x: toString (x - (x / 2) * 2)) (builtins.genList (i: i) __N__); in builtins.length (builtins.attrValues g)'; }

# ── SMALL: __N__ = number of CALLS, each on a 3-elem input (i defeats hoisting) ─
sm_map()           { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.map (x: x + i) [ 1 2 3 ])) 0 (builtins.genList (j: j) __N__)'; }
sm_filter()        { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.filter (x: x > i) [ 1 2 3 ])) 0 (builtins.genList (j: j) __N__)'; }
sm_foldl()         { echo 'builtins.foldl'"'"' (a: i: a + (builtins.foldl'"'"' (b: x: b + x + i) 0 [ 1 2 3 ])) 0 (builtins.genList (j: j) __N__)'; }
sm_concatMap()     { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.concatMap (x: [ x i ]) [ 1 2 3 ])) 0 (builtins.genList (j: j) __N__)'; }
sm_sort()          { echo 'builtins.foldl'"'"' (a: i: a + builtins.head (builtins.sort (x: y: x < y) [ (i + 3) (i + 1) (i + 2) ])) 0 (builtins.genList (j: j) __N__)'; }
sm_zipAttrsWith()  { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.attrNames (builtins.zipAttrsWith (k: vs: vs) [ { a = i; } { b = i; } ]))) 0 (builtins.genList (j: j) __N__)'; }
sm_all()           { echo 'builtins.foldl'"'"' (a: i: a + (if builtins.all (x: x > (i - 100)) [ 1 2 3 ] then 1 else 0)) 0 (builtins.genList (j: j) __N__)'; }
sm_any()           { echo 'builtins.foldl'"'"' (a: i: a + (if builtins.any (x: x > (i + 100)) [ 1 2 3 ] then 0 else 1)) 0 (builtins.genList (j: j) __N__)'; }
sm_partition()     { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.partition (x: x > i) [ 1 2 3 ]).right) 0 (builtins.genList (j: j) __N__)'; }
sm_groupBy()       { echo 'builtins.foldl'"'"' (a: i: a + builtins.length (builtins.attrNames (builtins.groupBy (x: toString (x + i)) [ 1 2 3 ]))) 0 (builtins.genList (j: j) __N__)'; }

# ── CHAIN: fusable chains over a large list (__N__ = size) ────────────────────
ch_mapFilterFold() { echo 'builtins.foldl'"'"' (a: x: a + x) 0 (builtins.filter (x: x > 1) (builtins.map (x: x + 1) (builtins.genList (i: i) __N__)))'; }
ch_mapMapFold()    { echo 'builtins.foldl'"'"' (a: x: a + x) 0 (builtins.map (x: x * 2) (builtins.map (x: x + 1) (builtins.genList (i: i) __N__)))'; }
ch_mapFilterLen()  { echo 'builtins.length (builtins.filter (x: x > 1) (builtins.map (x: x + 1) (builtins.genList (i: i) __N__)))'; }
ch_filterMapFold() { echo 'builtins.foldl'"'"' (a: x: a + x) 0 (builtins.map (x: x * 2) (builtins.filter (x: x > 1) (builtins.genList (i: i) __N__)))'; }

# primop | NO_BC_<VAR> | big-fn | big-sizes | small-fn | small-Ncalls
PRIMOPS=(
  "map          | MAP            | big_map          | 1000000 2000000 | sm_map          | 1000000 2000000"
  "filter       | FILTER         | big_filter       | 100000 200000   | sm_filter       | 1000000 2000000"
  "foldl'       | FOLDL          | big_foldl        | 1000000 2000000 | sm_foldl        | 1000000 2000000"
  "concatMap    | CONCATMAP      | big_concatMap    | 50000 100000    | sm_concatMap    | 1000000 2000000"
  "sort         | SORT           | big_sort         | 5000 10000      | sm_sort         | 1000000 2000000"
  "zipAttrsWith | ZIP_ATTRS_WITH | big_zipAttrsWith | 50000 100000    | sm_zipAttrsWith | 500000 1000000"
  "all          | ALL            | big_all          | 1000000 2000000 | sm_all          | 1000000 2000000"
  "any          | ANY            | big_any          | 1000000 2000000 | sm_any          | 1000000 2000000"
  "partition    | PARTITION      | big_partition    | 250000 500000   | sm_partition    | 1000000 2000000"
  "groupBy      | GROUPBY        | big_groupBy      | 250000 500000   | sm_groupBy      | 500000 1000000"
)
# chain | fn | sizes   (names use '.' not '|>' to avoid the field delimiter)
CHAINS=(
  "map.filter.foldl | ch_mapFilterFold | 1000000 2000000"
  "map.map.foldl    | ch_mapMapFold    | 1000000 2000000"
  "map.filter.len   | ch_mapFilterLen  | 1000000 2000000"
  "filter.map.foldl | ch_filterMapFold | 1000000 2000000"
)

# measure <fn> <size> <extra-env|->  → min user-CPU over RUNS | TIMEOUT | NOENG
measure() {
  local fn="$1" size="$2" env3="$3" best="" v i rc pid watcher envset=""
  [[ "$env3" != "-" ]] && envset="$env3"
  "$fn" | sed "s/__N__/$size/g" > "$WL"
  for ((i=0; i<RUNS; i++)); do
    /usr/bin/time -l env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 $envset "$NIX_BIN" \
        eval --extra-experimental-features 'nix-command flakes' --file "$WL" >/dev/null 2>"$TMP/m" &
    pid=$!
    ( sleep "$TIMEOUT_S"; kill -9 "$pid" 2>/dev/null; pkill -9 -f "$WL" 2>/dev/null ) & watcher=$!
    wait "$pid" 2>/dev/null; rc=$?
    kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
    [[ $rc -eq 137 ]] && { echo TIMEOUT; return; }
    grep -q 'v3-direct' "$TMP/m" || { echo NOENG; return; }
    v=$(grep -oE '[0-9.]+ user' "$TMP/m" | grep -oE '^[0-9.]+' | head -1)
    [[ -z "$v" ]] && { echo NODATA; return; }
    [[ -z "$best" ]] && best="$v" || best=$(awk "BEGIN{print ($v<$best)?$v:$best}")
  done
  echo "$best"
}

echo "bc-vs-cpp ($MODE) — host=$(hostname -s)  load=$(uptime|sed 's/.*averages*: //')  RUNS=$RUNS  TIMEOUT=${TIMEOUT_S}s"
echo "nix=$NIX_BIN"

if [[ "$MODE" == big || "$MODE" == all ]]; then
  echo; echo "### BIG — one call on a large collection ###"
  printf '%-14s %-9s   %-11s %-11s   %s\n' primop size BYTECODE C++ verdict
  printf '%s\n' "──────────────────────────────────────────────────────────────────────────────"
  for e in "${PRIMOPS[@]}"; do
    IFS='|' read -r name var bfn bsz _ _ <<<"$e"; name="${name// /}"; var="${var// /}"; bfn="${bfn// /}"
    for s in $bsz; do
      bc=$(measure "$bfn" "$s" "-"); cpp=$(measure "$bfn" "$s" "NIX_V3_NO_BC_${var}=1")
      vd=$(awk -v b="$bc" -v c="$cpp" 'BEGIN{if(b=="TIMEOUT"&&c+0>0){printf"BC REGRESSION (≥%d×)",int('"$TIMEOUT_S"'/c);exit}if(c+0<=0||b+0<=0){print"—";exit}r=b/c;if(r>=2)printf"BC REGRESSION (%.0f×)",r;else if(r<=0.66)printf"bytecode win (%.1f×)",c/b;else printf"~par (%.2f×)",r}')
      printf '%-14s %-9s   %-11s %-11s   %s\n' "$name" "$s" "$bc" "$cpp" "$vd"
    done
  done
fi

if [[ "$MODE" == small || "$MODE" == all ]]; then
  echo; echo "### SMALL — primop called N× on a 3-elem input (per-CALL dispatch) ###"
  printf '%-14s %-9s   %-12s %-12s   %s\n' primop calls "BC ns/call" "C++ ns/call" verdict
  printf '%s\n' "──────────────────────────────────────────────────────────────────────────────"
  for e in "${PRIMOPS[@]}"; do
    IFS='|' read -r name var _ _ sfn ssz <<<"$e"; name="${name// /}"; var="${var// /}"; sfn="${sfn// /}"
    s=$(echo $ssz | awk '{print $NF}')
    bc=$(measure "$sfn" "$s" "-"); cpp=$(measure "$sfn" "$s" "NIX_V3_NO_BC_${var}=1")
    bpc=$(awk -v b="$bc" -v n="$s" 'BEGIN{if(b~/^[0-9.]+$/)printf"%.0f",b/n*1e9;else print b}')
    cpc=$(awk -v c="$cpp" -v n="$s" 'BEGIN{if(c~/^[0-9.]+$/)printf"%.0f",c/n*1e9;else print c}')
    vd=$(awk -v b="$bc" -v c="$cpp" -v n="$s" 'BEGIN{if(b!~/^[0-9.]+$/||c!~/^[0-9.]+$/){print b" / "c;exit}d=(b-c)/n*1e9;if(d<-20)printf"bytecode WIN (saves %.0f ns/call)",-d;else if(d>20)printf"bytecode slower (+%.0f ns/call)",d;else printf"~par (Δ%.0f)",d}')
    printf '%-14s %-9s   %-12s %-12s   %s\n' "$name" "$s" "$bpc" "$cpc" "$vd"
  done
fi

if [[ "$MODE" == chain || "$MODE" == all ]]; then
  echo; echo "### CHAIN — fusable chain over a large list, 3-way ###"
  printf '%-20s %-9s   %-10s %-10s %-10s   %s\n' chain size "deflt(bc)" "FUSED" "C++" "verdict (fused vs C++)"
  printf '%s\n' "────────────────────────────────────────────────────────────────────────────────────────────"
  for e in "${CHAINS[@]}"; do
    IFS='|' read -r name fn szs <<<"$e"; name="${name// /}"; fn="${fn// /}"
    for s in $szs; do
      d=$(measure "$fn" "$s" "-"); f=$(measure "$fn" "$s" "NIX_V3_STREAM_FUSION=1"); c=$(measure "$fn" "$s" "NIX_V3_NO_BYTECODE_PRIMOPS=1")
      vd=$(awk -v f="$f" -v c="$c" 'BEGIN{if(f!~/^[0-9.]+$/||c!~/^[0-9.]+$/){print "fused="f" cpp="c;exit}r=f/c;if(r<=0.9)printf"FUSION WINS (%.2f× of C++)",r;else if(r>=1.1)printf"fusion loses (%.2f× C++)",r;else printf"~par (%.2f×)",r}')
      printf '%-20s %-9s   %-10s %-10s %-10s   %s\n' "$name" "$s" "$d" "$f" "$c" "$vd"
    done
  done
  echo
  echo "FUSION WINS = the bytecode+fusion path beats C++-with-intermediates → the modest-6's"
  echo "only justification. (Note: stream fusion is opt-in/default-OFF and documented as regressing.)"
fi
