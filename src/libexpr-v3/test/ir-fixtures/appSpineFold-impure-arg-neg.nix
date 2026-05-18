# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase F negative: impure arg refusal.  `throw "a"` is wrapped in
# MkThunk by the lowerer; MkThunk isn't in the pass's pure-args
# shortlist (LitInt/Float/Bool/Null/String/Path/VarRef/LitPrimOp/
# LitBuiltins).  The original Lambda + App spine MUST remain
# unchanged so the laziness of `throw "a"` is preserved at runtime
# — `(x: y: y) (throw "a") 5` evaluates to 5 without firing the
# throw (only `y` is needed).

(x: y: y) (throw "a") 5

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = Lambda f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = App
# CHECK: v{{[0-9]+}} = App
# CHECK: PrimOpCall "throw"
