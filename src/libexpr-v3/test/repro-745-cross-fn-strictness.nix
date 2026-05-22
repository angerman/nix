# #745 Stage 4 v4.3 cross-function strictness — synthetic exerciser.
#
# `inner x = x + 1` is strict in x (Add forces x). Without cross-fn
# propagation, `outer y = inner y` would NOT be marked strict in y
# (App alone doesn't force the arg).  With cross-fn propagation,
# outer.strictArgs[0] = true (App resolves to inner; inner is strict
# in arg0; therefore y is forced via the App).
#
# We can't directly observe strictArgs from the Nix language, but we
# can observe the runtime behavior: with cross-fn strictness +
# applyStrictnessAtCallSites elision, a strict-but-lazily-built arg
# at a wrapper call site should be PRE-FORCED at the wrapper site
# instead of being wrapped in a Thunk that the inner forces later.
#
# This file is the FIXTURE; the diagnostic counter
# `NIX_V3_DBG_STRICT_CALL_UNTHUNK=1` reports elision counts, and the
# fixed-point iteration count is reported by NIX_V3_DBG_STRICTNESS=1.
let
  # Single-level wrapper: inner is strict, outer wraps inner.
  inner = x: x + 1;
  outer = y: inner y;

  # Two-level wrapper: triple-nested calls should propagate strictness
  # through both layers via fixed-point iteration.
  inner2 = a: a + 2;
  middle = b: inner2 b;
  outer2 = c: middle c;

  # Use both: the result is forced at the top level so the wrappers'
  # bodies are exercised; cross-fn analysis can mark outer/middle/
  # outer2 strict iff the iteration converges through the call chain.
in
  outer (1 + 2) + outer2 (3 + 4)
