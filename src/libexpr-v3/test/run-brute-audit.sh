#!/usr/bin/env bash
# v3 nursery brute / audit diagnostic regression — 2026-05-21
#
# Closes ACTION_PLAN_2026-05-15.md Phase 1.7 exit criterion and
# GC_AUDIT_ROUND_2_2026-05-21.md §3.4 R1 ("highest-leverage tooling
# investment").
#
# Method: run a curated battery of allocating workloads under the
# `V3_DBG_NURSERY_BRUTE=1 V3_DBG_NURSERY_AUDIT=1` gates with a tiny
# nursery (1 MB) so scavenges fire frequently.  After each run:
#   - parse stderr for `v3 SCAVENGE BRUTE: N tenured words` where N>0
#     (a tenured-arena word pointing into the nursery survived
#     scavenge: missed root).
#   - parse stderr for `v3 SCAVENGE AUDIT: nursery .* reachable via`
#     (auditor's reachable-graph walk found a nursery pointer that
#     the scavenger didn't forward).
# Either hit → suite FAIL with the diagnostic line(s) echoed.
#
# Hit = missed root = Stage 3 default-on blocker.  These tests stay
# RED until the underlying root is forwarded; they're the
# correctness signal for "is scavenge complete?"
#
# Why not fold into all-v3-tests.sh `--brute` mode: the existing
# lang / property / derivation-parity scripts pipe their command
# output through `tail -1` etc., stripping the BRUTE / AUDIT stderr
# lines before they reach the all-v3-tests.sh log file.  A
# dedicated harness that captures stderr separately is the only way
# to surface hits reliably.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3_EVAL="${V3_EVAL:-$ROOT/build/src/libexpr-v3/v3-eval}"

if [[ ! -x "$V3_EVAL" ]]; then
    echo "brute-audit: $V3_EVAL not found, build first" >&2
    exit 2
fi

# Shared gates.  1 MB nursery so a 1000-iteration workload scavenges
# many times — exposes any sticky tenured→nursery edge.
export NIX_V3_NURSERY=1
export NIX_V3_NURSERY_SCAVENGE=1
export NIX_V3_NURSERY_SIZE=1
export V3_DBG_NURSERY_BRUTE=1
export V3_DBG_NURSERY_AUDIT=1

PASS=0
FAIL=0
fail_names=()

# run_case <name> <expected-stdout-substring> <expr>
# Runs the v3-eval invocation, captures stdout + stderr to separate
# temp files, then:
#   1. Asserts the expected substring appears in stdout.
#   2. Asserts NO BRUTE hit (`SCAVENGE BRUTE: [1-9][0-9]* tenured`)
#      and NO AUDIT hit (`SCAVENGE AUDIT: nursery .* reachable via`)
#      in stderr.
# A diagnostic hit is reported with the first three matching lines
# inline so the failure mode is obvious without opening the temp file.
run_case() {
    local name="$1" want="$2" expr="$3"
    local stdout_f stderr_f
    stdout_f="$(mktemp -t v3-brute-stdout.XXXXXX)"
    stderr_f="$(mktemp -t v3-brute-stderr.XXXXXX)"
    # 60 s + 2 G heap — workloads here are small (genList up to 50000);
    # the budget catches infinite-loop regressions cleanly.
    NIX_V3_MAX_WALL_TIME=60s NIX_V3_MAX_HEAP=2G \
        "$V3_EVAL" --expr "$expr" \
        >"$stdout_f" 2>"$stderr_f"
    local rc=$?
    local stdout_val brute_hits audit_hits
    stdout_val="$(cat "$stdout_f")"
    brute_hits="$(grep -E '^v3 SCAVENGE BRUTE: [1-9][0-9]* tenured words' "$stderr_f" || true)"
    audit_hits="$(grep -E '^v3 SCAVENGE AUDIT: nursery .* reachable via' "$stderr_f" || true)"
    local case_ok=1
    local why=""
    if (( rc != 0 )); then
        case_ok=0
        why="exit=$rc"
    elif [[ "$stdout_val" != *"$want"* ]]; then
        case_ok=0
        why="want='$want' got='$stdout_val'"
    elif [[ -n "$brute_hits" ]]; then
        case_ok=0
        why="BRUTE hit"
    elif [[ -n "$audit_hits" ]]; then
        case_ok=0
        why="AUDIT hit"
    fi
    if (( case_ok )); then
        PASS=$((PASS + 1))
        echo "OK    $name"
    else
        FAIL=$((FAIL + 1))
        fail_names+=("$name [$why]")
        echo "FAIL  $name [$why]"
        if [[ -n "$brute_hits" ]]; then
            echo "$brute_hits" | head -3 | sed 's/^/      /'
        fi
        if [[ -n "$audit_hits" ]]; then
            echo "$audit_hits" | head -3 | sed 's/^/      /'
        fi
    fi
    rm -f "$stdout_f" "$stderr_f"
}

# ---------- workloads ----------
#
# Each workload is sized so the 1 MB nursery cycles through several
# scavenges during eval — small enough that the suite stays under a
# minute total, large enough that scavenge actually fires.  The
# expected-stdout-substring is matched permissively (substring) so
# leading diagnostic lines on stdout don't break the assertion.

# 1) Small arithmetic + let-rec — sanity baseline.  Same workloads
#    that p1 of run-nursery-tests already validates for correctness;
#    here we also assert no diagnostic hit.
run_case "arith"        "3"      '1 + 2'
run_case "let-square"   "25"     'let x = 5; in x * x'
run_case "lambda-apply" "42"     'let f = x: x + 1; in f 41'
run_case "attrs-select" "2"      '{ a = 1; b = 2; c = 3; }.b'

# 2) Fix-point eval — exercises tenured Bindings + Closure capture.
run_case "fix-point"    "2"      'let lib = { fix = f: let x = f x; in x; }; in (lib.fix (self: { x = 1; y = self.x + 1; })).y'

# 3) Tail recursion — forces many scavenges through the dispatch loop.
run_case "tail-1000"    "0"      'let f = x: if x == 0 then 0 else f (x - 1); in f 1000'

# 4) genList + foldl' — known to trip BRUTE on the open missed-root
#    finding discovered 2026-05-21 during initial wiring.  Sized to
#    match nursery-tests' p8 (100000 elements) which exhibits the
#    bug consistently with ~2300+ BRUTE hits per scavenge.
run_case "fold-genlist-100k" "4999950000" 'builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i) 100000)'

# 5) Smaller fold to confirm threshold — genList 5000 still hits,
#    1000 does not (per initial measurement).  Both included so a
#    future regression that pulls the threshold down to 1000 is
#    caught immediately.
run_case "fold-genlist-5k"   "12497500"   'builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i) 5000)'
run_case "fold-genlist-1k"   "499500"     'builtins.foldl'"'"' (a: b: a + b) 0 (builtins.genList (i: i) 1000)'

# 6) Deep let-rec / fix combination — p7 from run-nursery-tests, but
#    here we also assert no diagnostic hit.
run_case "deep-let-rec-fix" "28000" '
  let
    rec1 = self: { a = 1; b = 2; c = self.a + self.b;
                   d = self.c * 2; e = self.d + self.a; };
    fix = f: let x = f x; in x;
    deep = n: if n == 0 then 0
              else (fix rec1).e + deep (n - 1);
  in deep 4000'

echo
echo "=== brute-audit: ok=$PASS fail=$FAIL ==="
if (( FAIL > 0 )); then
    echo "FAILED CASES:"
    for n in "${fail_names[@]}"; do echo "  - $n"; done
    echo
    echo "A BRUTE hit means a tenured arena word holds a nursery pointer"
    echo "that scavenge didn't forward — a missed-root in the scavenger."
    echo "Investigate: identify the object class at the hit address, the"
    echo "field within it, and add the corresponding walk in gc.cc."
    echo
    echo "An AUDIT hit means the post-scavenge reachable-graph walk"
    echo "(\`postScavengeAudit\`) found a nursery pointer reachable from"
    echo "a known root — usually a missed walk in one of the per-type"
    echo "scavenger walkers (walkClosure, walkThunk, walkBindings,"
    echo "walkList, walkPair) or in the run() root-set."
    exit 1
fi
exit 0
