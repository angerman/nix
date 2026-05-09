#!/usr/bin/env bash
# Regression tests for #546 -- OP_ATTRS_REC_INIT vs OP_ATTRS_LET_REC_INIT
# split.
#
# Background: lib.extends's `final: let prev = f final; in prev //
# overlay final prev` shape ran the let-rec's publishToNearest-
# BlackThunkFrame at OP_ATTRS_REC_INIT time, writing the size-1
# `{prev}` attrs onto lib.fix's outermost Black thunk's evaluated
# field.  A subsequent Tag::Slot deref of pkgs (= lib.fix's x) then
# returned `{prev}` instead of the in-progress merged_attrs, so
# `with pkgs; callPackage` looked up callPackage in `{prev}` and
# threw "name not found in with-scope".
#
# Fix: OP_ATTRS_LET_REC_INIT for `let-in-body` shapes (no publish),
# OP_ATTRS_REC_INIT preserved for `rec { ... }` literals (publishes,
# enabling `rec { x = 1; y = self.x; }` self-reference).  See
# CALLPACKAGE_BUG_2026-05-09.md for full analysis.
#
# Tests run under v3-direct (NIX_V3_DIRECT_EVAL=1) and compare to
# tree-walker output.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "nix CLI not found at $NIX" >&2
  exit 1
fi

ok=0; fail=0
fail_names=()

run_case() {
  local name="$1"
  local expr="$2"
  local expected="$3"

  local tw_out v3_out
  tw_out="$("$NIX" --extra-experimental-features "nix-command" eval --impure --expr "$expr" 2>&1 || echo "<TW-ERROR>")"
  v3_out="$(NIX_V3_DIRECT_EVAL=1 "$NIX" --extra-experimental-features "nix-command" eval --impure --expr "$expr" 2>&1 || echo "<V3-ERROR>")"

  if [[ "$tw_out" != "$expected" ]]; then
    echo "FAIL [$name]: TW unexpected output: '$tw_out' (wanted '$expected')"
    fail=$((fail + 1))
    fail_names+=("$name (TW)")
    return
  fi
  if [[ "$v3_out" != "$expected" ]]; then
    echo "FAIL [$name]: v3-direct unexpected output: '$v3_out' (wanted '$expected')"
    fail=$((fail + 1))
    fail_names+=("$name (v3)")
    return
  fi
  ok=$((ok + 1))
}

# ----------------- Positive: rec { ... } self-reference -----------------
# Must continue to publish so self-dot patterns work.

run_case "rec-self-dot" \
  'rec { x = 1; y = x + 1; }.y' \
  '2'

run_case "rec-self-dot-via-let-binding" \
  'let f = x: rec { a = x; b = a + 1; }; in (f 5).b' \
  '6'

run_case "rec-mutual-recursion" \
  'rec { a = 1; b = a; c = b + a; }.c' \
  '2'

# ----------------- Positive: let ... in body shapes -----------------
# The branch we changed.  These all should work with no-publish.

run_case "let-simple" \
  'let x = 1; in x + 1' \
  '2'

run_case "let-multi-binding" \
  'let x = 1; y = x + 1; in y * 2' \
  '4'

run_case "let-update-pattern" \
  'let prev = { a = 1; }; in prev // { b = 2; }' \
  '{ a = 1; b = 2; }'

# ----------------- Positive: lib.fix + lib.extends -----------------
# The actual lib.extends shape: `final: let prev = f final; in prev //
# overlay final prev`.  Single-stage form must work.

run_case "extends-single-stage" \
  'let lib = import <nixpkgs/lib>; in (lib.fix (lib.extends (final: prev: { bar = "X"; }) (self: { foo = "Y"; }))).bar' \
  '"X"'

run_case "extends-with-final" \
  'let lib = import <nixpkgs/lib>; in (lib.fix (lib.extends (final: prev: with final; { bar = foo + "/over"; }) (self: { foo = "base"; }))).bar' \
  '"base/over"'

# ----------------- Positive: default-bearing formals -----------------
# The synthetic LetRec inside lowerLambda for `{ a ? 1, b ? 2 }: a + b`
# is also marked hasBody=true.  Verify defaults still resolve.

run_case "formals-default-simple" \
  '({ a ? 1, b ? 2 }: a + b) {}' \
  '3'

run_case "formals-default-mutual" \
  '({ a ? 1, b ? a + 1 }: a + b) {}' \
  '3'

run_case "formals-default-with-arg" \
  '({ a ? 1, b ? 2 }: a + b) { a = 10; }' \
  '12'

# ----------------- Negative: schema rejection -----------------
# Old-format caches with OP_ATTRS_REC_INIT for hasBody=true cases must
# be rejected.  This is implicit (kSchemaVersion bumped to 6 + opcode
# fingerprint reset) -- can't repro at runtime without a stale cache,
# but the schema-mismatch test in run-disk-cache-tests.sh exercises
# the rejection path generically.

# ----------------- Summary -----------------
total=$((ok + fail))
echo
echo "let-rec publish-split tests: $ok/$total ok"
if [[ "$fail" -gt 0 ]]; then
  echo "Failed:"
  for n in "${fail_names[@]}"; do echo "  - $n"; done
  exit 1
fi
