#!/usr/bin/env bash
# LOW-6 decision proxy (measure-twice): is the under-applied-PAP spine walk
# (isUnderappliedClosurePap + the 4+ handshake walks it feeds) a measurable
# hotspot on a real eval?  If it's <1% of samples it's M-11-class ineffective
# standalone (the spine depths are O(1-3), correctness already fixed by
# C-8/9/10) and LOW-6's invasive repr change isn't justified.  If significant,
# implement the arity-cache.
#
# Invoke:  nix develop /Users/angerman/Projects/iohk/nix -c bash THIS
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
NIX=/Users/angerman/Projects/iohk/nix/build/src/nix/nix
EXPR='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath'
# Start the v3 eval in the background, sample it, wait.
NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=300s NIX_V3_MAX_HEAP=8G \
  "$NIX" eval --impure --expr "$EXPR" >/dev/null 2>&1 &
pid=$!
sleep 2   # let it get past startup into steady-state eval
sample "$pid" 12 -mayDie >/tmp/low6.sample 2>/dev/null || true
wait "$pid" 2>/dev/null || true
echo "=== total samples ==="
grep -m1 "Total number" /tmp/low6.sample || true
echo "=== PAP / spine-walk frames (isUnderappliedClosurePap, needsForce, op_force_slow, op_call) ==="
grep -iE "isUnderappliedClosurePap|needsForce|op_force_slow|op_call_have_fun" /tmp/low6.sample | head -20
echo "=== top-20 hottest frames (heaviest first) ==="
grep -E "^\s+[0-9]+ " /tmp/low6.sample | sort -rn | head -20
