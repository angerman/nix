#!/usr/bin/env bash
# Post-App3 combined-state baseline (commit 42543abc0, 2026-05-31)
#
# All currently-shipped levers active:
#   * fakeClo pool wire-back (40e6abbdb)
#   * Tag::App3 with separate evaluated slot (3d64028e8 + de0348d26)
#   * capWiths singleton interning (25bf129af)
#   * mergeBindings 2-pass + short-circuits (#747 / #748 / #752)
#   * Phase D barriers default-on
#   * Phase 4b LRU + Chain Phase C reverted (all falsified)
#
# Methodology: inline bash; N=10 per config; trim-2 mean + σ.
#
# Workloads:
#   HNE  = (builtins.getFlake "$HNE_PATH").packages.x86_64-linux.hello.drvPath
#   M5   = (builtins.getFlake "$CN_PATH").outputs.packages.aarch64-darwin.cardano-node.name
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
NIX="${NIX:-$ROOT/builddir/src/nix/nix}"
OUT="$ROOT/src/libexpr-v3/bench/baselines/2026-05-31-post-app3-combined"
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"

WORKLOAD="${1:-hne}"
N="${N:-10}"

case "$WORKLOAD" in
    hne)
        EXPR="(builtins.getFlake \"$HNE_PATH\").packages.x86_64-linux.hello.drvPath"
        MAX_HEAP="${MAX_HEAP:-4G}"
        MAX_WALL="${MAX_WALL:-120s}"
        ;;
    m5)
        EXPR="(builtins.getFlake \"$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name"
        MAX_HEAP="${MAX_HEAP:-16G}"
        MAX_WALL="${MAX_WALL:-180s}"
        ;;
    hello)
        EXPR='(import <nixpkgs> {}).hello.drvPath'
        MAX_HEAP="${MAX_HEAP:-2G}"
        MAX_WALL="${MAX_WALL:-30s}"
        ;;
    *)
        echo "Unknown workload: $WORKLOAD (use hne|m5|hello)" >&2
        exit 1
        ;;
esac

OUT_RAW="$OUT/${WORKLOAD}-raw.txt"
OUT_JSON="$OUT/${WORKLOAD}.json"

: > "$OUT_RAW"

echo "=== Post-App3 combined baseline: $WORKLOAD, N=$N ===" | tee -a "$OUT_RAW"
echo "git: $(git rev-parse --short HEAD)" | tee -a "$OUT_RAW"
echo "binary: $NIX" | tee -a "$OUT_RAW"
echo "expr: $EXPR" | tee -a "$OUT_RAW"
echo "" | tee -a "$OUT_RAW"

declare -a peaks=() arenas=() elsewheres=() walls=()
declare -i fail_count=0

for i in $(seq 1 "$N"); do
    t0=$(date +%s.%N)
    # NIX_V3_DIRECT_EVAL + NIX_VM_STATS captures the peak_rss / arena / elsewhere line
    output=$(
        NIX_V3_DIRECT_EVAL=1 \
        NIX_VM_STATS=1 \
        NIX_V3_NO_DISK_CACHE=1 \
        NIX_V3_MAX_HEAP="$MAX_HEAP" \
        NIX_V3_MAX_WALL_TIME="$MAX_WALL" \
        NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
        "$NIX" eval --impure --expr "$EXPR" 2>&1
    )
    rc=$?
    t1=$(date +%s.%N)
    wall=$(echo "$t1 - $t0" | bc -l)
    if (( rc != 0 )); then
        echo "[$i] FAIL rc=$rc wall=${wall}s" | tee -a "$OUT_RAW"
        echo "$output" | tail -10 | tee -a "$OUT_RAW"
        echo "" | tee -a "$OUT_RAW"
        fail_count+=1
        continue
    fi
    # Extract peak_rss / v3_arena / elsewhere from the LAST v3-direct memory line.
    # (Earlier lines are pre-eval sub-invocations with negligible footprint;
    # the final invocation does the full eval and reports the real peak.)
    mem_line=$(echo "$output" | grep "^v3-direct memory:" | tail -1 || echo "")
    if [[ -z "$mem_line" ]]; then
        echo "[$i] FAIL: no memory line found" | tee -a "$OUT_RAW"
        fail_count+=1
        continue
    fi
    peak=$(echo "$mem_line" | sed -nE 's/.*peak_rss=([0-9.]+)MB.*/\1/p')
    arena=$(echo "$mem_line" | sed -nE 's/.*v3_arena=([0-9.]+)MB.*/\1/p')
    elsewhere=$(echo "$mem_line" | sed -nE 's/.*elsewhere=([0-9.]+)MB.*/\1/p')
    drvPath=$(echo "$output" | grep -m1 "/nix/store/" || echo "no-drv")
    peaks+=("$peak")
    arenas+=("$arena")
    elsewheres+=("$elsewhere")
    walls+=("$wall")
    printf "[%d] wall=%.2fs peak=%.1fMB arena=%.1fMB elsewhere=%.1fMB drv=%s\n" \
        "$i" "$wall" "$peak" "$arena" "$elsewhere" "$drvPath" | tee -a "$OUT_RAW"
done

if (( fail_count > 0 )); then
    echo "" | tee -a "$OUT_RAW"
    echo "WARN: $fail_count runs failed; stats based on ${#peaks[@]} successes." | tee -a "$OUT_RAW"
fi

# Trim-2 mean + σ (drop min+max from raw[N]).  Python helper.
python3 - <<EOF | tee -a "$OUT_RAW"
import json, statistics
peaks = [${peaks[*]:+$(IFS=,; echo "${peaks[*]}")}]
arenas = [${arenas[*]:+$(IFS=,; echo "${arenas[*]}")}]
elsewheres = [${elsewheres[*]:+$(IFS=,; echo "${elsewheres[*]}")}]
walls = [${walls[*]:+$(IFS=,; echo "${walls[*]}")}]

def trim2(xs):
    if len(xs) <= 2:
        return xs[:]
    s = sorted(xs)
    return s[1:-1]

def stats(xs):
    if not xs:
        return {"n_raw": 0, "n_used": 0, "mean": None, "sigma": None, "min": None, "max": None, "raw": []}
    used = trim2(xs)
    return {
        "n_raw": len(xs),
        "n_used": len(used),
        "mean": round(statistics.mean(used), 2),
        "sigma": round(statistics.stdev(used), 2) if len(used) > 1 else 0.0,
        "min": min(xs),
        "max": max(xs),
        "raw": xs,
    }

result = {
    "schema_version": 1,
    "date": "2026-05-31",
    "workload": "$WORKLOAD".upper(),
    "binary": "$NIX",
    "git": "42543abc0",
    "n_requested": $N,
    "n_succeeded": len(peaks),
    "method": "Inline bash measurement; N=$N back-to-back; trim-2 mean ± σ (drop min+max).",
    "lever_state": {
        "fakeclo_wire_back": True,
        "tag_app3": True,
        "capwiths_intern": True,
        "phase_4b_lru": False,
        "chain_phase_c": False,
    },
    "stats": {
        "peak_rss_mb": stats(peaks),
        "v3_arena_mb": stats(arenas),
        "elsewhere_mb": stats(elsewheres),
        "wall_s": stats(walls),
    },
}
print(json.dumps(result, indent=2))
with open("$OUT_JSON", "w") as f:
    json.dump(result, f, indent=2)
EOF
echo "" | tee -a "$OUT_RAW"
echo "Saved: $OUT_JSON" | tee -a "$OUT_RAW"
