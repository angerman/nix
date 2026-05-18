# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase F: 3-arg curried call.  Triple curry collapses to a single
# LitInt 24 after substitution + cleanup folding.

(x: y: z: x * y * z) 2 3 4

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = LitInt 24
# CHECK-NOT: Mul
# CHECK-LABEL: ; func f1
