# PAP-in-attrset SELECT-writeback — POSITIVE.
#
# An under-applied closure-PAP stored in an attrset, forced to WHNF and then
# SELECTed and CALLED repeatedly, must keep working.  The OP_ATTRS_SELECT
# App-writeback (the memoizing force on an `isAppLike()` slot) must NOT fire on
# an under-applied PAP — a PAP is already WHNF, and forcing+memoizing it
# saturated it to its result type and poisoned the (often shared) entry.
#
# That was the 2026-06-11 systemic-drvPath root cause: nixpkgs python3's
# `passthru.pythonAtLeast = lib.versionAtLeast pythonVersion` (a PAP) had its
# SELECT slot mutated Thunk -> App(PAP) -> Bool, so a later
# `passthru.pythonAtLeast "3.14"` did `OP_CALL` on a Bool ("callee is not a
# closure"), native derivationStrict fell back to /v3-fake-store/, and the
# drvPath of every package depending on python3 diverged.
#
# Expected (matches TW): [ 11 21 31 "lambda" 101 102 103 ]
let
  f = a: b: a + b;
  s = { p = f 1; };               # PAP stored in an attrset
  t = rec { base = 100; q = builtins.add base; };  # PAP via rec-self
in
[
  (builtins.seq s.p (s.p 10))     # 11   — force slot to WHNF, then call
  (s.p 20)                        # 21   — re-select + call (slot not poisoned)
  (s.p 30)                        # 31
  (builtins.typeOf s.p)           # "lambda" — slot is still a function, not a Bool
  (t.q 1) (t.q 2) (t.q 3)         # 101 102 103
]
