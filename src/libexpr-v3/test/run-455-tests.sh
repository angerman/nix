#!/usr/bin/env bash
# #455 test driver — positive / negative / regression(XFAIL-until-fixed).
#
# #455 (current, v3-native): v3 spins on the nixpkgs lib/pkgset fixpoints where TW
# completes. Root cause localized to a mis-resolved upvalue in a lib function as
# COMPILED inside the full nixpkgs lib (see lode/RCA_455_VNATIVE_2026-06-10.md).
#
#   POSITIVE   repro-455-pos-standalone.nix  — the splitString logic (standalone
#              fixpoint-lib) MUST stay correct (= [ "x86_64" "linux" ]).
#   NEGATIVE   repro-455-neg-cycle.nix       — a GENUINE cycle MUST still error
#              (infinite recursion), never be masked into a spin or a value.
#   REGRESSION repro-455-systems-elaborate.nix — the real bug. XFAIL today (v3 spins,
#              wall-capped); AUTO-FLIPS to XPASS (announced) when #455 is fixed.
#
# Exit nonzero only on a REAL regression (POS wrong, or NEG not erroring). XFAIL
# staying XFAIL is exit 0. Usage: [V3=path] [NIXPKGS=path] run-455-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-8s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring'; }
pass=0; fail=0; xfail=0; xpass=0

# --- POSITIVE: standalone splitString logic must be correct + fast ---
out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --file "$HERE/repro-455-pos-standalone.nix" --strict 2>&1 | strip | tail -1)
if [ "$out" = '[ "x86_64" "linux" ]' ]; then
  echo "PASS  positive  (standalone splitString = $out)"; pass=$((pass+1))
else
  echo "FAIL  positive  (standalone splitString: expected [ \"x86_64\" \"linux\" ], got: $out)"; fail=$((fail+1))
fi

# --- NEGATIVE: a genuine cycle must error with infinite recursion (not spin/value) ---
out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --file "$HERE/repro-455-neg-cycle.nix" --strict 2>&1 | strip | tail -3)
if echo "$out" | grep -qi 'infinite recursion'; then
  echo "PASS  negative  (genuine cycle correctly rejected: infinite recursion)"; pass=$((pass+1))
elif echo "$out" | grep -qi 'WallTimeExceeded'; then
  echo "FAIL  negative  (genuine cycle SPUN instead of erroring — masked!)"; fail=$((fail+1))
else
  echo "FAIL  negative  (genuine cycle neither errored nor spun: $out)"; fail=$((fail+1))
fi

# --- REGRESSION (XFAIL until #455 fixed): real nixpkgs lib.strings.splitString ---
NP="${NIXPKGS:-}"
[ -z "$NP" ] && NP=$(nix eval --impure --raw --expr 'builtins.toString <nixpkgs>' 2>/dev/null)
[ -z "$NP" ] && NP=$(nix eval --raw --expr '(builtins.getFlake "nixpkgs").outPath' 2>/dev/null)
if [ -z "$NP" ] || [ ! -d "$NP/lib" ]; then
  echo "SKIP  regression (no nixpkgs; set NIXPKGS=<path>)"
else
  # Substitute the resolved path directly (v3-eval does not honor <nixpkgs>/NIX_PATH
  # here); the .nix fixture keeps the portable <nixpkgs> form for manual runs.
  out=$(env NIX_V3_MAX_WALL_TIME="$WALL" "$V3" \
        --expr "(import $NP/lib).strings.splitString \"-\" \"x86_64-linux\"" --strict 2>&1 | strip | tail -1)
  if [ "$out" = '[ "x86_64" "linux" ]' ]; then
    echo "XPASS *** #455 FIXED *** (lib.strings.splitString = $out) — flip this case to a PASS gate!"; xpass=$((xpass+1))
  elif echo "$out" | grep -qi 'WallTimeExceeded'; then
    echo "XFAIL regression (#455 still open: lib.strings.splitString spins — expected for now)"; xfail=$((xfail+1))
  else
    echo "XFAIL regression (#455 still open: $out)"; xfail=$((xfail+1))
  fi
fi

echo "=== #455: pass=$pass fail=$fail xfail=$xfail xpass=$xpass ==="
[ "$fail" -eq 0 ]
