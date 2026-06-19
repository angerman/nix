#!/usr/bin/env bash
#
# run-chain-bindings-parity.sh — Lever A (ChainBindings) byte-equality guard.
#
# The chain representation (NIX_V3_CHAIN_BINDINGS=1, see
# lode/MEMORY_REPRESENTATION_2026-06-07.md §10) is a PURE REFACTOR of attrset
# `//`: enabling it must NOT change any eval result.  This test runs a battery
# of chain-stressing expressions through v3 with chains OFF vs ON and asserts
# byte-identical stdout.  It is the regression guard for the consumer-audit
# bugs found 2026-06-07 (the structuredAttrs JSON serializer dropping parent
# entries; valuesEqual index-compare; OP_ATTRS_SELECT writeback contamination).
#
# Pure (no nixpkgs): chains are forced via NIX_V3_CHAIN_MIN_NA=2 so even small
# attrsets chain, and a structured-attrs `derivation` exercises the
# store-hash-critical serialiser without needing a channel.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -uo pipefail
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || echo "$SELF_DIR/../../..")"
NIX_BIN="${NIX_BIN:-$REPO_ROOT/build/src/nix/nix}"
[[ -x "$NIX_BIN" ]] || { echo "nix binary not found: $NIX_BIN" >&2; exit 2; }

if [[ -t 1 ]]; then GRN=$'\e[32m'; RED=$'\e[31m'; R=$'\e[0m'; else GRN=''; RED=''; R=''; fi
pass=0; fail=0

# Aggressive chaining so tiny attrsets chain too (exercises the audit surface).
COMMON_ENV=(NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1 NIX_V3_CHAIN_MIN_NA=2 NIX_V3_CHAIN_MAX_NB=64)

# check <label> <expr>  — eval with chains OFF and ON, assert identical stdout.
# Chains are default-ON (Lever A, 2026-06-07), so the OFF arm forces them off
# with NIX_V3_CHAIN_BINDINGS=0; the ON arm forces them on with =1.
check() {
  local label="$1" expr="$2"
  local off on
  off=$(env "${COMMON_ENV[@]}" NIX_V3_CHAIN_BINDINGS=0   "$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null)
  on=$( env "${COMMON_ENV[@]}" NIX_V3_CHAIN_BINDINGS=1   "$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null)
  if [[ "$off" == "$on" && -n "$off" ]]; then
    printf '  %s✓%s %s\n' "$GRN" "$R" "$label"; ((pass++))
  else
    printf '  %s✗%s %s\n' "$RED" "$R" "$label"
    printf '      chain-off: %s\n' "$off"
    printf '      chain-on : %s\n' "$on"
    ((fail++))
  fi
}

PAD='builtins.listToAttrs (builtins.genList (i: { name = "k" + toString i; value = i; }) 20)'

echo "── ChainBindings parity (chain-off == chain-on) ──"
check "attrNames count"        "let c = $PAD // { extra = 99; }; in builtins.length (builtins.attrNames c)"
check "attrValues sum"         "let c = $PAD // { extra = 99; }; in builtins.foldl' (a: b: a + b) 0 (builtins.attrValues c)"
check "toJSON full attrset"    "let c = $PAD // { extra = 99; }; in builtins.toJSON c"
check "toJSON nested chain"    "let c = $PAD // { sub = $PAD // { z = 1; }; }; in builtins.toJSON c"
check "valuesEqual self-mat"   "let c = $PAD // { extra = 99; }; s = builtins.mapAttrs (n: v: v) c; in c == s"
check "valuesEqual two chains" "let a = $PAD // { x = 1; }; b = $PAD // { x = 1; }; in a == b"
check "has parent + overlay"   "let c = $PAD // { extra = 99; }; in [ (c ? k5) (c ? extra) (c ? nope) ]"
check "static select parent"   "let c = $PAD // { extra = 99; }; in c.k7 + c.extra"
check "dynamic select"         "let c = $PAD // { extra = 99; }; k = \"k7\"; in c.\${k} + c.extra"
check "formals @-pattern"      "let c = $PAD // { extra = 99; }; f = ({ k5, extra, ... }@a: k5 + extra); in f c"
check "with-scope from chain"  "let c = $PAD // { extra = 99; }; in (with c; k3 + extra)"
check "__overrides source chain" \
  "let o = { x = 10; z = 99; } // { y = 20; };
       r = rec { x = 1; y = 2; __overrides = o; };
   in builtins.toString r.x + \":\" + builtins.toString r.y + \":\" + builtins.toString r.z"
check "removeAttrs"            "let c = $PAD // { extra = 99; }; in builtins.length (builtins.attrNames (removeAttrs c [ \"k5\" \"extra\" ]))"
check "intersectAttrs"         "let c = $PAD // { extra = 99; }; in builtins.length (builtins.attrNames (builtins.intersectAttrs c { k5 = 1; extra = 2; }))"
check "mapAttrs then call"     "let c = $PAD // { f = x: x + 1; }; m = builtins.mapAttrs (n: v: v) c; in m.f 41"
check "deep compose select"    "let c0 = $PAD; c1 = c0 // { a = 1; }; c2 = c1 // { b = 2; }; c3 = c2 // { cc = 3; }; in c3.k9 + c3.a + c3.b + c3.cc"
check "deep compose toJSON"    "let c0 = $PAD; c1 = c0 // { a = 1; }; c2 = c1 // { b = 2; }; c3 = c2 // { cc = 3; }; in builtins.toJSON c3"
# Store-hash-critical: a structured-attrs derivation whose env is a chain.
# This is the exact shape that broke python3.withPackages (overlay-only JSON).
check "structuredAttrs drvPath" \
  "let dep = derivation { name = \"dep\"; builder = \"/bin/sh\"; system = \"x86_64-linux\"; };
       c = $PAD // { ref = \"\${dep}/x\"; tag = \"t\"; };
   in (derivation { name = \"u\"; builder = \"/bin/sh\"; system = \"x86_64-linux\";
                    __structuredAttrs = true; outputs = [ \"out\" ]; env2 = c; }).drvPath"
check "structuredAttrs distinguishes" \
  "let mk = n: (derivation { name = \"u\"; builder = \"/bin/sh\"; system = \"x86_64-linux\";
                             __structuredAttrs = true; outputs = [ \"out\" ];
                             env2 = $PAD // { extra = n; }; }).drvPath;
   in builtins.toString (mk 1 != mk 2)"

echo
if [[ "$fail" -eq 0 ]]; then
  printf '%sPASS%s — %d/%d chain-parity checks byte-identical\n' "$GRN" "$R" "$pass" "$((pass+fail))"
  exit 0
else
  printf '%sFAIL%s — %d/%d chain-parity checks DIVERGED\n' "$RED" "$R" "$fail" "$((pass+fail))"
  exit 1
fi
