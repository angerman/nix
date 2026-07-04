#!/usr/bin/env bash
# LEVER-1 applied-import result cache regression tests (NIX_V3_APPLIED_CACHE).
#
# Guards the v1 cache (commit edf869bee) against its own killed-hypothesis
# regressions:
#   T1  CU-key collision: a formals-lambda DEFINED IN an imported file must
#       NOT hit the entry for the import-result application (the wrong-drvPath
#       bug that killed CU-keyed identity).
#   T2  distinct-args discrimination: the same import applied to different
#       args must yield different (correct) results — no key collision;
#       unhashable args must fall through to plain evaluation.
#   T3  repeated-eval collapse (the lever itself): two separate applications
#       of the same (import f) {} in one process — cache-ON insns must
#       collapse to ~half of cache-OFF (asserted < 0.75x; measured ~0.5x).
#       This is the FAILING-FIRST test: a broken/no-op cache makes ON==OFF.
#   T4  throws are never cached: insert happens only at OP_RETURN; the second
#       application of a throwing import-result must re-throw.
# All correctness cases assert cache-ON == cache-OFF == expected literal.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
FIX="$TEST_DIR/fixtures-applied-cache"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
if [[ ! -x "$NIX" ]]; then echo "applied-cache: nix not at $NIX" >&2; exit 2; fi

pass=0; fail=0

# eval with the applied cache OFF/ON; prints stdout (the value) only.
run() { # $1=cache(0|1) $2=expr
    NIX_V3_APPLIED_CACHE=$1 NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" eval --no-eval-cache --impure --expr "$2" 2>/dev/null
}

# max main-eval instruction count for an expr under a cache setting.
insns() { # $1=cache(0|1) $2=expr
    NIX_V3_APPLIED_CACHE=$1 NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
        "$NIX" eval --no-eval-cache --impure --expr "$2" 2>&1 >/dev/null \
      | grep -oE 'insns=[0-9]+' | cut -d= -f2 | sort -n | tail -1
}

check() { # $1=name $2=expr $3=expected
    local off on
    off=$(run 0 "$2"); on=$(run 1 "$2")
    if [[ "$off" == "$3" && "$on" == "$3" ]]; then
        pass=$((pass+1)); echo "PASS $1"
    else
        fail=$((fail+1)); echo "FAIL $1: expected=$3 off=$off on=$on"
    fi
}

# T1 — CU-key collision regression: inner {} must not alias import-result {}.
check T1-collision \
    "let r = import $FIX/lib.nix {}; i = r.inner {}; in \"\${toString r.v}-\${toString i.v}\"" \
    '"1-100"'

# T2 — distinct args, distinct results (n = 2000+|args| → sums differ).
check T2-distinct-args \
    "let a = import $FIX/heavy.nix {}; b = import $FIX/heavy.nix { x = 1; }; in \"\${toString a}-\${toString b}\"" \
    '"1999000-2001000"'

# T4 — throws are never cached: both tryEvals must fail identically.
check T4-throw-not-cached \
    "let t1 = builtins.tryEval (import $FIX/throwy.nix {}); t2 = builtins.tryEval (import $FIX/throwy.nix {}); in [ t1.success t2.success ]" \
    '[ false false ]'

# T3 — repeated-eval insns collapse (failing-first: a no-op cache => ON==OFF).
E3="(import $FIX/heavy.nix {}) + (import $FIX/heavy.nix {})"
check T3-correctness "$E3" '3998000'
i_off=$(insns 0 "$E3"); i_on=$(insns 1 "$E3")
if [[ -n "$i_off" && -n "$i_on" ]] && (( i_on * 100 < i_off * 75 )); then
    pass=$((pass+1)); echo "PASS T3-collapse (off=$i_off on=$i_on)"
else
    fail=$((fail+1)); echo "FAIL T3-collapse: off=${i_off:-?} on=${i_on:-?} (need on < 0.75*off)"
fi

echo "applied-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
