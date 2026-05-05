# Minimal reproducer for v3 #455.
#
# Trigger conditions (all must hold):
#   - NIX_USE_V3=1
#   - NIX_V3_ON_DEMAND_ROOT=1
#   - NIX_V3_SKIP_THRESHOLD=0  (forces eval-hook fallback so call hook fires)
#
# Pattern: `fix (self: with self; { N >= 5 attrset bindings })`.
#
# The 5-binding threshold is empirical -- 4 bindings produce correct
# output, 5+ produce `__v3_force_attr` infinite recursion at TW.
#
# Tree-walker baseline:           [3, 3]
# v3 default (no on-demand-root): [3, 3]
# v3 + ON_DEMAND_ROOT, 5 bindings: ERROR -- infinite recursion
#
# Hypothesis under investigation:
#
#   When on-demand-root populates the call-hook subCache for the outer
#   `self:` lambda, runLambda runs its body in v3.  The body is
#   `with self; { ... }` which builds an attrset of thunks.  Each
#   thunk's body does with-lookup on `self`, which is the lambda's
#   param.  When TW forces an entry of the returned attrset (via
#   `__v3_force_attr`), v3 runs the thunk; the thunk's with-lookup
#   resolves `pkg-a` etc against the captured `self`.
#
#   For some shape (≥5 bindings + with-self pattern over fix), this
#   chain produces a cycle that TW interprets as infinite recursion.
#   The default v3 path doesn't trigger it because the lambda runs
#   inside v3's eval-hook path with different bytecode emission
#   constraints.
#
# Suspect mechanisms:
#   - Per-attr thunks emit via `thunkify(e)` which creates a fresh
#     Function lowering `e` in the current `scopes` stack.  When
#     on-demand-root lowers the whole file, `scopes` contains the
#     outer file context that's absent from eval-hook's per-Expr
#     lowering -- the resulting Function captures different
#     freeVars / upvalueSources.
#   - The 5-binding threshold suggests a specific code path activates
#     at ATTRS_INIT count >= 5; check OP_ATTRS_INIT operand encoding,
#     IR optimisation passes, and lowerAttrs's branch selection.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0

let
  fix = f: let x = f x; in x;
  pkgs = fix (self: with self; {
    pkg-a = 1;
    pkg-b = 2;
    pkg-c = pkg-a + pkg-b;
    pkg-d = pkg-c + 1;
    derived = pkg-a + pkg-b;
    triple = pkg-a + pkg-b;
    q = pkg-a;
    r = pkg-b;
    s = pkg-c;
  });
in [pkgs.derived pkgs.s]
