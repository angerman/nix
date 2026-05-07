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
