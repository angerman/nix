# PAP recognition — NEGATIVE.
#
# The PAP fix must NOT over-classify: a fully-applied result (NOT a PAP) and
# plain non-functions must still report correctly.  Forcing a PAP-shaped value
# that is actually a saturated application yields its result type, not "lambda".
# Expected (matches TW exactly):
#
#   [ false false false false "int" ]
#
let
  f = a: b: a + b;
in
[
  (builtins.isFunction (f 1 2))    # saturated → int 3, not a function -> false
  (builtins.isFunction 42)         #                                   -> false
  (builtins.isFunction { a = 1; }) #                                   -> false
  (builtins.isFunction [ 1 ])      #                                   -> false
  (builtins.typeOf (f 1 2))        # saturated application             -> "int"
]
