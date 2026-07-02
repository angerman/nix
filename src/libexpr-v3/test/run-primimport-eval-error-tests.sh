#!/usr/bin/env bash
# v3 primImport disk-cache-HIT eval-error scoping regression (P1.3).
#
# Guards audit DEFECT_AUDIT_2026-07-02 §2.3: the disk-cache HIT branch used
# to wrap BOTH deserializeCU AND `out = run(cu)` in one try whose catch
# popped the CU and fell through to a fresh parse+compile+re-run.  Written
# for corrupt blobs, it ALSO caught eval errors / WallTimeExceeded / OOM
# thrown by run() — popping a CU that partially-evaluated closures/thunks
# already reference (dangling-CU deref on later force) and RE-RUNNING the
# import (duplicating any IFD side effects).  The fix scopes the try to
# deserializeCU only; run() executes outside it, so eval errors propagate
# and the import executes exactly once.
#
# Failing-first repro (kept forever per the debug story): an import whose
# evaluation throws is disk-cached (the CU is written before run()), then
# evaluated again in a SECOND process (a genuine disk-cache HIT).  A
# seq-forced `builtins.trace` marker counts how many times the import body
# actually runs on the warm HIT: PRE-fix = 2 (the buggy re-run), POST-fix
# = 1.  (A plain `trace msg (throw ...)` does NOT fire — trace's value arg
# is strict, so the throw beats the print; seq forces the trace first.)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "primimport-eval-error: v3-eval not at $V3" >&2; exit 2; fi

work="$(mktemp -d -t primimport-p13.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cache="$work/cache"
boom="$work/boom.nix"
good="$work/good.nix"
printf 'builtins.seq (builtins.trace "P13_EXEC_MARKER" 1) (throw "p13-boom")\n' > "$boom"
printf '{ ok = 41 + 1; }\n' > "$good"

runboom() { NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$cache" "$V3" --expr "builtins.tryEval (import $boom)" 2>"$1"; }

cold_out="$(runboom "$work/cold.err")"                 # COLD: disk MISS → caches the CU
warm_out="$(runboom "$work/warm.err")"                 # WARM: disk HIT → the fixed path
warm_mark="$(grep -c P13_EXEC_MARKER "$work/warm.err")" || true
# Contract: a subsequent SUCCESSFUL import through the same (post-error) cache is intact.
good_out="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_CACHE_DIR="$cache" "$V3" --expr "(import $good).ok" 2>/dev/null)"

expect='{ success = false; value = false; }'
fail=0; msgs=()
[[ "$cold_out" == "$expect" ]]  || { fail=1; msgs+=("cold result: '$cold_out'"); }
[[ "$warm_out" == "$expect" ]]  || { fail=1; msgs+=("warm result unstable: '$warm_out'"); }
[[ "$warm_mark" -eq 1 ]]        || { fail=1; msgs+=("warm executions=$warm_mark (expect 1; PRE-fix=2 dup re-run)"); }
[[ "$good_out" == "42" ]]       || { fail=1; msgs+=("post-error cache corrupted: good='$good_out'"); }

if [[ "$fail" -eq 0 ]]; then
  echo "primimport-eval-error: PASS (warm single-exec=$warm_mark; stable error; cache intact)"
  exit 0
fi
echo "primimport-eval-error: FAIL" >&2
printf '  %s\n' "${msgs[@]}" >&2
exit 1
