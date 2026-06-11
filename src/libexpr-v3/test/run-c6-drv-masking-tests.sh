#!/usr/bin/env bash
# C-6 (CODEBASE_REVIEW_2026-06-11) regression driver — derivationStrict
# loud-failure inversion.
#
# Bug class (still live at HEAD pre-fix): primDerivationStrict's native path
# was wrapped in `catch (const std::exception&)` that only logged under
# V3_DRV_DEBUG and fell through to the /v3-fake-store/ synthesizer EVEN WHEN A
# REAL STORE IS WIRED (state.nixEvalState != nullptr).  Any native-path
# exception — including a user `throw` inside builder/args/env — produced a
# silently-wrong fake drvPath instead of failing.  This single error-masking
# pattern cost weeks on the drvPath divergence.
#
# Fix: with a store wired, native-path exceptions rethrow; reaching the
# fake-store synthesizer with a store wired is itself an error.  Fake-store
# synthesis is reserved for standalone no-store eval, behind
# NIX_V3_ALLOW_FAKE_STORE=1 as a debug escape hatch.
#
# The C path (primDerivationStrict / __derivationStrictRaw) is what carries
# the fake-store fallback; the default `derivationStrict` is a bytecode
# wrapper → __derivationFromPreprocessed which already fails loudly.  We drive
# both: __derivationStrictRaw to exercise the C-path guard directly, and the
# default derivation builtin for the throwing-builder invariant.
#
#   POSITIVE   native ON + valid drv         → real /nix/store/ drvPath
#   NEGATIVE   throwing builder              → ERROR, never a path (no masking)
#   REGRESSION native OFF + store wired      → ERROR "refusing to synthesize"
#              (was: silent /v3-fake-store/ path)
#   ESCAPE     native OFF + ALLOW_FAKE_STORE → /v3-fake-store/ path (override)
#
# Usage: [V3=path] run-c6-drv-masking-tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
V3="${V3:-$ROOT/build/src/libexpr-v3/v3-eval}"
WALL="${NIX_V3_MAX_WALL_TIME:-15s}"
[ -x "$V3" ] || { echo "v3-eval not found at $V3" >&2; exit 2; }

strip() { grep -vE 'stack size|setrlimit|search path|does not exist, ignoring|warning:'; }
pass=0; fail=0

VALID='(builtins.__derivationStrictRaw { name="x"; system="s"; builder="/bin/sh"; }).drvPath'
THROWS='(derivation { name="x"; system="s"; builder=throw "boom"; }).drvPath'

# check_match label  env-prefix  expr  expected-substring
check_match() {
  local label="$1" envp="$2" expr="$3" want="$4" out
  out=$(env $envp NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --expr "$expr" 2>&1 | strip | tail -1)
  if echo "$out" | grep -qF "$want"; then
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  else
    echo "FAIL  $label  (wanted substring: $want  got: $out)"; fail=$((fail+1))
  fi
}

# check_nofake label  env-prefix  expr   — output must NOT be a fake-store
# *result value*.  A printed result value is quote-prefixed ("/v3-fake-store/…);
# an error message that merely mentions the path string is fine (and expected).
check_nofake() {
  local label="$1" envp="$2" expr="$3" out
  out=$(env $envp NIX_V3_MAX_WALL_TIME="$WALL" "$V3" --expr "$expr" 2>&1 | strip | tail -1)
  if echo "$out" | grep -qF '"/v3-fake-store/'; then
    echo "FAIL  $label  (MASKED — produced fake-store path: $out)"; fail=$((fail+1))
  else
    echo "PASS  $label  ($out)"; pass=$((pass+1))
  fi
}

check_match  "positive  native-real-path " ""                       "$VALID"  "/nix/store/"
check_nofake "negative  throwing-builder  " ""                       "$THROWS"
check_match  "negative  throwing-builder  " ""                       "$THROWS" "boom"
check_match  "regression no-native-throws " "V3_DRV_NO_NATIVE=1"      "$VALID"  "refusing to synthesize"
check_nofake "regression no-native-throws " "V3_DRV_NO_NATIVE=1"      "$VALID"
check_match  "escape    allow-fake-store  " "V3_DRV_NO_NATIVE=1 NIX_V3_ALLOW_FAKE_STORE=1" "$VALID" "/v3-fake-store/"

echo "--- C-6 drv-masking: $pass passed, $fail failed ---"
[ "$fail" -eq 0 ] || exit 1
