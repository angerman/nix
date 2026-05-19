# Repro for #669 follow-up — `nix eval --impure` printer prints the
# contextual binding name for let-bound / attr-bound lambdas, matching
# TW's `«lambda <name>? @ pos»` format.
#
# This fixture exercises the structural cases: anonymous, let-bound,
# attr-bound, and curried (where the outer binding's name should apply
# to the outermost lambda only).  The result is a string literal so
# the lang-test runner compares stable text.
#
# NB: the rich-printer output isn't directly checked here (the lang
# tests use the simplified printer); a separate `test/run-lambda-
# contextual-name.sh` test would diff `nix eval`'s output against TW.
# For this fixture the value is asserted via `builtins.functionArgs`,
# which exercises the same `desc->contextualName` plumbing without
# being printer-dependent.
let
  anon = x: x + 1;
  foo  = y: y + 2;
in {
  anon_argName = builtins.head (builtins.attrNames (builtins.functionArgs ({x ? 1}: x)));
  foo_argName  = builtins.head (builtins.attrNames (builtins.functionArgs ({y ? 1}: y)));
  # Functor-style chain: each step is a separate lambda; the outermost
  # gets the let-binding name `mkBox`, inner lambdas stay anonymous.
  mkBox_appliedOnce = (
    let mkBox = a: b: a + b;
    in (mkBox 1)
  ) 4;  # = 5
}
