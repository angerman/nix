#!/usr/bin/env bash
# v3 hot-path lint: no inline std::getenv() inside vm.cc dispatch loop.
#
# The 2026-05-09 perf session found seven inline `std::getenv(...)`
# calls inside vm.cc opcode handlers — each fired on every dispatched
# opcode (millions of times for fib).  Caching them as static-const-
# bool with `__builtin_expect(_, 0) [[unlikely]]` cut fib33 wall time
# in half (from 2.4× TW to 0.65× TW).
#
# This lint guards against future contributors silently re-introducing
# the per-call cost.  It greps the dispatch-loop region of vm.cc for
# `std::getenv(` calls that are NOT preceded by `static const` on the
# same or prior line.
#
# Allowlist: lines inside `forceValue(VMState&, Value)` and other slow-
# path helper bodies are still scanned -- the perf cost per call is
# the same.  Lines inside #if 0 or comments are exempt by the simple
# regex (we look at lexical structure, not parse).
#
# Usage:
#   bash src/libexpr-v3/test/lint-no-inline-getenv.sh
# Exit codes:
#   0 - no offenders found
#   1 - one or more inline getenv calls found in hot paths
#   2 - lint preflight failed (file missing, etc.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
VM="${ROOT}/src/libexpr-v3/vm.cc"
PRIMOPS="${ROOT}/src/libexpr-v3/primops.cc"

if [[ ! -f "$VM" ]]; then
    echo "lint-no-inline-getenv: cannot find $VM" >&2
    exit 2
fi

# scan_file PATH -- prints any inline std::getenv() call to stderr
# along with the previous line for context.  Returns the number of
# offenders found via printed count.
scan_file() {
    local path="$1"
    local offenders=0
    # Use awk to track line context: previous non-blank line.
    # Whitelist:
    #   - lines containing `static const` on the same line OR within
    #     2 prior non-blank lines (the canonical
    #     `static const bool s_X = std::getenv("Y") != nullptr;`
    #     pattern, which works whether the assignment fits on one
    #     line or two).
    #   - lines inside C++ comments (start with `//` or `*` after
    #     trim).
    #   - the lambda-init pattern
    #     `static const TYPE k = []{ ... std::getenv(...) ... }();`
    #     where the getenv is inside a lambda body that ran ONCE at
    #     function-static initialization time.  Detected by finding
    #     a `[]{` or `[](` within the previous 5 non-blank lines.
    awk -v path="$path" '
        function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
        function looks_safe(idx) {
            for (i = 0; i < 5 && (idx-i) >= 1; i++) {
                line = recent[(idx-i) % 5]
                if (line ~ /static const/ || line ~ /\[\][[:space:]]*\{/ \
                    || line ~ /\[\][[:space:]]*\(/) {
                    return 1
                }
            }
            return 0
        }
        {
            recent[NR % 5] = $0
            t = trim($0)
            # Skip comments + blank lines.
            if (t == "" || t ~ /^\/\// || t ~ /^\*/) next
            # Look for std::getenv call.
            if ($0 ~ /std::getenv\(/) {
                # Same-line guard: the assignment includes static const
                # on the same line.
                if ($0 ~ /static const/) next
                # Allowlist: builtins.getEnv primop legitimately calls
                # getenv with a USER-supplied string (args[0].payload).
                # This is the *implementation* of getEnv, not a debug
                # gate, so caching is impossible — the arg varies.
                if ($0 ~ /std::getenv\(args\[/) next
                # Explicit cold-site allowlist: a `lint:allow-getenv`
                # marker on the same line exempts a provably-cold one-off
                # call (e.g. a static-const fingerprint built once at load
                # time, where the lambda opener is too far back for the
                # 5-line []{...} lookback to catch).  The author must
                # justify coldness in the marker comment.
                if ($0 ~ /lint:allow-getenv/) next
                # Prior-line guard.
                if (looks_safe(NR)) next
                printf "%s:%d: inline std::getenv(): %s\n",
                       path, NR, t
                offenders++
            }
        }
        END { exit offenders > 0 ? 1 : 0 }
    ' "$path"
}

OFFENDERS=0

if ! out=$(scan_file "$VM"); then
    OFFENDERS=$((OFFENDERS + 1))
    echo "$out" >&2
fi

if [[ -f "$PRIMOPS" ]]; then
    if ! out=$(scan_file "$PRIMOPS"); then
        OFFENDERS=$((OFFENDERS + 1))
        echo "$out" >&2
    fi
fi

if [[ $OFFENDERS -gt 0 ]]; then
    cat >&2 <<EOF

lint-no-inline-getenv: found inline std::getenv() in hot paths.

The fix is to convert each call to a cached static-const-bool with
the canonical pattern:

    static const bool s_dbgX =
        std::getenv("X") != nullptr;
    if (__builtin_expect(s_dbgX, 0) [[unlikely]]) {
        ...
    }

See vm.cc:1808 (s_dbgFinalCall) for an exemplar.  Per-call
std::getenv() costs ~50ns on macOS due to the env-table strcmp
walk; on a 5.7M-call workload (fib33), that's 285ms — the difference
between "v3 is faster than TW" and "v3 is 1.5x slower".  Don't
regress this without a measured reason.

If your call site is provably cold (e.g. a one-off init or error
path), add a comment justifying the inline call so future scans
don't re-flag it, and consider routing it through this lint's
allow list (look for the static-const guard near the call).
EOF
    exit 1
fi

echo "lint-no-inline-getenv: clean (no inline getenvs in vm.cc hot paths)"
exit 0
