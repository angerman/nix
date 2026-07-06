#!/usr/bin/env bash
# Dedup-survey instrument-accuracy regression tests (NIX_V3_DEDUP_SURVEY).
#
# The bytecode-fragment dedup survey (dedup_survey.cc) is the B1 measurement
# instrument (lode/BEAT_TW_LEVERS_AND_ARCHITECTURE_2026-07-06.md): it counts
# per-lambda bytecode fragments process-wide and reports fn_dedup_lb /
# byte_dedup_lb — the gate for content-addressed IR (B2).  A B1 verdict is only
# trustworthy if the survey OBSERVES the imported CUs (the hundreds of nixpkgs
# files an eval pulls in), not just the top-level + builtins prelude.
#
# Two accuracy bugs were fixed together and are guarded here:
#   (1) disk-load observe (primops.cc:7523): a WARM import HITs the CU disk
#       cache and is restored via deserializeCU — which bypassed the
#       fresh-compile observe (primops.cc:8164).  Warm imports were counted as
#       ZERO fragments.  Fix: survey the deserialized CU too.
#   (2) process-exit FINAL report (dedup_survey.cc): the per-runRootExpr report
#       at run.cc:933 is only reached by the builtins-install evals (the user's
#       top-level eval runs its imports re-entrantly via run(), not
#       runRootExpr), so it printed a MID-EVAL snapshot (~131 fns) that
#       under-counted by ~700x.  Fix: an atexit reporter reads the same
#       process-wide accumulator AFTER every import has been surveyed and
#       prints a `dedup_survey [FINAL]:` line.
#
# +/-/R structure:
#   DS1 (+, cold): importing a file with N lambdas raises the FINAL fragment
#       count by ~N over the no-import baseline (imports ARE observed+reported).
#   DS2 (+, warm): the SAME import in a second process (warm disk cache →
#       deserialize path) ALSO raises the count by ~N — guards fix (1).
#   DS3 (-, off):  with NIX_V3_DEDUP_SURVEY unset, NO dedup_survey line at all.
#   The `[FINAL]` line is fix (2); it is ABSENT before the fix, so DS1/DS2 are
#   red-before / green-after (regression guard).
#
# Uses v3-eval directly (no nix/store dependency) with local .nix fixtures.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3EVAL="${V3EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3EVAL" ]]; then echo "dedup-survey: v3-eval not at $V3EVAL" >&2; exit 2; fi
pass=0; fail=0

# --- fixtures: a file with N=20 distinct lambdas, and a main that imports it.
FX=$(mktemp -d)
trap 'rm -rf "$FX"' EXIT
{
  echo '{'
  for i in $(seq 1 20); do
    # each body is DISTINCT (different added constant) so the fragments are not
    # collapsed to one hash — we are counting observed fragments, not unique.
    echo "  f$i = x: x + $i;"
  done
  echo '}'
} > "$FX/manyfns.nix"
# main forces the import and applies several of the lambdas.
{
  echo 'let m = import ./manyfns.nix; in'
  echo '  m.f1 1 + m.f2 2 + m.f3 3 + m.f20 20'
} > "$FX/main.nix"
# a no-import baseline expression (prelude + a tiny top-level CU only).
echo '1 + 2 + 3' > "$FX/baseline.nix"

# extract totalFunctions from the FINAL survey line (0 if absent).
final_fns() { # reads stderr stream on stdin
    grep 'dedup_survey \[FINAL\]' | sed -E 's/.*totalFunctions=([0-9]+).*/\1/' | tail -1
}
run_survey() { # $1=cachedir $2=file  -> prints FINAL totalFunctions (or empty)
    local dir="$1" file="$2"
    NIX_V3_DEDUP_SURVEY=1 NIX_VM_STATS=1 NIX_V3_CACHE_DIR="$dir" \
        NIX_V3_MAX_WALL_TIME=60s "$V3EVAL" --file "$file" 2>&1 >/dev/null | final_fns
}

# DS1 (+, cold): fresh cache → import compiles fresh; FINAL count must exceed
# the no-import baseline by ~N (>=15 of the 20 lambdas, allowing for the
# survey's fragment-range dropping the occasional zero-length slice).
DC=$(mktemp -d); base=$(run_survey "$DC" "$FX/baseline.nix")
DI=$(mktemp -d); withimp=$(run_survey "$DI" "$FX/main.nix")
if [[ -n "$base" && -n "$withimp" && $((withimp - base)) -ge 15 ]]; then
    pass=$((pass+1)); echo "PASS DS1-cold-import-observed (base=$base withimp=$withimp Δ=$((withimp-base)))"
else
    fail=$((fail+1)); echo "FAIL DS1-cold-import-observed (base=$base withimp=$withimp) — expected Δ>=15"
fi

# DS2 (+, warm): reuse a cache dir already populated by a first run → the second
# run HITs the CU disk cache (deserialize path).  The warm FINAL count must
# STILL exceed the baseline by ~N (guards the disk-load observe, fix 1).
DW=$(mktemp -d)
_=$(run_survey "$DW" "$FX/main.nix")          # populate disk cache
warm=$(run_survey "$DW" "$FX/main.nix")       # warm hit → deserialize path
if [[ -n "$warm" && -n "$base" && $((warm - base)) -ge 15 ]]; then
    pass=$((pass+1)); echo "PASS DS2-warm-import-observed (base=$base warm=$warm Δ=$((warm-base)))"
else
    fail=$((fail+1)); echo "FAIL DS2-warm-import-observed (base=$base warm=$warm) — expected Δ>=15 (disk-load observe)"
fi

# DS3 (-, off): survey disabled → NO dedup_survey line whatsoever.
DO=$(mktemp -d)
off=$(NIX_VM_STATS=1 NIX_V3_CACHE_DIR="$DO" NIX_V3_MAX_WALL_TIME=60s \
        "$V3EVAL" --file "$FX/main.nix" 2>&1 >/dev/null | grep -c 'dedup_survey')
if [[ "$off" == "0" ]]; then
    pass=$((pass+1)); echo "PASS DS3-survey-off-silent (dedup lines=$off)"
else
    fail=$((fail+1)); echo "FAIL DS3-survey-off-silent (dedup lines=$off) — expected 0"
fi

echo "dedup-survey: $pass passed, $fail failed"
[[ "$fail" == "0" ]]
