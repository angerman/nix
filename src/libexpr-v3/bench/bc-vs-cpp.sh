#!/usr/bin/env bash
#
# bc-vs-cpp.sh — A/B each v3 BYTECODE primop vs its C++ impl.
#
# v3 ships bytecode implementations of ~11 "hot" primops (bytecode_primops.cc)
# to cut dispatch overhead. But a bytecode impl can be ALGORITHMICALLY WORSE
# than the C++ one (the per-element callback runs through the VM, which can turn
# an O(n log n) algorithm into O(n²)/O(n³) — see sort, zipAttrsWith). This A/Bs
# each one using the built-in toggle (run.cc: NIX_V3_NO_BC_<NAME>=1 "for A/B")
# and reports whether bytecode is a WIN or a REGRESSION vs C++.
#
# Complements scaling-check.sh (absolute complexity guard); this is the
# "which implementation strategy wins, per primop" view. Run on an IDLE host.
#
# Usage:  [NIX_BIN=…] [RUNS=2] [TIMEOUT_S=40] ./bc-vs-cpp.sh
set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-2}"; TIMEOUT_S="${TIMEOUT_S:-40}"
TMP="$(mktemp -d)"; WL="$TMP/wl.nix"
trap 'rm -rf "$TMP"; pkill -9 -f "$TMP/wl.nix" 2>/dev/null' EXIT
[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN"; exit 2; }

wl_map()          { cat <<'N'
let xs = builtins.map (x: x + 1) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_filter()       { cat <<'N'
let xs = builtins.filter (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_foldl()        { cat <<'N'
let xs = builtins.genList (i: i) __N__; in builtins.foldl' (a: x: a + x) 0 xs
N
}
wl_concatMap()    { cat <<'N'
let xs = builtins.concatMap (x: [ x x ]) (builtins.genList (i: i) __N__); in builtins.length xs
N
}
wl_sort()         { cat <<'N'
let xs = builtins.sort (a: b: a < b) (builtins.genList (i: __N__ - i) __N__); in builtins.length xs
N
}
wl_zipAttrsWith() { cat <<'N'
let xs = builtins.genList (i: builtins.listToAttrs [ { name = toString i; value = i; } ]) __N__; in builtins.length (builtins.attrNames (builtins.zipAttrsWith (k: vs: vs) xs))
N
}
wl_all()          { cat <<'N'
builtins.all (x: x > (0 - 1)) (builtins.genList (i: i) __N__)
N
}
wl_any()          { cat <<'N'
builtins.any (x: x > __N__) (builtins.genList (i: i) __N__)
N
}
wl_partition()    { cat <<'N'
let p = builtins.partition (x: x > 1) (builtins.genList (i: i) __N__); in builtins.length p.right
N
}
wl_groupBy()      { cat <<'N'
let g = builtins.groupBy (x: toString (x - (x / 2) * 2)) (builtins.genList (i: i) __N__); in builtins.length (builtins.attrValues g)
N
}

# primop | NO_BC_<VAR> | wl-func | sizes (small=stress, large=measure)
PRIMOPS=(
  "map          | MAP            | wl_map          | 1000000 2000000"
  "filter       | FILTER         | wl_filter       | 100000 200000"
  "foldl'       | FOLDL          | wl_foldl        | 1000000 2000000"
  "concatMap    | CONCATMAP      | wl_concatMap    | 50000 100000"
  "sort         | SORT           | wl_sort         | 5000 10000"
  "zipAttrsWith | ZIP_ATTRS_WITH | wl_zipAttrsWith | 50000 100000"
  "all          | ALL            | wl_all          | 1000000 2000000"
  "any          | ANY            | wl_any          | 1000000 2000000"
  "partition    | PARTITION      | wl_partition    | 250000 500000"
  "groupBy      | GROUPBY        | wl_groupBy      | 250000 500000"
)

# measure <wl-func> <size> <NO_BC_var|->  → min user-CPU over RUNS | TIMEOUT
measure() {
  local wlfunc="$1" size="$2" novar="$3" best="" v i rc pid watcher envset=""
  [[ "$novar" != "-" ]] && envset="NIX_V3_NO_BC_${novar}=1"
  "$wlfunc" | sed "s/__N__/$size/g" > "$WL"
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

echo "bc-vs-cpp — host=$(hostname -s)  load=$(uptime|sed 's/.*averages*: //')  RUNS=$RUNS  TIMEOUT=${TIMEOUT_S}s"
echo "nix=$NIX_BIN"; echo
printf '%-14s %-10s   %-13s %-13s   %s\n' "primop" "size" "BYTECODE" "C++" "verdict"
printf '%s\n' "──────────────────────────────────────────────────────────────────────────────────"
for entry in "${PRIMOPS[@]}"; do
  IFS='|' read -r name var wlfunc sizes <<<"$entry"
  name="${name// /}"; var="${var// /}"; wlfunc="${wlfunc// /}"
  for s in $sizes; do
    bc=$(measure "$wlfunc" "$s" "-")
    cpp=$(measure "$wlfunc" "$s" "$var")
    verdict=$(awk -v b="$bc" -v c="$cpp" 'BEGIN{
      if (b=="TIMEOUT" && c!="TIMEOUT" && c+0>0) {print "BC REGRESSION (≥" int('"$TIMEOUT_S"'/c) "× slower)"; exit}
      if (b=="TIMEOUT") {print "both T/O?"; exit}
      if (c+0<=0||b+0<=0) {print "—"; exit}
      r=b/c;
      if (r>=2)   printf "BC REGRESSION (%.0f× slower)", r;
      else if (r<=0.66) printf "bytecode win (%.1f× faster)", c/b;
      else printf "~par (%.2f×)", r;
    }')
    printf '%-14s %-10s   %-13s %-13s   %s\n' "$name" "$s" "$bc" "$cpp" "$verdict"
  done
done
printf '%s\n' "──────────────────────────────────────────────────────────────────────────────────"
echo "BC REGRESSION = the bytecode impl is slower than C++ → candidate to drop/fix (NIX_V3_NO_BC_<NAME>=1 mitigates)."
