#!/usr/bin/env bash
# v3 valueLess Phase-D missed-root barrier regression test (§1.2).
#
# Guards DEFECT_REVIEW_2026-07-03 §1.2: valueLess (the `<`/`<=`/`>`/`>=`
# list comparison path in vm.cc) iterated two ListVecs, forced each element
# pair with forceValue, and then WROTE the forced value back into the
# (possibly tenured) ListVec storage WITHOUT a Phase-D cellWrite barrier.
# A tenured outer list holding freshly-forced nursery-resident inner lists
# is therefore NOT in the remembered set; the next scavenge moves/frees
# those cells out from under the ListVec — a PhD-6 missed-root UAF.
# The fix adds cellWrite(list, elem) after the forceValue memoize on both
# sides of the comparison.
#
# Nature of this fix (verified 2026-07-03): valueLess is a LATENT hardening
# fix, not a reproducible-today UAF.  The missing barrier is a real code-level
# asymmetry — valueEqual's identical writeback (vm.cc ~1086) uses cellWrite; this
# comparison writeback did not — but the defect is currently masked by the
# exitDepth==0 scavenge gate + conservative C-stack pin (§1.7's "safe today ONLY
# via the gate" class): a scavenge does not fire mid-comparison, and by the time
# a large outer list has tenured the nursery is under pressure so freshly-forced
# inner values bypass to tenured too — so the writeback rarely stores an actual
# nursery cell into a tenured list under current gating.  It becomes live when
# S2.1 safepoint work relaxes the gate.  This test therefore is a CORRECTNESS +
# EXERCISE guardrail (walks the writeback path over a large list-of-lists compare
# under the 1 MB-nursery audit; asserts the right boolean + 0 audit flags), not a
# pre-fix-tripping repro (verified: 0 flags both pre- and post-fix under current
# gating).  It guards byte-identity and audit-cleanliness against any future
# gating change that would make the raw store observable.
#
# Fixture shape: build
#   a = [ [N] [N-1] … [1] ]           (N inner singleton lists, desc order)
#   b = [ [N] [N-1] … [1] [0] ]       (same but one extra element at the end)
# so `a < b` is true (a is a proper prefix of b, list comparison returns true
# when the shared prefix is equal and the left list is shorter).  Then evaluate:
#   builtins.seq (a < b)
#     (builtins.deepSeq churn (a < b))
# The first `seq` forces the compare when the inner lists are freshly nursery-
# resident; the deepSeq churn fires a scavenge; the second `(a < b)` re-reads
# through the (now possibly stale) ListVec entries — catching the UAF.
# Expected result: true (both comparisons agree; no audit flags).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "valueless-barrier: v3-eval not at $V3" >&2; exit 2; fi

# N must be large enough that the literal inner lists fill the 1 MB nursery
# (~40k thunks ≈ 960 KB) so the outer ListVecs tenure with nursery-resident
# element pointers — the precondition for the missed-root UAF.
N="${VALUELESS_N:-40000}"
work="$(mktemp -t valueless-repro.XXXXXX)"
fixture="$work.nix"
errf="$work.err"
trap 'rm -f "$work" "$fixture" "$errf"' EXIT

# Generate a fixture that builds two LITERAL lists-of-inner-lists.
# `a` has N singleton inner lists [N]..[1] (descending).
# `b` is the same N singletons plus an extra [0] at the end.
# `a < b` is true because a == b on the shared prefix and a is shorter.
awk -v n="$N" 'BEGIN{
  printf "let\n";
  printf "  a = [ ";
  for (i = n; i >= 1; i--) printf "[ %d ] ", i;
  printf "];\n";
  printf "  b = [ ";
  for (i = n; i >= 1; i--) printf "[ %d ] ", i;
  printf "[ 0 ] ];\n";
  printf "  churn = builtins.genList (i: [ i i ]) 300000;\n";
  printf "in builtins.seq (a < b) (builtins.deepSeq churn (a < b))\n";
}' > "$fixture"

# One eval under the nursery-audit stress: stdout=result, stderr=audit lines.
result="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 \
           "$V3" --file "$fixture" 2>"$errf" | tail -1)"
# grep -c prints the count (exits 1 when 0) — capture the number only.
flags="$(grep -c "reachable via" "$errf")" || true

# Expected: true (a < b is a proper prefix), no audit flags.
if [[ "$flags" -eq 0 && "$result" == "true" ]]; then
  echo "valueless-barrier: PASS (N=$N; 0 missed-root flags; compare result=true)"
  exit 0
fi
echo "valueless-barrier: FAIL (N=$N flags=$flags result='$result')" >&2
grep -m3 "reachable via" "$errf" >&2 2>/dev/null || true
exit 1
