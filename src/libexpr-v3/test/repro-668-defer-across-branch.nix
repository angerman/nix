# Repro for #668 — `with`+interpolated-If under #542 emit-time deferring
#
# Before fix (`NIX_V3_NO_DEFER` unset, default-on deferring): v3 emits
# OP_SET_LOCAL for a deferred literal "_" INSIDE the then-branch of the
# If via flushAllDeferred-on-emitBlock-entry, but emits NO matching SET
# in the else-branch.  At runtime the else path leaves the deferred
# value un-consumed on the value stack; a later OP_GET_LOCAL reads an
# Uninitialized slot, OP_STR_CONCAT throws "cannot coerce type to
# string (tag=0)", and the secondary unwind through the boost-coroutine
# bridge produces a SIGTRAP via the std::terminate landing pad.
#
# The mechanism is the same for And/Or/Impl/If — any divergent
# control-flow opcode whose two paths must reach the merge with equal
# stack depth.  The fix flushes remaining pending entries to slots
# BEFORE the branch op via a scratch-slot stash/restore around the
# cond (so SETs run unconditionally on both paths).
#
# Symptom downstream: nixpkgs go.drvPath / haskell.compiler.ghc98.drvPath
# SIGTRAP.  go.drvPath was confirmed end-to-end fixed; ghc98 had a
# separate trap signature pursued under a follow-up sub-letter.
#
# Expected (matches TW): "_n_"
with { a = "x"; }; "_${if a == "z" then "y" else "n"}_"
