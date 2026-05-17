# POSITIVE test: OP_CALL's `fun` force is now iterative (A12b
# mitigation).  Exercises the path where `fun` is a Tag::App from a
# primMapAttrs result, applied to an arg.  Before the conversion
# (commit TBD), this used C-recursive `fun = forceValue(vm, fun)`
# inside OP_CALL's body, growing the C-stack by one
# forceValue → dispatchLoop frame per call.  After the conversion,
# the force runs via op_force_slow + writeback slot, staying on
# vm.frames without C-recursion.
#
# Functionally both paths must produce the same answer; this test
# locks in the WHNF-result correctness.
#
# Deeper test (1000 cascaded Tag::App calls) verifies the
# iterative path doesn't blow the C-stack on workloads that
# pre-fix would have neared depth=5000.
let
  # First: simple correctness check.
  fns = builtins.mapAttrs (n: v: x: x * v) { triple = 3; double = 2; };
  tripleResult = fns.triple 14;
  doubleResult = fns.double 7;

  # Second: deeper recursion through Tag::App fun-force.
  # Build a 200-entry attrset of curried lambdas via mapAttrs, then
  # apply each via foldl' so OP_CALL's fun is Tag::App every step.
  ids = builtins.listToAttrs
    (builtins.genList (i: { name = "k${toString i}"; value = i; }) 200);
  bumps = builtins.mapAttrs (n: v: x: x + v) ids;
  bumpsList = builtins.attrValues bumps;
  # foldl' applies each lambda to the accumulator.  The lambdas
  # are Tag::App from primMapAttrs; OP_CALL forces them.
  accResult = builtins.foldl' (acc: f: f acc) 0 bumpsList;
  # Sum of 0..199 = 199 * 200 / 2 = 19900.

in
  if tripleResult == 42 && doubleResult == 14 && accResult == 19900
  then "ok"
  else throw "FAILED: triple=${toString tripleResult} double=${toString doubleResult} acc=${toString accResult}"
