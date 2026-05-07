# #495 follow-on: smaller reproducer for the OP_ATTRS_SELECT upvalue
# regression that fires when NIX_V3_SELF_DOT_MAX_LEVEL >= 1.
#
# Triggers:
#
#   $ NIX_USE_V3=1 NIX_V3_SELF_DOT_MAX_LEVEL=1 \
#       nix eval --impure --raw -f this-file.nix
#   error: v3 OP_ATTRS_SELECT: attribute not found
#
# With NIX_V3_SELF_DOT_MAX_LEVEL unset (default 0), this returns "x86_64".
# Tree-walker also returns "x86_64".
#
# Failure trace (V3_DBG_ATTRS_SELECT=1) reports:
#
#   want sid=627 name="isx86" bindings=... size=2 present=[gcc,linux-kernel]
#   frame stack: platform → final → systemOrArgs
#
# `platform` is `lib.systems.platforms.select`, a closure that takes a
# platform record and accesses `.isx86`.  At the failing call site,
# `select` receives a `pc`-style attrset (`{ gcc = ...; linux-kernel = ...; }`)
# instead of the platform record (`final`) -- a wrong-arg / wrong-upvalue
# capture.
#
# Hypothesis: under broader thunkify (level >= 1 lambda-walking), the
# inherit-from clauses in lib/systems/default.nix:268-278 produce a
# thunk whose freeVar capture mis-binds `final`.  The non-rec attrset
# attr-from-expr there is an `ExprOpUpdate` of `{ kernel/gcc literals } //
# platforms.select final`, NOT a simple ExprSelect-on-Var, so my
# heuristic should NOT trigger.  Yet the failure correlates with
# MAX_LEVEL >= 1 -- some OTHER from-expr in the chain must be the
# trigger.
#
# Bisect (2026-05-07, NIX_V3_SELF_DOT_LIMIT counter):
#
#   * 42 self-dot fires total under MAX_LEVEL=1 (all in lib/default.nix
#     under the makeExtensible' lambda — "var=self level=1" patterns).
#   * LIMIT=41 → returns "x86_64" (correct).
#   * LIMIT=42 → fails with OP_ATTRS_SELECT.
#
#   The 42nd fire is line 609:
#     `inherit (self.flakes) parseFlakeRef flakeRefToString;`
#
#   So thunkifying that ONE clause's `self.flakes` from-expr is the
#   trigger for downstream lib.systems.elaborate breakage.  This is
#   structural state shared between the 42nd thunk and the lib.systems
#   path -- not direct: lib.systems doesn't reference flakes, and the
#   thunk wraps `self.flakes`, not `self.systems`.
#
#   Likely causes (worth narrowing further):
#     - Function-table size or VarId allocation crosses some threshold
#       at the 42nd added function.
#     - The thunk's freeVar capture for `self` interacts with the
#       rec-attr slot used by lib.systems' downstream consumers.
#     - cardano-node-style #455/#456 Slot-staleness manifests in the
#       lib makeExtensible' attrset's rec-slot when too many self-
#       dots are thunkified.
#
# Until root-caused, asserted as KNOWN-FAIL in the test suite so a
# silent change to the failure shape (e.g., someone broadens the
# default gate) is caught.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

let
  lib = import <nixpkgs/lib>;
  s = lib.systems.elaborate "x86_64-linux";
in s.parsed.cpu.name
