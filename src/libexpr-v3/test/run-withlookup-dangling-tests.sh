#!/usr/bin/env bash
# Q1.1 withLookup dangling-reference regression (DEFECT_REVIEW_2026-07-03 §1.1).
#
# withLookup held `Value & w = vm.withStack[i]` across `w = forceValue(vm, w)`.
# forceValue re-enters the dispatch loop; a with-source whose body establishes
# many nested `with` scopes push_backs onto vm.withStack (reserved to only 64),
# and once the stack crosses that capacity the vector REALLOCATES — leaving `w`
# dangling.  The old memoizing `w = forceValue(...)` wrote 8 bytes into freed
# heap and the following `w.isAttrs()/lookup()` read through it: a latent
# default-path UAF.
#
# Failing-first repro (deterministic on this platform): the outer with-source
# `(go DEPTH)`, when forced, recurses DEPTH deep through `with { dummy = n; };
# go (n-1)`, so at the base case ~DEPTH with-scopes are simultaneously live —
# crossing 64 and reallocating vm.withStack WHILE the outer withLookup holds a
# reference to withStack[0].  The outer with resolves to `{ target = 42; }`.
#   - PRE-fix: after the realloc, `w` dangles; `w.isAttrs()` reads the stale
#     (freed) thunk value → the with-entry is skipped → `target` is undefined
#     (eval error), or a crash if the freed buffer was reused.
#   - POST-fix: the value is forced into a LOCAL and memoized BY INDEX
#     (vm.withStack[i] = forced), so the outer with resolves → 42.
#
# Also run under the 1 MB-nursery brute stress so any residual missed-root shows.
#
# Usage:   bash src/libexpr-v3/test/run-withlookup-dangling-tests.sh
# Exit:    0 pass; 1 fail (wrong value / eval error / crash); 2 preflight.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
if [[ ! -x "$V3" ]]; then echo "withlookup-dangling: v3-eval not at $V3" >&2; exit 2; fi

# DEPTH must exceed the withStack reserve (64) with margin so the realloc is
# guaranteed to fire during the with-source force.
DEPTH="${WITH_DEPTH:-200}"
work="$(mktemp -t withlookup-repro.XXXXXX)"
fixture="$work.nix"
trap 'rm -f "$work" "$fixture"' EXIT

cat > "$fixture" <<NIX
let
  # Forcing (go DEPTH) recurses DEPTH deep; each level keeps its \`with\` scope
  # live while the deeper call runs, so ~DEPTH with-scopes stack up before the
  # base case returns { target = 42; } — reallocating vm.withStack mid-force.
  go = n: if n <= 0 then { target = 42; }
          else with { dummy = n; }; go (n - 1);
in with (go $DEPTH); target
NIX

fail=0

# Arm 1: default settings — deterministic value check.
r1="$(NIX_V3_DIRECT_EVAL=1 "$V3" --file "$fixture" 2>/dev/null | tail -1)"
if [[ "$r1" == "42" ]]; then
  echo "withlookup-dangling: PASS default (DEPTH=$DEPTH → target=42)"
else
  echo "withlookup-dangling: FAIL default (DEPTH=$DEPTH → '$r1', expected 42)" >&2
  fail=1
fi

# Arm 2: 1 MB-nursery brute stress + audit — must stay clean and correct.
errf="$work.err"
r2="$(NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY_SIZE=1 V3_DBG_NURSERY_AUDIT=1 \
        "$V3" --file "$fixture" 2>"$errf" | tail -1)"
flags="$(grep -c "reachable via" "$errf" 2>/dev/null)" || true
rm -f "$errf"
if [[ "$r2" == "42" && "${flags:-0}" -eq 0 ]]; then
  echo "withlookup-dangling: PASS brute-stress (target=42; 0 audit flags)"
else
  echo "withlookup-dangling: FAIL brute-stress (target='$r2', flags=${flags:-?})" >&2
  fail=1
fi

exit $fail
