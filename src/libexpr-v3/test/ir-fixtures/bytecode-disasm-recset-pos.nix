# RUN: v3-eval --file %s --emit-bytecode --no-opt | v3-check %s
#
# Regression guard for OP_ATTRS_REC_SET attr-name resolution — the
# disassembleModule-level annotation (disasm.cc): a REC_SET's operand is a
# sorted RANK into the governing OP_ATTRS_REC_INIT's SymbolId trailer, so
# resolving it needs the init's context, which disassembleModule tracks per
# function (nested attrsets are thunked into other functions, so within one
# function the inits are sequential and non-overlapping).
#
# A `rec` set is used because only recursive attrsets keep the REC_INIT/REC_SET
# form — non-recursive `{ ... }` literals are demoted to OP_ATTRS_INIT at emit
# (unconditional).  Source order here (b, a) deliberately != sorted trailer
# order (a, b), so this locks the rank INDIRECTION: the REC_SETs are emitted in
# sorted-trailer order and operand=0 must resolve to `a`, operand=1 to `b`
# through the sorted trailer.  A naive "operand = source index" would mislabel.
#
# `--no-opt` because the attrset construction is emitted at lowering, not by
# an optimiser pass — this locks the disassembler, not the optimiser.

rec { b = 1; a = 2; }

# The init lists the attrs SymbolId-sorted:
# CHECK: OP_ATTRS_REC_INIT{{.*}}; {a, b}

# Each REC_SET resolves to its attr name via the sorted trailer.  The REC_SETs
# emit in sorted-trailer order (a then b); the rank operands (0 then 1) resolve
# through the trailer to the right names:
# CHECK: OP_ATTRS_REC_SET{{.*}}operand=0{{.*}}; a
# CHECK: OP_ATTRS_REC_SET{{.*}}operand=1{{.*}}; b

# CHECK-NOT: OP_???
