#!/usr/bin/env bash
# v3 primSort Phase-D missed-root barrier regression test (P1.2).
#
# Guards audit DEFECT_AUDIT_2026-07-02 §2.2: primSort bulk-copies arbitrary
# source elements into `result` and, when the nursery is full at allocList
# time, `result` is itself TENURED.  Without listPostConstructBarrier(result),
# a tenured `result` holding nursery-resident cells is NOT in the remembered
# set, so the next scavenge moves/frees those cells out from under it (PhD-6
# missed-root UAF).  Sixteen sibling list-producing primops barrier; primSort
# was the gap.
#
# Repro (failing-first, kept forever per the debug story): sort a LITERAL list
# of inner lists large enough to FILL the 1 MB nursery (so early elements are
# nursery-resident when `result` overflows to tenured), then churn to trigger a
# post-sort scavenge, then deep-read the sorted result.  Under
# NIX_V3_NURSERY_SIZE=1 + V3_DBG_NURSERY_AUDIT the scavenge audit flags
# "reachable via ListVec(...).elems[N] lastWriter=(no-recorded-writer=raw/
# bulk-path)" PRE-fix (thousands of them) and is CLEAN post-fix.  genList/map
# sources do NOT trip it (their elements are tenured App pairs) — the literal
# nested list is what makes the elements direct nursery cells.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "primsort-barrier: v3-eval not at $V3" >&2; exit 2; fi

# N must be large enough that the literal inner lists FILL the 1 MB nursery
# (~40k thunks ≈ 960 KB) so `result` tenures with nursery-resident elements.
N="${PRIMSORT_N:-40000}"
work="$(mktemp -t primsort-repro.XXXXXX)"
fixture="$work.nix"
errf="$work.err"
trap 'rm -f "$work" "$fixture" "$errf"' EXIT

awk -v n="$N" 'BEGIN{
  printf "let sorted = builtins.sort (a: b: (builtins.head a) < (builtins.head b)) [ ";
  for (i = 0; i < n; i++) printf "[ %d ] ", n - i;
  printf "];\n  churn = builtins.genList (i: [ i i ]) 300000;\n";
  printf "in builtins.seq (builtins.length sorted) (builtins.deepSeq churn (builtins.head (builtins.head sorted)))\n";
}' > "$fixture"

# One eval under the nursery-audit stress: stdout=result, stderr=audit lines.
result="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 \
           "$V3" --file "$fixture" 2>"$errf" | tail -1)"
# grep -c prints the count (and exits 1 when 0) — capture the number only.
flags="$(grep -c "reachable via ListVec" "$errf")" || true

# Expected: smallest head is 1 (inner lists [n]..[1] sorted ascending), no flags.
if [[ "$flags" -eq 0 && "$result" == "1" ]]; then
  echo "primsort-barrier: PASS (N=$N; 0 missed-root flags; sort result=1)"
  exit 0
fi
echo "primsort-barrier: FAIL (N=$N flags=$flags result='$result')" >&2
grep -m3 "reachable via ListVec" "$errf" >&2 2>/dev/null || true
exit 1
