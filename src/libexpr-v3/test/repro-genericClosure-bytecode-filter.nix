# Regression: genericClosure called with a bytecode-filter operator.
#
# Pre-fix (b0a0ff2e1) error: `v3 primop genericClosure: operator must
# return a list` — callClosure returned a Tag::Thunk wrapping the
# bytecode-filter result, and the genericClosure shape-check expected
# WHNF.  Fix: force the callClosure return value before isList check.
#
# Positive test: this expression must evaluate to a list under
# v3-direct.  TW oracle gives the same result.
#
# Run:
#   NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
#     nix eval --impure -f repro-genericClosure-bytecode-filter.nix
#
# Expected output (both TW and v3):
#   [ { key = 1; } { key = 2; } { key = 3; } ]
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

builtins.genericClosure {
  startSet = [ { key = 1; } ];
  operator = node:
    builtins.filter (n: n.key != node.key)
      [ { key = 2; } { key = 3; } ];
}
