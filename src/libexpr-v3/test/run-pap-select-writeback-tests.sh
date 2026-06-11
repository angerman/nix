#!/usr/bin/env bash
# PAP-in-attrset SELECT-writeback test driver — positive / negative / regression.
#
# Bug (FIXED 2026-06-11): OP_ATTRS_SELECT's memoizing App-writeback (the
# `if (slot.isAppLike())` force-writeback at vm.cc ~8658 / ~8853) fired on an
# UNDER-APPLIED closure-PAP.  A PAP is already WHNF; forcing+memoizing it
# saturated it to its result type and poisoned the (shared) attrset entry.
# Manifested as the systemic v3-direct drvPath divergence: nixpkgs python3's
# `passthru.pythonAtLeast` (a PAP) mutated Thunk->App->Bool, so
# `passthru.pythonAtLeast "3.14"` did OP_CALL on a Bool ("callee is not a
# closure"), native derivationStrict fell back to /v3-fake-store/, and EVERY
# package depending on python3 got a wrong drvPath (run-759: 0/8).
# Fix: skip the writeback when isUnderappliedClosurePap(slot) (mirror OP_FORCE).
# See lode/RCA_DRVPATH_SYSTEMIC_DIVERGENCE_2026-06-11.md.
#
#   POSITIVE   repro-pap-select-pos.nix   PAP stored in an attrset, forced then
#              selected+called repeatedly, stays a callable function.
#   NEGATIVE   repro-pap-select-neg.nix   a saturated/lazy App attr (mapAttrs)
#              is STILL forced+memoized (the guard didn't disable that).
#   REGRESSION (nixpkgs, gated) python3.drvPath byte-identical v3 vs TW — the
#              canonical trigger (was /v3-fake-store/). Skipped without nixpkgs.
#
# Exit nonzero on any failure.  Usage: [V3=path] [NIX=path] [NIXPKGS=path] run-...
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }
strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

check() {
  local label="$1" file="$2" expected="$3"
  local out
  out=$(env NIX_V3_MAX_WALL_TIME=8s "$V3" --file "$HERE/$file" --strict 2>&1 | strip | tail -1)
  if [ "$out" = "$expected" ]; then echo "PASS  $label  ($out)"; pass=$((pass+1))
  else echo "FAIL  $label  (expected: $expected  got: $out)"; fail=$((fail+1)); fi
}

check "positive (PAP-in-attrset)   " repro-pap-select-pos.nix '[ 11 21 31 "lambda" 101 102 103 41 51 ]'
check "negative (saturated App memo)" repro-pap-select-neg.nix '[ 11 11 21 "set" ]'

# --- REGRESSION: python3.drvPath byte-identical (nixpkgs-gated) ---
NP="${NIXPKGS:-}"
[ -z "$NP" ] && NP=$("$NIX" eval --impure --raw --expr 'builtins.toString <nixpkgs>' 2>/dev/null)
if [ -z "$NP" ] || [ ! -d "$NP/pkgs" ] || [ ! -x "$NIX" ]; then
  echo "SKIP  regression-nixpkgs (set NIXPKGS=<path>; python3.drvPath parity)"
else
  tw=$("$NIX" eval --impure --raw --expr "(import $NP {}).python3.drvPath" 2>/dev/null)
  v3=$(env NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=120s NIX_V3_MAX_HEAP=8G \
        "$NIX" eval --impure --raw --expr "(import $NP {}).python3.drvPath" 2>/dev/null)
  if [ -n "$tw" ] && [ "$tw" = "$v3" ]; then
    echo "PASS  regression (python3.drvPath = ${tw##*/})"; pass=$((pass+1))
  else
    echo "FAIL  regression (python3.drvPath: tw=${tw##*/} v3=${v3##*/})"; fail=$((fail+1))
  fi
fi

echo "--- PAP-select-writeback: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
