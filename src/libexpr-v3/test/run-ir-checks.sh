#!/usr/bin/env bash
# IR-CHECK runner — lit-like driver for test/ir-fixtures/*.nix.
#
# For each fixture:
#   1. Extract `# RUN:` lines (one shell command each).
#   2. Substitute %s → fixture path, %t → per-test temp file.
#   3. Execute each RUN: as a shell command (in a subshell, with the
#      build's bin path prepended so `v3-eval`/`v3-check` resolve).
#   4. Aggregate exit codes.  Fixture PASSes iff every RUN: exits 0.
#
# Usage:
#   $ src/libexpr-v3/test/run-ir-checks.sh [--build DIR] [--filter REGEX]
#
# Env:
#   BUILD       build directory (default: build)
#   V3_FIXTURES override fixture directory
#
# Exit code: 0 if every fixture passes; non-zero otherwise.
#
# See lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md §3 for the design.
set -uo pipefail

# Don't `set -e` — we want to inspect each RUN's exit code without
# aborting the runner.

# Resolve repo root (this script lives at src/libexpr-v3/test/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURES_DIR="${V3_FIXTURES:-$SCRIPT_DIR/ir-fixtures}"

# Locate v3-eval and v3-check.  Priority order:
#   1. V3_BUILD_BIN env (absolute path to the build's libexpr-v3 dir)
#      — used by meson.global_build_root() / src / libexpr-v3.
#   2. BUILD env (legacy: $ROOT/$BUILD/src/libexpr-v3) — used by the
#      manual invocation `./src/libexpr-v3/test/run-ir-checks.sh`.
#   3. Default: $ROOT/build/src/libexpr-v3.
if [ -n "${V3_BUILD_BIN:-}" ]; then
    BIN_DIR="$V3_BUILD_BIN"
else
    BUILD="${BUILD:-build}"
    BIN_DIR="$ROOT/$BUILD/src/libexpr-v3"
fi

# Make `v3-eval` and `v3-check` resolve as plain names in RUN: lines.
# This matches LLVM's `lit` model where the build's bin dir is on PATH.
export PATH="$BIN_DIR:$PATH"

# Sanity check tools.
if ! command -v v3-eval >/dev/null 2>&1; then
    echo "FAIL: v3-eval not on PATH (looked under $BIN_DIR)" >&2
    exit 2
fi
if ! command -v v3-check >/dev/null 2>&1; then
    echo "FAIL: v3-check not on PATH (looked under $BIN_DIR)" >&2
    exit 2
fi

filter=""
verbose=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build)    shift; BUILD="$1"; shift ;;
        --filter)   shift; filter="$1"; shift ;;
        --verbose|-v) verbose=1; shift ;;
        --help|-h)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        *)
            echo "run-ir-checks: unknown arg '$1'" >&2
            exit 2
            ;;
    esac
done

declare -i total=0 passes=0 fails=0
declare -a failed_fixtures=()

shopt -s nullglob
for fixture in "$FIXTURES_DIR"/*.nix; do
    [ -f "$fixture" ] || continue
    if [ -n "$filter" ] && ! [[ "$fixture" =~ $filter ]]; then
        continue
    fi
    total=$((total + 1))

    # Extract RUN: lines.  Strip the `# RUN: ` prefix.  Accept any
    # amount of whitespace around the `#` and after `RUN:`.
    runs=$(grep -E '^[[:space:]]*#[[:space:]]*RUN:[[:space:]]*' "$fixture" \
           | sed -E 's/^[[:space:]]*#[[:space:]]*RUN:[[:space:]]*//')
    if [ -z "$runs" ]; then
        [ "$verbose" -eq 1 ] && echo "SKIP: $(basename "$fixture") (no # RUN: line)"
        total=$((total - 1))
        continue
    fi

    ok=1
    line_idx=0
    tmpdir="$(mktemp -d)"
    while IFS= read -r runline; do
        [ -z "$runline" ] && continue
        line_idx=$((line_idx + 1))
        # Substitute LLVM-`lit`-style tokens.
        tmp_t="$tmpdir/$(basename "$fixture").$line_idx.t"
        cmd="${runline//%s/$fixture}"
        cmd="${cmd//%t/$tmp_t}"

        if [ "$verbose" -eq 1 ]; then
            echo "  RUN[$line_idx]: $cmd"
        fi
        # Run in a subshell to isolate cd / env mutations.
        if ! ( eval "$cmd" ) >"$tmpdir/out.$line_idx" 2>"$tmpdir/err.$line_idx"; then
            ok=0
            echo "FAIL: $(basename "$fixture") — RUN line #$line_idx exit non-zero"
            echo "  cmd: $cmd"
            if [ -s "$tmpdir/err.$line_idx" ]; then
                echo "  stderr:"
                sed 's/^/    /' "$tmpdir/err.$line_idx" | head -25
            fi
            if [ -s "$tmpdir/out.$line_idx" ]; then
                echo "  stdout (last 10 lines):"
                tail -n 10 "$tmpdir/out.$line_idx" | sed 's/^/    /'
            fi
        fi
    done <<< "$runs"
    rm -rf "$tmpdir"

    if [ $ok -eq 1 ]; then
        passes=$((passes + 1))
        [ "$verbose" -eq 1 ] && echo "PASS: $(basename "$fixture")"
    else
        fails=$((fails + 1))
        failed_fixtures+=("$(basename "$fixture")")
    fi
done

echo
echo "=============================="
echo "  IR-CHECK summary: total=$total  pass=$passes  fail=$fails"
echo "=============================="
if [ $fails -gt 0 ]; then
    echo "Failed fixtures:"
    for f in "${failed_fixtures[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
exit 0
