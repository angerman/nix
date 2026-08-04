# Regression: IR Phase A beta-reduction must preserve semantics.
#
# Positive test: a battery of patterns that the pass IS supposed to
# inline (same-block, single-arg, no formals, simple body) and a few
# patterns it MUST NOT touch (multi-use, formals, nested closures).
# All must produce the same value vs TW (beta-reduction is unconditional).
#
# Run (parity):
#   NIX_V3_DIRECT_EVAL=1 nix eval --impure -f repro-beta-reduce.nix
#   nix eval --impure -f repro-beta-reduce.nix
#
# Expected output (both):
#   { aa = 6; bb = 30; cc = 60; dd = 100; ee = 31; ff = 42; gg = 6; hh = 21; }
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  # (a) Trivial inline-able: same-block, single-arg, simple body.
  aa = (x: x + 1) 5;

  # (b) Curried — inner App should inline first, then outer.
  bb = (x: y: x * y) 6 5;

  # (c) Chained let-bound lambda — should inline via VarRef chase.
  cc = let f = x: x * 2; in f 30;

  # (d) Lambda used twice — beta-reduce MUST NOT inline (would duplicate).
  dd = let f = x: x * 10; in (f 6) + (f 4);

  # (e) Lambda with multiple operations in body.
  ee = (x: let y = x + 1; in y * y - x) 5;  # (5+1)*(5+1) - 5 = 31

  # (f) Formals lambda — beta-reduce MUST NOT inline.
  ff = ({ a, b }: a * b) { a = 6; b = 7; };

  # (g) Lambda nested inside a let; both used once.
  gg = let
    inc = x: x + 1;
    f = x: inc (inc x);
  in f 4;  # 6

  # (h) Lambda body itself contains a lambda — Phase A refuses
  #     (would need recursive Function cloning).  Must still work
  #     via the normal OP_CALL path.
  hh = let mkAdder = x: y: x + y; in (mkAdder 1) 20;
in
{ inherit aa bb cc dd ee ff gg hh; }
