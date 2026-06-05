# RUN: v3-eval --file %s --emit-bytecode --no-opt | v3-check %s
#
# Regression guard for the high-quality bytecode disassembler
# (disasm.cc::disassembleModule, surfaced via `v3-eval --emit-bytecode`).
#
# `--no-opt` keeps the bytecode shape a function of lowering+compile only,
# so this fixture locks the DISASSEMBLER FORMAT rather than the optimiser's
# output (which other ir-fixtures already cover).  It asserts the four
# pillars that make the disassembler "high-quality":
#
#   1. per-function framing      — `; func K "name" arity=A ...` headers
#   2. operand resolution        — primops/ints/strings shown symbolically
#   3. symbolic jump labels      — `; -> L<off>` annotations + `L<off>:`
#                                  leaders for branch targets
#   4. full opcode coverage      — no `OP_???` (opName resolves every Op)
#
# The lambda exercises a primop comparison (`__lessThan`), an int literal,
# a two-armed conditional (BRANCH_FALSE + JUMP + two jump targets), string
# literals on both arms, and a top-level MAKE_CLOSURE that names the lifted
# function — i.e. one expression covering all four pillars.
#
# Offsets / operand indices / symbol ids are build-dependent, so every
# numeric is matched with `{{[0-9]+}}` and column padding with `{{.*}}`.

x: if x > 0 then "pos" else "neg"

# Module banner is present and reports a function count.
# CHECK: ; module functions={{[0-9]+}}

# The user lambda is lifted to its own named, arity-1 function frame.
# CHECK-LABEL: ; func {{[0-9]+}} "x"{{.*}}arity=1

# Its body resolves the comparison primop and the integer literal inline.
# CHECK: OP_LIT_PRIMOP{{.*}}; primop __lessThan
# CHECK: OP_LIT_INT{{.*}}; = 0

# The conditional lowers to a BRANCH_FALSE with a symbolic jump target,
# the THEN arm's string, a JUMP over the ELSE arm, a target leader label,
# and the ELSE arm's string.
# CHECK: OP_BRANCH_FALSE{{.*}}; -> L{{[0-9]+}}
# CHECK: OP_LIT_STR{{.*}}; "pos"
# CHECK: OP_JUMP{{.*}}; -> L{{[0-9]+}}
# CHECK: L{{[0-9]+}}:
# CHECK: OP_LIT_STR{{.*}}; "neg"

# The top-level frame is labelled and builds the closure, naming the
# lifted function it points at.
# CHECK-LABEL: ; func {{[0-9]+}}{{.*}}(top-level)
# CHECK: OP_MAKE_CLOSURE{{.*}}; func "x"

# Every opcode must have a known mnemonic — a desync in the opName table
# or the opExtraWords instruction-width table would surface as OP_???.
# CHECK-NOT: OP_???
