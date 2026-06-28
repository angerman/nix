# Why is the v3 bytecode VM slower than the tree-walker on EVAL? (2026-06-28)

The natural intuition — "a bytecode VM should beat a tree-walker" — is correct in general
and wrong for THIS workload. This documents why, with the fresh opcode histogram.

## The data — firefox.drvPath eval, 36,864,727 opcodes (NIX_VM_OPCOUNTS, HEAD 35d9a0276)

```
OP_GET_UPVALUE                 15.92%   variable read
OP_SET_LOCAL                   10.73%   variable write
OP_GET_LOCAL                    9.70%   variable read
OP_RETURN                       7.82%   frame management
OP_MAKE_THUNK                   7.81%   laziness (both VMs pay)
OP_GET_LOCAL2                   7.59%   variable read
OP_BRANCH_FALSE                 3.66%   control flow
OP_ATTRS_SELECT                 2.54%   real work
OP_SET_LOCAL_KEEP               2.49%   variable write
OP_GET_UPVALUE_REC_BINDING_SLOT 2.34%   variable read
OP_CALL_PRIMOP                  2.14%   real work
OP_TAIL_CALL_N                  1.99%   real work
AttrSelect family (total)       5.12%   real work
```

**~49% of all opcodes are pure variable shuffling** (get/set local + upvalue); **~60%** is
plumbing once RETURN + branches are added. Only **~25%** is actual semantic work (thunk,
select, call, primop).

## Why the bytecode advantage doesn't materialize

The classic "bytecode beats tree-walking" win is: a tree-walker RE-TRAVERSES the AST every
time it evaluates a node, and in tight LOOPS that re-walk dominates; bytecode is a flat array
walked without re-traversal.

**Nix eval has no such loops.** It is lazy graph reduction: each thunk is forced ~once (the
per-thunk `forces` counter ≈ 1). A thunk's bytecode is walked once — exactly like the
tree-walker walks that AST subtree once. There is NO re-traversal to amortize, so the
bytecode's primary advantage never appears.

## What v3 PAYS that TW does not

1. **The stack machine linearizes eval into micro-ops.** `let y = e; in y.a` →
   SET_LOCAL/GET_LOCAL/GET_UPVALUE/ATTRS_SELECT. A tree-walker accesses those variables as
   `env->values[i]` — an array index the C++ compiler keeps in a REGISTER, inlined, ~free.
   v3 turns each into a DISPATCHED opcode: fetch byte → decode operand → switch/goto →
   push/pop the value stack. That is the 49% above: ~18M dispatched moves vs ~free register
   reads in TW.
2. **Per-opcode dispatch.** TW's "dispatch" is C++ recursion + a switch on Expr type that the
   compiler inlines and the CPU predicts far better than a ~50-way interpreter switch.
3. **Thunk force = dispatch-loop re-entry** (push frame, run bytecode). TW's force is a plain
   recursive `forceValue` C++ call.
4. **8B NaN-boxed Value** costs encode/decode (tag extract + pointer untag) per access; TW's
   16B Value has a direct tag+union. The RSS-for-CPU trade Lever B made (+1–4% wall, baked).

So for the SAME evaluation v3 executes 36.86M dispatched stack-ops where TW does the
equivalent in native C++ control flow with values in registers. The interpreter's
per-operation constant factor is higher than tuned recursive-descent C++, and graph reduction
gives it no loop to amortize against. **This is architectural, not a bug** — the cheap CPU
levers (countDistinct memoize, superinstructions, dispatch tweaks) were already explored
(project_profile_at_scale): countDistinct shipped −9–15%, superinstructions neutral, the rest
document-closed.

## Why this is also the JIT ceiling

A NAIVE JIT removes #2 (dispatch) but its generated code still performs all the stack traffic
from #1 (SET_LOCAL/GET_LOCAL → `mov`s) + alloc + real work → the measured 2.5×→~1.5–2.2×
(#137). To actually beat TW you need an OPTIMIZING JIT with REGISTER ALLOCATION that
eliminates the stack shuffles entirely — keeping values in registers the way TW's compiled
C++ already does. That register-allocating codegen is the hard, multi-week part, and it is
the real reason "beat TW on CPU" = native codegen, not a faster interpreter.

## One-line answer

v3 is slower because a stack-based bytecode interpreter spends ~60% of its opcodes shuffling
values through a dispatched stack machine that a tree-walker does for free in C++
registers/recursion — and Nix's force-once graph reduction offers no loop re-walk for the
bytecode to amortize that overhead against.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
