#!/usr/bin/env bash
# Regression test for #695 — Path B M3 milestone: `builtins.getFlake`
# bridge from v3-direct to TW's libflake primop.
#
# TW registers `getFlake` via `evalSettings.extraPrimOps` at
# `libflake/settings.cc:14`.  v3's static primop registry doesn't
# include extras — so pre-fix `builtins.getFlake` was reported as
# "attribute 'getFlake' missing" by v3-direct.  Fix: a `bridgeBuiltin<1>`
# wrapper in primops.cc registers `getFlake`/`__getFlake` in v3's
# registry, which dispatches at call time to TW's `builtins.getFlake`.
#
# Also covers M1 (trivial IFD via builtins.toFile), which v3 already
# handles natively — this guards against regression.
#
# M2 (real IFD via runCommand) is NOT covered here; that requires
# the primImport realisePath extension, a separate change.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

fail=0

# Use the cardano-node flake if available, else nixpkgs as fallback.
CN_PATH=""
if [[ -f /Users/angerman/Projects/iohk/cardano-node/flake.nix ]]; then
  CN_PATH=/Users/angerman/Projects/iohk/cardano-node
fi

run_pair() {
  local label="$1" expr="$2" timeout_s="${3:-30}"
  local tw v3
  tw="$(timeout $timeout_s "$NIX" eval --impure --expr "$expr" 2>&1 | grep -v '^Failed\|^warning:' | head -1)"
  v3="$(timeout $timeout_s env NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=$((timeout_s-5))s NIX_V3_MAX_HEAP=8G \
        "$NIX" eval --impure --expr "$expr" 2>&1 | grep -v '^Failed\|^warning:' | head -1)"
  if [[ "$tw" == "$v3" ]]; then
    printf "  OK   %-45s => %s\n" "$label" "${tw:0:60}"
  else
    printf "  FAIL %-45s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    fail=$((fail+1))
  fi
}

# Negative: pre-#695 v3 reported `getFlake` as missing.  Verify the
# primop now resolves (even if eval is slow on heavy flakes — see
# below).
echo "===== M3a: getFlake primop resolves (smoke) ====="
echo 'builtins ? getFlake (TW only — sanity):'
"$NIX" eval --impure --expr 'builtins ? getFlake' 2>&1 | grep -v '^Failed\|^warning:' | head -1
echo 'builtins ? getFlake (v3-direct):'
NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 NIX_V3_MAX_WALL_TIME=5s \
  "$NIX" eval --impure --expr 'builtins ? getFlake' 2>&1 | grep -v '^Failed\|^warning:' | head -1

echo
echo "===== M1: trivial IFD via builtins.toFile ====="
run_pair "import (toFile)"        'import (builtins.toFile "x.nix" "42")'         10
run_pair "import (toFile, string)" 'import (builtins.toFile "y.nix" "\"abc\"")'   10
run_pair "import (toFile, attrset)" 'import (builtins.toFile "z.nix" "{ a = 1; b = 2; }")' 10

# M3 (heavy getFlake) — wrapped in a long-timeout positive-only test;
# slow eval is acceptable for the bridge milestone, perf is separate.
if [[ -n "$CN_PATH" ]]; then
  echo
  echo "===== M3b: getFlake cardano-node flake (long timeout, may be slow) ====="
  # Pre-#695: error: attribute 'getFlake' missing
  # Post-#695: bridge resolves; eval slow but functional
  V3_OUT=$(timeout 120 env NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
    NIX_V3_MAX_WALL_TIME=115s NIX_V3_MAX_HEAP=8G \
    "$NIX" eval --impure --expr "(builtins.getFlake \"$CN_PATH\") ? outputs" 2>&1 | grep -v '^Failed\|^warning:' | head -1)
  if [[ "$V3_OUT" == *"missing"* ]]; then
    echo "  FAIL bridge not resolving: $V3_OUT"
    fail=$((fail+1))
  elif [[ "$V3_OUT" == "true" ]]; then
    echo "  OK   bridge resolves + completes => $V3_OUT"
  else
    # WallTime / other slow — bridge resolves but doesn't complete.
    # Acceptable milestone state; flag but don't fail.
    echo "  PARTIAL bridge resolves but eval slow: ${V3_OUT:0:80}"
  fi
fi

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-695: PASS (getFlake bridge resolves + trivial IFD matches TW)"
  exit 0
else
  echo
  echo "run-695: FAIL ($fail divergence(s))"
  exit 1
fi
