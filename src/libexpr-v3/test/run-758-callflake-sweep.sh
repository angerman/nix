#!/usr/bin/env bash
# #758 cross-eval verification: v3-native callFlake vs TW bridge.
#
# Compares byte-for-byte output of `nix eval --impure --raw` between
# the default (v3-native callFlake) and `NIX_V3_NO_NATIVE_CALL_FLAKE=1`
# (bridge) modes on a sample of public flakes.  Both modes route through
# v3 for the rest of the eval — only the callFlake (== `builtins.getFlake`)
# step differs.
#
# Exit codes:
#   0 - all queries byte-identical
#   1 - one or more divergences
#   2 - infrastructure error (nix binary missing, etc.)
#
# Usage:
#   ./run-758-callflake-sweep.sh                  # default sample
#   ./run-758-callflake-sweep.sh --quick          # 4 queries
#   V3_TEST_VERBOSE=1 ./run-758-callflake-sweep.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${ROOT}/build/src/nix/nix"

if [[ ! -x "$NIX" ]]; then
    echo "run-758: nix binary not found at $NIX" >&2
    exit 2
fi

VERBOSE="${V3_TEST_VERBOSE:-0}"
MODE="${1:-full}"

PASS=0
FAIL=0
FAILED=()

# Each line: <name> <expr>
QUERIES_FULL=(
    # nixpkgs flake
    "nixpkgs:hello.name|(import (builtins.getFlake \"flake:nixpkgs\") {}).hello.name"
    "nixpkgs:hello.drvPath|(import (builtins.getFlake \"flake:nixpkgs\") {}).hello.drvPath"
    "nixpkgs:firefox.name|(import (builtins.getFlake \"flake:nixpkgs\") {}).firefox.name"
    "nixpkgs:bash.drvPath|(import (builtins.getFlake \"flake:nixpkgs\") {}).bash.drvPath"
    "nixpkgs:gcc.name|(import (builtins.getFlake \"flake:nixpkgs\") {}).gcc.name"
    "nixpkgs:python3.name|(import (builtins.getFlake \"flake:nixpkgs\") {}).python3.name"
    # flake-attr lookups against the flake itself (no second getFlake)
    "nixpkgs:flake.outputs.lib?|builtins.typeOf (builtins.getFlake \"flake:nixpkgs\").outputs.lib"
    "nixpkgs:flake.outputs.legacyPackages.aarch64-darwin.hello.drvPath|(builtins.getFlake \"flake:nixpkgs\").outputs.legacyPackages.aarch64-darwin.hello.drvPath"
    "nixpkgs:flake.outputs.legacyPackages.aarch64-darwin.firefox.name|(builtins.getFlake \"flake:nixpkgs\").outputs.legacyPackages.aarch64-darwin.firefox.name"
)

# cardano-node flake (if local checkout present)
CN="${CARDANO_NODE_FLAKE:-path:/Users/angerman/Projects/iohk/cardano-node}"
if [[ -d "${CN#path:}" ]]; then
    QUERIES_FULL+=(
        "cn:apps.cardano-node.type|(builtins.getFlake \"$CN\").outputs.apps.aarch64-darwin.cardano-node.type"
        "cn:apps.bech32.program|(builtins.getFlake \"$CN\").outputs.apps.aarch64-darwin.bech32.program"
        "cn:packages.cardano-node.name|(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.cardano-node.name"
        "cn:packages.bech32.name|(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.bech32.name"
        "cn:devShells.default ? name|(builtins.getFlake \"$CN\").outputs.devShells.aarch64-darwin.default ? name"
        "cn:legacyPackages.haskell-nix typeOf|builtins.typeOf (builtins.getFlake \"$CN\").outputs.legacyPackages.aarch64-darwin.haskell-nix"
    )
fi

QUERIES_QUICK=(
    "nixpkgs:hello.drvPath|(import (builtins.getFlake \"flake:nixpkgs\") {}).hello.drvPath"
    "nixpkgs:firefox.name|(import (builtins.getFlake \"flake:nixpkgs\") {}).firefox.name"
)
if [[ -d "${CN#path:}" ]]; then
    QUERIES_QUICK+=(
        "cn:packages.cardano-node.name|(builtins.getFlake \"$CN\").outputs.packages.aarch64-darwin.cardano-node.name"
    )
fi

case "$MODE" in
    --quick) QUERIES=("${QUERIES_QUICK[@]}") ;;
    --full|full) QUERIES=("${QUERIES_FULL[@]}") ;;
    *) echo "unknown mode: $MODE" >&2; exit 2 ;;
esac

run_one() {
    local label="${1%%|*}"
    local expr="${1#*|}"

    # Native (default).
    local out_native
    out_native=$(env \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_HEAP=8G NIX_V3_MAX_WALL_TIME=300s \
        "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null)
    local rc_native=$?

    # Bridge (opt-out).
    local out_bridge
    out_bridge=$(env \
        NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_NO_NATIVE_CALL_FLAKE=1 \
        NIX_V3_MAX_HEAP=8G NIX_V3_MAX_WALL_TIME=300s \
        "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null)
    local rc_bridge=$?

    if [[ "$out_native" = "$out_bridge" ]] && [[ "$rc_native" = "$rc_bridge" ]]; then
        printf "  [PASS] %s  (rc=%d)\n" "$label" "$rc_native"
        [[ "$VERBOSE" = "1" ]] && printf "         out=%s\n" "$out_native"
        PASS=$((PASS+1))
    else
        printf "  [FAIL] %s\n" "$label"
        printf "         native (rc=%d): %s\n" "$rc_native" "$out_native"
        printf "         bridge (rc=%d): %s\n" "$rc_bridge" "$out_bridge"
        FAIL=$((FAIL+1))
        FAILED+=("$label")
    fi
}

echo "=== #758 callFlake cross-eval sweep (mode=$MODE, ${#QUERIES[@]} queries) ==="
echo ""

for q in "${QUERIES[@]}"; do
    run_one "$q"
done

echo ""
echo "==================== summary ===================="
echo "  total:  $((PASS+FAIL))"
echo "  pass:   $PASS"
echo "  fail:   $FAIL"
if (( FAIL > 0 )); then
    echo "  failed:"
    for f in "${FAILED[@]}"; do
        echo "    - $f"
    done
    exit 1
fi
exit 0
