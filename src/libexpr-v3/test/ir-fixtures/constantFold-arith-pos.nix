# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase B / opt_const_fold: integer Mul over two literal operands
# folds to a single LitInt 6.  No Force, no Mul opcode emitted.
#
# Note: `2 + 3` is intentionally NOT used here because Nix's `+`
# lowers to `ConcatStrings` (the lowerer can't statically decide
# string vs int), and ConcatStrings folding is a separate concern
# (not in const-fold's scope today).

2 * 3

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitInt 6
# CHECK-NOT: LitInt 2
# CHECK-NOT: LitInt 3
# CHECK-NOT: Mul
# CHECK: return v{{[0-9]+}}
