#!/usr/bin/env bash
# #2 Part C store — CHEAP PRE-FALSIFIER (measure the ceiling before the spike).
#
# A persistent result store can only save what the WARM CU disk cache doesn't
# already: the top-level RUN that builds the result skeleton (parse+lower are
# already persisted as bytecode).  Its win is also CROSS-process only (the
# in-memory applied cache, now default-on, makes eval#2 free within a process).
#
# So the store's addressable ceiling = T_run (import + run the nixpkgs top-level
# skeleton, no package forcing) as a fraction of T_leaf (import + run + force a
# leaf projection).  If T_run/T_leaf is small, the store adds little over the CU
# cache for leaf projections (which #741 EvalResults already caches) → lean KILL
# or narrow scope, and DON'T build the expensive thunk-serialization spike.  If
# large, headroom exists → build the spike + run the real T_hit/T_eval ≤0.20 gate.
#
# Warm (CU disk cache populated) — the production compile-once model.  darwin-4.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
N="${N:-5}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"

med() { sort -n | awk '{a[NR]=$0} END{ if(!NR){print "NA";exit} m=int((NR+1)/2); if(NR%2)print a[m]; else printf "%.3f",(a[m]+a[m+1])/2 }'; }
cpu_of() { # $1=expr  → median user+sys over N warm runs
  local cs="" i t
  # warm the CU cache
  NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --raw --expr "$1" >/dev/null 2>&1
  for i in $(seq "$N"); do
    t=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=120s /usr/bin/time -l "$NIX" eval --impure --raw --expr "$1" 2>&1 >/dev/null \
        | awk '/ real .* user .* sys/{printf "%.3f",$3+$5}')
    cs+="$t"$'\n'
  done
  echo "$cs" | grep . | med
}

echo "== #2 store pre-falsifier (warm CU cache, N=$N, $(hostname)) =="
# T_run: import + build the top-level nixpkgs attrset skeleton, force ONLY its
# key set (attrNames forces the attrset spine, not the package values).
T_RUN=$(cpu_of 'builtins.length (builtins.attrNames (import <nixpkgs> {}))')
# T_leaf: import + run + force one package's drvPath (a full leaf projection).
T_LEAF=$(cpu_of '(import <nixpkgs> {}).hello.drvPath')
# T_seq: import + force the top attrset to WHNF only (minimal skeleton touch).
T_SEQ=$(cpu_of 'builtins.seq (import <nixpkgs> {}) "ok"')

awk -v run="$T_RUN" -v leaf="$T_LEAF" -v seq="$T_SEQ" 'BEGIN{
  printf "T_seq   (import+WHNF top)      = %ss\n", seq;
  printf "T_run   (import+attrNames spine)= %ss\n", run;
  printf "T_leaf  (import+run+hello.drvPath)= %ss\n", leaf;
  r = (leaf>0)? run/leaf : 0;
  printf "T_run / T_leaf = %.2f  (store ceiling over warm CU cache for leaf projections)\n", r;
  printf "VERDICT: %s\n", (r < 0.20) ? "LEAN-KILL/narrow (store adds little over CU cache + #741 EvalResults; DO NOT build the thunk-serialize spike without a bigger target)" : "HEADROOM (build the thunk-serialize spike + run the T_hit/T_eval <=0.20 gate)";
}'
echo "(NB: the store is CROSS-process only; the in-memory applied cache — now"
echo " default-on — already makes eval#2 free within a process.  The real"
echo " comparand is cold-process-2 with-store vs cold-process-2 CU-cache-warm.)"
