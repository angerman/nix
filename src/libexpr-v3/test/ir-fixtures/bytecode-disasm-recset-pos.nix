# RUN: v3-eval --file %s --emit-bytecode --no-opt | v3-check %s
#
# Regression guard for OP_ATTRS_REC_SET attr-name resolution — the
# disassembleModule-level annotation (disasm.cc): a REC_SET's operand is a
# sorted RANK into the governing OP_ATTRS_REC_INIT's SymbolId trailer, so
# resolving it needs the init's context, which disassembleModule tracks per
# function (nested attrsets are thunked into other functions, so within one
# function the inits are sequential and non-overlapping).
#
# Source order here (b, a) deliberately != sorted order (a, b), so this also
# locks the rank INDIRECTION: REC_SET operand=1 must resolve to `b`, not the
# source-index-0 attr.  A naive "operand = source index" would mislabel.
#
# `--no-opt` because the attrset construction is emitted at lowering, not by
# an optimiser pass — this locks the disassembler, not the optimiser.

{ b = 1; a = 2; }

# The init lists the attrs SymbolId-sorted:
# CHECK: OP_ATTRS_REC_INIT{{.*}}; {a, b}

# Each REC_SET resolves to its attr name.  Emit order is source order
# (b then a), and the rank operands (1 then 0) resolve through the sorted
# trailer to the right names:
# CHECK: OP_ATTRS_REC_SET{{.*}}operand=1{{.*}}; b
# CHECK: OP_ATTRS_REC_SET{{.*}}operand=0{{.*}}; a

# CHECK-NOT: OP_???
