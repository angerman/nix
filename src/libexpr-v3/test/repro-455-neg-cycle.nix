# #455 NEGATIVE test — a GENUINELY cyclic expression must still be REJECTED.
#
# `final`'s WHNF needs the `//` RHS, which needs `final.a`, which needs `final`'s WHNF
# → a real infinite recursion.  BOTH the tree-walker and v3 throw "infinite recursion
# encountered" on this.  The #455 fix (which makes v3 terminate on the *non*-cyclic
# pkgset/lib fixpoints) MUST NOT mask this — it must still error, not spin and not
# return a bogus value.  The driver asserts v3 reports infinite recursion (NOT a
# WallTimeExceeded spin, NOT a value).
let
  final = { a = 1; } // (if final.a == 1 then { b = 2; } else { });
in
final.b
