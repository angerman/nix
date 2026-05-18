# RUN: v3-eval --expr "builtins.genList (i: i * 2) 4" --emit-ir | v3-check %s --check-prefix=N4
# RUN: v3-eval --expr "builtins.genList (i: i + 100) 1" --emit-ir | v3-check %s --check-prefix=N1
# RUN: v3-eval --expr "builtins.genList (i: i) 16" --emit-ir | v3-check %s --check-prefix=N16
#
# Phase H (2026-05-18): static genList unrolling.  Recognises
# `PrimOpCall("genList", [f, n])` where `n` resolves to a LitInt
# in 0..8 and rewrites to an N-element ListExpr whose elements
# are MkThunks each wrapping `App(f, LitInt i)`.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase H
# exit criterion: "`genList id 4` → 4-element ListExpr with no
# OP_CALL_PRIMOP genList."
#
# Laziness preservation: each unrolled element is a MkThunk
# (forced on demand), not a strict App.  This mirrors tree-walker's
# `primGenList` which builds Tag::App entries lazily.  Verified by
# `head (genList (i: if i==0 then 42 else throw "err") 4) == 42`
# (only first element evaluated; throws never fire).

# N=4: expect 4 MkThunk bindings + 1 ListExpr + a VarRef rewrite.
# No PrimOpCall "genList" remains.
# N4-LABEL: ; func f0
# N4: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# N4: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# N4: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# N4: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# N4: v{{[0-9]+}} = ListExpr
# N4-NOT: PrimOpCall "genList"

# N=1: single-element list — still unrolls (1 ≤ 8).
# N1-LABEL: ; func f0
# N1: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# N1: v{{[0-9]+}} = ListExpr
# N1-NOT: PrimOpCall "genList"

# N=16: above the kMaxUnrollN=8 threshold — pass refuses, original
# PrimOpCall remains.
# N16-LABEL: ; func f0
# N16: v{{[0-9]+}} = PrimOpCall "genList"
# N16-NOT: name="<genListThunk>"
