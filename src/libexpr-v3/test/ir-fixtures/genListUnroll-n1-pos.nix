# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase H boundary case: N=1 still unrolls (1 ≤ kMaxUnrollN=8).
# A single-element list is constructed from one MkThunk, and the
# original PrimOpCall "genList" is gone.

builtins.genList (i: i + 100) 1

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = ListExpr
# CHECK-NOT: PrimOpCall "genList"
