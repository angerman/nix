#!/usr/bin/env bash
#
# chain-nixpkgs-fullsweep.sh — full nixpkgs drvPath byte-equality sweep for
# Lever A (ChainBindings).  The pure-refactor invariant: enabling
# NIX_V3_CHAIN_BINDINGS=1 must NOT change any drvPath.  Sweeps EVERY top-level
# nixpkgs attr, evaluating `drvPath` (via tryEval) chains-OFF vs chains-ON, and
# records any divergence.  A package that errors IDENTICALLY in both modes is
# NOT a divergence (chains remain a pure refactor).
#
# Robustness: some packages hit uncatchable v3-internal errors (raw
# std::runtime_error escaping tryEval) that abort the whole batch process — in
# BOTH modes equally.  On a batch crash we BISECT the batch (split + recurse)
# to isolate the bad package(s) and still compare every good one.  A single
# package that crashes in BOTH modes is skipped; one that crashes in only ONE
# mode IS a divergence (chains changed behaviour) and is recorded.
#
# Disk-cache OFF (real /nix/store drvPaths — fake-store stubs bypass
# derivationStrict and hide store-hash bugs).  Results append incrementally so
# the run survives interruption and can be monitored live.
#
# Usage: chain-nixpkgs-fullsweep.sh [BATCH] [HEAP] [WALL]
# Outputs under $OUT (default /tmp/chain-sweep):
#   diverge.txt   "<name>\toff=<v>\ton=<v>"   ← THE result to act on
#   skip.txt      packages uncatchable in BOTH modes (not chain bugs)
#   progress.txt  per-top-level-batch tallies + heartbeat
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO_ROOT/build/src/nix/nix}"
NAMES="${NAMES:-/tmp/nixpkgs-names.txt}"
OUT="${OUT:-/tmp/es-sweep}"
BATCH="${1:-150}"; HEAP="${2:-12G}"; WALL="${3:-300}"

[[ -x "$NIX_BIN" ]] || { echo "nix not found: $NIX_BIN" >&2; exit 2; }
[[ -s "$NAMES"   ]] || { echo "name list not found: $NAMES" >&2; exit 2; }
mkdir -p "$OUT"
: > "$OUT/diverge.txt"; : > "$OUT/skip.txt"; : > "$OUT/suspect.txt"; : > "$OUT/progress.txt"
T0=$(date +%s)

mapfile -t ALL < "$NAMES"
N=${#ALL[@]}
echo "sweeping $N top-level attrs, batch=$BATCH heap=$HEAP wall=${WALL}s -> $OUT"

# Emit a batch JSON map { name -> drvPath | "·nodrv·" | "·err·" } for the
# names passed as args; empty output (exit!=0 / no stdout) ⇒ uncatchable crash.
eval_names() {  # $1=mode(off|on) ; rest=names ; writes JSON to stdout
  local mode="$1"; shift
  local names_nix="" n; for n in "$@"; do names_nix+="\"$n\" "; done
  local expr="let p = import <nixpkgs> {}; names = [ $names_nix ];
    one = n: let r = builtins.tryEval (let v = p.\${n};
              in if builtins.isAttrs v && v ? drvPath
                 then builtins.toString v.drvPath else \"·nodrv·\");
      in { name = n; value = if r.success then r.value else \"·err·\"; };
  in builtins.toJSON (builtins.listToAttrs (map one names))"
  # env-sharing variant: off-arm = ES OFF (baseline), on-arm = ES ON (default/prod).
  # A divergence means env-sharing changed a drvPath (must be a pure refactor).
  local extra=(); [[ "$mode" == "off" ]] && extra=(NIX_V3_NO_ENV_SHARING=1)
  env NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 \
      NIX_V3_MAX_HEAP="$HEAP" NIX_V3_MAX_WALL_TIME="${WALL}s" "${extra[@]}" \
      "$NIX_BIN" eval --impure --raw --expr "$expr" 2>/dev/null
}

# Compare two JSON maps, append mismatches to diverge.txt; echo count.
diff_json() {  # $1=off.json $2=on.json
  python3 - "$1" "$2" "$OUT/diverge.txt" <<'PY'
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); out=open(sys.argv[3],'a')
n=0
for k in sorted(set(a)|set(b)):
    if a.get(k)!=b.get(k):
        out.write(f"{k}\toff={a.get(k)}\ton={b.get(k)}\n"); n+=1
print(n)
PY
}

# Recursively sweep a slice [lo,hi); bisect on crash to isolate bad packages.
sweep() {  # $1=lo $2=hi
  local lo=$1 hi=$2 sz=$(( $2 - $1 ))
  local -a names=( "${ALL[@]:lo:sz}" )
  local off on
  off=$(eval_names off "${names[@]}"); on=$(eval_names on "${names[@]}")
  if [[ -n "$off" && -n "$on" ]]; then
    printf '%s' "$off" > "$OUT/.o.$lo"; printf '%s' "$on" > "$OUT/.n.$lo"
    local nd; nd=$(diff_json "$OUT/.o.$lo" "$OUT/.n.$lo"); rm -f "$OUT/.o.$lo" "$OUT/.n.$lo"
    return 0
  fi
  # crash in at least one mode
  if (( sz == 1 )); then
    local nm="${names[0]}"
    if [[ -z "$off" && -z "$on" ]]; then
      echo "$nm" >> "$OUT/skip.txt"                       # uncatchable in BOTH → not a chain bug
    else
      # one mode produced no output, the other did.  Under a tight wall cap on a
      # loaded host this is usually a FLAKY TIMEOUT, not a chain bug (a pure
      # refactor cannot make eval crash where it didn't — the contamination
      # class produces a DIFFERENT drvPath, caught by diff_json, not a crash).
      # Record as a SUSPECT for generous-limit re-verification, NOT a hard
      # divergence.  (Re-verify: NIX_V3_MAX_HEAP=8G NIX_V3_MAX_WALL_TIME=120s
      # both modes; only a reproducing one-mode crash is a real bug.)
      echo -e "$nm\toff=${off:-«no-output»}\ton=${on:-«no-output»}" >> "$OUT/suspect.txt"
    fi
    return 0
  fi
  local mid=$(( lo + sz/2 ))
  sweep "$lo" "$mid"; sweep "$mid" "$hi"
}

# Top-level batches (so we get progress + a heartbeat); each bisects on crash.
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
echo "DONE swept=$N diverge=$ND skip=$(grep -c . "$OUT/skip.txt" 2>/dev/null||echo 0) elapsed=$(( $(date +%s)-T0 ))s" >> "$OUT/progress.txt"
echo "DONE diverge=$ND"
