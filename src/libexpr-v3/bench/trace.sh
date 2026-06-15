#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════════════
#  trace.sh — regenerate the TW-vs-v3 time-series picture on demand.
#
#  Two artifacts per workload (the question "do we graph live-bytes + CPU over
#  time, TW vs v3" — see lode/WHY_V3_USES_MORE_MEMORY_2026-06-14.md §infra):
#
#   1. <out>/<name>.svg        perf-trace.py — TW vs v3 OVERLAY of CPU% + RSS +
#                              Boehm-heap over wall-time (psutil sampler).
#   2. <out>/<name>-mem.svg    plot-v3-memory.py — v3-only, from the in-process
#                              periodic live-trace: the DEAD-BAND panel (arena
#                              mapped − live = unreclaimed dead), the per-Tag
#                              live stack, L(t), and RSS/CPU% from heap-trace.
#
#  (1) is cross-engine but RSS-only (RSS conflates live+dead+reservation).
#  (2) is the v3 live-bytes decomposition TW structurally cannot produce
#  (conservative Boehm has no precise mark) — so the contrast is, by necessity:
#  cross-engine RSS/CPU over time (1) + v3 live/dead breakdown (2).
#
#  Usage:
#    make trace WL=firefox            # shorthand workloads: hello|git|firefox|m5
#    make trace WL='(import <nixpkgs> {}).hello.outPath'   # or a raw expr
#    RUNS=5 K=8 make trace WL=hello   # more runs / finer live-trace cadence (MB)
#
#  Knobs (env):  RUNS=3  K=16(MB)  NIX_BIN=…  PYTHON=…  OUT=samples/trace
#                V3_WALL=300s  V3_HEAP=8G  SMOKE=1(fast: 1 run, skip if no deps)
#
#  Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#  Input Output Group.  SPDX-License-Identifier: Apache-2.0
# ════════════════════════════════════════════════════════════════════════════
set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SELF_DIR/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"
RUNS="${RUNS:-3}"
K="${K:-16}"                      # live-trace cadence in MB of arena alloc
OUT="${OUT:-$SELF_DIR/samples/trace}"
V3_WALL="${V3_WALL:-300s}"
V3_HEAP="${V3_HEAP:-8G}"
SMOKE="${SMOKE:-0}"
WL="${WL:-}"

# SMOKE supplies its own synthetic workload below, so WL is optional then.
[[ -n "$WL" || "$SMOKE" == 1 ]] || { echo "trace.sh: set WL=<hello|git|firefox|m5|nix-expr>" >&2; exit 2; }
[[ -x "$NIX_BIN" ]] || { echo "trace.sh: no nix binary at $NIX_BIN (set NIX_BIN=)" >&2; exit 2; }
: "${WL:=smoke}"   # placeholder so the case below has a value; SMOKE overrides

# --- resolve workload shorthand → (name, expr) ------------------------------
case "$WL" in
  hello)   name="hello-drvpath";   expr='(import <nixpkgs> {}).hello.drvPath' ;;
  git)     name="git-drvpath";     expr='(import <nixpkgs> {}).git.drvPath' ;;
  firefox) name="firefox-drvpath"; expr='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
  m5)      CN="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
           [[ -d "$CN" ]] || { echo "trace.sh: cardano-node checkout missing at $CN; SKIP m5" >&2; exit 0; }
           name="m5-cardano-node"
           expr="(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.cardano-node.name" ;;
  *)       name="custom"; expr="$WL" ;;
esac

# SMOKE mode: a fast, dependency-tolerant freshness check for the ratchet.
# 1 run, tiny synthetic workload (overrides WL), skip cleanly if the plotting
# deps are absent (so CI never hard-fails on an optional matplotlib/psutil).
if [[ "$SMOKE" == 1 ]]; then
  RUNS=1
  K=4                                    # fine cadence so the short eval samples
  name="smoke"
  # Allocate a ~32 MB live ListVec at depth>0 (genList+length run as primops),
  # so the smoke actually exercises EVERY panel — incl. the de-gated live
  # sampler + the dead-band — not just the perf-trace overlay.  ~1 s.
  expr='builtins.length (builtins.genList (i: i) 4000000)'
fi

# --- pick a python with psutil + matplotlib --------------------------------
# `python3.withPackages` bakes the deps into a standalone wrapper binary
# (the shebang use-case), so the resolved absolute path is callable directly
# with args — no need to stay inside the nix-shell env.
PY="${PYTHON:-python3}"
have_deps() { "$1" -c 'import psutil, matplotlib' >/dev/null 2>&1; }
PKGEXPR='python3.withPackages(ps: with ps; [psutil matplotlib])'
if ! have_deps "$PY" && command -v nix-shell >/dev/null 2>&1; then
  echo "trace.sh: resolving a python with psutil+matplotlib via nix-shell…" >&2
  NIXPY="$(nix-shell -p "$PKGEXPR" --run 'command -v python3' 2>/dev/null)"
  [[ -n "$NIXPY" ]] && have_deps "$NIXPY" && PY="$NIXPY"
fi
if ! have_deps "$PY"; then
  msg="trace.sh: no python with psutil+matplotlib. Set PYTHON=<path>, or:
    nix-shell -p 'python3.withPackages(ps: with ps; [psutil matplotlib])'"
  if [[ "$SMOKE" == 1 ]]; then echo "$msg" >&2; echo "trace.sh: SMOKE skip (no plot deps)"; exit 0; fi
  echo "$msg" >&2; exit 3
fi

mkdir -p "$OUT"
echo "trace.sh: workload=$name runs=$RUNS K=${K}MB out=$OUT" >&2

# --- artifact 1: cross-engine CPU+RSS overlay (perf-trace.py) ---------------
$PY "$SELF_DIR/perf-trace.py" \
    --workload "$expr" --mode tw,v3-native --runs "$RUNS" \
    --nix "$NIX_BIN" --out "$OUT/$name" --wall-time "${V3_WALL%s}" \
  || { echo "trace.sh: perf-trace.py failed" >&2; exit 1; }

# --- artifact 2: v3 live / dead-band figure (one dedicated v3 run) ----------
csv="$OUT/$name-live.csv"; ht="$OUT/$name.heaptrace.log"
rm -f "$csv"
NIX_V3_DIRECT_EVAL=1 \
NIX_V3_LIVE_TRACE_PERIODIC="$K" NIX_V3_LIVE_TRACE_PERIODIC_OUT="$csv" \
NIX_V3_HEAP_TRACE=1 \
NIX_V3_MAX_WALL_TIME="$V3_WALL" NIX_V3_MAX_HEAP="$V3_HEAP" \
  "$NIX_BIN" eval --impure --expr "$expr" >/dev/null 2>"$ht" \
  || { echo "trace.sh: v3 mem-capture eval failed (see $ht)" >&2; exit 1; }

rows=$( [[ -f "$csv" ]] && { wc -l < "$csv" | tr -d ' '; } || echo 0 )
# --live-periodic only when the sampler actually produced a CSV (deep evals
# below the K-MB threshold write none — then the mem figure is heap-trace
# only: RSS + CPU% over time, still useful, no dead-band panel).
plotargs=( --heap-trace "$ht" --x wall
           --out "$OUT/$name-mem" --title "v3 memory-over-time — $name" )
[[ -s "$csv" ]] && plotargs=( --live-periodic "$csv" "${plotargs[@]}" )
$PY "$SELF_DIR/plot-v3-memory.py" "${plotargs[@]}" \
  || { echo "trace.sh: plot-v3-memory.py failed" >&2; exit 1; }

echo "trace.sh: wrote" >&2
echo "  $OUT/$name.svg            (TW vs v3: CPU% + RSS + Boehm-heap over time)" >&2
echo "  $OUT/$name-mem.svg        (v3 dead-band + per-Tag live + L(t) + RSS/CPU; live CSV rows=$((rows>0?rows-1:0)))" >&2
[[ "${rows:-0}" -le 2 ]] && echo "  NOTE: only $((rows>0?rows-1:0)) live sample(s) — increase the run or lower K=<MB>." >&2
exit 0
