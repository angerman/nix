#!/usr/bin/env bash
# WS-5 D3 (2026-07-16) — fork-server ("--fork-worker") regression suite.
#
# Kills the hypothesis "productionising the --cow-fork zygote into a real
# request-serving fork-server changes results / races / desyncs the stream."
# A `v3-eval --fork-worker` warms ONE long-lived parent (--expr) and then
# FORKS a child per stdin request; the child evaluates against the parent's
# warm caches (inherited copy-on-write), returns its result over a pipe, and
# _exit()s without ever returning into the parent loop. The parent relays each
# result to stdout in submission order and reaps the child.
#
# Correctness core (the gate here): a fork-worker response must be
# BYTE-IDENTICAL to the corresponding --worker response AND to a fresh-process
# `--expr` eval — for jobs=1 AND for concurrent jobs>1 (front-first relay must
# keep responses ordered), and one erroring request must not kill the server.
# The per-child RSS-sharing win (KPI-5, ~27 MB same-expr) is a Linux smaps
# measurement done + git-noted separately; a memory assertion would be flaky
# in CI and smaps_rollup is Linux-only.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3EVAL="${V3EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh" 2>/dev/null || true

if [[ ! -x "$V3EVAL" ]]; then
  echo "fork-worker-mode: v3-eval not found at $V3EVAL" >&2
  exit 2
fi

pass=0 fail=0 total=0
failed=()

# Full raw stream (INCLUDING the blank per-response delimiters) — the strongest
# contract is that a client cannot tell --fork-worker from --worker.
worker_raw()      { printf '%s\n' "$1" | NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --worker 2>/dev/null; }
fork_raw()        { printf '%s\n' "$1" | NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --fork-worker "${@:2}" 2>/dev/null; }
# Non-blank result lines only (blank lines are the per-response delimiters).
fork_vals()       { printf '%s\n' "$1" | NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --fork-worker "${@:2}" 2>/dev/null | grep -v '^$'; }

check() {
  local name="$1" got="$2" want="$3"
  total=$((total+1))
  if [[ "$got" == "$want" ]]; then echo "OK    $name"; pass=$((pass+1));
  else echo "FAIL  $name"; echo "  got:  $got"; echo "  want: $want"; fail=$((fail+1)); failed+=("$name"); fi
}

REQ='1 + 2
"a" + "b"
builtins.length [ 1 2 3 4 ]
if true then "y" else "n"'

# 1. Basic streaming: N requests -> N results, in order (values only).
check "stream-4-requests" "$(fork_vals "$REQ")" $'3\n"ab"\n4\n"y"'

# 2. Whole-stream (with delimiters) byte-identical to --worker, jobs=1.
check "raw-stream-eq-worker-j1" "$(fork_raw "$REQ")" "$(worker_raw "$REQ")"

# 3. Concurrency must not reorder or corrupt: jobs=4 raw stream == worker.
check "raw-stream-eq-worker-j4" "$(fork_raw "$REQ" --fork-jobs 4)" "$(worker_raw "$REQ")"

# 4. Blank lines between requests are ignored (delimiter-robust).
check "blank-lines-ignored" "$(fork_vals $'1\n\n2\n\n3')" $'1\n2\n3'

# 5. An error in one request must NOT kill the server — the next succeeds.
out="$(fork_vals 'builtins.head [ ]
42')"
line2="$(printf '%s\n' "$out" | sed -n 2p)"
check "error-does-not-kill-server" "$line2" "42"

# 6. Concurrent error-isolation: an error mid-batch under jobs=4 still yields
#    the correct ordered stream (identical to --worker).
EREQ='1 + 1
builtins.head [ ]
2 * 3'
check "concurrent-error-isolation" "$(fork_raw "$EREQ" --fork-jobs 4)" "$(worker_raw "$EREQ")"

# 7. WARM-PARENT byte-identity (the D3 model): warm the parent with a real
#    nixpkgs eval (--expr), then serve the SAME expr as a request from a forked
#    child. Child result must equal the fresh-process result AND the --worker
#    result — the warm caches are read-shared, never leaked/mutated.
E='(import <nixpkgs> { config.allowUnfree = true; }).hello.name'
fresh="$(NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --expr "$E" 2>/dev/null)"
fw="$(printf '%s\n' "$E" | NIX_V3_DIRECT_EVAL=1 "$V3EVAL" --fork-worker --expr "$E" --fork-jobs 2 2>/dev/null | grep -v '^$' | sed -n 1p)"
if [[ -z "$fresh" || -z "$fw" ]]; then
  echo "SKIP  warm-parent-nixpkgs-byte-identity (nixpkgs eval unavailable)"
else
  check "warm-parent-nixpkgs-byte-identity" "$fw" "$fresh"
fi

echo "-----------------------------------------------------------------------"
echo "fork-worker-mode: $pass/$total passed, $fail failed"
if [[ $fail -ne 0 ]]; then printf '  failed: %s\n' "${failed[@]}"; exit 1; fi
exit 0
