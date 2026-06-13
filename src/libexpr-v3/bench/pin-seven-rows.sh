#!/usr/bin/env bash
# PLAN_BEAT_TW_V2 QG-4 — pin the 7 gate rows to a checked-in TSV.
#
# Emits one row per workload: v3 CPU (min user over RUNS), TW CPU, ratio,
# and v3 arena MB (NIX_VM_STATS v3_arena — the deterministic RSS proxy;
# laptop maxRSS is noise per QG-2 so we pin arena, not maxRSS).  All v3
# runs assert byte-identity vs the TW arm (DIVERGENT rows are flagged, not
# silently pinned).
#
#   pin-seven-rows.sh            # print the TSV to stdout
#   pin-seven-rows.sh > baselines/seven-rows.tsv   # re-pin
#
# `ratchet-check.sh` reads the committed TSV and fails if a fresh run
# regresses any row >3% CPU or >5% arena without a justification line.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
RUNS="${RUNS:-3}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"

# workload | needs --impure | expr
FIB='let f = n: if n < 2 then n else f (n - 1) + f (n - 2); in f 33'
FOLD="builtins.foldl' (a: o: a + builtins.length (builtins.attrNames o)) 0 (builtins.genList (i: builtins.listToAttrs (builtins.genList (j: { name = toString j; value = i + j; }) 500)) 2000)"
ATTRNAMES='builtins.length (builtins.attrNames (import <nixpkgs> {}))'
HELLO='(import <nixpkgs> {}).hello.drvPath'
GIT='(import <nixpkgs> {}).git.drvPath'
FIREFOX='(import <nixpkgs> {}).firefox.drvPath'
M5="(builtins.getFlake \"path:$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name"

# measure_arm <result-var> <cpu-var> <arena-var> <env> <impure> <expr>
# echoes "cpu arena result"
run_arm() {
  local envp="$1" impure="$2" expr="$3" best="" arena="" res="" i u a
  local -a IMP=(); [[ "$impure" == 1 ]] && IMP=(--impure)
  for ((i=0;i<RUNS;i++)); do
    res="$(env $envp NIX_VM_STATS=1 NIX_V3_MAX_WALL_TIME=240s NIX_V3_MAX_HEAP=10G \
        /usr/bin/time -l "$NIX" eval "${IMP[@]}" --expr "$expr" 2>/tmp/pin.err)"
    u="$(grep -oE '[0-9]+\.[0-9]+ user' /tmp/pin.err | grep -oE '^[0-9.]+' | head -1)"
    a="$(grep -oE 'v3_arena=[0-9.]+MB' /tmp/pin.err | grep -oE '[0-9.]+' | tail -1)"
    [[ -n "$u" ]] && { [[ -z "$best" ]] && best="$u" || best="$(awk "BEGIN{print ($u<$best)?$u:$best}")"; }
    [[ -n "$a" ]] && arena="$a"
  done
  echo "${best:-NA} ${arena:-NA} ${res}"
}

printf "# workload\tv3_cpu_s\ttw_cpu_s\tratio\tv3_arena_mb\tidentical\n"
emit() { # name impure expr
  local name="$1" impure="$2" expr="$3"
  read -r v3cpu v3arena v3res < <(run_arm "NIX_V3_DIRECT_EVAL=1" "$impure" "$expr")
  read -r twcpu twarena twres < <(run_arm "" "$impure" "$expr")
  local ratio ident
  ratio="$(awk "BEGIN{ if (\"$twcpu\"==\"NA\"||\"$v3cpu\"==\"NA\"||$twcpu==0) print \"NA\"; else printf \"%.2f\", $v3cpu/$twcpu }")"
  [[ "$v3res" == "$twres" ]] && ident="yes" || ident="DIVERGENT"
  printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$name" "$v3cpu" "$twcpu" "$ratio" "$v3arena" "$ident"
}
emit attrNames 1 "$ATTRNAMES"
emit fib       0 "$FIB"
emit foldl     0 "$FOLD"
emit hello     1 "$HELLO"
emit git       1 "$GIT"
emit firefox   1 "$FIREFOX"
emit M5        1 "$M5"
