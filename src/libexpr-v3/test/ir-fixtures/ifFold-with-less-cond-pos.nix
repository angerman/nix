# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase G + Phase B composition.  `1 < 2` is folded by constantFold
# (Phase B) to `LitBool true`; ifThenFold (Phase G) then picks the
# THEN branch.  Post-opt shape is identical to the static-true case
# in ifFold-true-pos.nix: just LitInt 100, no Less, no If.

if 1 < 2 then 100 else 200

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitInt 100
# CHECK-NOT: Less
# CHECK-NOT: If
# CHECK-NOT: LitInt 200
