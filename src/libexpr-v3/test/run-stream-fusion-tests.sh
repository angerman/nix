#!/usr/bin/env bash
# Phase C stream-fusion regression driver.
#
# 1. semantic parity: fixture results match between fusion ON and OFF.
# 2. perf guard: a N=100K foldl'+map runs within 5s wall-clock with
#    fusion ON (catches the C-recursive primFoldlMap dispatch
#    regression — 60x slowdown — if anyone removes the bytecode
#    install or reverts the App-chain emit).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
FIXTURE="$(cd "$(dirname "$0")" && pwd)/repro-stream-fusion.nix"

if [ ! -x "$NIX" ]; then
    echo "FAIL: nix binary not found: $NIX" >&2
    exit 1
fi

NIX_FLAGS="--extra-experimental-features nix-command --extra-experimental-features flakes"

# Run the fixture and verify result attrs match expected.
run_fixture() {
    local label="$1"
    local extra_env="$2"
    local attr="$3"
    local expected="$4"

    # Run with v3-direct enabled so the stream-fusion pass is actually
    # exercised.  Without NIX_V3_DIRECT_EVAL=1, the expression goes
    # through TW and the NIX_V3_NO_STREAM_FUSION gate has no effect —
    # the test was previously a no-op.
    #
    # Important: do NOT redirect stderr to /dev/null — if v3 emit
    # fails (e.g. "unbound VarId" from a bad hoist), the empty stdout
    # would silently masquerade as an empty string that happens to
    # match no expected value.  Capture stderr to a temp file and
    # surface it on failure.
    local val errfile
    errfile=$(mktemp)
    val=$(env NIX_V3_DIRECT_EVAL=1 $extra_env \
        "$NIX" $NIX_FLAGS eval --impure \
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
for mode in "OFF:NIX_V3_NO_STREAM_FUSION=1" "ON:"; do
    label="${mode%%:*}"
    env_part="${mode##*:}"

    run_fixture "$label" "$env_part" small      90      || fail=1
    run_fixture "$label" "$env_part" mid        9900    || fail=1
    run_fixture "$label" "$env_part" mixed      360360  || fail=1
    run_fixture "$label" "$env_part" empty      42      || fail=1
    run_fixture "$label" "$env_part" singleton  90      || fail=1
done

# Cross-check string result.  Same v3-direct enforcement as above.
for mode in "OFF:NIX_V3_NO_STREAM_FUSION=1" "ON:"; do
    label="${mode%%:*}"
    env_part="${mode##*:}"
    errfile=$(mktemp)
    val=$(env NIX_V3_DIRECT_EVAL=1 $env_part "$NIX" $NIX_FLAGS eval --impure --raw \
        --expr "(import $FIXTURE).strings" 2>"$errfile")
    if [ "$val" != "1,2,3," ]; then
        echo "FAIL [$label]: strings = '$val' (expected '1,2,3,')"
        if [ -s "$errfile" ]; then
            echo "  stderr:"
            sed 's/^/    /' "$errfile" | head -10
        fi
        fail=1
    fi
    rm -f "$errfile"
done

# Perf guard: N=100K foldl' + map under v3-direct must finish quickly.
# The pre-fix bytecode primop (PrimOpCall shape over __foldlMap) took
# 60x as long; this guard catches a re-regression of that pattern.
# Skip when v3-direct gate isn't available (e.g. running with TW only).
if env NIX_V3_DIRECT_EVAL=1 "$NIX" $NIX_FLAGS eval --impure \
       --expr "1+1" >/dev/null 2>&1; then
    expr='let xs = builtins.genList (i: i) 100000;
              r = builtins.foldl'"'"' (acc: x: acc + x) 0 (map (x: x * 2) xs);
          in r'

    t0=$(perl -MTime::HiRes=time -e 'print time()' 2>/dev/null \
         || python3 -c 'import time; print(time.time())')
    NIX_V3_DIRECT_EVAL=1 "$NIX" $NIX_FLAGS eval --impure \
        --expr "$expr" >/dev/null 2>&1
    t1=$(perl -MTime::HiRes=time -e 'print time()' 2>/dev/null \
         || python3 -c 'import time; print(time.time())')

    elapsed=$(python3 -c "print($t1 - $t0)")
    # 5s guard: actual on the dev machine is ~0.25s; the regressed
    # PrimOpCall shape measured 15s.  Wide enough margin to survive
    # CI variance.
    over=$(python3 -c "print(1 if $elapsed > 5.0 else 0)")
    if [ "$over" = "1" ]; then
        echo "FAIL [perf guard]: N=100K foldl'+map took ${elapsed}s (>5s budget)"
        echo "  This typically means __foldlMap is being invoked as a"
        echo "  C primop instead of via the bytecode-closure replacement"
        echo "  (App-chain shape over LitPrimOp).  See opt_stream_fusion.cc."
        fail=1
    fi
fi

if [ $fail -eq 0 ]; then
    echo "PASS: stream fusion regression suite (semantic + perf guard)"
else
    exit 1
fi
