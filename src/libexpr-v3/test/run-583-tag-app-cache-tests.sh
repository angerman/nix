#!/usr/bin/env bash
# Test driver for A12 Tag::App-cache fix (commit 189497b81 +
# the broader valueEqual/valueLess writebacks).
#
# Tests are split into positive/negative/regression categories.
# Each case runs under TW (oracle) and v3-direct (subject) and
# verifies output parity AND, where applicable, FORCE-trace count
# parity.
set -u

cd "$(dirname "$0")/../../.."

NIX_BIN=build/src/nix/nix
TESTDIR=src/libexpr-v3/test

if [ ! -x "$NIX_BIN" ]; then
    echo "ERROR: $NIX_BIN not built; run ninja first" >&2
    exit 2
fi

fail=0
pass=0

# Run a file under both modes and compare outputs.  If $4 is non-empty,
# also count FORCE traces and assert v3 matches TW.
check_parity() {
    local label="$1"
    local file="$2"
    local expect="$3"
    local count_traces="${4:-}"

    local tw_out tw_traces v3_out v3_traces

    tw_out=$("$NIX_BIN" eval --impure --file "$file" 2>&1) || true
    v3_out=$(NIX_V3_DIRECT_EVAL=1 \
        "$NIX_BIN" eval --impure --file "$file" 2>&1) || true

    # Strip trace lines for output comparison (traces go to stderr,
    # but builtins.trace also prints to stderr by default).
    local tw_stripped v3_stripped
    tw_stripped=$(printf "%s" "$tw_out" | grep -v "^trace:\|^warning:" | tail -1)
    v3_stripped=$(printf "%s" "$v3_out" | grep -v "^trace:\|^warning:" | tail -1)

    if [ "$tw_stripped" != "$expect" ]; then
        echo "FAIL  [$label] TW: expected '$expect', got '$tw_stripped'"
        fail=$((fail + 1))
        return
    fi
    if [ "$v3_stripped" != "$expect" ]; then
        echo "FAIL  [$label] v3: expected '$expect', got '$v3_stripped'"
        echo "                full v3 output: $v3_out"
        fail=$((fail + 1))
        return
    fi

    if [ -n "$count_traces" ]; then
        tw_traces=$(printf "%s" "$tw_out" | grep -c "^trace: $count_traces" || true)
        v3_traces=$(printf "%s" "$v3_out" | grep -c "^trace: $count_traces" || true)
        if [ "$tw_traces" != "$v3_traces" ]; then
            echo "FAIL  [$label] trace-count: TW=$tw_traces v3=$v3_traces (pattern '$count_traces')"
            fail=$((fail + 1))
            return
        fi
        echo "PASS  [$label] output='$expect' traces=$tw_traces (parity)"
    else
        echo "PASS  [$label] output='$expect'"
    fi

    pass=$((pass + 1))
}

# Run a file expected to THROW; assert both TW and v3 produce error
# messages matching $4.
check_throws() {
    local label="$1"
    local file="$2"
    local expect_msg="$3"

    local tw_out v3_out

    tw_out=$("$NIX_BIN" eval --impure --file "$file" 2>&1) || true
    v3_out=$(NIX_V3_DIRECT_EVAL=1 \
        "$NIX_BIN" eval --impure --file "$file" 2>&1) || true

    if ! printf "%s" "$tw_out" | grep -q "$expect_msg"; then
        echo "FAIL  [$label] TW didn't throw '$expect_msg'; output: $tw_out"
        fail=$((fail + 1))
        return
    fi
    if ! printf "%s" "$v3_out" | grep -q "$expect_msg"; then
        echo "FAIL  [$label] v3 didn't throw '$expect_msg'; output: $v3_out"
        fail=$((fail + 1))
        return
    fi

    echo "PASS  [$label] both threw '$expect_msg'"
    pass=$((pass + 1))
}

echo "=== A12 Tag::App-cache fix tests ==="

# POSITIVE: caching matches TW.
check_parity "POS-1 primElem cache" \
    "$TESTDIR/repro-583-tag-app-cache-positive-1.nix" \
    "0" \
    "FORCE-"

check_parity "POS-2 valueEqual List cache" \
    "$TESTDIR/repro-583-tag-app-cache-positive-2.nix" \
    "\"equal\"" \
    "[AB]-"

check_parity "POS-3 valueEqual Attrs cache" \
    "$TESTDIR/repro-583-tag-app-cache-positive-3.nix" \
    "\"equal\"" \
    "[AB]-"

# NEGATIVE: laziness preserved.
check_parity "NEG-1 length on lazy mapAttrs" \
    "$TESTDIR/repro-583-tag-app-cache-negative-1.nix" \
    "3"

check_throws "NEG-2 throw propagates from forced entry" \
    "$TESTDIR/repro-583-tag-app-cache-negative-2.nix" \
    "from-b"

# REGRESSION: parsedPlatform-shaped workload.
check_parity "REG-1 parsedPlatform.check shape" \
    "$TESTDIR/repro-583-tag-app-cache-regression-1.nix" \
    "0" \
    "FORCE-"

# POSITIVE: A12b iterative OP_CALL fun-force.
# Exercises OP_CALL's iterative `fun` force (op_force_slow +
# writeback slot) on a Tag::App fun (from primMapAttrs) — including a
# 200-step cascade.  Correctness check; parity with TW required.
check_parity "POS-4 OP_CALL iter-force" \
    "$TESTDIR/repro-a12b-op-call-iter-force.nix" \
    "\"ok\""

echo
echo "=== Results: $pass pass, $fail fail ==="
exit $fail
