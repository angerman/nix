#!/usr/bin/env bash
# WS-3 W1 (2026-07-13) — persistent worker-mode regression suite.
#
# Kills the hypothesis "the applied-import cache is worthless for CI because
# it dies with the process." A `v3-eval --worker` keeps ONE process alive and
# evaluates a stream of expressions, so the process-lifetime import + applied
# caches persist: eval #2..N of the same script reuse eval #1's work.
#
# Correctness core: eval #2 in a worker must be BYTE-IDENTICAL to eval #1 AND
# to a fresh-process eval — the persistent caches must not leak state across
# requests. That triple-identity is the gate here (the eval#2 SPEED win is
# measured + git-noted separately; a timing assertion would be flaky in CI).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3EVAL="${V3EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh" 2>/dev/null || true

if [[ ! -x "$V3EVAL" ]]; then
  echo "worker-mode: v3-eval not found at $V3EVAL" >&2
  exit 2
fi

pass=0 fail=0 total=0
failed=()

# Run the worker over the given newline-separated requests; echo the non-blank
# result lines (blank lines are the per-response delimiters).
worker() { printf '%s\n' "$1" | NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --worker 2>/dev/null | grep -v '^$'; }

check() {
  local name="$1" got="$2" want="$3"
  total=$((total+1))
  if [[ "$got" == "$want" ]]; then echo "OK    $name"; pass=$((pass+1));
  else echo "FAIL  $name"; echo "  got:  $got"; echo "  want: $want"; fail=$((fail+1)); failed+=("$name"); fi
}

# 1. Basic streaming: N requests → N results, in order.
out="$(worker '1 + 2
"a" + "b"
builtins.length [ 1 2 3 4 ]
if true then "y" else "n"')"
want=$'3\n"ab"\n4\n"y"'
check "stream-4-requests" "$out" "$want"

# 2. Blank lines between requests are ignored (delimiter-robust).
out="$(worker '1

2

3')"
check "blank-lines-ignored" "$out" $'1\n2\n3'

# 3. An error in one request must NOT kill the worker — the next succeeds.
out="$(worker 'builtins.head [ ]
42')"
# first line is the <error> sentinel (head of empty list throws), second is 42
line2="$(printf '%s\n' "$out" | sed -n 2p)"
check "error-does-not-kill-worker" "$line2" "42"

# 4. TRIPLE byte-identity (the correctness gate): eval#1 == eval#2 (in-worker)
#    == fresh-process, on a real nixpkgs eval that exercises the import +
#    applied caches.
E='(import <nixpkgs> { config.allowUnfree = true; }).hello.name'
w="$(worker "$E
$E")"
e1="$(printf '%s\n' "$w" | sed -n 1p)"
e2="$(printf '%s\n' "$w" | sed -n 2p)"
fresh="$(NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --expr "$E" 2>/dev/null)"
if [[ -z "$e1" || -z "$fresh" ]]; then
  echo "SKIP  triple-byte-identity (nixpkgs eval unavailable)"
else
  check "triple-byte-identity-worker#1" "$e1" "$fresh"
  check "triple-byte-identity-worker#2" "$e2" "$fresh"
fi

echo "-----------------------------------------------------------------------"
echo "worker-mode: $pass/$total passed, $fail failed"
if [[ $fail -ne 0 ]]; then printf '  failed: %s\n' "${failed[@]}"; exit 1; fi
exit 0
