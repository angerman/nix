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

# p5 — scavenge ON, small nursery (1 MB) — forces multiple scavenge
# cycles for any non-trivial expression.  Validates Phase C: live
# nursery objects are correctly forwarded to tenured each cycle.
for expr in 'let f = x: if x == 0 then 0 else f (x - 1); in f 1000' \
            'let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 20' \
            '(let lib = { fix = f: let x = f x; in x; }; in (lib.fix (self: { x = 1; y = self.x + 1; z = self.y * 2; }))).z' \
            'builtins.length (builtins.genList (i: i * 2) 5000)' \
            'builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i) 1000)'
do
    expected=$(NIX_V3_DIRECT_EVAL=1 "$NIX" --extra-experimental-features \
        nix-command eval --impure --expr "$expr" 2>&1)
    actual=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 \
        NIX_V3_NURSERY_SIZE=1 "$NIX" --extra-experimental-features \
        nix-command eval --impure --expr "$expr" 2>&1)
    assert_eq "p5[$expr] scavenge==no-scavenge" "$expected" "$actual"
done

# p6 — scavenge ON with default nursery size (32 MB) — should
# rarely trigger scavenge for short evals, but exercises the gate.
big6=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 \
    "$NIX" --extra-experimental-features nix-command eval --impure \
    --expr 'let lib = { fix = f: let x = f x; in x; }; in (lib.fix (self: { x = 1; y = self.x + 1; })).y' 2>&1)
assert_eq "p6 default-size scavenge" "2" "$big6"

# p7 — deep recursion through a let-rec / fix combination, with
# 1 MB nursery forcing many scavenge cycles per eval.  Validates
# the Phase C exitDepth==0 gate: forceValue chains re-enter
# dispatchLoop with exitDepth>0; scavenge MUST stay disabled there
# so the outer opcode handler's C++ Value locals stay valid across
# the nested call.  Pre-fix (no exitDepth gate), this would have
# corrupted `arg`/`fun` C-stack locals after an inner scavenge.
deep_lr=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 \
    NIX_V3_NURSERY_SIZE=1 "$NIX" --extra-experimental-features \
    nix-command eval --impure --expr '
      let
        rec1 = self: { a = 1; b = 2; c = self.a + self.b;
                       d = self.c * 2; e = self.d + self.a; };
        fix = f: let x = f x; in x;
        deep = n: if n == 0 then 0
                  else (fix rec1).e + deep (n - 1);
      in deep 4000' 2>&1)
assert_eq "p7 deep let-rec/fix scavenge" "28000" "$deep_lr"

# p8 — primop callback chain (foldl' over a long genList).  The
# primop body iterates in C++; nursery fills before the outer
# dispatchLoop iterates again.  Pre-exitDepth-gate: any inner
# scavenge inside the foldl' callback's forceValue would have
# corrupted the outer's C-locals.  Verifies the gate keeps the
# call chain safe even when the nursery overflows during it.
fold_pc=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=1 NIX_V3_NURSERY_SCAVENGE=1 \
    NIX_V3_NURSERY_SIZE=1 "$NIX" --extra-experimental-features \
    nix-command eval --impure --expr 'builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i) 100000)' 2>&1)
assert_eq "p8 primop chain scavenge" "4999950000" "$fold_pc"

echo
echo "=== nursery tests: ok=$PASS fail=$FAIL ==="
if [[ $FAIL -gt 0 ]]; then
    for n in "${fail_names[@]}"; do echo "  FAIL: $n"; done
    exit 1
fi
exit 0
