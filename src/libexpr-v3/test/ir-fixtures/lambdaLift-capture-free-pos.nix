# RUN: v3-eval --expr 'x: x * 2' --emit-ir-raw | v3-check %s --check-prefix=FREE
# RUN: v3-eval --expr 'let n = 10; in x: x + n' --emit-ir-raw | v3-check %s --check-prefix=CAPT
#
# Phase D / cachedSingletonClosure precondition: the lowerer must
# correctly mark a capture-free lambda's freeVars as empty (no upvalues
# captured), and a capturing lambda's freeVars as non-empty (one or
# more upvalues).  These IR properties are what Phase D's runtime
# intern check consults via LambdaDescriptor::nUpvalues (set by emit
# from freeVars.size()).
#
# This fixture asserts the LOWERER contract, not Phase D's runtime
# behaviour (covered separately by repro-lambda-lift.nix +
# run-lambda-lift-tests.sh).  If this fixture regresses, Phase D's
# intern fast-path silently stops firing — no correctness bug, but a
# silent perf regression.
#
# Format note: the dumper omits `freeVars=[...]` when the set is
# empty (see ir_dump.cc dumpModule).  So `freeVars=[v` appearing on a
# func line indicates a capturing closure; its absence indicates a
# capture-free one.

# `x: x * 2` — capture-free.  Lambda's func header shows nUp=0 and
# omits the `freeVars=` field.
# FREE: ; func f{{[0-9]+}} entry=B{{[0-9]+}} nUp=0 param=v{{[0-9]+}} name="x"
# FREE-NOT: freeVars=[v

# `let n = 10; in x: x + n` — captures n (lowered via
# RecBindingSlotRef → the let-rec slot).  Lambda's func header shows
# nUp=1 and `freeVars=[v...]`.
# CAPT: ; func f{{[0-9]+}} entry=B{{[0-9]+}} nUp=1 param=v{{[0-9]+}} freeVars=[v{{[0-9]+}}] name="x"
