#!/usr/bin/env bash
# PLAN_BEAT_TW_V2 workstream A (chain-SELECT lookup-without-materialize) —
# STEP 1: tests-first.  Pins the 2026-06-07 failure set as a byte-identity
# regression guard vs the tree-walker.
#
# These five drvPaths were the chain-SELECT corruption witnesses on 2026-06-07
# (the C-1 stale-KEEP / shared-parent writeback class — see
# project_drvpath_systemic_divergence_2026-06-11).  They are byte-identical at
# HEAD; this suite is the canary that MUST stay green through any future
# chain-SELECT / materialize-removal work (QG-1).  Per LESSONS §4.8 we keep the
# repro forever, even though the bug is currently closed.
#
# Run with disk cache scoped to a temp dir so a stale CU cache can't mask a
# fresh divergence (lint-cache-coherence rationale).
#
#   NIX=<nix> ./run-chain-select-0607-failure-set.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
CACHE="$(mktemp -d)"; trap 'rm -rf "$CACHE"' EXIT

# Each entry is a Nix expression whose v3-direct result must byte-equal TW.
EXPRS=(
  '(import <nixpkgs> {}).git.drvPath'
  '(import <nixpkgs> {}).cargo.drvPath'
  '(import <nixpkgs> {}).rustc.drvPath'
  '(import <nixpkgs> {}).cargo-auditable.cargoDeps.drvPath'
  '((import <nixpkgs> {}).python3.withPackages (ps: [ ps.requests ])).drvPath'
)

pass=0; fail=0
for e in "${EXPRS[@]}"; do
  v3="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$CACHE/v3" NIX_V3_MAX_WALL_TIME=120s \
        "$NIX" eval --impure --expr "$e" 2>/dev/null)"
  tw="$("$NIX" eval --impure --expr "$e" 2>/dev/null)"
  if [[ -n "$tw" && "$v3" == "$tw" ]]; then
    printf '  PASS  %s\n         = %s\n' "${e:0:60}" "$v3"; pass=$((pass+1))
  else
    printf '  FAIL  %s\n         v3=[%s]\n         tw=[%s]\n' "${e:0:60}" "$v3" "$tw"; fail=$((fail+1))
  fi
done

echo
echo "  chain-select-0607: pass=$pass fail=$fail"
[[ $fail -eq 0 ]] && { echo "OK — 06-07 failure set byte-identical"; exit 0; } \
                  || { echo "FAIL — chain-SELECT regression (a shared-parent writeback corruption is back)"; exit 1; }
