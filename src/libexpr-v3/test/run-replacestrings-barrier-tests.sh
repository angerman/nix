#!/usr/bin/env bash
# builtins.replaceStrings from-list writeback: correctness + exercise guardrail
# (DEFECT_REVIEW_2026-07-03 §1.3, WITH a verified correction to its claim).
#
# §1.3 claimed a reachable PhD-6 missed root: replaceStrings memoized each forced
# `from` element into the (tenured) argument list BEFORE the type check, so on the
# error path (an element forcing to a non-string NURSERY value) "under
# builtins.tryEval the caller-visible list survives holding an unbarriered nursery
# pointer, and the next scavenge invalidates it."
#
# CORRECTION (verified 2026-07-03, both codebases): that scenario is NOT reachable.
#   1. builtins.tryEval does NOT catch the replaceStrings type error — in v3 AND
#      in the tree-walker (`tryEval (replaceStrings ["ok" {bad=1;}] ...)` throws
#      "expected a string but found a set" to top level in both).  So eval
#      terminates at the throw; the list does not survive to a later scavenge.
#   2. On the happy path the forced elements are STRINGS, which are tenured, so a
#      raw store creates no tenured->nursery edge (a 40000-element from-list under
#      the 1 MB-nursery audit shows 0 flags both pre- and post-fix).
#
# The fix applied is therefore (a) a real premature-argument-mutation cleanup —
# force into a LOCAL, type-check, THEN store, so replaceStrings never mutates a
# caller-visible argument with a value it is about to reject — plus (b) a
# defensive cellWrite barrier consistent with the sibling string-storing sites
# (valueEqual/valueLess/primSort), hardening the writeback against the S2.1
# safepoint work that relaxes the gate.  It is latent hardening, not a repro'd UAF.
#
# This test guards: a valid large replaceStrings evaluates correctly and stays
# audit-clean under the 1 MB-nursery brute stress; and the type-error case throws
# (TW-parity), never silently succeeds.
#
# Usage:   bash src/libexpr-v3/test/run-replacestrings-barrier-tests.sh
# Exit:    0 pass; 1 fail; 2 preflight.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "replacestrings-barrier: v3-eval not at $V3" >&2; exit 2; fi

N="${REPLACESTRINGS_N:-40000}"
work="$(mktemp -t replacestrings-repro.XXXXXX)"
fixture="$work.nix"; errf="$work.err"
trap 'rm -f "$work" "$fixture" "$errf"' EXIT

fail=0

# Arm 1: large valid replaceStrings under the 1 MB-nursery audit — the from-loop
# forces + stores N elements; assert correct output length and 0 audit flags.
cat > "$fixture" <<NIX
let
  n = $N;
  from = builtins.genList (i: "s" + builtins.toString i) n;
  to   = builtins.genList (i: "t" + builtins.toString i) n;
  churn = builtins.genList (i: [ i i ]) 200000;
  r = builtins.replaceStrings from to "s1 s2 s3";
in builtins.seq r (builtins.deepSeq churn (builtins.stringLength r))
NIX
r1="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 \
        "$V3" --file "$fixture" 2>"$errf" | tail -1)"
flags="$(grep -c "reachable via" "$errf")" || true
# "s1 s2 s3" -> "t1 t2 t3" = 8 chars.
if [[ "$r1" == "8" && "${flags:-0}" -eq 0 ]]; then
  echo "replacestrings-barrier: PASS valid (N=$N → len=8; 0 audit flags)"
else
  echo "replacestrings-barrier: FAIL valid (N=$N → '$r1', flags=${flags:-?})" >&2
  fail=1
fi

# Arm 2: type-error case must THROW (never silently succeed) — a non-string in
# `from`.  tryEval does not catch it (see header); we assert v3 errors out.
cat > "$fixture" <<'NIX'
builtins.replaceStrings [ "ok" { bad = 1; } ] [ "OK" "BAD" ] "some string"
NIX
if NIX_V3_DIRECT_EVAL=1 "$V3" --file "$fixture" >/dev/null 2>&1; then
  echo "replacestrings-barrier: FAIL type-error (non-string 'from' did not throw)" >&2
  fail=1
else
  echo "replacestrings-barrier: PASS type-error (non-string 'from' throws)"
fi

exit $fail
