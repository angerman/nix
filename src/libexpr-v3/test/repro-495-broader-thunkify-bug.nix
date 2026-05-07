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
# Bisect (2026-05-07, NIX_V3_SELF_DOT_LIMIT + SKIP_NTH):
#
#   42 self-dot fires total under MAX_LEVEL=1.  The ordinal-numbered
#   list shows: positions 126, 189, ..., 609 in source order (21
#   distinct positions in lib/default.nix's `(self: let in {...})`
#   body), THEN THE EXACT SAME 21 POSITIONS REPEAT (#22-#42).
#
#   So lib/default.nix is being LOWERED TWICE, each lowering creates
#   21 self-dot thunks.
#
#   SKIP_NTH bisect:
#     * Skip #N where N is in the FIRST batch (1-21): FAILS
#       (e.g. SKIP_NTH=1, =21).
#     * Skip #N where N is in the SECOND batch (22-42): PASSES
#       (e.g. SKIP_NTH=22, =42).
#     * LIMIT=21 (only first batch): PASSES.
#     * LIMIT=42 (full): FAILS.
#
#   Conclusion: the bug requires the SAME AST nodes to be lowered
#   twice, with self-dot thunkify firing in BOTH lowerings.  When
#   the heuristic is disabled in EITHER lowering, the bug doesn't
#   trigger.  This points at SHARED STATE between the two Modules:
#     - Each `lowerNixExpr` call creates an independent ir::Module.
#     - But both modules write to the v3 global registries
#       (subExprFuncs / force-hook cache / disk-cache key).
#     - Hypothesis: the second module's freeVar capture for the
#       21 thunks ALIASES with the first module's, producing an
#       upvalue that points at the wrong VMState's frame at force
#       time.
#
#   This is the SAME "stale slot/upvalue across structural change"
#   bug class as C2 (REVIEW_2026-05-06b: OP_APPLY_OVERRIDES creates a
#   fresh Bindings while outstanding Tag::Slot pointers still point
#   at the original).  Per the reviewer's hypothesis: fixing one
#   likely fixes both.
#
#   Tooling for further bisect (lower.cc):
#     V3_DBG_SELF_DOT_FIRES=1     -- log each fire's ordinal + position
#     NIX_V3_SELF_DOT_LIMIT=N     -- gate to first N fires module-wide
#     NIX_V3_SELF_DOT_SKIP_NTH=N  -- skip the Nth fire (1-indexed)
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
