# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase A negative fixture: betaReduce MUST NOT fire when the
# lambda's body itself contains a nested Lambda or MkThunk binding.
# Inlining such a body into the call site would either duplicate
# captured-state machinery or break sharing of the inner
# lambda/thunk's closure.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 + §9 #3
# ("negative fixtures are harder than positive ones... per-pass
# purity analysis is the example").  Phase A's safety check
# explicitly refuses bodies containing Lambda / MkThunk / LetRec /
# If / With / Assert / And / Or / Impl.
#
# Shape: `(x: y: x + y) 5` — outer `x:` body returns inner `y:`
# Lambda.  Phase A refuses; post-opt IR still has the outer App.

(x: y: x + y) 5

# The outer App must remain — beta-reduce refused to inline.
# CHECK-LABEL: ; func f0
# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = Lambda f1
# CHECK: v{{[0-9]+}} = LitInt 5
# CHECK: v{{[0-9]+}} = App
# CHECK-NOT: v{{[0-9]+}} = Lambda f2

# The outer lambda's body still contains the inner Lambda — the
# original (un-inlined) shape is preserved.
# CHECK-LABEL: ; func f1
# CHECK-LABEL: B2:
# CHECK: v{{[0-9]+}} = Lambda f2
