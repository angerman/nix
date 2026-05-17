#!/usr/bin/env bash
# v3 derivation-hybrid parity test harness — 2026-05-17
#
# Compares v3-direct's drvPath output against TW for a battery of
# derivation shapes that exercise the primDerivationStrict iteration
# logic.  Run every time we change the bytecode wrapper or the
# preprocessor FFI leaf.
#
# Exit codes:
#   0   all cases match
#   1   any case diverges
#   2   harness preflight failed
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "derivation-parity: nix not found at $NIX" >&2
  exit 2
fi

pass=0
fail=0
total=0
failed_cases=()

run() {
  local name="$1" expr="$2"
  local tw v3
  tw=$("$NIX" eval --impure --expr "$expr" 2>&1 | tail -1)
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>&1 | tail -1)
  total=$((total + 1))
  if [[ "$tw" == "$v3" ]]; then
    echo "OK    $name"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    echo "  TW: $tw"
    echo "  v3: $v3"
    fail=$((fail + 1))
    failed_cases+=("$name")
  fi
}

# Case 1: minimal derivation
run "minimal" '
(derivation {
  name = "min";
  system = "x86_64-linux";
  builder = "/bin/sh";
}).drvPath'

# Case 2: with args list
run "with-args" '
(derivation {
  name = "args";
  system = "x86_64-linux";
  builder = "/bin/sh";
  args = ["-e" "-c" "echo hello"];
}).drvPath'

# Case 3: multiple outputs
run "multi-output" '
(derivation {
  name = "multi";
  system = "x86_64-linux";
  builder = "/bin/sh";
  outputs = ["out" "dev" "doc"];
}).drvPath'

# Case 4: env vars (strings, numbers, bools)
run "mixed-env" '
(derivation {
  name = "envmix";
  system = "x86_64-linux";
  builder = "/bin/sh";
  envStr = "hello";
  envInt = 42;
  envBool = true;
}).drvPath'

# Case 5: derivation with derivation in env (cross-ref)
run "drv-cross-ref" '
let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
in (derivation {
  name = "b";
  system = "x86_64-linux";
  builder = "/bin/sh";
  src = a;
}).drvPath'

# Case 6: buildInputs as list of derivations
run "build-inputs-list" '
let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; };
in (derivation {
  name = "c";
  system = "x86_64-linux";
  builder = "/bin/sh";
  buildInputs = [a b];
}).drvPath'

# Case 7: nested chain (3 levels)
run "3-level-chain" '
let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
  b = derivation { name = "b"; system = "x86_64-linux"; builder = "/bin/sh"; buildInputs = [a]; };
  c = derivation { name = "c"; system = "x86_64-linux"; builder = "/bin/sh"; buildInputs = [a b]; };
in c.drvPath'

# Case 8: ignoreNulls=true with null attr
run "ignore-nulls" '
(derivation {
  name = "ign";
  system = "x86_64-linux";
  builder = "/bin/sh";
  __ignoreNulls = true;
  nullAttr = null;
  realAttr = "yes";
}).drvPath'

# Case 9: fixed-output (outputHash)
run "fixed-output" '
(derivation {
  name = "fixed";
  system = "x86_64-linux";
  builder = "/bin/sh";
  outputHash = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
  outputHashAlgo = "sha256";
  outputHashMode = "flat";
}).drvPath'

# Case 10: with passthru (attrset that shouldn't break the hybrid)
run "with-passthru" '
(derivation {
  name = "pt";
  system = "x86_64-linux";
  builder = "/bin/sh";
  passthru = { extra = "meta-info"; };
}).drvPath'

# Case 11: outPath cross-ref (derivation referenced via outPath)
run "outpath-ref" '
let
  a = derivation { name = "a"; system = "x86_64-linux"; builder = "/bin/sh"; };
in (derivation {
  name = "b";
  system = "x86_64-linux";
  builder = "/bin/sh";
  src = a.outPath;
}).drvPath'

# Case 12: list with mixed types
run "mixed-list" '
(derivation {
  name = "mixedlist";
  system = "x86_64-linux";
  builder = "/bin/sh";
  things = ["str1" 42 true];
}).drvPath'

# Case 13: deeper nest — 10-level stdenv-like chain
run "10-level-chain" '
let
  mk = name: deps: derivation {
    inherit name;
    system = "x86_64-linux";
    builder = "/bin/sh";
    buildInputs = deps;
  };
  l1 = mk "l1" [];
  l2 = mk "l2" [l1];
  l3 = mk "l3" [l2];
  l4 = mk "l4" [l3];
  l5 = mk "l5" [l4];
  l6 = mk "l6" [l5];
  l7 = mk "l7" [l6];
  l8 = mk "l8" [l7];
  l9 = mk "l9" [l8];
  l10 = mk "l10" [l9];
in l10.drvPath'

echo
echo "=== derivation-parity results ==="
echo "  total: $total"
echo "  pass:  $pass"
echo "  fail:  $fail"
if (( fail > 0 )); then
  echo "  failed: ${failed_cases[*]}"
  exit 1
fi
exit 0
