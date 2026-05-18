# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase F / opt_app_spine_fold: 2-arg curried call.
# `(x: y: x * y) 6 7` substitutes both params into the deepest body
# in one shot; cleanup chain (elimRedundantForce + constantFold)
# then folds `6 * 7` to LitInt 42.

(x: y: x * y) 6 7

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = LitInt 42
# CHECK-NOT: Mul
# CHECK-LABEL: ; func f1
