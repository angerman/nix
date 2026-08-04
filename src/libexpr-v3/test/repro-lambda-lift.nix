# Phase D — closure-free lambda lifting regression fixture.
#
# Asserts: TW and v3-direct (with Phase D ON and OFF) produce the
# same result for a variety of capture-free + capturing lambda
# shapes.
#
# Lesson: IR_OPTIMIZATION_PLAN_2026-05-18.md Phase D — when a
# Lambda IR node has empty freeVars AND empty lexicalWiths, every
# OP_MAKE_CLOSURE produces a semantically identical Closure (same
# desc, no upvalues, no captured withs).  Intern by descriptor in
# vm.cc OP_MAKE_CLOSURE (unconditional).
#
# Coverage:
#   1. Plain capture-free lambda: f = x: x + 1.
#   2. Capturing lambda (must NOT be interned): mkAdder = n: (x: x + n).
#   3. Capture-free lambda inside `with`: shouldn't intern the inner
#      lambda if it references an enclosing with — but our outer
#      `(a: ...)` here doesn't reference X, so it's still safe.
#   4. Higher-order: a factory returning a capture-free lambda.
#   5. Nested currying: outer is capture-free, inner captures the
#      outer's arg.
#   6. Recursive lambda via let-rec: f references itself, NOT
#      capture-free (free var f).  Should fall through to the
#      generic path.
let
  # 1: trivially capture-free
  inc = x: x + 1;
  incApp = inc 41;

  # 2: capturing — n is the closure's upvalue
  mkAdder = n: (x: x + n);
  addThree = mkAdder 3;
  addedTwelve = addThree 9;

  # 3: with-enclosed but body doesn't reference X (lexicalWiths empty)
  withTest = with { X = 100; }; (a: a * 2) 21;

  # 4: factory returning capture-free lambda
  mkConst = _: (x: 42);
  constLambda = mkConst null;
  constApp = constLambda "ignored";

  # 5: nested currying — outer capture-free, inner captures
  addAndDouble = a: b: (a + b) * 2;
  curried = addAndDouble 3;
  curriedApp = curried 7;

  # 6: recursive (NOT capture-free)
  fact = n: if n <= 1 then 1 else n * fact (n - 1);
  fact5 = fact 5;

in
{
  inherit incApp addedTwelve withTest constApp curriedApp fact5;
  # Expected values:
  #  incApp        = 42      (41 + 1)
  #  addedTwelve   = 12      ((9 + 3))
  #  withTest      = 42      ((21 * 2))
  #  constApp      = 42      (constLambda ignores arg)
  #  curriedApp    = 20      ((3 + 7) * 2)
  #  fact5         = 120     (5! = 120)
  expectedSum = 42 + 12 + 42 + 42 + 20 + 120;  # = 278
  actualSum = incApp + addedTwelve + withTest + constApp + curriedApp + fact5;
}
