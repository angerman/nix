#!/usr/bin/env bash
# WS-1 (2026-07-13 senior review) — realise-parity regression suite.
#
# Kills the hypothesis "v3's read-class primops need not realise their
# argument's context."  The tree-walker builds an un-realised derivation
# output before reading it; pre-WS-1 v3 did NOT, for:
#   C1  builtins.hashFile      (raw nix::hashFile on the coerced path)
#   C2  builtins.readFileType  (raw std::filesystem::symlink_status)
#   C3  builtins.findFile      (per-element search path context dropped)
#   C4  builtins.pathExists    (context dropped AND a failed IFD build was
#                               swallowed by catch(...) into `false`)
#
# The fix routes every one through EvalState::realisePath with the string
# context FORWARDED (primops.cc v3RealisePathArg), matching TW.
#
# Test design.  Two tiers:
#   * ALWAYS-RUN — no builder needed: literal source paths (no context, the
#     common path must be unchanged) and `toFile` store paths (context, but
#     already valid).  These prove parity + guard the fast path.
#   * BUILD-GATED — needs a working store/builder (preflight below).  Each
#     case builds a UNIQUE, never-before-built derivation (nonce in the
#     builder script) so its output is genuinely un-realised at eval time.
#     That is what discriminates the fix: with the fix reverted, v3 errors
#     ("does not exist") where TW builds-and-succeeds → divergence → FAIL.
#     The C4 failing-build case is failing-first: pre-fix v3 prints `false`,
#     post-fix v3 (and TW) surface the build error.
#
# Exit codes: 0 all pass (or build-tier cleanly SKIPPED), 1 any divergence,
#             2 harness preflight failed.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -x "$NIX" ]]; then
  echo "ws1-realise-parity: nix not found at $NIX" >&2
  exit 2
fi

pass=0
fail=0
skip=0
total=0
failed_cases=()

# TW vs v3 last-line equality on a single expression.
run_eq() {
  local name="$1" expr="$2"
  local tw v3
  tw=$("$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  total=$((total + 1))
  if [[ "$tw" == "$v3" ]]; then
    echo "OK    $name  => $v3"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    echo "  TW: $tw"
    echo "  v3: $v3"
    fail=$((fail + 1))
    failed_cases+=("$name")
  fi
}

# Build-tier parity: run v3 FIRST on a fresh (never-built) derivation, THEN
# TW.  v3-first matters — if TW ran first it would populate the store and mask
# whether v3 realises on its own.  With v3 first, a reverted fix makes v3 error
# ("does not exist") while TW builds-and-succeeds → divergence → FAIL.
run_eq_v3first() {
  local name="$1" expr="$2"
  local tw v3
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  tw=$("$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  total=$((total + 1))
  if [[ "$tw" == "$v3" ]]; then
    echo "OK    $name  => $v3"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    echo "  TW: $tw"
    echo "  v3: $v3"
    fail=$((fail + 1))
    failed_cases+=("$name")
  fi
}

# C4-shaped assertion: a read over a FAILING build must NOT collapse to a
# boolean.  Both evaluators must surface an error (neither `true` nor
# `false`).  This is the failing-first guard: pre-fix v3 printed `false`.
run_must_error() {
  local name="$1" expr="$2"
  local tw v3
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  tw=$("$NIX" eval --impure --expr "$expr" 2>&1 | tail -1 | sed 's/^[[:space:]]*//')
  total=$((total + 1))
  # Pass iff neither evaluator returned a bare boolean (i.e. both errored),
  # matching TW's "propagate the build failure" behaviour.
  if [[ "$v3" != "true" && "$v3" != "false" && "$tw" != "true" && "$tw" != "false" ]]; then
    echo "OK    $name  (both surfaced an error, not a boolean)"
    pass=$((pass + 1))
  else
    echo "FAIL  $name  (a failed IFD build must not become a boolean)"
    echo "  TW: $tw"
    echo "  v3: $v3"
    fail=$((fail + 1))
    failed_cases+=("$name")
  fi
}

# ---------------------------------------------------------------------------
# Tier 1 — ALWAYS-RUN (no builder).
# ---------------------------------------------------------------------------

# Regression: literal source paths carry NO context — the common fast path
# must be byte-identical to TW and must NOT attempt a build.  Use this very
# script as a stable on-disk regular file.
SELF="$TEST_DIR/run-ws1-realise-parity-tests.sh"
run_eq "C2-nocontext-readFileType-file"   "builtins.readFileType $SELF"
run_eq "C2-nocontext-readFileType-dir"    "builtins.readFileType $TEST_DIR"
run_eq "C4-nocontext-pathExists-true"     "builtins.pathExists $SELF"
run_eq "C4-nocontext-pathExists-false"    "builtins.pathExists $TEST_DIR/does-not-exist-$$"
run_eq "C1-nocontext-hashFile"            "builtins.hashFile \"sha256\" $SELF"

# Context-bearing but already-valid store path (toFile): exercises the
# context-forwarding decode/realiseContext path with no builder required.
TOFILE='(builtins.toFile "ws1-tofile" "ws1-realise-parity-content")'
run_eq "C2-toFile-readFileType"           "builtins.readFileType \"\${$TOFILE}\""
run_eq "C4-toFile-pathExists"             "builtins.pathExists \"\${$TOFILE}\""
run_eq "C1-toFile-hashFile"               "builtins.hashFile \"sha256\" \"\${$TOFILE}\""

# ---------------------------------------------------------------------------
# Tier 2 — BUILD-GATED (needs a working store/builder).
# ---------------------------------------------------------------------------

# Preflight: can we build a trivial derivation here?  If not, SKIP the tier
# (constrained CI without a store) rather than FAIL.
PREFLIGHT_OK=0
preflight_expr='(derivation {
  name = "ws1-preflight";
  system = builtins.currentSystem;
  builder = "/bin/sh";
  args = [ "-c" "echo -n ok > $out" ];
}).outPath'
if "$NIX" build --impure --no-link --expr "$preflight_expr" >/dev/null 2>&1 \
   || "$NIX" build --impure --no-link --expr "$preflight_expr" 2>&1 | grep -q '/nix/store/'; then
  PREFLIGHT_OK=1
fi

# Build a derivation whose output is UNIQUE (nonce) → never-before-built →
# genuinely un-realised at eval time, so realisation is actually required.
drv_reading() {   # $1 = query template with %D for the drv ref
  local nonce="ws1-$$-$RANDOM-$total"
  local drv='derivation {
    name = "ws1-rt";
    system = builtins.currentSystem;
    builder = "/bin/sh";
    args = [ "-c" "echo -n hi-'"$nonce"' > $out" ];
  }'
  echo "let d = $drv; in $1"
}

failing_drv_reading() {  # $1 = query template using `f`
  local nonce="ws1fail-$$-$RANDOM-$total"
  local drv='derivation {
    name = "ws1-fail";
    system = builtins.currentSystem;
    builder = "/bin/sh";
    args = [ "-c" "exit 1 # '"$nonce"'" ];
  }'
  echo "let f = $drv; in $1"
}

if [[ "$PREFLIGHT_OK" == "1" ]]; then
  # C1/C2/C4 discriminators — reading an un-realised output must BUILD it
  # (both evaluators) and agree.  Reverting the fix makes v3 error here.
  run_eq_v3first "C2-unbuilt-readFileType" "$(drv_reading 'builtins.readFileType "${d}"')"
  run_eq_v3first "C1-unbuilt-hashFile"     "$(drv_reading 'builtins.hashFile "sha256" "${d}"')"
  run_eq_v3first "C4-unbuilt-pathExists"   "$(drv_reading 'builtins.pathExists "${d}"')"

  # C4 failing-first: pathExists over a FAILING build must surface the error,
  # not silently return false.
  run_must_error "C4-failing-build-pathExists" "$(failing_drv_reading 'builtins.pathExists "${f}"')"

  # C3 — a search-path element that is a derivation output must be built and
  # rewritten (findFile).  Query `<ws1>` against a nixPath list whose entry
  # path is an un-realised drv output.
  c3="$(drv_reading 'builtins.findFile [ { prefix = "ws1"; path = "${d}"; } ] "ws1"')"
  # `d` here is a single-file output; findFile resolves the prefix root.  We
  # only require TW/v3 agreement (both build+resolve, or both error alike).
  run_eq_v3first "C3-searchpath-drv-output" "$c3"

  # C6 — scopedImport must realise its path arg too (H4-surfaced).  Build a
  # derivation whose output is a valid Nix expression, then scopedImport it.
  nonce6="ws1si-$$-$RANDOM"
  c6='let d = derivation {
    name = "ws1-si";
    system = builtins.currentSystem;
    builder = "/bin/sh";
    args = [ "-c" "printf 42 > $out # '"$nonce6"'" ];
  }; in builtins.scopedImport {} "${d}"'
  run_eq_v3first "C6-scopedImport-drv-output" "$c6"
else
  echo "SKIP  build tier (no working store/builder in this environment)"
  skip=$((skip + 7))
fi

# ---------------------------------------------------------------------------
echo "-----------------------------------------------------------------------"
echo "ws1-realise-parity: $pass/$total passed, $fail failed, ${skip} skipped"
if [[ $fail -ne 0 ]]; then
  printf '  failed: %s\n' "${failed_cases[@]}"
  exit 1
fi
exit 0
