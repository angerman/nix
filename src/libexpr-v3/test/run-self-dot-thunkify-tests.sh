#!/usr/bin/env bash
# v3 #528 — self-dot inherit-from thunkify regression tests.
#
# Captures the lib.fix-shape bug:
#
#   makeExt = rattrs: let self = rattrs self // {extra}; in self;
#   lib = makeExt (self: let helper = ...; in {
#     sub = { a = 42; };
#     inherit (self.sub) a;       # eager v3 lowering hits self while it's
#                                 # mid-construction → BlackholeError /
#                                 # attribute-not-found.
#   });
#
# Tree-walker handles this because `inherit (X) Y` builds a thunk that
# defers evaluating `X` until `Y` is accessed, by which time `self`
# is fully constructed.  v3's default lowering only thunkifies
# inherit-from when `self` is at level=0 (the immediately-enclosing
# simple-arg lambda).  The lib.fix shape has self at level≥1 (a
# `let X = ...` inside the lambda body bumps the level).
#
# Tests verify v3-direct output matches TW for representative shapes:
#  - level=0 (no inner let)        — already worked.
#  - level=1 (lib.fix shape)       — the bug being fixed.
#  - level=2 (nested let)          — deeper case.
#  - non-buggy patterns            — make sure we don't regress them.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX_BIN="${NIX_BIN:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX_BIN" ]]; then
  echo "nix not found at $NIX_BIN" >&2
  exit 1
fi

ok=0
fail=0
fail_names=()

run_pair() {
  local name="$1" expr="$2"
  local tw v3
  tw=$("$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null) || tw="<tw-error>"
  v3=$(NIX_V3_DIRECT_EVAL=1 "$NIX_BIN" eval --impure --expr "$expr" 2>/dev/null) \
    || v3="<v3-direct-error>"
  if [[ "$tw" == "$v3" ]]; then
    ok=$((ok + 1))
  else
    fail=$((fail + 1))
    fail_names+=("$name  TW=[$tw]  v3=[$v3]")
  fi
}

# POSITIVE — patterns where v3-direct already worked.

# Level-0 self-dot inherit-from (no inner let between lambda and inherit-from).
run_pair "level-0 inherit-self" '
  let mk = self: { inherit (self.sub) a; };
      v = mk { sub = { a = 42; }; };
  in v.a'

# Level-1 self-dot inherit-from with NO fix-point — works because self
# is concrete at call time.
run_pair "level-1 no-fixpoint" '
  let mk = self: let helper = "h"; in { inherit (self.sub) a; };
      v = mk { sub = { a = 42; }; };
  in v.a'

# Fix-point WITHOUT inner let — works.
run_pair "fixpoint no-inner-let" '
  let
    makeExt = rattrs: let self = rattrs self // { extra = 1; }; in self;
    lib = makeExt (self: { sub = { a = 42; }; inherit (self.sub) a; });
  in lib.a'

# REGRESSION — the bug being fixed.

# Level-1 self-dot + fix-point — the lib.fix shape.
run_pair "lib.fix shape (level-1)" '
  let
    makeExt = rattrs: let self = rattrs self // { extra = 1; }; in self;
    lib = makeExt (self: let helper = x: x; in {
      sub = { a = 42; };
      inherit (self.sub) a;
    });
  in lib.a'

# Level-2 — even deeper nesting.
run_pair "level-2 nested let" '
  let
    makeExt = rattrs: let self = rattrs self // { extra = 1; }; in self;
    lib = makeExt (self: let helper = "h"; in let inner = "i"; in {
      sub = { a = 42; };
      inherit (self.sub) a;
    });
  in lib.a'

# Multiple inherit-from-self at level=1.
run_pair "multi inherit-self level-1" '
  let
    makeExt = rattrs: let self = rattrs self; in self;
    lib = makeExt (self: let cl = file: file; in {
      sub = { a = 1; b = 2; c = 3; };
      inherit (self.sub) a b c;
    });
  in lib.a + lib.b + lib.c'

# NEGATIVE — make sure non-self-dot inherit-from is unchanged.
run_pair "inherit-from-non-self" '
  let src = { a = 1; b = 2; };
      v = { inherit (src) a b; c = 99; };
  in v.a + v.b + v.c'

run_pair "let-binding inherit-from" '
  let
    src = { x = 10; y = 20; };
    mk = self: let s2 = src; in { inherit (s2) x y; };
    v = mk { };
  in v.x + v.y'

# NEGATIVE — single-self at level-0 must still match.
run_pair "level-0 multi-attr inherit-self" '
  let mk = self: { inherit (self.sub) a b; };
      v = mk { sub = { a = 1; b = 2; }; };
  in v.a + v.b'

# REGRESSION — `with self;` + `inherit (X) Y` where X is a fromWith Var
# (#529).  The canonical nixpkgs all-packages.nix:196 shape:
#   with pkgs;
#   {
#     inherit (nix-update) nix-update-script;
#   }
# Eager lowering of `nix-update` forces the with-stack `pkgs` which is
# mid-construction when `self = rattrs self` is being computed.
# Thunkifying defers the with-lookup until the `nix-update-script`
# entry is forced.
run_pair "with-self inherit-from-Var" '
  let
    makeExt = rattrs: let self = rattrs self; in self;
    pkgs = makeExt (self: with self; {
      nix-update = "u";
      nix-update-script = "us";
      inherit (nix-update) something;
    });
  in pkgs.nix-update'

# REGRESSION — `with self;` + `inherit (callExpr X) Y` (#529 follow-up).
# The from-expr is an ExprCall whose head is a fromWith Var.  Same fix
# class as the bare-Var case.  Mirrors nixpkgs all-packages.nix:532
# `inherit (callPackages ../some/path { }) name1 name2;`.
run_pair "with-self inherit-from-Call" '
  let
    makeExt = rattrs: let self = rattrs self; in self;
    pkgs = makeExt (self: with self; {
      mkSet = path: { greeting = "g"; };
      inherit (mkSet ./somewhere) greeting;
    });
  in pkgs.greeting'

echo "=== self-dot-thunkify test results ==="
echo "  passing: $ok"
echo "  failing: $fail"
if (( fail > 0 )); then
  for n in "${fail_names[@]}"; do
    echo "  FAIL $n"
  done
  exit 1
fi
exit 0
