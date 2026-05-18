# RUN: v3-eval --file %s --emit-ir-raw | v3-check %s
#
# Phase D / cachedSingletonClosure precondition: a capture-free
# lambda's freeVars must be empty.  The dumper omits `freeVars=[...]`
# when the set is empty, so its absence on a func line indicates
# the lowerer correctly recognised no upvalue capture.
#
# This fixture asserts the LOWERER contract.  Phase D's runtime
# intern fast-path (LambdaDescriptor::cachedSingletonClosure) keys
# off `nUpvalues == 0`, which is set by emit.cc from
# `Function::freeVars.size()`.  Companion fixture
# `lambdaLift-capturing-neg.nix` checks the inverse (capturing
# lambda DOES have freeVars).

x: x * 2

# Capture-free lambda: nUp=0, no `freeVars=[v...]` field.
# CHECK: ; func f{{[0-9]+}} entry=B{{[0-9]+}} nUp=0 param=v{{[0-9]+}} name="x"
# CHECK-NOT: freeVars=[v
