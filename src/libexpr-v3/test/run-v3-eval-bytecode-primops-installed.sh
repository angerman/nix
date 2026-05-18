#!/usr/bin/env bash
# Regression test for commit 170167498: v3-eval must call
# `installAllBytecodePrimops(state)` before lowering the user
# expression.  Without this call, derivation evaluation falls into
# the C primDerivation -> primDerivationStrict -> primDerivationStrictNative
# chain whose direct C-recursive forceValue helpers overflow the
# C stack on deep nixpkgs eval (SIGBUS on hello.drvPath et al).
#
# Strategy: invoke v3-eval on a synthetic derivation expression with
# NIX_VM_STATS=1.  Assert that the `__derivationFromPreprocessed`
# primop (the v3-native FFI leaf called by the bytecode wrapper)
# appears in the dumped primop call counts — this proves the
# bytecode wrapper for `derivationStrict` was actually invoked.
#
# If a future change reverts v3-eval to bypass `installAllBytecodePrimops`,
# this test fails because `derivationStrict` (the unwrapped C primop)
# would show up instead.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
V3_EVAL="${V3_EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"

if [ ! -x "$V3_EVAL" ]; then
    echo "FAIL: v3-eval not at $V3_EVAL" >&2
    exit 2
fi

errfile=$(mktemp)
outfile=$(mktemp)

NIX_VM_STATS=1 "$V3_EVAL" --expr \
    '(derivation { name = "tst"; system = "x86_64-linux"; builder = "/bin/sh"; }).drvPath' \
    >"$outfile" 2>"$errfile"
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "FAIL: v3-eval exited rc=$rc" >&2
    echo "--- stderr (last 20):"
    tail -20 "$errfile" >&2
    rm -f "$outfile" "$errfile"
    exit 1
fi

# Positive assertion: __derivationFromPreprocessed must be in the stats.
if ! grep -q "__derivationFromPreprocessed" "$errfile"; then
    echo "FAIL: __derivationFromPreprocessed not in primop stats" >&2
    echo "  => bytecode wrapper for derivationStrict was NOT invoked" >&2
    echo "  => v3-eval likely bypassed installAllBytecodePrimops" >&2
    echo "--- stderr (last 30):"
    tail -30 "$errfile" >&2
    rm -f "$outfile" "$errfile"
    exit 1
fi

# Negative assertion: derivationStrict (the unwrapped C primop) must
# NOT appear.  If it does, the bytecode wrapper was bypassed AND fell
# through to the C primop.
if grep -qE "^[[:space:]]+[0-9]+[[:space:]]+derivationStrict[[:space:]]*$" "$errfile"; then
    echo "FAIL: unwrapped C 'derivationStrict' primop was invoked" >&2
    echo "  => bytecode wrapper bypassed" >&2
    echo "--- stderr (last 30):"
    tail -30 "$errfile" >&2
    rm -f "$outfile" "$errfile"
    exit 1
fi

echo "PASS: v3-eval invokes bytecode wrapper for derivationStrict"
rm -f "$outfile" "$errfile"
exit 0
