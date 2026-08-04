# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# `builtins.seq a b` must lower to a Force on `a` (its strictness effect),
# NOT to the seq primop's App-spine (LitPrimOp "seq" + the partial-app
# closure for `seq a` + a saturating OP_CALL/CALL_PRIMOP).  seq returns its
# 2nd arg WITHOUT forcing it, so `b` stays lazy — only `a` is forced.
#
# This removes ~3-4 ops/elem (and a closure alloc) from the bytecode foldl''s
# per-iteration `seq next (go (i+1) next)`.
#
# Implemented at lowering (cli/lower_v3.hh, a::Kind::Call).
# Regression guard: the seq→force rewrite is unconditional; without it this
# would emit the seq primop call.

a: b: builtins.seq a (b + b)

# The strictness effect on `a` is a Force, not a seq primop call:
# CHECK: Force
# CHECK-NOT: LitPrimOp "seq"
