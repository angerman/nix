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
#   T5  unhashable args (a function in the arg set) fall through the
#       structural pre-check (task #16c) to plain evaluation — correct result.
#   T6  pre-check mirror exactness: keyExceptionBail must be 0 — canonicalHash
#       throwing AFTER the pre-check accepted means appliedKeyPrecheck drifted
#       from serializeOne's acceptance (the tax would silently return).
#   T7  SHADOW mode (#16a): would-HITs re-evaluate (insns must NOT collapse)
#       and lockstep-compare — result correct, shadowCompares>0, mismatch=0.
#   T9  register-call HIT writeback (miscompile fixed 2026-07-04): a HIT reached
#       via OP_R_CALL must route the cached value into the armed dst register,
#       not just push the stack — else `(import f) {a=1;} + (import f) {a=1;}`
#       returns a float half-sum.  Guards the shipped-cache correctness fix.
#   T10 impurity (default-on #1.1): an import-result whose body reads an impure
#       builtin (currentTime) applied twice in one process — cache-on == off
#       (currentTime is process-constant; the cache must never introduce a
#       divergence).  Locks the default-on safety invariant.
#   T12 gate polarity (default-on flip #1.2): cache ACTIVE when unset, OFF on
#       "0"/"probe", on for "shadow" — asserted via the X+X insns collapse.
#       Failing-first: pre-flip unset==off (no collapse).
#   T13 re-entry-force × R_CALL writeback (review #1, 2026-07-05): a THUNK arg
#       applied via OP_R_CALL forces the memo hook's WHNF re-entry while
#       CFF_FORCE_WB=dst is armed — the untested analogue of the a50609461
#       float-half bug (T9 args are const-eager WHNF, skip it).  Verified safe;
#       locked.
#   T11 GC root-survival (default-on #1.1): the MISS-insert→HIT collapse must
#       stay correct under aggressive scavenge (tiny nursery + brute audit) —
#       the cached graph is GC-rooted (walkAppliedCacheRoots) and must survive
#       a collection between insert and hit.  (Also covered inside --brute; this
#       runs it standalone so the guard fires without the full battery.)
#   T8  const-eager literal keys (#17): a NESTED-literal arg (`{ config = {
#       allowUnfree = true; }; }`) and a const-LIST arg (`{ xs = [ 1 2 ]; }`)
#       must be hashable — pre-#17 the inner value was a MkThunk (Suspended →
#       unhashable) so the firefox/allowUnfree daemon scenario missed the
#       cache.  Repeated nested-literal application must collapse like T3.
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

# T5 — unhashable arg (function value): pre-check bails, eval still correct.
check T5-unhashable-arg \
    "import $FIX/heavy.nix { f = (x: x); }" \
    '2001000'

# T6 — pre-check mirror exactness: no canonicalHash exceptions past the
# pre-check across the whole fixture set (hashable + unhashable shapes).
# SCALAR root (+): a list root returns WHNF from runRootExpr and the CLI
# forces elements via bridge re-entry AFTER the stats dump — counters would
# read 0 (diagnosed 2026-07-04; the dump is not wrong, just pre-render).
E6="($E3) + (import $FIX/heavy.nix { f = (x: x); }) + (import $FIX/heavy.nix { xs = [ 1 \"a\" { y = 2; } ]; })"
statline=$(NIX_V3_APPLIED_CACHE=1 NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
    "$NIX" eval --no-eval-cache --impure --expr "$E6" 2>&1 >/dev/null | grep 'APPLIED-CACHE:')
if echo "$statline" | grep -q 'keyExceptionBail=0'; then
    pass=$((pass+1)); echo "PASS T6-precheck-exact ($statline)"
else
    fail=$((fail+1)); echo "FAIL T6-precheck-exact: $statline"
fi

# T7 — shadow mode: correct result, compares happen, ZERO mismatches, and
# NO reuse (insns must stay at the OFF level — shadow never short-circuits).
sres=$(NIX_V3_APPLIED_CACHE=shadow NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s "$NIX" eval --no-eval-cache --impure --expr "$E3" 2>/dev/null)
sline=$(NIX_V3_APPLIED_CACHE=shadow NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
    "$NIX" eval --no-eval-cache --impure --expr "$E3" 2>&1 >/dev/null | grep 'APPLIED-CACHE:')
s_insns=$(NIX_V3_APPLIED_CACHE=shadow NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
    "$NIX" eval --no-eval-cache --impure --expr "$E3" 2>&1 >/dev/null \
  | grep -oE 'insns=[0-9]+' | cut -d= -f2 | sort -n | tail -1)
if [[ "$sres" == "3998000" ]] \
   && echo "$sline" | grep -q 'shadowMismatch=0' \
   && echo "$sline" | grep -qE 'shadowCompares=[1-9]' \
   && [[ -n "$s_insns" && -n "$i_off" ]] && (( s_insns * 100 > i_off * 90 )); then
    pass=$((pass+1)); echo "PASS T7-shadow (insns=$s_insns; $sline)"
else
    fail=$((fail+1)); echo "FAIL T7-shadow: res=$sres insns=${s_insns:-?} off=${i_off:-?} $sline"
fi

# T9 — register-call HIT writeback (miscompile fixed 2026-07-04): a cache HIT
# reached via OP_R_CALL (register-addressed call) arms CFF_FORCE_WB=dst; the HIT
# must route the cached value into regs[dst], not just push the stack.  The bug:
# `(import f) {a=1;} + (import f) {a=1;}` — the second app is an R_CALL HIT
# feeding OP_R_STR_CONCAT2; the un-routed HIT left dst uninitialised (float 0.0),
# so `+` returned a FLOAT half-sum (2001000.0 instead of int 4002000).  Assert
# BOTH the correct sum AND integer type (the float was the tell).  Failing-first:
# reproduces on the shipped cache (flat args hashable since #16); NOT #17-specific.
FLAT="import $FIX/heavy.nix { a = 1; }"
check T9-rcall-hit-plus "($FLAT) + ($FLAT)" '4002000'
check T9-rcall-hit-type "builtins.typeOf (($FLAT) + ($FLAT))" '"int"'
check T9-rcall-let-form "let imp = import $FIX/heavy.nix; in (imp { a = 1; }) + (imp { a = 1; })" '4002000'
check T9-rcall-triple "($FLAT) + ($FLAT) + ($FLAT)" '6003000'

# T8 — const-eager literal keys (#17): a NESTED-literal arg must be hashable
# and collapse on repeat; a const-LIST arg must be hashable too.  Failing-
# first: pre-#17 the inner attrset/list was a MkThunk → uncacheable → ON==OFF.
E8="(import $FIX/heavy.nix { config = { allowUnfree = true; }; }) + (builtins.seq 1 (import $FIX/heavy.nix { config = { allowUnfree = true; }; }))"
check T8-nested-correct "$E8" '4002000'
i8_off=$(insns 0 "$E8"); i8_on=$(insns 1 "$E8")
if [[ -n "$i8_off" && -n "$i8_on" ]] && (( i8_on * 100 < i8_off * 75 )); then
    pass=$((pass+1)); echo "PASS T8-nested-collapse (off=$i8_off on=$i8_on)"
else
    fail=$((fail+1)); echo "FAIL T8-nested-collapse: off=${i8_off:-?} on=${i8_on:-?} (need on < 0.75*off)"
fi
check T8-list-arg-correct "import $FIX/heavy.nix { xs = [ 1 2 ]; }" '2001000'

# T10 — impurity: currentTime-bearing import-result applied twice, cache must
# not introduce a divergence (process-constant → equal on both settings).
check T10-impure-consistent \
    "(import $FIX/impure-body.nix {}) == (import $FIX/impure-body.nix {})" \
    'true'
# and the cross-setting lock: same expression, cache-on result == cache-off.
to_off=$(run 0 "import $FIX/impure-body.nix {}"); to_on=$(run 1 "import $FIX/impure-body.nix {}")
# (Two SEPARATE processes → currentTime may differ by a second; only assert
#  that within ONE process the doubled application is self-consistent, which
#  T10-impure-consistent already does.  Here just assert both are integers.)
if [[ "$(run 1 "builtins.typeOf (import $FIX/impure-body.nix {})")" == '"int"' ]]; then
    pass=$((pass+1)); echo "PASS T10-impure-type"
else
    fail=$((fail+1)); echo "FAIL T10-impure-type"
fi

# T11 — GC root-survival: X+X collapse under aggressive scavenge must stay
# correct (cached graph rooted via walkAppliedCacheRoots survives collection
# between MISS-insert and HIT).  Tiny nursery + brute audit = the brute's env.
t11=$(NIX_V3_APPLIED_CACHE=1 NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY_SIZE=1 \
      V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1 NIX_V3_MAX_WALL_TIME=60s \
      "$NIX" eval --no-eval-cache --impure \
      --expr "($FLAT) + ($FLAT)" 2>/dev/null)
if [[ "$t11" == "4002000" ]]; then
    pass=$((pass+1)); echo "PASS T11-gc-root-survival"
else
    fail=$((fail+1)); echo "FAIL T11-gc-root-survival: got $t11 (expect 4002000)"
fi

# T12 — gate polarity (default-on flip #1.2): the cache must be ACTIVE by
# default (unset), OFF on explicit "0"/"probe", and shadow on "shadow".  Uses
# the X+X insns collapse as the "cache active" signal.  Failing-first: before
# the flip, unset==off (no collapse) → this fails; after, unset collapses.
XX="($FLAT) + ($FLAT)"
# `env -u NIX_V3_APPLIED_CACHE`: the TRUE default (variable absent), robust to
# an ambient setting — the --brute wrapper exports NIX_V3_APPLIED_CACHE=0 for
# the whole battery, which would otherwise make this "unset" probe inherit =0
# and spuriously fail the polarity check (caught 2026-07-05 in the #1.2 off-brute).
i_unset=$(env -u NIX_V3_APPLIED_CACHE NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
    "$NIX" eval --no-eval-cache --impure --expr "$XX" 2>&1 >/dev/null \
  | grep -oE 'insns=[0-9]+' | cut -d= -f2 | sort -n | tail -1)
i_zero=$(insns 0 "$XX")
i_probe=$(NIX_V3_APPLIED_CACHE=probe NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s NIX_VM_STATS=1 \
    "$NIX" eval --no-eval-cache --impure --expr "$XX" 2>&1 >/dev/null \
  | grep -oE 'insns=[0-9]+' | cut -d= -f2 | sort -n | tail -1)
# unset must collapse (< 0.75× the =0 baseline); =0 and =probe must NOT.
if [[ -n "$i_unset" && -n "$i_zero" && -n "$i_probe" ]] \
   && (( i_unset * 100 < i_zero * 75 )) \
   && (( i_probe * 100 >= i_zero * 90 )); then
    pass=$((pass+1)); echo "PASS T12-polarity (unset=$i_unset off=$i_zero probe=$i_probe)"
else
    fail=$((fail+1)); echo "FAIL T12-polarity: unset=${i_unset:-?} off=${i_zero:-?} probe=${i_probe:-?} (unset must be <0.75*off, probe ~=off)"
fi
# default-on correctness: unset result must equal explicit-off result.
check T12-default-correct "$XX" '4002000'

# T13 — re-entry-force × R_CALL writeback (adversarial-review #1, 2026-07-05):
# a THUNK arg (not const-eager, not the empty singleton) applied to an
# import-result via OP_R_CALL triggers the memo hook's WHNF-force re-entry
# `forceValue(arg)` WHILE the caller frame has CFF_FORCE_WB=dst armed.  The
# review flagged this as the untested analogue of the a50609461 float-half
# bug (T9's args are const-eager WHNF → they SKIP this re-entry).  Verified
# safe (dispatchLoop's exitDepth boundary returns the forced value without
# running the caller-resume writeback, so the armed WB survives the force and
# the HIT delivers to dst correctly).  Locked so it can't silently regress.
# `g` makes the arg a Call (thunkified, non-const-eager) → forces the re-entry.
IMPG="let imp = import $FIX/heavy.nix; g = x: { k = x; }; in"
check T13-reentry-single  "$IMPG imp (g 1)"                    '2001000'
check T13-reentry-plus    "$IMPG (imp (g 1)) + (imp (g 1))"    '4002000'
check T13-reentry-type    "builtins.typeOf ($IMPG (imp (g 1)) + (imp (g 1)))" '"int"'
check T13-reentry-spine   "builtins.head [ ($IMPG imp (g 1)) ]" '2001000'

echo "applied-cache: $pass passed, $fail failed"
[[ $fail -eq 0 ]]
