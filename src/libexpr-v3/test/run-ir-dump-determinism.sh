#!/usr/bin/env bash
# IR-dump determinism guard — §1.7 of IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md.
#
# For each expression in the corpus, run `v3-eval --emit-ir` THREE
# times and assert byte-identical output across all runs.  Catches
# any nondeterminism introduced by:
#   - std::unordered_map iteration over symbols (hash-randomised).
#   - Pointer-order-dependent traversal in opt passes.
#   - Set iteration in freeVars (must be sorted before dump —
#     ir.cc:332 already does this).
#   - VarId / BlockId assignment that varies across runs.
#
# Without this guard, fixtures would be silently flaky: pass on the
# author's machine, fail in CI.  Loud determinism failure is much
# easier to debug than intermittent fixture flake.
set -uo pipefail

if [ -n "${V3_BUILD_BIN:-}" ]; then
    BIN_DIR="$V3_BUILD_BIN"
else
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
    BIN_DIR="$ROOT/${BUILD:-build}/src/libexpr-v3"
fi
V3_EVAL="$BIN_DIR/v3-eval"

if [ ! -x "$V3_EVAL" ]; then
    echo "FAIL: v3-eval not at $V3_EVAL" >&2
    exit 2
fi

# Representative corpus.  Each line is one expression to exercise.
# Picked to cover: literals, lambdas, let-rec, captures, primops,
# attrsets, lists, with-blocks, conditionals.
declare -a corpus=(
    '42'
    '2 * 3'
    '"hello" + "world"'
    '(x: x * 2) 21'
    '(x: y: x + y) 5'
    'let n = 10; in x: x + n'
    'let xs = [1 2 3 4 5]; in builtins.length xs'
    'builtins.foldl'"'"' (a: b: a + b) 0 (map (x: x * 2) [1 2 3 4 5])'
    'rec { a = 1; b = a + 1; c = a + b; }'
    'let attrs = { x = 1; y.z = 2; }; in attrs.y.z'
    'with { v = 100; }; v + 1'
    'if 1 < 2 then "yes" else "no"'
    'let f = x: y: x * y; in f 3 4'
)

declare -a modes=( "--emit-ir" "--emit-ir-raw" )

fail=0
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

for mode in "${modes[@]}"; do
    for i in "${!corpus[@]}"; do
        expr="${corpus[$i]}"
        # Run 3 times, capture stdout.
        for run in 1 2 3; do
            "$V3_EVAL" --expr "$expr" "$mode" \
                > "$tmpdir/out.$mode.$i.$run" 2>"$tmpdir/err.$mode.$i.$run"
            rc=$?
            if [ $rc -ne 0 ]; then
                echo "FAIL: '$mode $expr' exited $rc on run $run" >&2
                if [ -s "$tmpdir/err.$mode.$i.$run" ]; then
                    sed 's/^/  /' "$tmpdir/err.$mode.$i.$run" >&2
                fi
                fail=1
                continue 2  # next expr
            fi
        done
        # Diff the three runs.
        d12=$(diff "$tmpdir/out.$mode.$i.1" "$tmpdir/out.$mode.$i.2")
        d23=$(diff "$tmpdir/out.$mode.$i.2" "$tmpdir/out.$mode.$i.3")
        if [ -n "$d12" ] || [ -n "$d23" ]; then
            echo "FAIL: non-deterministic dump for '$mode $expr'" >&2
            echo "  run1 vs run2:" >&2
            echo "$d12" | head -20 | sed 's/^/    /' >&2
            echo "  run2 vs run3:" >&2
            echo "$d23" | head -20 | sed 's/^/    /' >&2
            fail=1
        fi
    done
done

if [ $fail -eq 0 ]; then
    echo "PASS: IR-dump determinism (${#corpus[@]} exprs × ${#modes[@]} modes × 3 runs = $((${#corpus[@]} * ${#modes[@]} * 3)) total)"
    exit 0
fi
exit 1
