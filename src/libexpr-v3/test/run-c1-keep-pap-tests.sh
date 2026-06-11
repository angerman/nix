#!/usr/bin/env bash
# C-1 (CODEBASE_REVIEW_2026-06-11) regression driver — stale
# CFF_FORCE_WB_PTR_KEEP writeback disarm.
#
# Bug (verified-in-code): the OP_ATTRS_SELECT* writeback arm sites set
# CFF_FORCE_WB_PTR_KEEP on a raw pointer into a (often shared) Bindings entry
# whose value is App(Thunk,arg) — a not-yet-forced spine.  op_force_slow then
# forces the leaf, which becomes an under-applied closure-PAP, and
# applyForceWriteback's KEEP branch saw Tag::App and `return false` WITHOUT
# disarming.  A PAP is permanently App-tagged, so the KEEP stayed armed; the
# NEXT force in the same frame fired the stale KEEP and cross-wrote ITS WHNF
# (e.g. the String "21" from forcing llvmVersion) into the old entry — the
# firefox getLib="21" residual.
#
# Fix: applyForceWriteback's KEEP branch treats isUnderappliedClosurePap(top)
# as WHNF (writes the PAP — it IS the entry's true value — and disarms), and
# never memoizes a Tag::Blackhole.  See vm.cc applyForceWriteback + the
# armKeepBeltCheck belt at the 3 arm sites.
#
# These cases exercise PAP-valued rec-attrset entries selected and then
# interleaved with forcing sibling entries (the cross-write shape).  They are
# POSITIVE guards: the firefox cross-write itself is context-specific and not
# synthetically reproducible (the C-1 falsifier V3_DBG_KEEP_PAP=1 confirms the
# mechanism is live on firefox.drvPath), so these guard against a regression in
# the KEEP/PAP disarm logic rather than reproduce the original failure.
#
# Usage: [V3=path] run-c1-keep-pap-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-10s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

check() {
  local label="$1" expr="$2" expected="$3" out
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --strict --expr "$expr" 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  elif echo "$out" | grep -qi 'WallTimeExceeded'; then
    echo "FAIL  $label  (SPUN — regression! expected: $expected)"; fail=$((fail+1))
  else
    echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1))
  fi
}

# PAP entry selected, then a sibling int forced, then the PAP applied again.
check "pap-then-sibling-then-apply" \
  'let add = a: b: a + b; s = rec { f = add 1; g = 21; }; in [ (s.f 100) s.g (s.f 5) ]' \
  '[ 101 21 6 ]'

# getLib/llvmVersion shape: a string-returning PAP entry + a sibling string
# "21", selected interleaved.  Pre-fix family: probe = llvmVersion gets the
# getLib PAP's slot cross-written / vice-versa.
check "getlib-llvmversion-shape" \
  'let k = a: b: a; X = rec { getLib = k "L"; llvmVersion = "21"; probe = X.llvmVersion; q = X.getLib 9; }; in [ X.probe X.q ]' \
  '[ "21" "L" ]'

# PAP fed through foldl' (the original cross-write was firefox-context but
# foldl' is a frequent interleave site).
check "pap-via-foldl" \
  'let add = a: b: a + b; s = rec { f = add 1; g = 21; }; in builtins.foldl'"'"' (acc: x: acc + x) 0 [ (s.f 100) s.g ]' \
  '122'

# Selecting the same PAP entry repeatedly must keep returning the SAME
# partial application (a lambda), never a saturated/poisoned result.
check "repeated-pap-select-stays-lambda" \
  'let add = a: b: a + b; s = rec { f = add 1; }; in [ (builtins.isFunction s.f) (builtins.isFunction s.f) (s.f 7) ]' \
  '[ true true 8 ]'

echo "--- C-1 KEEP-PAP: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
