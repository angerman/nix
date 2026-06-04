# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# NEGATIVE guard for the seq→Force lowering (seqForce-pos.nix): the rewrite
# fires ONLY for `seq` (shallow WHNF force).  `deepSeq` deep-forces its 1st
# arg recursively — a shallow ir::Force does NOT implement that — so deepSeq
# must keep going through its primop call (LitPrimOp "deepSeq" + OP_CALL),
# never collapse to a bare Force.

a: b: builtins.deepSeq a b

# deepSeq is NOT rewritten — the primop call survives:
# CHECK: LitPrimOp "deepSeq"
