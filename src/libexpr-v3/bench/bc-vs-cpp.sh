#!/usr/bin/env bash
#
# bc-vs-cpp.sh — A/B each v3 BYTECODE primop vs its C++ impl, in BOTH regimes.
#
# v3 ships bytecode impls of ~11 hot primops (bytecode_primops.cc) to cut
# dispatch overhead. Whether that's a win depends on the regime:
#
#   BIG   — one call on a large collection. Per-element work dominates; a
#           bytecode loop just ADDS per-iteration VM overhead (and 4 impls are
#           super-linear). C++ wins here (measured: only map ties).
#   SMALL — the primop called N× on tiny (3-elem) inputs. This is what the
#           subsystem was BUILT for: the per-CALL dispatch saving. Metric =
#           per-call cost; the driver (foldl' over genList N) is identical in
#           both arms so it cancels — the per-call DELTA is the dispatch saving.
#
# Uses the built-in toggle (run.cc: NIX_V3_NO_BC_<NAME>=1 "for A/B comparison").
# file-based workloads + per-eval timeout that reaps. Run on an IDLE host.
#
# Usage:  [NIX_BIN=…] [RUNS=2] [TIMEOUT_S=25] ./bc-vs-cpp.sh [big|small|both]
set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-2}"; TIMEOUT_S="${TIMEOUT_S:-25}"; MODE="${1:-both}"
TMP="$(mktemp -d)"; WL="$TMP/wl.nix"
trap 'rm -rf "$TMP"; pkill -9 -f "$TMP/wl.nix" 2>/dev/null' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN"; exit 2; }

# ── BIG workloads: __N__ = collection size, one call ──────────────────────────
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

# ── SMALL workloads: __N__ = number of CALLS, each on a 3-elem input (i mixes in
#    the outer index so each call is distinct — no hoisting/CSE). foldl' drives. ─
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

# measure <fn> <size> <NO_BC_var|->  → min user-CPU over RUNS | TIMEOUT
measure() {
  local fn="$1" size="$2" novar="$3" best="" v i rc pid watcher envset=""
  [[ "$novar" != "-" ]] && envset="NIX_V3_NO_BC_${novar}=1"
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

if [[ "$MODE" == big || "$MODE" == both ]]; then
  echo; echo "### BIG — one call on a large collection (per-element-work regime) ###"
  printf '%-14s %-9s   %-11s %-11s   %s\n' "primop" "size" "BYTECODE" "C++" "verdict"
  printf '%s\n' "──────────────────────────────────────────────────────────────────────────────"
  for entry in "${PRIMOPS[@]}"; do
    IFS='|' read -r name var bfn bsz _ _ <<<"$entry"
    name="${name// /}"; var="${var// /}"; bfn="${bfn// /}"
    for s in $bsz; do
      bc=$(measure "$bfn" "$s" "-"); cpp=$(measure "$bfn" "$s" "$var")
      verdict=$(awk -v b="$bc" -v c="$cpp" 'BEGIN{
        if(b=="TIMEOUT"&&c+0>0){printf "BC REGRESSION (≥%d×)", int('"$TIMEOUT_S"'/c);exit}
        if(c+0<=0||b+0<=0){print "—";exit} r=b/c;
        if(r>=2)printf "BC REGRESSION (%.0f×)",r; else if(r<=0.66)printf "bytecode win (%.1f×)",c/b; else printf "~par (%.2f×)",r}')
      printf '%-14s %-9s   %-11s %-11s   %s\n' "$name" "$s" "$bc" "$cpp" "$verdict"
    done
  done
fi

if [[ "$MODE" == small || "$MODE" == both ]]; then
  echo; echo "### SMALL — primop called N× on a 3-elem input (per-CALL dispatch regime) ###"
  printf '%-14s %-9s   %-12s %-12s   %s\n' "primop" "calls" "BC ns/call" "C++ ns/call" "verdict"
  printf '%s\n' "──────────────────────────────────────────────────────────────────────────────"
  for entry in "${PRIMOPS[@]}"; do
    IFS='|' read -r name var _ _ sfn ssz <<<"$entry"
    name="${name// /}"; var="${var// /}"; sfn="${sfn// /}"
    # use the largest N (best per-call signal)
    s=$(echo $ssz | awk '{print $NF}')
    bc=$(measure "$sfn" "$s" "-"); cpp=$(measure "$sfn" "$s" "$var")
    verdict=$(awk -v b="$bc" -v c="$cpp" -v n="$s" 'BEGIN{
      if(b!~/^[0-9.]+$/||c!~/^[0-9.]+$/){print b" / "c;exit}
      bpc=b/n*1e9; cpc=c/n*1e9; d=bpc-cpc;
      if(d<-20)      printf "bytecode WIN  (saves %.0f ns/call)", -d;
      else if(d>20)  printf "bytecode slower (+%.0f ns/call)", d;
      else           printf "~par (Δ%.0f ns/call)", d;
      printf "  [bc %.0f / cpp %.0f]", bpc, cpc}')
    bpc=$(awk -v b="$bc" -v n="$s" 'BEGIN{if(b~/^[0-9.]+$/)printf "%.0f", b/n*1e9; else print b}')
    cpc=$(awk -v c="$cpp" -v n="$s" 'BEGIN{if(c~/^[0-9.]+$/)printf "%.0f", c/n*1e9; else print c}')
    printf '%-14s %-9s   %-12s %-12s   %s\n' "$name" "$s" "$bpc" "$cpc" "$verdict"
  done
  echo
  echo "ns/call = total user-CPU / N (driver overhead cancels in the BC-vs-C++ delta)."
  echo "bytecode WIN here = the per-CALL dispatch saving the subsystem was built for."
fi
