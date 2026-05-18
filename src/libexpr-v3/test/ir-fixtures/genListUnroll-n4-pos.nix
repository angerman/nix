# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase H / opt_genlist_unroll: `genList f 4` (N=4, ≤ kMaxUnrollN=8)
# expands to a 4-element ListExpr with one MkThunk per element.
# The original PrimOpCall "genList" is replaced by a VarRef to the
# constructed ListExpr.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase H
# exit criterion: "`genList id 4` lowers to a 4-element ListExpr
# with no OP_CALL_PRIMOP genList."

builtins.genList (i: i * 2) 4

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# CHECK: v{{[0-9]+}} = ListExpr
# CHECK-NOT: PrimOpCall "genList"
