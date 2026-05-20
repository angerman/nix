#!/usr/bin/env bash
# Regression test for #696 — Path B M2 milestone: IFD support in
# v3's `primImport`.
#
# Pre-fix: `import (runCommand "x" {} "echo 42 > $out")` failed with
# `error: v3 primop import: expected string or path` because v3's
# primImport only accepted string/path-tagged args.
#
# Fix: when args[0] is an attrset (e.g. a derivation), bridge to TW
# and call `nixEvalState->realisePath(noPos, twValue)`.  TW handles
# the value's string-context — including `DrvDeep` entries — by
# realising the underlying build via `realiseContext` → `buildPaths`.
# The returned `SourcePath` then drives v3's existing parse+lower+
# compile pipeline.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

fail=0

run_pair() {
  local label="$1" expr="$2" timeout_s="${3:-30}"
  local tw v3
  tw="$(timeout $timeout_s "$NIX" eval --impure --expr "$expr" 2>&1 \
        | grep -v '^Failed\|^warning:\|^building ' | head -1)"
  v3="$(timeout $timeout_s env NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
        NIX_V3_MAX_WALL_TIME=$((timeout_s-5))s NIX_V3_MAX_HEAP=8G \
        "$NIX" eval --impure --expr "$expr" 2>&1 \
        | grep -v '^Failed\|^warning:\|^building ' | head -1)"
  if [[ "$tw" == "$v3" ]]; then
    printf "  OK   %-45s => %s\n" "$label" "${tw:0:60}"
  else
    printf "  FAIL %-45s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    fail=$((fail+1))
  fi
}

echo "===== M2: real IFD via runCommand ====="
run_pair "import (runCommand) → 42" \
  'let pkgs = import <nixpkgs> {}; in import (pkgs.runCommand "x" {} "echo 42 > $out")' 60
run_pair "import (writeText) → string" \
  'let pkgs = import <nixpkgs> {}; in import (pkgs.writeText "y" "\"hi\"")' 60
run_pair "import (runCommand) → list" \
  'let pkgs = import <nixpkgs> {}; in import (pkgs.runCommand "z" {} "echo \"[ 1 2 3 ]\" > $out")' 60

echo
echo "===== Regression: plain string/path import still works ====="
run_pair "import (toFile) int" \
  'import (builtins.toFile "a.nix" "42")' 15
run_pair "import (toFile) string" \
  'import (builtins.toFile "b.nix" "\"hello\"")' 15
run_pair "import <nixpkgs>.lib has fold" \
  '(import <nixpkgs> {}).lib ? fold' 30

if [[ "$fail" -eq 0 ]]; then
  echo
  echo "run-696: PASS (real IFD via runCommand + writeText + regression)"
  exit 0
else
  echo
  echo "run-696: FAIL ($fail divergence(s))"
  exit 1
fi
