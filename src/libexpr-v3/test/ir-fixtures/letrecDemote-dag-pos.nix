# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# #2 DAG demotion (NEXT_STEPS_2026-06-05 §2/§3, QUANTIFICATION #2): a `let`
# whose bindings reference siblings but form an ACYCLIC dependency graph still
# needs no rec-attrset.  The lowerer (cli/lower_v3.hh lowerLetRec DAG branch)
# topologically orders the entries and re-lowers each in a plain scope, so a
# sibling reference becomes a direct VarRef (→ GET_LOCAL / upvalue) instead of
# the rec machinery (LetRec + RecBindingSlotRef + a per-binding thunk function
# read through the synthetic rec-attrset).
#
# Here `c` depends on `a` and `b`; `b` depends on `a`; `a` depends on nothing
# — a DAG (no cycle), so ALL of it demotes.  This extends the non-recursive
# demotion (letrecDemote-nonrec-pos.nix) to the acyclic-multi-binding case,
# which the real-corpus measurement showed is ~70% of recursive lets.
#
# Regression guard: DAG demotion is unconditional; without it this would
# emit the LetRec form (and the truly-cyclic case — self / mutual
# recursion — always keeps it).

let b = a + 1; a = 2; c = a + b; in c

# CHECK-NOT: LetRec
# CHECK-NOT: RecBindingSlotRef
