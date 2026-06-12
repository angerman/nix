#!/usr/bin/env bash
# P-11 (CODEBASE_REVIEW_2026-06-11) regression battery.
#
# betaReduce now inlines single-use immediate-application lambdas whose body
# contains If / With / Assert / And / Or / Impl sub-blocks (previously rejected
# by bodyIsSimple).  Each case below is an IMMEDIATE application `((x: body) a)`
# — the shape beta-reduce fires on — with a sub-block body.  We assert v3-eval
# == tree-walker (nix-instantiate --eval) for every case, with the disk cache
# DISABLED so the freshly-emitted (post-inline) bytecode is exercised, not a
# stale cached CU.
#
# Positive (inline fires + result matches) + negative (multi-use lambda must
# NOT be miscloned) + nested (If inside If) coverage.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
TW="${TW:-$ROOT/build/src/nix/nix-instantiate}"

if [[ ! -x "$V3" || ! -x "$TW" ]]; then
  echo "p11: missing binaries ($V3 / $TW)" >&2; exit 2
fi

export NIX_V3_NO_DISK_CACHE=1

pass=0; fail=0; total=0
failed=()

# $1 = label, $2 = nix expression
check() {
  local label="$1" expr="$2"
  total=$((total+1))
  local v t
  v="$("$V3" --optimize --expr "$expr" 2>/dev/null)"
  t="$("$TW" --eval --expr "$expr" 2>/dev/null)"
  if [[ -n "$t" && "$v" == "$t" ]]; then
    pass=$((pass+1)); printf 'OK    %-26s %s\n' "$label" "$v"
  else
    fail=$((fail+1)); failed+=("$label"); printf 'FAIL  %-26s v3=[%s] tw=[%s]\n' "$label" "$v" "$t"
  fi
}

# --- If sub-block (then/else) ---
check if-then-branch     '((x: if x > 2 then x * 10 else x + 1) 5)'
check if-else-branch     '((x: if x > 2 then x * 10 else x + 1) 1)'
check if-nested          '((x: if x > 0 then (if x > 5 then "big" else "small") else "neg") 7)'
# --- And / Or / Impl (rhsBlock) ---
check and-rhs            '((x: x > 0 && x < 10) 5)'
check or-rhs             '((x: x < 0 || x > 3) 5)'
check impl-rhs           '((x: x > 0 -> x < 10) 5)'
# --- With (bodyBlock) ---
check with-body          '((x: with { a = x; b = x + 1; }; a + b) 7)'
# --- Assert (bodyBlock) ---
check assert-body        '((x: assert x > 0; x * 2) 4)'
# --- mixed: If whose branch is a With ---
check if-with            '((x: if x > 0 then (with { y = x; }; y * 3) else 0) 2)'
# --- negative: multi-use lambda (must NOT inline; still correct) ---
check multiuse-lambda    'let f = x: if x > 0 then x else 0 - x; in (f 3) + (f (0 - 4))'
# --- control: simple body (the pre-P-11 path still works) ---
check simple-body        '((x: x + 1) 41)'

echo
echo "=== P-11 beta sub-block results ==="
echo "  total: $total"
echo "  pass:  $pass"
echo "  fail:  $fail"
if (( fail > 0 )); then
  printf '  failed: %s\n' "${failed[*]}"
  exit 1
fi
exit 0
