# POSITIVE test 1: primElem on mapAttrs result caches list entries.
#
# Builds a 5-entry mapAttrs result, calls `elem -1 values` 4 times.
# Without writeback (pre-fix): mapAttrs lambda runs 5 * 4 = 20 times.
# With writeback (TW-parity): mapAttrs lambda runs 5 times total.
#
# Validation via trace counter:
#   TW: 5 FORCE traces.
#   v3-direct + fix: 5 FORCE traces (matches TW).
#   v3-direct, no fix: 20+ FORCE traces.
#
# Driver: test/run-583-tag-app-cache-tests.sh counts trace lines.
let
  src = { a = 0; b = 1; c = 2; d = 3; e = 4; };
  mapped = builtins.mapAttrs (n: v: builtins.trace "FORCE-${n}" v) src;
  values = builtins.attrValues mapped;
  needle = -1;
  result = builtins.foldl'
    (acc: i: acc + (if builtins.elem needle values then 1 else 0))
    0
    (builtins.genList (i: i) 4);
in
  result
