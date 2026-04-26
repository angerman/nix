#!/usr/bin/env bash
# Benchmark the v2 bytecode VM against the tree-walker, with optional
# disk-cache warm-up.
#
# Usage: scripts/bench-vm-v2.sh [N_RUNS]
#
# Outputs three blocks:
#   * tree-walker:   default (NIX_VM_V2 unset)
#   * v2 cold:       NIX_VM_V2=1 with fresh XDG_CACHE_HOME (in-process bytecodeCache only)
#   * v2 warm:       NIX_VM_V2=1 NIX_BYTECODE_DISK_CACHE=1, second-run hits the disk cache
#
# Each block runs the eval N_RUNS times and reports min/med/max wall.
#
# Default expression: nixpkgs#hello.name (a small flake eval).
# Override with EXPR env var.

set -euo pipefail

NIX="${NIX:-./build/src/nix/nix}"
N="${1:-5}"
EXPR_DEFAULT="nixpkgs#hello.name"
EXPR="${EXPR:-$EXPR_DEFAULT}"

if ! command -v "$NIX" &>/dev/null && ! [[ -x "$NIX" ]]; then
    echo "error: $NIX not found.  Build first: nix develop -c ninja -C build" >&2
    exit 1
fi

# Prime the eval-cache for nixpkgs first so neither the tree-walker
# nor v2 pays the flake-fetch cost.  These warm runs aren't measured.
echo "Priming flake cache..." >&2
"$NIX" eval --raw --extra-experimental-features 'nix-command flakes' "$EXPR" >/dev/null 2>&1 || true
"$NIX" eval --raw --extra-experimental-features 'nix-command flakes' "$EXPR" >/dev/null 2>&1 || true

bench() {
    local label="$1"; shift
    local runs=()
    for ((i = 1; i <= N; i++)); do
        local t
        t=$( { TIMEFORMAT='%R'; time "$@" "$EXPR" >/dev/null 2>&1; } 2>&1 )
        runs+=("$t")
    done
    # min/median/max via sort.
    local sorted; mapfile -t sorted < <(printf '%s\n' "${runs[@]}" | sort -n)
    local min="${sorted[0]}"
    local max="${sorted[-1]}"
    local mid_idx=$((N / 2))
    local median="${sorted[$mid_idx]}"
    printf '  %-22s min=%-7s med=%-7s max=%-7s  raw=%s\n' \
        "$label" "$min" "$median" "$max" "${runs[*]}"
}

EVAL_ARGS=(eval --raw --extra-experimental-features 'nix-command flakes')

echo "=== Bench: $EXPR ($N runs each) ==="

echo "[tree-walker]"
bench "default" \
    "$NIX" "${EVAL_ARGS[@]}"

echo "[v2 (no disk cache)]"
bench "NIX_VM_V2=1" \
    env NIX_VM_V2=1 "$NIX" "${EVAL_ARGS[@]}"

# Fresh disk cache for the cold/warm split.
TMPCACHE="$(mktemp -d)"
trap 'rm -rf "$TMPCACHE"' EXIT

echo "[v2 + disk cache (cold, each run uses a fresh cache dir)]"
bench_cold() {
    local label="$1"; shift
    local runs=()
    for ((i = 1; i <= N; i++)); do
        local d
        d="$(mktemp -d)"
        local t
        t=$( { TIMEFORMAT='%R'; time env NIX_VM_V2=1 NIX_BYTECODE_DISK_CACHE=1 \
            NIX_BYTECODE_CACHE_DIR="$d" "$@" "$EXPR" >/dev/null 2>&1 || true; } 2>&1 )
        runs+=("$t")
        rm -rf "$d"
    done
    local sorted; mapfile -t sorted < <(printf '%s\n' "${runs[@]}" | sort -n)
    local min="${sorted[0]}"
    local max="${sorted[-1]}"
    local mid_idx=$((N / 2))
    local median="${sorted[$mid_idx]}"
    printf '  %-22s min=%-7s med=%-7s max=%-7s  raw=%s\n' \
        "$label" "$min" "$median" "$max" "${runs[*]}"
}
bench_cold "compile+insert" "$NIX" "${EVAL_ARGS[@]}"

echo "[v2 + disk cache (warm, all runs share one cache)]"
# Prime the cache once.
env NIX_VM_V2=1 NIX_BYTECODE_DISK_CACHE=1 NIX_BYTECODE_CACHE_DIR="$TMPCACHE" \
    "$NIX" "${EVAL_ARGS[@]}" "$EXPR" >/dev/null 2>&1 || true
bench "lookup+deserialize" \
    env NIX_VM_V2=1 NIX_BYTECODE_DISK_CACHE=1 NIX_BYTECODE_CACHE_DIR="$TMPCACHE" \
    "$NIX" "${EVAL_ARGS[@]}"
