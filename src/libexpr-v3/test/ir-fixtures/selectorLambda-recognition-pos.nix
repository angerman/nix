# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase E (2026-05-18): selector-lambda recognition.  Asserts that
# the canonical IR shape for `paramVar: AttrSelect(paramVar, sym)`
# is preserved through the optimisation pipeline.  This IR shape is
# what the emit.cc peephole consumes to set
# `LambdaDescriptor::selectorSym` (#424), which in turn enables the
# three OP_CALL fast-paths in vm.cc (3499, 8821, 10260) that elide
# frame allocation for selector calls.
#
# Why a higher-order context (`map (p: p.name) [...]`)?  In an
# inline `(p: p.name) someAttrs` call, Phase A would beta-reduce
# the lambda away — eliminating the selector entirely.  Behind a
# higher-order function like `map`, the lambda survives to the
# emit phase, where the peephole sets its selectorSym.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase E
# exit criterion: "x: x.foo lambda has selectorSym = foo set."
# The peephole was previously gated by NIX_V3_SELECTOR_LAMBDA=1;
# this commit makes it default-ON, opt-out via
# NIX_V3_NO_SELECTOR_LAMBDA=1.

map (p: p.name) [ { name = "alice"; } { name = "bob"; } ]

# The selector lambda's function header.  No upvalues (freeVars=[],
# omitted from dump), single named param.  Body is a single
# AttrSelect on the param.
# CHECK: ; func f{{[0-9]+}} entry=B{{[0-9]+}} nUp=0 param=v{{[0-9]+}} name="p"
# CHECK-NEXT: B{{[0-9]+}}:
# CHECK-NEXT: v{{[0-9]+}} = AttrSelect v{{[0-9]+}} "name"
# CHECK-NEXT: return v{{[0-9]+}}

# Negative: there must NOT be any Force / VarRef hops between the
# param and the AttrSelect — those would defeat the 4-instruction
# peephole at emit time.  inlineTrivialBindings + elimRedundantForce
# (Phase A's cleanup passes) are what collapse the lowerer's
# A-normal-form scaffolding.
# CHECK-NOT: v{{[0-9]+}} = VarRef v{{[0-9]+}}
# CHECK-NOT: v{{[0-9]+}} = Force v{{[0-9]+}}{{.*}}return
