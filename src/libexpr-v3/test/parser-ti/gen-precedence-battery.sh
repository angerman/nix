#!/usr/bin/env bash
# v3 parser TI.4 — operator-precedence battery generator.
#
# Per PARSER_PROJECT_PLAN_2026-06-01.md §4 (TI.4):
#   "Operator precedence battery (~80 fixtures) — currently ZERO
#    dedicated tests."  Per NATIVE_PARSER_FEASIBILITY §3.3 gap #2:
#   "14 precedence levels, ZERO dedicated precedence tests."
#
# This script authors the precedence/associativity battery as
# self-contained .nix fixtures (variables bound via wrapping lambdas
# so `nix-instantiate --parse` accepts them — --parse runs bindVars
# and rejects undefined variables), then generates the golden
# parenthesized AST via TW's nix-instantiate.
#
# The golden .exp files become the SHIP-gate corpus: the v3 parser
# (once it emits AST) must reproduce these byte-for-byte.
#
# Each fixture probes a specific precedence boundary or associativity
# rule.  The wrapping lambda is invisible-to-precedence: `a: b: c: <expr>`
# binds a/b/c without affecting how <expr> parses.
#
# Run once to (re)generate fixtures + goldens; commit both.
#   ./gen-precedence-battery.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -eu

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/builddir/src/nix/nix-instantiate}"
FIXDIR="$(cd "$(dirname "$0")" && pwd)/fixtures/precedence"

mkdir -p "$FIXDIR"

if [[ ! -x "$NIX_INST" ]]; then
    echo "gen-precedence-battery: nix-instantiate not found at $NIX_INST" >&2
    exit 2
fi

# Each entry: "name|expression".  The expression must bind all its
# free variables (wrapping lambda) so --parse's bindVars accepts it.
# Single-letter vars a/b/c/d/f keep the wrapping minimal + readable.
battery=(
  # --- associativity within each level ---
  "impl-right|a: b: c: a -> b -> c"
  "or-left|a: b: c: a || b || c"
  "and-left|a: b: c: a && b && c"
  "update-right|a: b: c: a // b // c"
  "add-left|a: b: c: a + b - c"
  "sub-left|a: b: c: a - b - c"
  "mul-left|a: b: c: a * b / c"
  "div-left|a: b: c: a / b / c"
  "concat-right|a: b: c: a ++ b ++ c"
  # --- cross-level precedence (loosest pairs first) ---
  "impl-vs-or|a: b: c: a -> b || c"
  "or-vs-and|a: b: c: a || b && c"
  "and-vs-eq|a: b: c: a && b == c"
  "and-vs-neq|a: b: c: a && b != c"
  "eq-vs-rel|a: b: c: a == b < c"
  "rel-vs-update|a: b: c: a < b // c"
  "update-vs-add|a: b: c: a // b + c"
  "add-vs-mul|a: b: c: a + b * c"
  "sub-vs-div|a: b: c: a - b / c"
  "mul-vs-concat|a: b: c: a * b ++ c"
  "concat-vs-hasattr|a: b: a ++ b ? x"
  # --- NOT (prefix !) placement ---
  "not-vs-and|a: b: !a && b"
  "not-vs-eq|a: b: !a == b"
  "not-vs-update|a: b: !a // b"
  "not-vs-add|a: b: !a + b"
  # --- NEGATE (unary -) placement ---
  "negate-vs-add|a: b: -a + b"
  "negate-vs-mul|a: b: -a * b"
  "negate-vs-concat|a: b: -a ++ b"
  "negate-vs-select|a: -a.b"
  # --- relational lowering to __lessThan ---
  "rel-lt|a: b: a < b"
  "rel-gt|a: b: a > b"
  "rel-leq|a: b: a <= b"
  "rel-geq|a: b: a >= b"
  # --- application binds tighter than all operators ---
  "app-left|f: a: b: f a b"
  "app-vs-add|f: a: b: f a + b"
  "app-vs-mul|f: a: b: f a * b"
  "app-vs-concat|f: a: b: f a ++ b"
  "app-vs-update|f: a: b: f a // b"
  # --- select (.) binds tighter than application ---
  "select-chain|a: a.b.c"
  "select-vs-app|f: a: f a.b"
  "app-of-select|a: f: a.b f"
  "select-or-default|a: b: a.b or b"
  "select-nested-or|a: b: a.b.c or b"
  # --- hasattr (?) ---
  "hasattr-simple|a: a ? x"
  "hasattr-path|a: a ? x.y"
  # --- mixed deep nesting ---
  "deep-arith|a: b: c: d: a + b * c - d"
  "deep-bool|a: b: c: d: a || b && c || d"
  "deep-mixed|a: b: c: a -> b && c == a"
  "paren-override|a: b: c: (a + b) * c"
  "paren-bool|a: b: c: (a || b) && c"
)

count=0
for entry in "${battery[@]}"; do
    name="${entry%%|*}"
    expr="${entry#*|}"
    nix_file="$FIXDIR/$name.nix"
    exp_file="$FIXDIR/$name.exp"
    printf '%s\n' "$expr" > "$nix_file"
    if ! out=$("$NIX_INST" --parse "$nix_file" 2>/dev/null); then
        echo "GEN ERROR: $name failed to parse: $expr" >&2
        rm -f "$nix_file"
        continue
    fi
    printf '%s\n' "$out" > "$exp_file"
    count=$((count + 1))
done

echo "Generated $count precedence fixtures + goldens in $FIXDIR"
