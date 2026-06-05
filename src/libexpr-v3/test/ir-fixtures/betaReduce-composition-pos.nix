# RUN: v3-eval --file %s --emit-ir-raw | v3-check %s --check-prefix=RAW
# RUN: v3-eval --file %s --emit-ir     | v3-check %s --check-prefix=OPT
#
# Phase A (betaReduce) + Phase B (constantFold) composition.
# `(x: x * 2) 21` lowers to App(Lambda, 21).  Beta-reduction inlines
# the body (substituting 21 for x), then constantFold computes 21*2=42.
# Post-opt the entry block contains only the literal result.
#
# Multi-RUN demonstrates --check-prefix= for testing the same source
# under different optimisation modes (RAW = pre-opt, OPT = post-opt).

(x: x * 2) 21

# Pre-opt: the unfolded App-of-Lambda + the body's `*` are present.
# NOTE (2026-06-05): operator `*` lowers to an App-chain over a
# LitPrimOp("__mul"), NOT an ir::Mul node (the lowerer never builds Mul/Add/…
# for operator syntax); fusePrimOpApps only later collapses it to a
# PrimOpCall, so the RAW (pre-opt) form shows LitPrimOp + Apps.
# RAW-LABEL: B1:
# RAW: v{{[0-9]+}} = Lambda f1
# RAW: v{{[0-9]+}} = LitInt 21
# RAW: v{{[0-9]+}} = App
# RAW: B2:
# RAW: v{{[0-9]+}} = LitPrimOp "__mul"

# Post-opt: B1 just produces the folded LitInt 42 — no Lambda, no App
# (the partial-app __mul residue is swept by the under-applied-App DCE),
# no Mul.  21*2 folds via primOpFold's arithmetic rule.
# OPT-LABEL: B1:
# OPT-NOT: Lambda
# OPT-NOT: App
# OPT-NOT: Mul
# OPT: v{{[0-9]+}} = LitInt 42
# OPT: return v{{[0-9]+}}
