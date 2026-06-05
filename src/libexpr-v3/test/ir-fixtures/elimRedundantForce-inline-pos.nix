# RUN: v3-eval --file %s --emit-ir-raw | v3-check %s --check-prefix=RAW
# RUN: v3-eval --file %s --emit-ir     | v3-check %s --check-prefix=OPT
#
# inlineTrivialBindings + elimRedundantForce composition.  The lowerer
# emits a VarRef chain + redundant Force for every variable access
# (the A-normal-form-style binding-per-subexpression pattern from
# lower.cc).  Without optimisation, even `let z = "hi"; in z` lowers
# to ~4 bindings; after opt, the chain collapses to a single Force on
# the binding source.
#
# Plan reference: IR_OPTIMIZATION_PLAN_2026-05-18.md §2.5 — these
# cleanup passes are correctness-clean rewrites with no behaviour
# change; the fixture's value is catching silent regressions where a
# cleanup stops firing (which alone wouldn't break tests but blows up
# bytecode size and runtime).

let z = "hi"; in z

# NOTE (2026-06-05): non-recursive `let` no longer lowers via a rec-attrset
# (RecBindingSlotRef) — the letrec-demote optimisation (cd1da2577) binds the
# value as a plain MkThunk and lowers the body in a non-rec scope.  The shape
# this fixture verifies is unchanged (binding source + VarRef hop chain + one
# Force, collapsing to a single Force after opt); only the source node is now
# MkThunk instead of RecBindingSlotRef.
#
# RAW form — every variable hop produces a VarRef + Force pair, even
# when the source is already a known literal in scope.
# RAW: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# RAW: v{{[0-9]+}} = VarRef v{{[0-9]+}}
# RAW: v{{[0-9]+}} = VarRef v{{[0-9]+}}
# RAW: v{{[0-9]+}} = Force v{{[0-9]+}}

# OPT form — the VarRef chain collapses; only one Force remains on
# the binding source directly.
# OPT-LABEL: B1:
# OPT: v{{[0-9]+}} = MkThunk f{{[0-9]+}}
# OPT-NEXT: v{{[0-9]+}} = Force v{{[0-9]+}}
# OPT-NEXT: return v{{[0-9]+}}
# OPT-NOT: VarRef
