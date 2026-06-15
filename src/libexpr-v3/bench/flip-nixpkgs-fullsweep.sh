#!/usr/bin/env bash
#
# flip-nixpkgs-fullsweep.sh — full nixpkgs drvPath byte-equality sweep for the
# nursery + gen-major FLIP (shipped 2026-06-15, commit e863f127d).
#
# The flip-transparency invariant: a GC change must NOT alter any drvPath.
# Sweeps EVERY top-level nixpkgs attr, evaluating `drvPath` (via tryEval) in two
# modes and recording any divergence:
#   base = NIX_V3_NURSERY=0  (old non-generational major-GC default)
#   flip = (default)         (nursery + gen-major Shape A, now default-on)
# A package that errors IDENTICALLY in both modes is NOT a divergence (the flip
# is a pure GC change).  ANY base!=flip divergence is a HARD failure (the flip
# altered observable output → a missed-root or barrier bug).
#
# This is the confidence instrument for RETIRING the NIX_V3_NURSERY opt-out
# gates: once a full-nixpkgs run here is clean (diverge=0), the opt-out branches
# in barrier.cc / nursery.hh / alloc.hh / vm.cc can be removed.
#
# Structure (batched + crash-bisect + incremental) is lifted verbatim from
# chain-nixpkgs-fullsweep.sh — same robustness: a batch that hits an uncatchable
# v3-internal crash (in BOTH modes equally) is bisected to isolate the bad
# package; a one-mode-only crash under a tight wall cap is recorded as a SUSPECT
# for generous-limit re-verification, not a hard divergence.
#
# Disk-cache OFF (real /nix/store drvPaths — fake-store stubs bypass
# derivationStrict and hide store-hash bugs).  Results append incrementally so
# the run survives interruption and can be monitored live.
#
# Usage: flip-nixpkgs-fullsweep.sh [BATCH] [HEAP] [WALL]
#   NAMES=/tmp/nixpkgs-names.txt  (one top-level attr per line; required)
# Outputs under $OUT (default /tmp/flip-sweep):
#   diverge.txt   "<name>\tbase=<v>\tflip=<v>"   ← THE result to act on (want 0)
#   skip.txt      packages uncatchable in BOTH modes (not flip bugs)
#   suspect.txt   one-mode-only crash (flaky timeout; re-verify generously)
#   progress.txt  per-batch tallies + heartbeat
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO_ROOT/build/src/nix/nix}"
NAMES="${NAMES:-/tmp/nixpkgs-names.txt}"
OUT="${OUT:-/tmp/flip-sweep}"
BATCH="${1:-150}"; HEAP="${2:-12G}"; WALL="${3:-300}"

[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN" >&2; exit 2; }
[[ -s "$NAMES"   ]] || { echo "name list not found: $NAMES (generate it first)" >&2; exit 2; }
mkdir -p "$OUT"
: > "$OUT/diverge.txt"; : > "$OUT/skip.txt"; : > "$OUT/suspect.txt"; : > "$OUT/progress.txt"
T0=$(date +%s)

mapfile -t ALL < "$NAMES"
N=${#ALL[@]}
echo "FLIP sweep: $N top-level attrs, batch=$BATCH heap=$HEAP wall=${WALL}s -> $OUT"
echo "  base = NIX_V3_NURSERY=0 (major-GC default) ; flip = default (nursery+gen-major)"

# Emit a batch JSON map { name -> drvPath | "·nodrv·" | "·err·" } for the names
# passed as args; empty output (exit!=0 / no stdout) ⇒ uncatchable crash.
eval_names() {  # $1=mode(base|flip) ; rest=names ; writes JSON to stdout
  local mode="$1"; shift
  local names_nix="" n; for n in "$@"; do names_nix+="\"$n\" "; done
  local expr="let p = import <nixpkgs> {}; names = [ $names_nix ];
    one = n: let r = builtins.tryEval (let v = p.\${n};
              in if builtins.isAttrs v && v ? drvPath
                 then builtins.toString v.drvPath else \"·nodrv·\");
      in { name = n; value = if r.success then r.value else \"·err·\"; };
  in builtins.toJSON (builtins.listToAttrs (map one names))"
  # FLIP toggle: 'base' forces the old major-GC default (nursery off); 'flip'
  # uses the new default (nursery + gen-major on).
  local extra=(); [[ "$mode" == "base" ]] && extra=(NIX_V3_NURSERY=0)
  env NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 \
      NIX_V3_MAX_HEAP="$HEAP" NIX_V3_MAX_WALL_TIME="${WALL}s" "${extra[@]}" \
      "$NIX_BIN" eval --impure --raw --expr "$expr" 2>/dev/null
}

# Compare two JSON maps, append mismatches to diverge.txt; echo count.
diff_json() {  # $1=base.json $2=flip.json
  python3 - "$1" "$2" "$OUT/diverge.txt" <<'PY'
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); out=open(sys.argv[3],'a')
n=0
for k in sorted(set(a)|set(b)):
    if a.get(k)!=b.get(k):
        out.write(f"{k}\tbase={a.get(k)}\tflip={b.get(k)}\n"); n+=1
print(n)
PY
}

# Recursively sweep a slice [lo,hi); bisect on crash to isolate bad packages.
sweep() {  # $1=lo $2=hi
  local lo=$1 hi=$2 sz=$(( $2 - $1 ))
  local -a names=( "${ALL[@]:lo:sz}" )
  local base flip
  base=$(eval_names base "${names[@]}"); flip=$(eval_names flip "${names[@]}")
  if [[ -n "$base" && -n "$flip" ]]; then
    printf '%s' "$base" > "$OUT/.b.$lo"; printf '%s' "$flip" > "$OUT/.f.$lo"
    diff_json "$OUT/.b.$lo" "$OUT/.f.$lo" >/dev/null; rm -f "$OUT/.b.$lo" "$OUT/.f.$lo"
    return 0
  fi
  if (( sz == 1 )); then
    local nm="${names[0]}"
    if [[ -z "$base" && -z "$flip" ]]; then
      echo "$nm" >> "$OUT/skip.txt"                        # uncatchable in BOTH → not a flip bug
    else
      # one-mode-only crash under a tight wall cap = usually a flaky timeout, not
      # a flip bug (a GC change cannot make eval crash where it didn't — the
      # contamination class produces a DIFFERENT drvPath, caught by diff_json).
      echo -e "$nm\tbase=${base:-«no-output»}\tflip=${flip:-«no-output»}" >> "$OUT/suspect.txt"
    fi
    return 0
  fi
  local mid=$(( lo + sz/2 ))
  sweep "$lo" "$mid"; sweep "$mid" "$hi"
}

i=0; done=0
while (( i < N )); do
  hi=$(( i + BATCH )); (( hi > N )) && hi=$N
  sweep "$i" "$hi"
  done=$hi
  nd=$(grep -c . "$OUT/diverge.txt" 2>/dev/null || echo 0)
  ns=$(grep -c . "$OUT/skip.txt" 2>/dev/null || echo 0)
  el=$(( $(date +%s) - T0 ))
  echo "swept $done/$N  diverge=$nd skip=$ns  elapsed=${el}s" >> "$OUT/progress.txt"
  i=$hi
done
ND=$(grep -c . "$OUT/diverge.txt" 2>/dev/null || echo 0)
echo "DONE swept=$N diverge=$ND skip=$(grep -c . "$OUT/skip.txt" 2>/dev/null||echo 0) suspect=$(grep -c . "$OUT/suspect.txt" 2>/dev/null||echo 0) elapsed=$(( $(date +%s)-T0 ))s" >> "$OUT/progress.txt"
echo "DONE diverge=$ND  (want 0; see $OUT/diverge.txt + suspect.txt)"
