# RUN: v3-eval --file %s --emit-bytecode | v3-check %s
#
# Regression guard (2026-06-05): `--emit-bytecode` MUST run the eval-path
# strictness passes (ir::applyStrictnessPasses = computeFunctionStrictness +
# applyStrictnessAtCallSites) so the disassembly reflects what production
# (run.cc::runRootExpr) actually executes.
#
# WHY THIS EXISTS: `--emit-bytecode` used to run only `optimise` (NOT the
# strictness passes), so its disassembly showed a PRE-strictness form with the
# argument thunks still present.  That divergence misled a bytecode review
# into concluding fib's (n-1)/(n-2) arg thunks were unreachable — when in fact
# eval de-thunks them.  See run.cc (shared `ir::applyStrictnessPasses`).
#
# A recursive function that unconditionally forces its arg (`fib`: `n < 2`
# forces n) has its (n-1)/(n-2) call args DE-THUNKED by the caller-side
# strict-arg unthunk — so the disasm shows an INLINE `__sub` (OP_CALL_PRIMOP)
# and NO OP_MAKE_THUNK in the recursive body.  If `--emit-bytecode` ever
# regresses to skipping strictness, the args reappear as OP_MAKE_THUNK and the
# CHECK-NOT below fires.

let fib = n: if n < 2 then n else fib (n - 1) + fib (n - 2); in fib 5

# The recursive lambda's body (func "n") must contain NO OP_MAKE_THUNK before
# its final `+`: the strict (n-1)/(n-2) args are computed inline because
# strictness de-thunked them.  CHECK-NOT is bounded by the `+` op — which is
# OP_R_STR_CONCAT2 with the register VM on (the default) or OP_STR_CONCAT under
# NIX_V3_NO_R_STRCONCAT2=1; the regex matches either, present either way.  If
# strictness is skipped, the args reappear as OP_MAKE_THUNK before the `+`,
# tripping CHECK-NOT.
# CHECK-LABEL: ; func {{[0-9]+}} "n"
# CHECK-NOT: OP_MAKE_THUNK
# CHECK: OP_{{R_STR_CONCAT2|STR_CONCAT}}
