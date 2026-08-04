# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# opt_letrec_demote: a NON-recursive `let x = e; in body` must NOT compile to
# the rec-attrset machinery (LetRec + RecBindingSlotRef + a per-binding thunk
# function).  `x` references nothing recursive, so it should be a plain thunk
# binding read via VarRef.  This is the fold-add-1M 17× root cause: the
# bytecode foldl''s `let next = op acc elem; in ...` allocated a rec-attrset
# per iteration.  Affects EVERY non-recursive `let` in nixpkgs.
#
# Fixed at lowering (cli/lower_v3.hh lowerLetRec non-recursive demotion).
# Regression guard: the demotion is unconditional; without it this would
# emit the LetRec form.

a: b: let x = a + b; in x + x

# CHECK-NOT: LetRec
# CHECK-NOT: RecBindingSlotRef
