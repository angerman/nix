# Regression: IR Phase B pure-primop constant folding must preserve
# semantics for the patterns it rewrites.
#
# This fixture covers POSITIVE cases (statically foldable patterns).
# NEGATIVE cases (throws, dynamic operands) are in
# `repro-primop-fold-negative.nix` because Nix's --strict / nested
# attrset rendering differs between TW and v3 for throws-as-values
# (a separate, pre-existing semantic gap).
#
# Run (parity):
#   nix eval --impure -f repro-primop-fold.nix
#   NIX_V3_DIRECT_EVAL=1 nix eval --impure -f repro-primop-fold.nix
#
# Run (gate-off):
#   NIX_V3_NO_PRIMOP_FOLD=1 NIX_V3_DIRECT_EVAL=1 \
#     nix eval --impure -f repro-primop-fold.nix
#
# All three must produce identical output.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

{
  posLength      = builtins.length [ 1 2 3 4 5 ];
  posStringLen   = builtins.stringLength "hello, world";
  posHead        = builtins.head [ 100 200 300 ];
  posTail        = builtins.tail [ "a" "b" "c" "d" ];
  posElemAt      = builtins.elemAt [ 10 20 30 40 50 ] 3;
  posToStrInt    = builtins.toString 42;
  posToStrTrue   = builtins.toString true;
  posToStrFalse  = builtins.toString false;
  posToStrString = builtins.toString "already a string";
  posAttrNames   = builtins.attrNames { c = 3; a = 1; b = 2; };

  # Dynamic operand — pass must NOT fold, but result must match
  # runtime evaluation.
  posDynamic = let mk = n: builtins.length (builtins.genList (i: i) n);
               in mk 7;

  # Nested folding: length over a (head + ... concat) — recursive
  # ListExpr handling.  The result is statically known if both
  # sub-lists are ListExpr.
  posConcatLength = builtins.length ([ 1 2 ] ++ [ 3 4 5 ]);
}
