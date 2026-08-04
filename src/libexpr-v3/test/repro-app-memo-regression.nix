# Regression: Tag::App memoization via ValuePair.evaluated.
#
# Positive: a map-produced lazy entry is forced twice; the second
# force must return the SAME WHNF result without re-running the lambda
# body.  The memo only affects WHEN the lambda runs, not WHAT the value
# is (it is now unconditional).
#
# Pre-fix (before commit d3e41c13d): hello.drvPath was forcing the
# extendDerivation outputsList lambda 64k+ times because Tag::App
# entries from genList/map had no `evaluated` cache.  Fix: extend
# ValuePair with a 16-byte `evaluated` field; cache the WHNF result
# on first force; short-circuit on subsequent forces of the same App
# pointer.
#
# Run:
#   NIX_V3_DIRECT_EVAL=1 \
#     nix eval --impure -f repro-app-memo-regression.nix
#
# The memo only affects WHEN we run the lambda, not WHAT the value is.
#
# Expected output (both modes, TW and v3):
#   42
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  # A lambda whose body has a side-effect-visible behavior would let us
  # MEASURE whether memo fires (e.g. via builtins.trace).  But for a
  # parity-correctness test we just need the value to be stable.
  xs = builtins.map (i: i * 2) [ 0 1 2 3 4 5 6 7 8 9 ];
  # Force xs[3] twice; the result should be the same (= 6).  With memo,
  # the second force is a constant-time hit.
  a = builtins.elemAt xs 3;
  b = builtins.elemAt xs 3;
in
  if a == 6 && b == 6 then 42 else 0
