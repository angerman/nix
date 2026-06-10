# PAP recognition — POSITIVE.
#
# The eval/apply optimisation (NIX_V3_EVAL_APPLY, default-ON) collapses curried
# `a: b: …` lambdas into a single multi-arity closure, so a partial application
# materialises as a WHNF Tag::App / App3 PAP.  builtins.typeOf / isFunction /
# functionArgs MUST treat such a PAP as the tree-walker treats the inner lambda
# it stands for.  Expected (matches TW exactly):
#
#   [ "lambda" true "lambda" "lambda" { } "int" 3 ]
#
let
  f = a: b: a + b;          # arity-2 collapsed closure
  g = a: b: c: a + b + c;   # arity-3 collapsed closure
in
[
  (builtins.typeOf (f 1))         # PAP (1/2 applied)        -> "lambda"
  (builtins.isFunction (f 1))     # PAP is a function        -> true
  (builtins.typeOf (g 1))         # PAP (1/3 applied)        -> "lambda"
  (builtins.typeOf (g 1 2))       # PAP (2/3 applied)        -> "lambda"
  (builtins.functionArgs (f 1))   # next param is positional -> { }
  (builtins.typeOf (f 1 2))       # fully applied            -> "int"
  ((f 1) 2)                       # PAP is still callable     -> 3
]
