#!/usr/bin/env bash
# PLAN_BEAT_TW_V2 §1.7 — OP_APPLY_OVERRIDES correctness regression guard.
#
# 1.7 adds a chain-privatize guard at OP_APPLY_OVERRIDES (vm.cc): the
# in-place `const_cast` writeback into a slot returned by the CHAIN-AWARE
# `dst->lookup(k)` would corrupt a SHARED parent layer if `dst` were ever a
# Chain Bindings (the C-1 stale-KEEP class).  rec-attrsets are Sorted today,
# so the guard is a no-op for the common case; this test pins that the
# `__overrides` semantics it wraps stay byte-identical to the tree-walker
# (so the guard never regresses the overwrite / grow paths), plus the
# nixpkgs makeOverridable / overrideAttrs forms.
#
# NOTE (separate, PRE-EXISTING, out of scope for 1.7): `nix eval` *display*
# of a literal LIST whose elements are selects from a __overrides rec —
# e.g. `[ f.x f.y ]` — prints empty under v3-direct (exit 0, values are
# individually correct; a plain-rec list prints fine).  Tracked here as an
# XFAIL so it's not lost; it is NOT caused by the §1.7 guard (which only
# fires on a Chain dst, never on these Sorted recs).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
pass=0; fail=0; xfail=0

# byte-identity assertion: v3-direct result must equal tree-walker result.
chk() { # <label> <expr>
  local label="$1" expr="$2"
  local v3 tw
  v3="$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>/dev/null)"
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null)"
  if [[ "$v3" == "$tw" && -n "$tw" ]]; then
    printf '  PASS  %-44s = %s\n' "$label" "$v3"; pass=$((pass+1))
  else
    printf '  FAIL  %-44s v3=[%s] tw=[%s]\n' "$label" "$v3" "$tw"; fail=$((fail+1))
  fi
}
xchk() { # <label> <expr> — known-divergent (pre-existing, documented above)
  local label="$1" expr="$2" v3 tw
  v3="$(NIX_V3_DIRECT_EVAL=1 "$NIX" eval --impure --expr "$expr" 2>/dev/null)"
  tw="$("$NIX" eval --impure --expr "$expr" 2>/dev/null)"
  if [[ "$v3" == "$tw" && -n "$tw" ]]; then
    printf '  XPASS %-44s = %s  (XFAIL now passes — update test!)\n' "$label" "$v3"; fail=$((fail+1))
  else
    printf '  xfail %-44s (pre-existing list-display quirk)\n' "$label"; xfail=$((xfail+1))
  fi
}

echo "=== §1.7 OP_APPLY_OVERRIDES byte-identity (v3-direct vs TW) ==="
# Raw `rec { __overrides = ...; }` — the ONLY shape that emits
# OP_APPLY_OVERRIDES.  Overwrite an existing key (in-place writeback path
# the §1.7 guard wraps) + reference it transitively.
ROV='rec { x = 1; y = 2; sum = x + y; __overrides = { x = 10; z = 99; }; }'
chk "overwrite: f.x"        "($ROV).x"            # 10
chk "untouched: f.y"        "($ROV).y"            # 2
chk "transitive: f.sum"     "($ROV).sum"          # 12 (sum sees overridden x)
chk "grow: f.z"             "($ROV).z"            # 99 (new key added)
chk "aggregate via with"    "with ($ROV); builtins.toString (x + y + sum + z)"  # "123"
chk "overwrite-only.x"      "(rec { x = 1; __overrides = { x = 7; }; }).x"      # 7

# nixpkgs override mechanisms (functional smoke; byte-identical).
chk "makeOverridable"       "((import <nixpkgs> {}).lib.makeOverridable (a: { v = a.n or 5; }) { n = 7; }).v"
chk "overrideAttrs.pname"   "((import <nixpkgs> {}).hello.overrideAttrs (o: { pname = \"hi\"; })).pname"

# Pre-existing, documented: list display of __overrides-rec selects.
xchk "XFAIL list display"   "[ ($ROV).x ($ROV).y ]"

echo
echo "  pass=$pass fail=$fail xfail=$xfail"
[[ $fail -eq 0 ]] && { echo "§1.7 OK"; exit 0; } || { echo "§1.7 FAIL"; exit 1; }
