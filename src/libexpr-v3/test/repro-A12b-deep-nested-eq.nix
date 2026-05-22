# A12b regression: iterative valueEqual must handle deeply nested
# list/attrset comparison without C-stack overflow.  Pre-fix this
# would trigger kMaxCallDepth=5000 via valueEqual recursing through
# itself on every nested level.
#
# Build two nested lists, 10000 deep — identical shape, so the
# comparison must traverse every level.  Returns `true` iff
# byte-equality holds AND no stack overflow occurred.
let
  # Nest n levels: { x = { x = { x = ... = 1 ... } } }.
  nest = n: if n == 0 then 1 else { x = nest (n - 1); };
  # Nested-lists analogue (more interesting because list-tag recursion
  # used to be the heaviest C-recursion site in valueEqual).
  nestList = n: if n == 0 then [ 1 ] else [ (nestList (n - 1)) ];
in {
  attrsEq = (nest 10000) == (nest 10000);
  listEq  = (nestList 10000) == (nestList 10000);
  # Sanity: differing nesting should be detectable too (not equal).
  differingDepth = (nest 5) == (nest 6);
}
