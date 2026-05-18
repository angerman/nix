# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase F: 4-arg curried call.  Well within kMaxSpineDepth=8.
#
# Note: Nix's `+` lowers to `ConcatStrings` (the parser can't
# statically decide string vs int).  constantFold doesn't fold
# ConcatStrings, so the cleanup chain leaves ConcatStrings bindings
# in B1 — that's expected.  The per-arg substitution still
# happened; the final folding requires int-specific recognition
# which is out of scope for Phase F's pure-substitution role.

(a: b: c: d: a + b + c + d) 1 2 3 4

# CHECK-LABEL: ; func f0
# CHECK: v{{[0-9]+}} = ConcatStrings
# CHECK-LABEL: ; func f1
