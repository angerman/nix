#!/usr/bin/env bash
# Regression test for #676 — `nix eval --impure --apply ...` must not
# SIGTRAP when the apply function returns a value not derived from arg.
#
# Pre-fix:
#   RootResult was returned by value from runRootExpr; std::optional::
#   emplace MOVED it in src/nix/eval.cc.  Closures embedded in the
#   apply result had `c->cu` baked to `&local_out.cu` at OP_MAKE_CLOSURE
#   time; the move shifted the CU to a new address.  When callClosure
#   tail-called dispatchLoop, the closure's stale cu pointer made
#   dispatchLoop read garbage bytecode → SIGTRAP (exit 133) at a brk #1
#   landing pad inside std::vector::__throw_length_error.
#
# Why a shell driver instead of a `repro-*.nix` fixture:
#   The bug only manifests when runRootExpr is called TWICE (once for
#   --expr, once for --apply) AND the apply-result closure dispatches
#   into its bytecode body.  A standalone .nix file resolves all
#   lambda applications inside the SAME runRootExpr call and never
#   trips the move-across-emplace path.
#
# Fix (this commit): RootResult holds `std::unique_ptr<CompilationUnit>`
# so the CU is heap-stable; moves of RootResult shift the unique_ptr
# but the CU's heap address never changes, keeping all closure cu
# pointers valid.
#
# Each case below was a confirmed SIGTRAP pre-fix; post-fix all must
# match TW byte-for-byte.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "run-676: nix not executable at $NIX" >&2
  exit 2
fi

run_case() {
  local label="$1" apply="$2" expr="$3"
  local tw v3
  tw="$("$NIX" eval --impure --apply "$apply" --expr "$expr" 2>/dev/null || true)"
  v3="$(NIX_V3_DIRECT_EVAL=1 \
        NIX_V3_MAX_WALL_TIME=10s \
        "$NIX" eval --impure --apply "$apply" --expr "$expr" 2>/dev/null || true)"
  if [[ "$tw" == "$v3" && -n "$tw" ]]; then
    printf "  MATCH    %-30s => %s\n" "$label" "${tw:0:60}"
    return 0
  else
    printf "  DIVERGE  %-30s\n    TW: %s\n    V3: %s\n" "$label" "$tw" "$v3"
    return 1
  fi
}

fail=0
run_case "const-int"      '(x: 42)'              '1'    || fail=$((fail+1))
run_case "const-string"   '(x: "lit")'           '1'    || fail=$((fail+1))
run_case "const-attrs"    '(x: { a = 1; })'      '1'    || fail=$((fail+1))
run_case "const-list"     '(x: [ 1 2 3 ])'       '1'    || fail=$((fail+1))
run_case "arith-on-arg"   '(x: x + 1)'           '41'   || fail=$((fail+1))
run_case "deref-attr"     '(x: x.outPath)'       '{ outPath = "/store/abc"; }' || fail=$((fail+1))
run_case "identity"       '(x: x)'               '7'    || fail=$((fail+1))
run_case "tostring-arg"   '(x: toString x)'      '42'   || fail=$((fail+1))
run_case "nested-call"    '(x: (y: y * 2) x)'    '21'   || fail=$((fail+1))

if [[ "$fail" -eq 0 ]]; then
  echo "run-676: PASS (9/9 --apply shapes match TW)"
  exit 0
else
  echo "run-676: FAIL ($fail divergence(s))"
  exit 1
fi
