# POSITIVE test 3: valueEqual on attrsets-of-mapAttrs-results caches entries.
#
# Exercises vm.cc valueEqual case Tag::Attrs writeback.  Two parallel
# attrsets contain mapAttrs results as values.  Comparing them via
# `==` recursively drives valueEqual into the inner entries.
#
# Without writeback, inner entries re-evaluate on every comparison.
# With writeback, each inner entry is evaluated once.
#
# Trace assertion: 8 traces (4 per side) under v3 + fix, matches TW.
let
  src = { p = 10; q = 20; r = 30; s = 40; };
  mkSide = tag: builtins.mapAttrs
    (n: v: builtins.trace "${tag}-${n}" v)
    src;
  sideA = mkSide "A";
  sideB = mkSide "B";
in
  if sideA == sideB then "equal" else "not-equal"
