# RUN: v3-eval --expr 'builtins.length [1 2 3 4 5]' --emit-ir | v3-check %s
#
# Phase B / opt_primop_fold: `builtins.length` on a literal ListExpr
# folds to a LitInt with the list's length.  No PrimOpCall "length"
# remains in the post-opt IR.

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitInt 5
# CHECK-NOT: PrimOpCall "length"
# CHECK-NOT: ListExpr
