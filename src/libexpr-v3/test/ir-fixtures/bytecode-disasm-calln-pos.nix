# RUN: v3-eval --file %s --emit-bytecode --no-opt | v3-check %s
#
# Regression guard for the OP_CALL_N / OP_TAIL_CALL_N resolved annotation
# in the bytecode disassembler (disasm.cc::disassembleOne).
#
# A saturated multi-argument application lowers to OP_CALL_N <argcount>
# (eval/apply; App-spine coalescing is unconditional).  Its operand is the arg
# count, not a slot/constant index, so the disassembler annotates it as
# "; N args" — otherwise a bare `operand=2` is ambiguous.  This fixture
# pins that annotation (and the binary lambda's arity framing).
#
# `--no-opt` because OP_CALL_N is emitted at lowering/compile, not by an
# optimiser pass — so this locks the disassembler, not the optimiser.

(a: b: a + b) 1 2

# The lambda is lifted with arity=2:
# CHECK-LABEL: ; func {{[0-9]+}} "a"{{.*}}arity=2

# The saturated 2-arg call carries its arg count.  Accept either the
# direct or tail form (the emitter may pick OP_TAIL_CALL_N in tail
# position) via the {{(TAIL_)?}} alternation:
# CHECK: OP_{{(TAIL_)?}}CALL_N{{.*}}; 2 args

# opName covers the whole call-N family — no unknown opcode leaks:
# CHECK-NOT: OP_???
