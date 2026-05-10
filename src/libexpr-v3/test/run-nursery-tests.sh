#!/usr/bin/env bash
# Cheney nursery (#434) sanity tests.
#
# Phase A (this file's initial form): nursery is bump-pointer with
# fall-back-to-tenured-on-overflow; no scavenge yet.  These tests
# verify:
#   p1  nursery-on produces same eval result as nursery-off (no
#       behaviour change from routing).
#   p2  the nursery actually allocates SOMETHING when on (we can't
#       directly observe in stdout; rely on the tests existing as
#       contract — Phase B+C will add a stats-dump diagnostic).
#   p3  oversized nurseries still work (NIX_V3_NURSERY_SIZE).
#
# Background: lode/CHENEY_NURSERY_DESIGN.md.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
    echo "nursery-tests: $NIX not found, build first" >&2
    exit 1
fi

PASS=0; FAIL=0
fail_names=()

assert_eq() {
    local name="$1" expected="$2" got="$3"
    if [[ "$expected" == "$got" ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        fail_names+=("$name: expected=$expected got=$got")
    fi
}

# p1 — nursery-on result == nursery-off result for a few exprs.
for expr in '1 + 2' \
            'let x = 5; in x * x' \
            'let f = x: x + 1; in f 41' \
            '{ a = 1; b = 2; c = 3; }.b' \
            'let lib = { fix = f: let x = f x; in x; }; in lib.fix (self: { x = 1; y = self.x + 1; }).y'
do
    off=$(NIX_V3_DIRECT_EVAL=1 "$NIX" --extra-experimental-features \
        nix-command eval --impure --expr "$expr" 2>&1)
    on=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 "$NIX" \
        --extra-experimental-features nix-command eval --impure \
        --expr "$expr" 2>&1)
    assert_eq "p1[$expr] on==off" "$off" "$on"
done

# p2 — small nursery (1 MB) still works (forces fall-back path
# to engage early; the fall-back to tenured arena should be
# transparent).
small=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SIZE=1 \
    "$NIX" --extra-experimental-features nix-command eval --impure \
    --expr 'builtins.length (builtins.attrNames (builtins.functionArgs ({a, b, c, d, e}: 0)))' 2>&1)
assert_eq "p2 small nursery" "5" "$small"

# p3 — large nursery (256 MB) still works (no surprises at upper
# bounds).
big=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SIZE=256 \
    "$NIX" --extra-experimental-features nix-command eval --impure \
    --expr '{ a = 1; b = 2; }.a' 2>&1)
assert_eq "p3 large nursery" "1" "$big"

# p4 — disabled nursery (NIX_V3_NURSERY=0) takes legacy path.
off2=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=0 "$NIX" \
    --extra-experimental-features nix-command eval --impure \
    --expr 'let g = x: x * 2; in g 21' 2>&1)
assert_eq "p4 NIX_V3_NURSERY=0 disabled" "42" "$off2"

echo
echo "=== nursery tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
    for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
    exit 1
fi
exit 0
