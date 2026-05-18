# Regression test for commit bb735aeb5:
#   v3 lower: thunkify ConcatStrings args with interpolation-bearing operands
#
# Minimal repro of the bug.  Before the fix, v3-eval ignored the
# `forceString=true` flag on the INNER ConcatStrings operand of an
# OUTER ConcatStrings(forceString=false) when passed as a function arg,
# eagerly lowering the interpolation in the parent block and forcing
# the interpolated `${X}` references unconditionally — even when the
# callee (e.g. `optionalString false`) never used the arg.
#
# The original real-world manifestation was at cc-wrapper/default.nix:
# 665-682 where the body of `optionalString cond body` was
# `''shell ${gccForLibs}'' + optionals (!isArocc) (...)`.  v3 lowered
# the outer `+` eagerly, forced ${gccForLibs}, hit pkgs.gccForLibs's
# fix-point reference to `targetPackages.stdenv.cc.isGNU` and tripped
# OP_ATTRS_SELECT on null.
#
# Positive test: v3 and TW must both return `""` without forcing the
# throw.  TW returns `""` because lazy-arg semantics; post-fix v3
# returns `""` because the body is now correctly thunkified.
#
# Negative test (regression for arithmetic eagerness): the `n + 1`
# pattern in the lang test `eval-okay-fib` is covered by the lang
# test suite.  fib chains 250k+ `f (n - 1) + f (n - 2)` calls — its
# correctness AND speed are an implicit assertion that pure-arithmetic
# ConcatStrings(forceString=false) operands are still eager-lowered
# under this fix (the operands of `+` there are Int/Var, all trivial,
# so the recursive check returns "safe eager").
#
# Reference: src/libexpr-v3/lower.cc:1606+ (isTrivialForLazy heuristic)
# and project_cc_wrapper_bisection_2026-05-18.md.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  # User-defined optionalString to keep the fixture self-contained
  # (no nixpkgs dependency).  Identical to lib.optionalString.
  optionalString = cond: string: if cond then string else "";

  # The `throw` here MUST NOT be evaluated when optionalString's cond
  # is false; otherwise the test fails with the throw message.
  forced = throw "FORCED FORCED FORCED — laziness broken!";

in
[
  # Case A: outer fS=false `+`, inner fS=true `''${X}''` literal
  (optionalString false ("aaa${toString forced}bbb" + "ccc"))

  # Case B: ditto, but interpolation in the SECOND operand of the +
  (optionalString false ("ddd" + "eee${toString forced}fff"))

  # Case C: outer fS=false `+` with multi-line `''...''` containing
  # interpolation (matches cc-wrapper's shape exactly)
  (optionalString false (
    ''
      multiline body with ${toString forced} interpolation
    ''
    + ''ggg''
  ))

  # Case D: triple chain — inner-inner interpolation must remain lazy
  # too.  Outer is fS=false (`+`); operand 1 is fS=false ConcatStrings
  # whose operand is fS=true `''${X}''`.  Documented limitation: the
  # commit-bb735aeb5 fix only checks DIRECT operands of the outer for
  # fS=true.  Operand 1 here is fS=false, NOT fS=true — so the fix's
  # one-level check would NOT trip on the outer in isolation.  HOWEVER,
  # when v3 recursively lowers operand 1, the SAME check fires at
  # operand-1 level: operand 1's operand `''${X}''` IS fS=true, so
  # operand 1 itself is thunkified.  Thus the nested case is handled.
  (optionalString false (
    ("a${toString forced}b" + "c")
    + "d"
  ))
]
