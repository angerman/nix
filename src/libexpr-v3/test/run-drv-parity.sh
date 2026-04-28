#!/usr/bin/env bash
# BR-3.8 — byte-equal drvPath parity harness for the v3 native
# derivationStrict path (BR-3 Phase A).
#
# For a battery of representative simple-shape derivations:
#  1. Eval drvPath under tree-walker.
#  2. Eval drvPath under NIX_USE_V3=1 (native + bridge fallback).
#  3. Assert byte-equal.
#  4. Where they differ, dump both .drv files for diff.
#
# Phase A only covers deferred-output simple cases (no outputHash,
# no __structuredAttrs, no __contentAddressed, no __impure) — fixed-
# output and CA cases live in Phases B/C/D, so they're explicitly
# excluded from this harness for now.
#
# Usage:
#   ./run-drv-parity.sh               # summary
#   V3_PARITY_VERBOSE=1 ./run-drv-parity.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_INST="${NIX_INST:-$ROOT/build/src/nix/nix-instantiate}"

if [[ ! -x "$NIX_INST" ]]; then
  echo "nix-instantiate not found at $NIX_INST" >&2
  exit 1
fi

verbose="${V3_PARITY_VERBOSE:-0}"

# Each test case is a (name, expression) pair.  The expression must
# evaluate to a string ending in `.drv` (typically `drv.drvPath`).
TESTS=(
  # 1. Minimal three-attr derivation.
  "minimal:(derivation { name = \"a\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }).drvPath"

  # 2. With args.
  "with-args:(derivation { name = \"b\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; args = [ \"-c\" \"true\" ]; }).drvPath"

  # 3. With env vars (string).
  "with-env-string:(derivation { name = \"c\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; PATH = \"/bin\"; LANG = \"C\"; }).drvPath"

  # 4. With int env.
  "with-env-int:(derivation { name = \"d\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; verbosity = 3; }).drvPath"

  # 5. With bool env.
  "with-env-bool:(derivation { name = \"e\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; verbose = true; quiet = false; }).drvPath"

  # 6. With null env (coerces to empty string).
  "with-env-null:(derivation { name = \"f\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; OPT = null; }).drvPath"

  # 7. With list env (string-joined).
  "with-env-list:(derivation { name = \"g\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; FLAGS = [ \"-O2\" \"-Wall\" ]; }).drvPath"

  # 8. Multiple outputs.
  "multi-outputs:(derivation { name = \"h\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; outputs = [ \"out\" \"dev\" \"lib\" ]; }).drvPath"

  # 9. Derivation depending on another (Built context).
  "drv-dep:let dep = derivation { name = \"dep\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }; in (derivation { name = \"i\"; system = \"x86_64-linux\"; builder = \"\${dep}/bin/sh\"; }).drvPath"

  # 10. Long name with all the legal chars.
  "complex-name:(derivation { name = \"my-pkg.0+1?test_v=2-final\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; }).drvPath"

  # --- Phase B: fixed-output derivations (BR-3.10) ---

  # 11. fetchurl-shaped flat-mode fixed-output drv.
  "fixed-flat:(derivation { name = \"fetched\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; outputHash = \"0jqkajk1c0pjabwx6dknh6sjs61b7llbifs6yiyzy7lks5njgxw0\"; outputHashAlgo = \"sha256\"; outputHashMode = \"flat\"; }).drvPath"

  # 12. tarball-shaped recursive-mode fixed-output drv (64-hex sha256).
  "fixed-recursive:(derivation { name = \"tarball\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; outputHash = \"1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef\"; outputHashAlgo = \"sha256\"; outputHashMode = \"recursive\"; }).drvPath"

  # 13. nar-mode (newer name for recursive) fixed-output drv.
  "fixed-nar:(derivation { name = \"narred\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; outputHash = \"abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234\"; outputHashAlgo = \"sha256\"; outputHashMode = \"nar\"; }).drvPath"

  # 14. Default-method fixed-output (no outputHashMode → defaults to flat).
  "fixed-default:(derivation { name = \"defaulted\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; outputHash = \"0jqkajk1c0pjabwx6dknh6sjs61b7llbifs6yiyzy7lks5njgxw0\"; outputHashAlgo = \"sha256\"; }).drvPath"

  # --- Phase C: contentAddressed / impure / __ignoreNulls (BR-3.11) ---

  # 15. __ignoreNulls=true: null attrs filtered from env.
  "ignore-nulls-true:(derivation { name = \"ign\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; OPT = null; SET = \"v\"; __ignoreNulls = true; }).drvPath"

  # 16. __ignoreNulls=true with multiple null attrs (filter cascades).
  "ignore-nulls-multi:(derivation { name = \"mul\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; A = null; B = null; C = \"keep\"; __ignoreNulls = true; }).drvPath"

  # 17. __contentAddressed=true (CA derivation, deferred floating outputs).
  # Requires the ca-derivations experimental feature.
  "ca-flat:(derivation { name = \"ca\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; __contentAddressed = true; outputHashAlgo = \"sha256\"; outputHashMode = \"recursive\"; }).drvPath"

  # 18. __impure=true (impure derivation).
  # Requires the impure-derivations experimental feature.
  "impure:(derivation { name = \"imp\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; __impure = true; outputHashAlgo = \"sha256\"; outputHashMode = \"recursive\"; }).drvPath"

  # --- Phase D: __structuredAttrs (BR-3.12) ---

  # 19. Minimal __structuredAttrs derivation.
  "structured-min:(derivation { name = \"sa\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; __structuredAttrs = true; }).drvPath"

  # 20. __structuredAttrs with mixed-type attrs (string, int, bool, list).
  "structured-mixed:(derivation { name = \"sb\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; __structuredAttrs = true; CFLAGS = \"-O2\"; jobs = 4; verbose = true; deps = [ \"a\" \"b\" \"c\" ]; }).drvPath"

  # 21. __structuredAttrs with nested attrset value.
  "structured-nested:(derivation { name = \"sc\"; system = \"x86_64-linux\"; builder = \"/bin/sh\"; __structuredAttrs = true; outputChecks = { out = { allowedReferences = [ \"x\" ]; }; }; }).drvPath"
)

# Phase C tests need the experimental features enabled.
export NIX_CONFIG="experimental-features = ca-derivations impure-derivations"

pass=0
fail=0
total=0
failed_cases=()

for entry in "${TESTS[@]}"; do
  name="${entry%%:*}"
  expr="${entry#*:}"
  total=$((total + 1))

  tw_out=$("$NIX_INST" --eval --strict -E "$expr" 2>/dev/null) || tw_out="<tw-error>"
  v3_out=$(NIX_USE_V3=1 "$NIX_INST" --eval --strict -E "$expr" 2>/dev/null) || v3_out="<v3-error>"

  if [[ "$tw_out" == "$v3_out" && "$tw_out" != "<tw-error>" ]]; then
    pass=$((pass + 1))
    [[ "$verbose" == "1" ]] && echo "OK     $name : $tw_out"
  else
    fail=$((fail + 1))
    failed_cases+=("$name")
    echo "FAIL   $name"
    echo "  tree-walker: $tw_out"
    echo "  v3-native:   $v3_out"
  fi
done

echo
echo "=== v3 native derivationStrict drvPath parity ==="
echo "  total tests: $total"
echo "  passing:     $pass"
echo "  failing:     $fail"

if [[ $fail -gt 0 ]]; then
  echo
  echo "Failed cases: ${failed_cases[*]}"
  exit 1
fi
