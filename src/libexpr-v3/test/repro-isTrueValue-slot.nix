# Regression: Tag::Slot reaching OP_NOT through the
# CFF_FORCE_WB_PTR_KEEP writeback chain.
#
# Pre-fix (within commit 7adc7e61f) error: `v3: expected bool` thrown
# from isTrueValue at OP_NOT call site.  Root cause: OP_ATTRS_SELECT_IC's
# memoizing writeback wrote a Tag::Slot back into the source slot;
# subsequent OP_NOT re-read found the Slot and the WHNF-check passed
# (Slot != Thunk/App), but isTrueValue's isBool check failed.
#
# Fix: defensive Tag::Slot chase inside isTrueValue (up to 32 hops),
# mirroring op_force_slow's chase loop.  See vm.cc:539.
#
# Positive test: this expression must evaluate to `true`.  The exact
# shape that exercises the path is hard to isolate synthetically;
# this is a minimised proxy that exercises:
#   - mapAttrs (creates Tag::App slot entries)
#   - attr-select with the IC fast path
#   - boolean negation on the result
#
# Run:
#   NIX_V3_DIRECT_EVAL=1 NIX_V3_SKIP_INSTALLABLE_PREEVAL=1 \
#     nix eval --impure -f repro-isTrueValue-slot.nix
#
# Expected output (both TW and v3):
#   true
#
# The robust whole-system test for this regression is the parity check
# on hello.name (which the bug originally broke).
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

let
  attrs = builtins.mapAttrs (_: v: v) {
    a = false;
    b = true;
  };
in
  !(attrs.a)
