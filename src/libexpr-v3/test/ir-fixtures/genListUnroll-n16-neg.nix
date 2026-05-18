# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase H negative: N=16 is above the kMaxUnrollN=8 threshold, so
# the pass refuses to unroll.  The original PrimOpCall "genList"
# remains in the post-opt IR — large lists fall through to the
# runtime primop.

builtins.genList (i: i) 16

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = PrimOpCall "genList"
# CHECK-NOT: name="<genListThunk>"
