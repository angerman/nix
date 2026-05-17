# POSITIVE test 2: valueEqual on lists of mapAttrs results caches entries.
#
# Same caching semantics as primElem (test 1), but exercises the
# vm.cc valueEqual case Tag::List writeback specifically.
#
# Construction:
#   - Two parallel mapAttrs-of-same-shape attrsets.
#   - attrValues each into two lists.
#   - Compare via `==` (drives valueEqual).
#
# Without writeback (pre-fix), entries re-evaluate on every compare.
# With writeback, each side's entries are evaluated once.
#
# Each entry has a trace tag.  Comparison should yield exactly 5
# traces per side = 10 total under v3 + fix (matches TW).
let
  src = { a = 0; b = 1; c = 2; d = 3; e = 4; };
  mappedA = builtins.mapAttrs (n: v: builtins.trace "A-${n}" v) src;
  mappedB = builtins.mapAttrs (n: v: builtins.trace "B-${n}" v) src;
  valuesA = builtins.attrValues mappedA;
  valuesB = builtins.attrValues mappedB;
in
  if valuesA == valuesB then "equal" else "not-equal"
