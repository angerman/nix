# REGRESSION test 1: mimic parsedPlatform.check pattern at small N.
#
# This is the structural shape that drove the A12 hot-loop on real
# nixpkgs: nested enum-check on mapAttrs results.
#
# Without writeback, calling check 10 times on a 10-entry list
# (none of which match) is 10×10 = 100 lambda invocations.
# With writeback, the first check forces all 10 entries; the
# subsequent 9 checks hit cached WHNF (0 invocations).  Total: 10.
#
# Returns the count of "matches" across 10 calls.  All calls miss,
# so returns 0.  The TRACE COUNT (driver counts FORCE-${n} lines)
# distinguishes cached (10) from uncached (100).
let
  N = 10;
  src = builtins.listToAttrs
    (builtins.genList (i: { name = "k${toString i}"; value = i; }) N);
  enriched = builtins.mapAttrs
    (n: v: builtins.trace "FORCE-${n}" { _type = "kind"; inherit n; v_doubled = v * 2; })
    src;
  values = builtins.attrValues enriched;
  needle = { _type = "kind"; n = "missing"; v_doubled = -1; };
  result = builtins.foldl'
    (acc: i: acc + (if builtins.elem needle values then 1 else 0))
    0
    (builtins.genList (i: i) N);
in
  result
