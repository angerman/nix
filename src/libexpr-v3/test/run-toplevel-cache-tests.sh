#!/usr/bin/env bash
# Top-level result cache regression tests (NIX_V3_TOPLEVEL_CACHE).
#
# Guards the ACTIVE (skip-on-hit) cache's SOUNDNESS — a wrong top-level result
# is a silent whole-eval miscompile:
#   TL1  pure expr round-trips: eval, then re-eval → ACTIVE hit returns the
#        byte-identical value (the cache actually reuses + is correct).
#   TL2  IMPURITY taint (the core soundness guard): `getEnv "X"` with X=v1 then
#        X=v2 must return v2, NOT v1 — a tainted eval must never be reused
#        (found 2026-07-05: a pre-taint stale entry served a wrong value).
#   TL3  currentTime-tainted eval is not reused (result differs across a >1s
#        gap → must re-eval, never serve a stale timestamp).
# Each test uses a FRESH cache dir (NIX_V3_CACHE_DIR) so it is hermetic and
# immune to stale cross-version entries.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$NIX" ]]; then echo "toplevel-cache: nix not at $NIX" >&2; exit 2; fi
pass=0; fail=0

# eval in ACTIVE mode with a per-call FRESH cache dir; extra env via "$@".
ev() { # $1=cachedir $2=expr ; rest=env assignments
    local dir="$1" expr="$2"; shift 2
    env "$@" NIX_V3_CACHE_DIR="$dir" NIX_V3_TOPLEVEL_CACHE=active NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null
}
chk() { # $1=name $2=got $3=want
    if [[ "$2" == "$3" ]]; then pass=$((pass+1)); echo "PASS $1"
    else fail=$((fail+1)); echo "FAIL $1: got=$2 want=$3"; fi
}

# TL1 — pure expr round-trips through the ACTIVE cache (miss→insert→hit).
D=$(mktemp -d)
r1=$(ev "$D" 'builtins.toString (1 + 2 + 3)')       # miss, insert
r2=$(ev "$D" 'builtins.toString (1 + 2 + 3)')       # ACTIVE hit
chk TL1-pure-roundtrip-miss "$r1" '6'
chk TL1-pure-roundtrip-hit  "$r2" '6'
rm -rf "$D"

# TL2 — impurity taint: getEnv must NOT be reused across differing env values.
D=$(mktemp -d)
g1=$(ev "$D" 'builtins.getEnv "TLCACHE_X"' TLCACHE_X=first)
g2=$(ev "$D" 'builtins.getEnv "TLCACHE_X"' TLCACHE_X=second)
chk TL2-taint-first  "$g1" 'first'
chk TL2-taint-nostale "$g2" 'second'   # must be 'second', not a stale 'first'
rm -rf "$D"

# TL3 — currentTime-tainted eval is not served stale (differs across a >1s gap).
D=$(mktemp -d)
t1=$(ev "$D" 'builtins.toString builtins.currentTime')
sleep 2
t2=$(ev "$D" 'builtins.toString builtins.currentTime')
if [[ -n "$t1" && -n "$t2" && "$t1" != "$t2" ]]; then
    pass=$((pass+1)); echo "PASS TL3-currenttime-not-stale ($t1 -> $t2)"
else
    fail=$((fail+1)); echo "FAIL TL3-currenttime-not-stale: t1=$t1 t2=$t2 (must differ — a stale hit froze the clock)"
fi
rm -rf "$D"

echo "toplevel-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
