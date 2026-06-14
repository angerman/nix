# PhD-6 regression (2026-06-15): nursery missed-root via unbarriered
# list-construction primops.
#
# Each of these primops allocates a result ListVec (via Alloc::allocList →
# nurseryOrArena) and fills it with values that can be nursery-resident
# (lazy thunks, attrset/list children).  Under nursery pressure the result
# list is arena-allocated (tenured) while its elements stay nursery — so the
# construction site MUST call listPostConstructBarrier or the scavenger
# strands the elements (use-after-free).  primZipAttrsWith was the concrete
# git.drvPath blocker; the rest are the same family found by the C3-2 sweep.
#
# Run under: NIX_V3_NURSERY=1 NIX_V3_NURSERY_SIZE=1 NIX_V3_NURSERY_SCAVENGE=1
#            NIX_V3_GEN_MAJOR=1 NIX_V3_NO_MAJOR_GC=1 V3_DBG_NURSERY_AUDIT=1
# Expect: "v3 SCAVENGE AUDIT: clean" (zero "reachable via" lines) + result "ok".
let
  # Lists of lazy thunks (each element is an unforced attrset / arithmetic thunk).
  xs   = builtins.genList (i: { n = i; s = toString i; sq = i * i; }) 600;
  ys   = builtins.genList (i: [ i (i + 1) ]) 300;

  gb   = builtins.groupBy (x: if x.n - (x.n / 10) * 10 < 5 then "lo" else "hi") xs;
  cm   = builtins.concatMap (x: [ x.n x.sq ]) xs;
  cl   = builtins.concatLists ys;
  flt  = builtins.filter (x: x.n > 100) xs;
  prt  = builtins.partition (x: x.n > 300) xs;
  av   = builtins.attrValues (builtins.listToAttrs
           (builtins.genList (i: { name = toString i; value = i * i; }) 50));
  za   = builtins.zipAttrsWith (_n: vs: vs)
           (builtins.genList (i: { ${toString i} = i; shared = i; }) 40);
  fj   = builtins.fromJSON ''[1, 2, [3, 4, [5]], {"x": 6, "y": [7, 8]}]'';
  sp   = builtins.split "(a+)" "xaaxaxaaax";
  ca   = builtins.catAttrs "n" xs;
in
  builtins.deepSeq [ gb cm cl flt prt za fj sp ca ] "ok"
