#!/usr/bin/env bash
# WS-2 V3 (2026-07-13) — IFD wall-time decomposition.
#
# Answers DECISION INPUT #1 for the CI-throughput plan: "on a WARM
# (CI-shaped) eval, what fraction of the wall is IFD-blocked?"  If IFD is a
# large fraction of warm wall on the IFD-heavy workloads, the fiber-overlap
# track (WS-4) is worth funding; if it is small, memory/COW (WS-5/6) rank
# higher.
#
# Method (per workload):
#   1. WARM-UP run — populate the store (build all IFDs) + CU disk cache.
#   2. MEASURED warm run — IFDs now already-valid, disk cache hot; this is the
#      CI steady state.  Read the wall split from two independent sources:
#        (a) v3's default-on end-of-eval summary (WS-2 V2): "blocked Xs in
#            realise (Y% of Zs eval wall)" — in-process, always emitted.
#        (b) profile-import-from-derivation + NIX_SHOW_STATS: nrIFDs,
#            nrIFDsCached, totalIFDTimeUs — the tree-walker's authoritative
#            per-IFD build/substitute timer (cross-check).
#   3. Report wall = compute + IFD-blocked; store-RPC on a warm run is the
#      already-valid check cost folded into "blocked" (no build happens warm).
#
# Cold builds dominate the FIRST run (that is expected and out of scope — CI
# reruns the same eval); we measure the second, warm run.
#
# Usage:
#   bench/ifd-decomp.sh                 # firefox M5 HNE (skips missing checkouts)
#   bench/ifd-decomp.sh firefox         # one workload
#   COMMIT=<hash> bench/ifd-decomp.sh --git-note   # stamp + attach a git note
#
# CPU/RSS gate rules (CLAUDE.md) do NOT apply here — this reports a wall
# RATIO (IFD-blocked / eval-wall), which is far less host-sensitive than
# absolute CPU; still, prefer the quiet host (darwin-4) for the authoritative
# number and git-note it.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"

CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"

GIT_NOTE=0
WORKLOADS=()
for a in "$@"; do
  case "$a" in
    --git-note) GIT_NOTE=1 ;;
    *) WORKLOADS+=("$a") ;;
  esac
done
[ ${#WORKLOADS[@]} -eq 0 ] && WORKLOADS=(firefox M5 HNE)

GITHASH="${COMMIT:-$(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo unknown)}"

# workload -> env + expr (mirrors profile-at-scale.sh so numbers are comparable).
V3="NIX_V3_DIRECT_EVAL=1"
V3F="$V3 NIX_V3_NO_NATIVE_CALL_FLAKE=1"
wl_env()  { case "$1" in firefox) echo "$V3";; M5|HNE) echo "$V3F";; esac; }
wl_expr() {
  case "$1" in
    firefox) echo '(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
    M5)  echo "(builtins.getFlake \"path:$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name" ;;
    HNE) echo "(builtins.getFlake \"path:$HNE_PATH\").packages.aarch64-darwin.hello.drvPath" ;;
  esac
}
wl_available() {
  case "$1" in
    firefox) return 0 ;;                              # <nixpkgs> pin always present
    M5)  [ -e "$CN_PATH/flake.nix" ] ;;
    HNE) [ -e "$HNE_PATH/flake.nix" ] ;;
  esac
}

if [[ ! -x "$NIX" ]]; then echo "ifd-decomp: nix not found at $NIX" >&2; exit 2; fi

printf '=== IFD wall decomposition @ %s (warm/CI-shaped) ===\n' "$GITHASH"
printf '%-9s %10s %12s %8s %8s %10s  %s\n' \
       workload eval_wall_s ifd_blocked_s blocked% nrIFDs ifd_us note

DECISION=""
for wl in "${WORKLOADS[@]}"; do
  if ! wl_available "$wl"; then
    printf '%-9s %10s %12s %8s %8s %10s  %s\n' "$wl" - - - - - "SKIP (checkout absent)"
    continue
  fi
  env="$(wl_env "$wl")"; expr="$(wl_expr "$wl")"

  # 1. Warm-up (build IFDs + warm CU cache).  Discard output.
  eval "$env $NIX eval --impure --expr '$expr'" >/dev/null 2>&1

  # 2. Measured warm run, profiling on + show-stats.  Capture stderr (v3 IFD
  #    summary + IFD# logs) and the stats JSON.
  statsfile="$(mktemp -t ifd-stats.XXXX)"
  err="$(eval "$env NIX_SHOW_STATS=1 NIX_SHOW_STATS_PATH='$statsfile' \
        $NIX eval --impure --option profile-import-from-derivation true \
        --expr '$expr'" 2>&1 >/dev/null)"

  # v3 V2 summary line: "... blocked <B>s in realise across N call(s) (<P>% of <W>s eval wall)"
  v2line="$(printf '%s\n' "$err" | grep 'v3: IFD —' | head -1)"
  blocked_s="$(printf '%s\n' "$v2line" | sed -n 's/.*blocked \([0-9.]*\)s in realise.*/\1/p')"
  pct="$(printf '%s\n' "$v2line" | sed -n 's/.*(\([0-9.]*\)% of.*/\1/p')"
  wall_s="$(printf '%s\n' "$v2line" | sed -n 's/.*% of \([0-9.]*\)s eval wall.*/\1/p')"
  [ -z "$blocked_s" ] && { blocked_s=0; pct=0; }

  # NIX_SHOW_STATS cross-check (TW-side authoritative IFD build timer).
  nrifds="$(sed -n 's/.*"nrIFDs":[[:space:]]*\([0-9]*\).*/\1/p' "$statsfile" 2>/dev/null | head -1)"
  ifdus="$(sed -n 's/.*"totalIFDTimeUs":[[:space:]]*\([0-9]*\).*/\1/p' "$statsfile" 2>/dev/null | head -1)"
  [ -z "$nrifds" ] && nrifds=0
  [ -z "$ifdus" ] && ifdus=0
  rm -f "$statsfile"

  note=""
  [ "$nrifds" = "0" ] && [ "${blocked_s%.*}" = "0" ] && note="no IFD → not IFD-bound"
  printf '%-9s %10s %12s %8s %8s %10s  %s\n' \
         "$wl" "${wall_s:-?}" "$blocked_s" "${pct:-0}" "$nrifds" "$ifdus" "$note"

  DECISION+="$wl: IFD-blocked=${pct:-0}% of ${wall_s:-?}s warm wall (${nrifds} IFDs); "
done

echo "-----------------------------------------------------------------------"
echo "DECISION INPUT #1 (warm/CI-shaped): $DECISION"
echo "Read: IFD-blocked% >= ~40% on the IFD-heavy workloads ⇒ fund WS-4 fiber"
echo "overlap; small ⇒ WS-5/WS-6 (memory/COW) rank higher."

if [[ "$GIT_NOTE" == "1" ]]; then
  echo "(attach the above to commit $GITHASH: git notes append $GITHASH -F -)"
fi
