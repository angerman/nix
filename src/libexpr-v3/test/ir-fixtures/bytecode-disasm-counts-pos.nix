# RUN: v3-eval --file %s --emit-bytecode --no-opt | v3-check %s
#
# Regression guard for the count/packed-operand annotations added to the
# disassembler (disasm.cc::disassembleOne) after the annotation-gap scan:
#
#   OP_LIST_INIT      operand = element count        → "; N elems"
#   OP_STR_CONCAT     operand = (nParts<<1)|force    → "; N parts[, force]"
#   OP_ATTRS_INIT_DYN operand = (nStatic<<12)|nDyn   → "; S static + D dyn"
#
# STR_CONCAT and ATTRS_INIT_DYN PACK their operand, so the raw value is
# misleading (STR_CONCAT operand=5 is *2* parts, not 5) — these annotations
# decode it.  `--no-opt` because all three are emitted at lowering, not by
# an optimiser pass, so this locks the disassembler, not the optimiser.
#
# One expression hits all three: a dynamic attr key (ATTRS_INIT_DYN), a
# list literal (LIST_INIT), and a string interpolation (STR_CONCAT).

{ "k${toString 1}" = 1; xs = [ 10 20 30 ]; s = "a${toString 1}b"; }

# CHECK: OP_LIST_INIT{{.*}}; {{[0-9]+}} elem
# CHECK: OP_STR_CONCAT{{.*}}; {{[0-9]+}} part
# CHECK: OP_ATTRS_INIT_DYN{{.*}}; {{[0-9]+}} static + {{[0-9]+}} dyn

# No unknown opcode leaks (opName covers everything emitted here):
# CHECK-NOT: OP_???
