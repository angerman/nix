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
#   Root-cause narrowed (V3_DBG_LOWER_CALLS, 2026-05-07):
#
#   Five lowerNixExpr calls in this repro path:
#     1. v3EvalEntry@1999  (kind=13 Let, no-pos)  -- top-level repro file
#     2. primImport@5671   (kind=13 Let, no-pos)  -- lib/default.nix
#                          -> fires #1-21 on its 21 inherit clauses
#     3. primImport@5671   (kind=11 Lambda, lib/systems/default.nix:1:1)
#     4. primImport@5671   (kind=11 Lambda, platforms.nix:8:1)
#     5. v3EvalEntry@1999  (kind=13 Let, no-pos)  -- ???
#                          -> fires #22-42 on the SAME 21 inherit clauses
#
#   Call 2 lowers lib/default.nix's full AST.  Call 5 fires v3EvalEntry
#   for ANOTHER Expr* whose AST tree OVERLAPS with lib/default.nix.
#   v3HookCache (keyed by Expr*) doesn't hit because the call-5 Expr*
#   has a different address from call-2's parsed root.
#
#   The two modules share AST nodes (sub-Expr*s) for the inherit-from
#   from-exprs but produce DISTINCT IR (own m.functions table, own
#   FuncIds, own freeVars analysis).  populateSubExprCacheLocal stores
#   the FIRST module's (cu, FuncIdx) per Expr*; the second module's
#   thunks reference its OWN FuncIds.  When TW later forces a thunk
#   that landed in the cache from module-1, the bytecode path runs
#   module-1's CU; its freeVars list might mismatch what the second
#   call site (compiled per module-2) expects.
#
#   PARTIAL FIX (commit 0a480cfb0, content-keyed cache):
#     primImport now registers (sourceHash → CU) in an in-memory
#     content cache.  v3EvalEntry consults it on Expr* miss, hits,
#     and skips re-lowering.  Net effect: fire count drops from
#     42 to 21.  But the broader-thunkify upvalue bug STILL fires
#     at 21 fires -- so the duplicate-lowering theory was only
#     partially right.  At MAX_LEVEL=1, thunkifying ALL 21 of
#     lib/default.nix's `inherit (self.X) Y` clauses (within the
#     `(self: let callLibs = ...; in {...})` lambda body) causes
#     the lib.systems.elaborate path to fail.  Skipping ANY ONE of
#     the 21 makes it work (NIX_V3_SELF_DOT_LIMIT=20 passes,
#     NIX_V3_SELF_DOT_SKIP_NTH=1..21 all pass; LIMIT=21 fails).
#     Convergence cap (kMaxIters=16 in computeFreeVars) raised to
#     256 didn't help.
#
#   The underlying bug remains: SOMETHING about thunkifying all
#   21 self-dot from-exprs simultaneously corrupts an upvalue
#   binding in a downstream closure (`platforms.select` receives
#   a 2-attr `pc`-style attrset instead of the platform record
#   `final`).  Pure mystery: skipping any single thunkify makes
#   the bug disappear, no matter which one.
#
#   ROOT CAUSE NARROWED (V3_DBG_OP_CALL_POST=1, 2026-05-07):
#
#   At the OP_CALL invoking `platforms.select final`, arg.tag = 16
#   = Tag::Slot.  So `select` IS being called with a slot pointer
#   (correctly -- final is a let-binding, slot semantics expected).
#   The bug is at the slot DEREF inside select's body: forcing
#   slot → expects to find final's full attrset; instead finds the
#   2-attr literal `{gcc=...; linux-kernel=...}` (the LEFT side of
#   `// platforms.select final`).
#
#   This IS the C2 bug class: Tag::Slot semantic-staleness across
#   structural change.  The slot's backing storage was assigned the
#   wrong value somewhere -- either the LEFT-side literal in error,
#   or the slot pointer itself aliases the wrong storage.
#
#   Hypothesis: 21 `inherit (self.X) Y` thunks each capture `self`
#   as a Tag::Slot upvalue.  Combined with `final = ... // ...`
#   OP_APPLY_OVERRIDES, the slot pointer space gets confused when
#   threshold structures align (always 21 in the lib makeExtensible'
#   shape).  Specific overlap not yet bisected.
#
#   NEXT INVESTIGATION:
#   - Walk OP_REC_SLOT_PUBLISH and OP_APPLY_OVERRIDES to confirm
#     where final's slot storage is written.
#   - Compare slot pointer values at LIMIT=20 vs LIMIT=21 around
#     elaborate's `... // platforms.select final` evaluation.
#   - Check if the slot allocated for final aliases another slot
#     in the makeExtensible' rec scope.
#
#   Tooling for further bisect (lower.cc + per-call-site labels):
#     V3_DBG_LOWER_CALLS=1        -- log each lowerNixExpr call
#     V3_DBG_SELF_DOT_FIRES=1     -- log each fire's ordinal + position
#     NIX_V3_SELF_DOT_LIMIT=N     -- gate to first N fires module-wide
#     NIX_V3_SELF_DOT_SKIP_NTH=N  -- skip the Nth fire (1-indexed)
#     NIX_V3_NO_CONTENT_CACHE=1   -- disable the content cache layer
#                                    (returns to the old 42-fire shape)
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
