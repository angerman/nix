# RUN: v3-eval --expr 'if true then 100 else 200' --emit-ir | v3-check %s --check-prefix=TRUE
# RUN: v3-eval --expr 'if false then 100 else 200' --emit-ir | v3-check %s --check-prefix=FALSE
# RUN: v3-eval --expr 'if 1 < 2 then 100 else 200' --emit-ir | v3-check %s --check-prefix=COMP
# RUN: v3-eval --expr 'if (1 == 2) then "no" else "yes"' --emit-ir | v3-check %s --check-prefix=CMP2
#
# Phase G (2026-05-18): pure if-then-else folding.  Recognises
# `If(LitBool, thenBlock, elseBlock)` patterns and rewrites to
# inline the chosen branch's bindings + a VarRef to its terminal.
# Eliminates the `If` IR node + the OP_BRANCH_FALSE bytecode that
# emit.cc would otherwise produce.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase G
# exit criterion: "if true then a else b → a (and no
# OP_BRANCH_FALSE in compiled bytecode for static-branch shapes)."
#
# Composition: Phase G runs AFTER Phase B (constantFold + primOpFold)
# in the pipeline, so any condition expression that resolves to a
# literal at IR level — including `1 < 2`, `(1 == 2)`, etc. — is
# folded into the same shape as a written-out `true`/`false`.

# `if true then 100 else 200` → block has LitInt 100, no If, no
# LitInt 200 (else branch discarded).
# TRUE-LABEL: B1:
# TRUE: v{{[0-9]+}} = LitInt 100
# TRUE-NOT: v{{[0-9]+}} = If
# TRUE-NOT: v{{[0-9]+}} = LitBool
# TRUE-NOT: LitInt 200

# `if false then 100 else 200` → block has LitInt 200, no If, no
# LitInt 100 (then branch discarded).
# FALSE-LABEL: B1:
# FALSE: v{{[0-9]+}} = LitInt 200
# FALSE-NOT: v{{[0-9]+}} = If
# FALSE-NOT: v{{[0-9]+}} = LitBool
# FALSE-NOT: LitInt 100

# `if 1 < 2 then 100 else 200` — Phase B folds `1 < 2 → true`, then
# Phase G folds the If.  Post-opt shape is identical to the static-
# true case: just LitInt 100.
# COMP-LABEL: B1:
# COMP: v{{[0-9]+}} = LitInt 100
# COMP-NOT: Less
# COMP-NOT: If

# `if (1 == 2) then "no" else "yes"` — Phase B folds `1 == 2 → false`,
# Phase G picks the else branch.
# CMP2-LABEL: B1:
# CMP2: v{{[0-9]+}} = LitString "yes"
# CMP2-NOT: LitString "no"
# CMP2-NOT: If
# CMP2-NOT: Eq
