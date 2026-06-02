#!/usr/bin/env bash
# readDir / import attrset-coercion parity — TW vs v3-direct byte-equality.
#
# Regression guard for the TW_VALUE_ERADICATION F4 readDir/import
# regression (2026-06-02): when the #875 bridge was deleted, primReadDir
# and primImport replaced TW's `realisePath -> coerceToPath ->
# coerceToString` with a bespoke handler that only resolved a SINGLE
# level of STRING `outPath`.  haskell.nix's `cleanSourceWith` passes
# `builtins.readDir { outPath = <derivation-attrs>; filterPath; }` — i.e.
# `outPath` is ITSELF an attrset — so the coercion must RECURSE
# (outPath -> its outPath -> ... -> store-path string) and also handle
# `__toString`.  The one-level handler fell through to
# `expected a string or path` -> HNE eval broke.  Fixed by routing the
# attrset case through the v3-native `toStringCoerceCtx` (the recursive
# coerceToString mirror).
#
# These cases exercise the coercion shapes WITHOUT needing haskell.nix or
# a real IFD build: empty-context local paths realise to themselves, so
# the test is offline + fast.  The real-workload guard is HNE itself.
#
# Usage:  NIX=/path/to/nix ./run-readdir-import-coerce-parity.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "readdir-import-coerce-parity: nix not found at $NIX" >&2
  exit 1
fi

EXF=(--extra-experimental-features "nix-command flakes")
pass=0
fail=0
failed=()

# Positive: TW and v3-direct must agree byte-for-byte on the JSON result.
check() {  # label expr
  local label="$1" expr="$2" tw v3
  tw=$("$NIX" "${EXF[@]}" eval --impure --json --expr "$expr" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" "${EXF[@]}" eval --impure --json --expr "$expr" 2>/dev/null)
  if [[ -n "$tw" && "$tw" == "$v3" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$label (TW=$tw v3=$v3)")
  fi
}

# Negative: both must FAIL, and the error message must match byte-for-byte
# (the "cannot coerce a set to a string: ..." path).
check_err() {  # label expr expect_substr
  local label="$1" expr="$2" want="$3" twerr v3err
  twerr=$("$NIX" "${EXF[@]}" eval --impure --expr "$expr" 2>&1 >/dev/null)
  v3err=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" "${EXF[@]}" eval --impure --expr "$expr" 2>&1 >/dev/null)
  if [[ "$twerr" == *"$want"* && "$v3err" == *"$want"* ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$label (TWerr=...$twerr... v3err=...$v3err...)")
  fi
}

# --- Fixture dir with a couple of known entries + an importable file. ---
D=$(mktemp -d)
touch "$D/a.txt" "$D/b.txt"
mkdir -p "$D/sub"
cat > "$D/default.nix" <<'EOF'
{ answer = 42; nested = { deep = "ok"; }; }
EOF
# String form of the path (no string context).
P="\"$D\""

# ---------------------------------------------------------------------
# readDir
# ---------------------------------------------------------------------
# Baseline: plain string / path.
check "readDir/string"  "builtins.attrNames (builtins.readDir $P)"
check "readDir/path"    "builtins.attrNames (builtins.readDir $D)"
# One-level STRING outPath (the shape the old handler could do).
check "readDir/outPath-string" \
  "builtins.attrNames (builtins.readDir { outPath = $P; })"
# RECURSIVE attrset outPath — THE haskell.nix regression shape:
# outPath is itself an attrset whose outPath is the path string.
check "readDir/outPath-attrs-recursive" \
  "builtins.attrNames (builtins.readDir { outPath = { outPath = $P; }; filterPath = null; })"
# __toString coercion (called first, before outPath).
check "readDir/toString" \
  "builtins.attrNames (builtins.readDir { __toString = self: $P; })"
# Two levels of recursion via mixed __toString / outPath.
check "readDir/outPath-then-toString" \
  "builtins.attrNames (builtins.readDir { outPath = { __toString = self: $P; }; })"

# ---------------------------------------------------------------------
# import
# ---------------------------------------------------------------------
check "import/string"   "(import $P).answer"
check "import/path"     "(import $D).answer"
check "import/outPath-string" "(import { outPath = $P; }).answer"
check "import/outPath-attrs-recursive" \
  "(import { outPath = { outPath = $P; }; filterPath = null; }).nested.deep"
check "import/toString" "(import { __toString = self: $P; }).answer"

# ---------------------------------------------------------------------
# Negative: an attrset with neither outPath nor __toString must fail with
# TW's exact "cannot coerce a set to a string" wording on BOTH evaluators.
# ---------------------------------------------------------------------
check_err "readDir/no-coerce" \
  "builtins.readDir { foo = 1; }" \
  "cannot coerce a set to a string"
check_err "import/no-coerce" \
  "import { foo = 1; }" \
  "cannot coerce a set to a string"

rm -rf "$D"

# ---------------------------------------------------------------------
echo
echo "=== readDir/import coerce parity ==="
echo "  passing: $pass"
echo "  failing: $fail"
if (( fail > 0 )); then
  printf '  FAIL: %s\n' "${failed[@]}"
  exit 1
fi
echo "  ALL PASS"
