# A12b regression: iterative `forceDeep` must walk deeply-nested
# attrset / list trees without C-stack overflow.  Pre-fix
# `print.cc::forceDeep` C-recursed at every nested level (with a
# DeepForceGuard pushed per level for GC safety).  Iterative form
# uses `tlDeepForceRoots` as the GC-protected work queue.
#
# Build a 5000-deep attrset wrapping a 5000-deep list — both branches
# of the recursion paths get exercised in the same value.  Use a
# `builtins.deepSeq` to force the full graph.  Pre-fix this would
# hit kMaxCallDepth=5000 (or worse, segfault on C-stack overflow);
# post-fix, completes in <1 s.
let
  nestAttrs = n: if n == 0 then "leaf" else { x = nestAttrs (n - 1); };
  nestList  = n: if n == 0 then [ "leaf" ] else [ (nestList (n - 1)) ];
  mixed = {
    attrs = nestAttrs 5000;
    list  = nestList 5000;
  };
in
  # deepSeq forces the full graph.  Returns the second arg unchanged.
  builtins.deepSeq mixed "ok"
