# Phase C stream-fusion semantic regression fixture.
#
# Asserts: the `foldl' + map` pattern produces the same result with
# fusion ON (default) and OFF (NIX_V3_NO_STREAM_FUSION=1).
#
# Driver: run-stream-fusion-tests.sh sweeps both modes and compares.
#
# Lesson: IR_OPTIMIZATION_PLAN_2026-05-18.md Phase C — fuse
# `foldl' op nul (map f xs)` to `__foldlMap op nul f xs` to skip
# intermediate list allocation.  The fused call must dispatch via the
# bytecode-closure replacement (App-chain shape over LitPrimOp), NOT
# PrimOpCall: emitting PrimOpCall bypasses the OP_LIT_PRIMOP redirect
# and invokes the C-recursive `primFoldlMap` directly, which measures
# 60x slower on N=200K than either OFF or the bytecode dispatch path.
let
  # Small-range static-data case: known answer.
  small =
    let xs = builtins.genList (i: i) 10;
    in builtins.foldl' (acc: x: acc + x) 0 (map (x: x * 2) xs);

  # Mid-range case: still small enough to compute at parse-time.
  mid =
    let xs = builtins.genList (i: i) 100;
    in builtins.foldl' (acc: x: acc + x) 0 (map (x: x * 2) xs);

  # Mixed initial accumulator + non-trivial map function.
  mixed =
    let xs = builtins.genList (i: i + 1) 5;
    in builtins.foldl' (acc: x: acc * x) 1 (map (x: x + 10) xs);

  # foldl' over an empty list — fused __foldlMap must return nul.
  empty =
    builtins.foldl' (acc: x: acc + x) 42 (map (x: x * 100) []);

  # foldl' over a singleton.
  singleton =
    builtins.foldl' (acc: x: acc - x) 100 (map (x: x * 2) [ 5 ]);

  # String concat fold over map.
  strings =
    builtins.foldl' (acc: x: acc + x) "" (map (x: toString x + ",") [ 1 2 3 ]);

in
{
  inherit small mid mixed empty singleton strings;

  # Expected: 90 (2*(0+1+..+9))
  smallExpected = 90;

  # Expected: 9900 (2*sum 0..99 = 2*4950)
  midExpected = 9900;

  # Expected: 11*12*13*14*15 = 360360
  mixedExpected = 360360;

  # Expected: 42 (empty list → initial value)
  emptyExpected = 42;

  # Expected: 100 - (5*2) = 90
  singletonExpected = 90;

  # Expected: "1,2,3,"
  stringsExpected = "1,2,3,";
}
