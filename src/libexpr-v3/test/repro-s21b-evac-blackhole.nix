# S2.1b repro — deterministically trigger the moving-compactor Blackhole corruption
# (in-force thunk relocated mid-force).  KEY: a DEEP nested thunk-force chain so MANY
# thunks are simultaneously blackholed when the bottom allocates heavily → a mid-eval
# evac fires while they're blackholed (bhThunks>0) → fix-off relocates them → tag=14 /
# infinite recursion.  (A shallow per-element force, by contrast, blackhole→evaluates
# within one step and the evac fires between forces = bhThunks=0 = no repro.)
#
# Run under:  NIX_V3_MIDEVAL_GC=1 NIX_V3_NO_CONSERV_SCAN=1 NIX_V3_EVAC=1
#   NIX_V3_EVAC_PRECISE_ONLY=1 NIX_V3_EVAC_PCT=1.0 NIX_V3_MIDEVAL_GC_THRESHOLD_MB=1
#   NIX_V3_MIDEVAL_GC_GROWTH=1.0 NIX_VM_STATS=1
# fix-off = NIX_V3_EVAC_MOVE_BLACKHOLE=1 (expect tag=14/infinite recursion + bhThunks>0);
# fix-on (default) should stay correct WITH bhThunks>0 (proves the pin works).
let
  # mk n: forcing it forces `inner = mk (n-1)` (a let-thunk) → the whole chain of
  # `inner` thunks is blackholed at once; at the bottom a 200k-element genList+foldl'
  # allocates several MB → crosses the 1MB evac threshold WHILE ~depth thunks are
  # blackholed.
  mk = n:
    if n == 0
    then builtins.foldl' (a: b: a + b) 0 (builtins.genList (j: j) 200000)
    else let inner = mk (n - 1); in inner + n;
  # Repeat the deep force many times so an evac is very likely to land mid-chain.
  one = k: mk 120;
in builtins.foldl' (a: b: a + b) 0 (builtins.genList one 40)
