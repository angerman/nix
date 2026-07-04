#!/usr/bin/env bash
#
# v3-vs-tw-table.sh — current v3-VM-vs-tree-walker picture across workloads.
#
# Same discipline as v3-vs-tw-gate.sh (ENGAGED: v3 emits v3-direct stats;
# IDENTICAL: byte-equal result; same binary both arms; load-immune metrics)
# but as a COMPACT TABLE across a regime spread. user-CPU (min of RUNS,
# idle-host) + peak-RSS (deterministic). drvPath rows are DIVERGENT (v3
# /v3-fake-store/ path) → memory ratio valid, correctness not verified.
#
# Usage:  [NIX_BIN=…] [RUNS=3] ./v3-vs-tw-table.sh
set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO/build/src/nix/nix}"
RUNS="${RUNS:-3}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
EXTRA="--extra-experimental-features"; FEAT="nix-command flakes"

# workload | regime | nix-expr
WORKLOADS=(
  "fib33        | compute/dispatch | let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 33"
  "map.foldl    | list iterate     | builtins.foldl' (a: x: a + x) 0 (builtins.map (x: x + 1) (builtins.genList (i: i) 2000000))"
  "filter       | list (was blown) | builtins.length (builtins.filter (x: x > 1) (builtins.genList (i: i) 2000000))"
  "sort         | list (was blown) | builtins.length (builtins.sort (a: b: a < b) (builtins.genList (i: 200000 - i) 200000))"
  "listToAttrs  | attrs build      | let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) 200000); in builtins.length (builtins.attrNames s)"
  "getAttr      | attrs lookup     | let s = builtins.listToAttrs (builtins.genList (i: { name = toString i; value = i; }) 50000); in builtins.foldl' (a: k: a + (builtins.getAttr k s)) 0 (builtins.attrNames s)"
  "concatSep    | string build     | builtins.stringLength (builtins.concatStringsSep \",\" (builtins.genList (i: toString i) 1000000))"
  "hello.name   | nixpkgs eval     | (import <nixpkgs> {}).hello.name"
  "hello.drvPath| derivation-bound | (import <nixpkgs> {}).hello.drvPath"
)

# arm <expr> <DIRECT|TW>  → "user_cpu rss_bytes engaged result"  (rss/result via tmp)
arm() {
  local expr="$1" mode="$2" best="" v r i out env=""
  # v3 limits = safety net for the heavy arm (drvPath); generous so they never
  # trigger on a completing workload — a trigger surfaces as ERROR, not a hang.
  [[ "$mode" == DIRECT ]] && env="NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME=180s NIX_V3_MAX_HEAP=6G NIX_V3_MAX_CPU_TIME=180s"
  for ((i=0;i<RUNS;i++)); do
    /usr/bin/time -l env $env "$NIX_BIN" eval $EXTRA "$FEAT" --impure --expr "$expr" \
        >"$TMP/out.$mode" 2>"$TMP/err.$mode"
    v=$(grep -oE '[0-9.]+ user' "$TMP/err.$mode" | grep -oE '^[0-9.]+' | head -1)
    r=$(grep 'maximum resident set size' "$TMP/err.$mode" | grep -oE '[0-9]+' | head -1)
    [[ -z "$best" ]] && best="$v" || best=$(awk "BEGIN{print ($v<$best)?$v:$best}")
    [[ -z "${maxr:-}" || "$r" -gt "${maxr:-0}" ]] && maxr="$r"
  done
  local eng=no; [[ "$mode" == DIRECT ]] && { grep -q v3-direct "$TMP/err.$mode" && eng=yes; }
  echo "$best ${maxr:-0} $eng"; unset maxr
}

mb(){ awk "BEGIN{printf \"%.0f\", $1/1048576}"; }
ratio(){ awk "BEGIN{ if($2+0==0) print \"NA\"; else printf \"%.2f\", $1/$2 }"; }

echo "v3 vs tree-walker — host=$(hostname -s)  load=$(uptime|sed 's/.*averages*: //')  nix=$NIX_BIN  RUNS=$RUNS"
echo "TW=$("$NIX_BIN" --version 2>/dev/null|head -1)  (same binary both arms; v3 = NIX_V3_DIRECT_EVAL=1)"
echo
printf '%-13s %-17s  %8s %8s %6s   %8s %8s %6s   %s\n' workload regime "TW cpu" "v3 cpu" "v3/TW" "TW rss" "v3 rss" "v3/TW" "result"
printf '%s\n' "──────────────────────────────────────────────────────────────────────────────────────────────────────"
for e in "${WORKLOADS[@]}"; do
  IFS='|' read -r name regime expr <<<"$e"; name="${name//  /}"; name="${name% }"; regime="$(echo $regime)"; expr="${expr# }"
  read tcpu trss _   < <(arm "$expr" TW)
  read vcpu vrss veng< <(arm "$expr" DIRECT)
  ot=$(cat "$TMP/out.TW"); ov=$(cat "$TMP/out.DIRECT")
  if [[ -z "$ot" || -z "$ov" ]]; then res="ERROR (eval failed)"
  elif [[ "$veng" != yes ]]; then res="v3 NOT ENGAGED"
  elif [[ "$ot" == "$ov" ]]; then res="identical ✓"
  else res="DIVERGENT (fake-store)"; fi
  printf '%-13s %-17s  %7ss %7ss %5sx   %7sM %7sM %5sx   %s\n' \
    "$name" "$regime" "$tcpu" "$vcpu" "$(ratio "$vcpu" "$tcpu")" \
    "$(mb "$trss")" "$(mb "$vrss")" "$(ratio "$vrss" "$trss")" "$res"
done
printf '%s\n' "──────────────────────────────────────────────────────────────────────────────────────────────────────"
echo "cpu/rss ratios >1 = v3 costs more. user-CPU is load-insensitive; peak-RSS deterministic."
