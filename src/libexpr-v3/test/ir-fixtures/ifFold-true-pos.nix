# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase G / opt_if_fold: `If(LitBool true, ...)` inlines the THEN
# branch.  Post-opt the entry block contains only LitInt 100 — no
# If node, no LitBool, no LitInt 200 (else discarded).

if true then 100 else 200

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitInt 100
# CHECK-NOT: v{{[0-9]+}} = If
# CHECK-NOT: v{{[0-9]+}} = LitBool
# CHECK-NOT: LitInt 200
