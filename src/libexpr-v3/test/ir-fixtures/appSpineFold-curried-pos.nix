# RUN: v3-eval --expr '(x: y: x * y) 6 7' --emit-ir | v3-check %s --check-prefix=N2
# RUN: v3-eval --expr '(x: y: z: x * y * z) 2 3 4' --emit-ir | v3-check %s --check-prefix=N3
# RUN: v3-eval --expr '(a: b: c: d: a + b + c + d) 1 2 3 4' --emit-ir | v3-check %s --check-prefix=N4
# RUN: v3-eval --expr '(x: y: y) (throw "a") 5' --emit-ir | v3-check %s --check-prefix=IMPURE
#
# Phase F (2026-05-18): static App-spine folding.  Recognises curried
# call chains `App(...App(App(f, a0), ...), aN-1)` where `f` is an
# N-deep canonical curried Lambda chain (each intermediate body is
# exactly `return Lambda L_{i+1}`), all args are pure, and the leaf
# Lambda has use-count 1.  Substitutes all N args into the deepest
# body in one shot.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 Phase F
# exit criterion: "PartialApp / Tag::PrimOpApp counters in
# allocStats drop measurably on the bench corpus."  This pass
# eliminates N-1 intermediate PartialApp values per curried-call
# spine that survives Phase A's single-arg inlining.

# N=2: post-opt the body's Mul has both literal args substituted.
# Constants then fold via Phase B → `LitInt 42` directly in B1.
# CHECK-NOT scans up to the next positive directive (the B2: label),
# so the Mul check is scoped to the OUTER block only — the inner
# Lambda's body (in B2) still contains a Mul as an orphan binding,
# which is expected (DCE keeps App / Mul / Force bindings even
# when unreferenced).
# N2-LABEL: ; func f0
# N2: v{{[0-9]+}} = LitInt 42
# N2-NOT: Mul
# N2-LABEL: ; func f1

# N=3: triple curry collapses to `LitInt 24`.
# N3-LABEL: ; func f0
# N3: v{{[0-9]+}} = LitInt 24
# N3-NOT: Mul
# N3-LABEL: ; func f1

# N=4: still inside kMaxSpineDepth (=8).  Note: Nix's `+` lowers to
# ConcatStrings (the parser can't statically decide string vs int);
# constantFold doesn't fold ConcatStrings, so we get the chain
# `ConcatStrings [1, 2]`-style bindings in B1, not a folded LitInt.
# That's expected — the per-arg substitution still happened, just
# the final folding requires int-specific recognition (out of scope
# for Phase F's pure-substitution role).
# N4-LABEL: ; func f0
# N4: v{{[0-9]+}} = ConcatStrings
# N4-LABEL: ; func f1

# Refusal: arg is impure (`throw "a"` is wrapped in MkThunk by the
# lowerer, and MkThunk isn't in this pass's pure-args shortlist).
# The original Lambda + App spine MUST remain unchanged.
# IMPURE-LABEL: B1:
# IMPURE: v{{[0-9]+}} = Lambda f{{[0-9]+}}
# IMPURE: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# IMPURE: v{{[0-9]+}} = App
# IMPURE: v{{[0-9]+}} = App
# IMPURE: PrimOpCall "throw"
