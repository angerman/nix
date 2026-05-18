# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# Phase G + Phase B composition.  `(1 == 2)` is folded by
# constantFold (Phase B) to `LitBool false`; ifThenFold (Phase G)
# then picks the ELSE branch.  Post-opt: just LitString "yes".

if (1 == 2) then "no" else "yes"

# CHECK-LABEL: B1:
# CHECK: v{{[0-9]+}} = LitString "yes"
# CHECK-NOT: LitString "no"
# CHECK-NOT: If
# CHECK-NOT: Eq
