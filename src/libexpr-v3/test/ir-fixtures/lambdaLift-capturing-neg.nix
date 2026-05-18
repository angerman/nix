# RUN: v3-eval --file %s --emit-ir-raw | v3-check %s
#
# Phase D / cachedSingletonClosure negative: a lambda that captures
# an outer let-binding has non-empty freeVars and nUp ≥ 1.  Phase D's
# runtime intern fast-path does NOT fire for these — each invocation
# allocates a fresh Closure with the captured upvalues.
#
# Companion fixture to `lambdaLift-capture-free-pos.nix`.

let n = 10; in x: x + n

# Capturing lambda: nUp=1, includes a `freeVars=[v...]` field.
# CHECK: ; func f{{[0-9]+}} entry=B{{[0-9]+}} nUp=1 param=v{{[0-9]+}} freeVars=[v{{[0-9]+}}] name="x"
