# A12 hot-loop hypothesis repro (2026-05-17).
#
# Hypothesis: primAttrValues copies Tag::App entries from a primMapAttrs
# result by VALUE into a ListVec.  When primElem / valueEqual / valueLess
# iterate that ListVec, they call forceValue on `src->elems[i]` BY VALUE
# — forceValue's signature is `Value forceValue(VMState&, Value v)`.  The
# resolved WHNF is returned but NEVER written back to `src->elems[i]`,
# so each elem-call re-evaluates every entry's mapAttrs lambda.
#
# Tree-walker doesn't have this: its forceValue takes `Value&` and its
# list elements are `Value *` shared with the source bindings — forces
# memoize via the pointer.
#
# This repro mimics `enum (attrValues (setTypes openCpuType cpuTypes))`'s
# hot path: a mapped attrset whose lambda body is heavy, attrValues'd
# into a list, then `elem x list` called many times.  If the hypothesis
# holds, v3 is dramatically slower than TW because each elem call
# re-runs the mapAttrs lambda for every list entry.
#
# Expected (after fix lands):
#   TW: O(N) per elem-call after first (cached)
#   v3: O(N) per elem-call after first (cached) — currently O(N) every call
let
  # Source attrset with 10 entries (matches typical cpuTypes/significantBytes scale).
  src = {
    a = 0; b = 1; c = 2; d = 3; e = 4;
    f = 5; g = 6; h = 7; i = 8; j = 9;
  };

  # Heavy mapAttrs lambda: builds 200 ints, folds them, returns a record.
  # Mimics setTypes' `assert openCpuType.check value; setType type.name (...)`
  # in cost (~few hundred opcodes per body).
  enriched = builtins.mapAttrs (n: v:
    let
      acc = builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i) 200);
    in {
      inherit n;
      value = v + acc;
      _type = "kind";
    }
  ) src;

  # attrValues materializes the list, copying entries by value.
  values = builtins.attrValues enriched;

  # Marker not in the list — forces full iteration each call.
  needle = { n = "zzz"; value = -1; _type = "kind"; };

  # Call `elem needle values` M times.  Each call iterates all 10 list
  # entries.  If list entries cache after first force, total lambda
  # invocations = 10.  Without caching: 10 * M.
  iters = 2000;
  result = builtins.foldl'
    (acc: i: acc + (if builtins.elem needle values then 1 else 0))
    0
    (builtins.genList (i: i) iters);
in
  result
