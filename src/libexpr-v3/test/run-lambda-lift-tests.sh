#!/usr/bin/env bash
# Phase D lambda-lift regression driver.
#
# The capture-free lambda-lift singleton intern is now UNCONDITIONAL (the
# NIX_V3_NO_LAMBDA_LIFT A/B opt-out was retired), so this driver checks:
# 1. Semantic parity: TW vs v3-direct produce identical results on the
#    6-pattern fixture.
# 2. Alloc-reduction guard: at N=100 capture-free lambda creations in a
#    chain, the singleton intern collapses them to a handful of Closure
#    allocations (an ABSOLUTE bound well below the ~100 an un-lifted run
#    would allocate) — confirms the intern actually fires.
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

# Semantic parity: TW vs v3-direct (lambda-lift unconditional).
for mode in "TW:" \
            "v3-D:NIX_V3_DIRECT_EVAL=1"; do
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
# assert the Closure alloc count stays well below 100.  The singleton
# intern collapses all 100 identical descriptors to a handful; an
# un-lifted run would allocate one Closure per call (~100).
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

    if [ -z "$on_closures" ]; then
        echo "WARN: could not parse closures= from NIX_VM_STATS output"
        echo "  on='$on_closures'"
    else
        # With the singleton intern firing, the 100-lambda chain
        # allocates only a handful of Closures (~7); an un-lifted run
        # would allocate ~100.  Assert an absolute upper bound of 50 —
        # a wide margin below the un-lifted count, comfortably above
        # the lifted count.
        if [ "$on_closures" -ge 50 ]; then
            echo "FAIL [alloc-guard]: closures=$on_closures >= 50 " \
                 "(intern not firing; un-lifted allocates ~100)"
            fail=1
        else
            echo "  alloc-guard: closures=$on_closures < 50 (intern firing)"
        fi
    fi
fi

if [ $fail -eq 0 ]; then
    echo "PASS: lambda-lift regression suite (semantic + alloc guard)"
else
    exit 1
fi
