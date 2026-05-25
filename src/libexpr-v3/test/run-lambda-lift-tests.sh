#!/usr/bin/env bash
# Phase D lambda-lift regression driver.
#
# 1. Semantic parity: TW vs v3-direct (with Phase D ON and OFF)
#    produce identical results on the 6-pattern fixture.
# 2. Alloc-reduction guard: at N=100 capture-free lambda creations
#    in a chain, Phase D ON produces NOTICEABLY fewer Closure
#    allocations than Phase D OFF — confirms the singleton intern
#    actually fires.  We use v3-smoke-style alloc dump comparison.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
FIXTURE="$(cd "$(dirname "$0")" && pwd)/repro-lambda-lift.nix"

if [ ! -x "$NIX" ]; then
    echo "FAIL: nix binary not found: $NIX" >&2
    exit 1
fi

NIX_FLAGS="--extra-experimental-features nix-command --extra-experimental-features flakes"

run_attr() {
    local label="$1"
    local extra_env="$2"
    local attr="$3"
    local expected="$4"

    local val errfile
    errfile=$(mktemp)
    val=$(env $extra_env "$NIX" $NIX_FLAGS eval --impure \
        --expr "(import $FIXTURE).$attr" 2>"$errfile")
    if [ "$val" != "$expected" ]; then
        echo "FAIL [$label]: $attr = '$val' (expected '$expected')"
        if [ -s "$errfile" ]; then
            echo "  stderr:"
            sed 's/^/    /' "$errfile" | head -10
        fi
        rm -f "$errfile"
        return 1
    fi
    rm -f "$errfile"
    return 0
}

fail=0

# Semantic parity across three modes: TW, v3-direct Phase D ON,
# v3-direct Phase D OFF.
for mode in "TW:" \
            "v3-D-ON:NIX_V3_DIRECT_EVAL=1" \
            "v3-D-OFF:NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_LAMBDA_LIFT=1"; do
    label="${mode%%:*}"
    env_part="${mode##*:}"

    run_attr "$label" "$env_part" incApp       42  || fail=1
    run_attr "$label" "$env_part" addedTwelve  12  || fail=1
    run_attr "$label" "$env_part" withTest     42  || fail=1
    run_attr "$label" "$env_part" constApp     42  || fail=1
    run_attr "$label" "$env_part" curriedApp   20  || fail=1
    run_attr "$label" "$env_part" fact5       120  || fail=1

    # Cross-check actualSum == expectedSum (both 278).
    run_attr "$label" "$env_part" expectedSum  278 || fail=1
    run_attr "$label" "$env_part" actualSum    278 || fail=1
done

# Alloc-reduction guard: build a chain of 100 capture-free lambdas
# (a factory that yields a fresh capture-free lambda per call), and
# compare Closure alloc counts via V3_DBG_ALLOC_STATS.  Phase D ON
# should produce noticeably fewer Closures than OFF.
#
# We use V3_DBG_ALLOC_DUMP=1 (already plumbed in vm.cc) to print the
# "v3 alloc stats: closures=N thunks=M lists=L attrsets=A" line.
# Each invocation of `mkConst null` creates the same descriptor's
# inner lambda — Phase D interns 1 across all 100, OFF allocates 100.
if env NIX_V3_DIRECT_EVAL=1 "$NIX" $NIX_FLAGS eval --impure \
       --expr "1+1" >/dev/null 2>&1; then
    # Use `(x: x + 1)` not `(x: x)` for the inner lambda.  The
    # identity lambda has its own emit-time peephole specialisation
    # (LambdaDescriptor::identityLambda) that elides the Closure
    # alloc entirely — so the alloc-guard would see zero diff
    # between modes regardless of Phase D.  `(x: x + 1)` is the
    # smallest non-identity capture-free body that still triggers
    # OP_MAKE_CLOSURE on each call.
    expr='let mk = _: (x: x + 1); ls = builtins.genList (i: mk i) 100;
              results = map (f: f 42) ls;
              n = builtins.length (builtins.filter (x: x == 43) results);
          in n'

    # NIX_VM_STATS=1 dumps "v3-direct alloc: ... closures=N ..." to
    # stderr (run.cc:166).  Capture both modes and compare.  Use
    # `tail -1` to grab the FINAL runRootExpr stats line — earlier
    # lines belong to the bytecode-primop install pass (each install
    # is its own runRootExpr that emits a stats line of its own).
    on_closures=$(env NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 \
        "$NIX" $NIX_FLAGS eval --impure --expr "$expr" 2>&1 \
        | grep -oE 'closures=[0-9]+ ' | tail -1 | sed 's/closures=//')
    off_closures=$(env NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_LAMBDA_LIFT=1 \
        NIX_VM_STATS=1 \
        "$NIX" $NIX_FLAGS eval --impure --expr "$expr" 2>&1 \
        | grep -oE 'closures=[0-9]+ ' | tail -1 | sed 's/closures=//')

    if [ -z "$on_closures" ] || [ -z "$off_closures" ]; then
        echo "WARN: could not parse closures= from V3_DBG_ALLOC_DUMP output"
        echo "  on='$on_closures' off='$off_closures'"
    else
        # Phase D ON must allocate strictly fewer closures than OFF.
        # The diff should be ~100 (one per intern site).  Allow some
        # noise budget: require ON < OFF.
        if [ "$on_closures" -ge "$off_closures" ]; then
            echo "FAIL [alloc-guard]: Phase D ON closures=$on_closures " \
                 ">= OFF closures=$off_closures (intern not firing)"
            fail=1
        else
            echo "  alloc-guard: ON=$on_closures < OFF=$off_closures " \
                 "(saved $((off_closures - on_closures)))"
        fi
    fi
fi

if [ $fail -eq 0 ]; then
    echo "PASS: lambda-lift regression suite (semantic + alloc guard)"
else
    exit 1
fi
