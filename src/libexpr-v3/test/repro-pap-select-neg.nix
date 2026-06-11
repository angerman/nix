# PAP-in-attrset SELECT-writeback — NEGATIVE.
#
# The fix is narrow: it skips the SELECT App-writeback ONLY for under-applied
# PAPs (isUnderappliedClosurePap).  A genuine lazy / saturated App attr value
# (mapAttrs / genList style) must STILL be forced + memoized by the writeback —
# repeated SELECTs return the same forced value with no re-computation or
# divergence.  This guards that the guard didn't disable legitimate App-entry
# memoization.
#
# Expected (matches TW): [ 11 11 21 "set" ]
let
  m = builtins.mapAttrs (n: v: v + 1) { a = 10; b = 20; };  # lazy App entries
in
[
  m.a m.a m.b           # 11 11 21 — saturated App attrs force + memoize correctly
  (builtins.typeOf m)   # "set"
]
