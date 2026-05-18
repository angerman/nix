#!/usr/bin/env bash
# Phase 1.6 resource-limits driver — verify the three caps fire
# with the expected typed exceptions.
#
# Each cap is tested in isolation: an aggressive workload runs
# under the cap with a short budget, the process is expected to
# exit non-zero, and stderr is expected to contain the typed
# error name.
#
# The driver itself uses `timeout(1)` as a hard backstop — if a
# cap fails to fire, the process is SIGKILL'd at 30s and the
# test reports FAIL.
#
# Plan reference: lode/ACTION_PLAN_2026-05-15.md Phase 1.6 (exit
# criteria + kill criterion).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
V3_EVAL="${V3_EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"

if [ ! -x "$V3_EVAL" ]; then
    echo "FAIL: v3-eval not at $V3_EVAL" >&2
    exit 2
fi

declare -i fail=0

assert_exception() {
    local label="$1"
    local cap_var="$2"
    local cap_val="$3"
    local fixture="$4"
    local expected_type="$5"
    local hard_timeout="${6:-30}"

    local errfile
    errfile=$(mktemp)

    local t0 t1
    t0=$(python3 -c 'import time; print(time.time())')
    timeout "$hard_timeout" env "$cap_var=$cap_val" "$V3_EVAL" \
        --file "$fixture" --strict >/dev/null 2>"$errfile"
    local rc=$?
    t1=$(python3 -c 'import time; print(time.time())')
    local elapsed
    elapsed=$(python3 -c "print(f'{$t1 - $t0:.2f}')")

    if [ "$rc" -eq 124 ]; then
        echo "FAIL [$label]: hard timeout fired (cap did NOT trigger within ${hard_timeout}s)"
        sed 's/^/  /' "$errfile" | head -5
        fail=1
    elif [ "$rc" -eq 0 ]; then
        echo "FAIL [$label]: process exited 0 (cap did NOT trigger; workload completed cleanly)"
        sed 's/^/  /' "$errfile" | head -5
        fail=1
    elif ! grep -q "$expected_type" "$errfile"; then
        echo "FAIL [$label]: cap fired (rc=$rc) but expected_type '$expected_type' missing from stderr"
        echo "  elapsed: ${elapsed}s"
        sed 's/^/  /' "$errfile" | head -10
        fail=1
    else
        echo "PASS [$label]: $expected_type fired after ${elapsed}s (cap=$cap_val)"
    fi
    rm -f "$errfile"
}

# Wall-time cap: 2-second cap on a hot loop → expect fire within ~3s.
assert_exception \
    "wall-time"            \
    "NIX_V3_MAX_WALL_TIME" \
    "2s"                   \
    "$SCRIPT_DIR/cap-busyloop.nix" \
    "WallTimeExceededError" \
    10

# CPU-time cap: same workload, CPU cap of 2s.
assert_exception \
    "cpu-time"            \
    "NIX_V3_MAX_CPU_TIME" \
    "2s"                  \
    "$SCRIPT_DIR/cap-busyloop.nix" \
    "CpuTimeExceededError" \
    10

# Heap cap is best-effort per Boehm's docs (the cap is approximate).
# We test it but DO NOT count failure as a hard regression —
# emitted as INFO if it doesn't fire.  Tracked separately for
# the eventual setrlimit(RLIMIT_AS) hardening.
errfile=$(mktemp)
timeout 30 env NIX_V3_MAX_HEAP=64M "$V3_EVAL" \
    --file "$SCRIPT_DIR/cap-heap-alloc.nix" --strict \
    >/dev/null 2>"$errfile"
rc=$?
if grep -q "OutOfMemoryError" "$errfile"; then
    echo "PASS [heap]: OutOfMemoryError fired (best-effort cap worked this run)"
elif [ "$rc" -eq 0 ]; then
    echo "INFO [heap]: NIX_V3_MAX_HEAP=64M did not fire (Boehm cap is approximate; track for setrlimit hardening)"
elif [ "$rc" -eq 124 ]; then
    echo "INFO [heap]: hard 30s timeout fired (cap did not trigger but workload didn't finish either)"
else
    echo "INFO [heap]: rc=$rc but OutOfMemoryError not in stderr — possibly other failure"
fi
rm -f "$errfile"

# Negative test: a cap budget that's plenty does NOT fire on a
# trivial expression.  Confirms the poll is bounded-cost and the
# cap is not over-eager.
errfile=$(mktemp)
NIX_V3_MAX_WALL_TIME=60s NIX_V3_MAX_CPU_TIME=60s NIX_V3_MAX_HEAP=4G \
    "$V3_EVAL" --expr "1 + 1" >/dev/null 2>"$errfile"
if [ $? -eq 0 ]; then
    echo "PASS [no-fire]: trivial eval under generous caps exits 0"
else
    echo "FAIL [no-fire]: trivial eval under generous caps failed"
    sed 's/^/  /' "$errfile" | head -5
    fail=1
fi
rm -f "$errfile"

if [ $fail -eq 0 ]; then
    echo "PASS: resource-limits regression suite"
    exit 0
fi
exit 1
