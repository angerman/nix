# Regression test for #667 (commit landing 2026-05-19):
#   OP_ASSERT must force its operand through the CFF_FORCE_RETRY
#   iterative-force protocol — same as OP_NOT / OP_AND_BRANCH /
#   OP_OR_BRANCH / OP_BRANCH_FALSE.
#
# Pre-fix v3 popped the operand raw and called isTrueValue, which
# throws "v3: expected bool" on Tag::Thunk / Tag::App / Tag::Slot.
# All the OTHER bool-consuming opcodes handle this by rewinding `ip`
# to the opcode and setting CFF_FORCE_RETRY before `goto
# op_force_slow`; OP_ASSERT was simply missed.
#
# Symptom on nixpkgs: every derivation that ended up landing an
# unforced Slot on top-of-stack at an `assert ...; body` site
# threw "v3: expected bool" — gtk3 / firefox-unwrapped (and so
# firefox) couldn't compute .drvPath / .outPath in v3-direct mode,
# while TW completed them.
#
# Synthetic re-creates the pattern: an assertion whose condition is
# evaluated lazily and lands on the stack as a non-WHNF value at
# the moment OP_ASSERT executes.  We use a let-bound thunk + a
# wrapper function so v3's IR keeps the condition unforced past
# the OP_ASSERT site without the lower-time strictness analysis
# folding it away.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0.

let
  # `cond` is a thunk — never forced before the assert sees it.
  # The closure body forces `cond` via the assert.  Pre-fix v3 saw
  # the closure-arg Slot on top-of-stack and OP_ASSERT threw.
  withAssert = cond: body: assert cond; body;

  # A "true" computed by a more roundabout path — the optimiser
  # can't statically fold it to a literal, so the IR keeps an
  # unforced thunk for the value.
  computed = (x: x == "y") (builtins.head [ "y" "n" ]);
in
{
  # Case A: assert on a let-bound bool that arrives via closure-arg
  # binding.
  caseA = withAssert true 1;

  # Case B: assert on a computed condition (more like nixpkgs
  # `assert lib.assertMsg ...; body`).
  caseB = withAssert computed 2;

  # Case C: nested assert — the body itself contains another
  # closure-arg-driven assert.
  caseC = withAssert true (withAssert computed 3);
}
