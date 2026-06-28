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

CORRECTION (do not repeat the earlier error): TW is NOT compiled — it is a tree-walking
INTERPRETER (`eval(Expr*, Env&)` recursive descent). The honest framing is below.

## Why a bytecode VM loses to a tree-walker here (two distinct reasons)

Bytecode's advantages over tree-walking are (1) no AST re-traversal in loops, (2) flat
cache-friendly code, (3) fewer dispatches. For Nix: (1) doesn't apply (force-once graph
reduction; both walk once); (2) is small.

REASON A — v3's STACK bytecode dispatches MORE than the tree-walk, not fewer. The tree-walk
passes sub-results back in C++ return registers; the stack VM shuffles them through the value
stack with explicit GET/SET/PUSH ops (the 49%). So v3 throws away bytecode's (3) advantage. A
REGISTER VM would fold those into operands and reach ~dispatch-parity with the tree-walk —
this is the ~25% of v3 CPU that is dispatch (see the JIT ceiling).

REASON B (decisive) — even at ZERO dispatch, v3 is still 1.5–2.2× TW (the JIT ceiling, #137).
So v3's per-operation SEMANTIC work is ~2× the tree-walker's TOTAL work, independent of
stack-vs-register. That residual is:
  - the 8B NaN-box codec on every Value access (TW's 16B Value has a direct tag field);
  - the PRECISE MOVING-GC tax v3 pays per op/alloc — write barriers, nursery routing,
    safepoint polls — that TW's Boehm (conservative, non-moving) simply does NOT pay;
  - more/heavier allocations: separate thunk/pair cells + HAMT-node indirection vs TW's flat
    sorted Bindings (great for the small attrsets that dominate);
  - cppnix's TW being ~15 years tuned (tight switch dispatch, interned symbols, optimized Env).

## The reframe — v3 pays its RSS bets' CPU cost without the RSS benefit

The 8B NaN-box and the precise moving GC were taken on FOR RSS. v3 pays their CPU cost every
op/alloc. The campaign showed the RSS payoff never materialized (arena never returns pages;
all reclamation levers dead). So v3 currently carries the CPU COST of those bets WITHOUT the
RSS benefit — a core reason the bytecode VM ended up slower than the tree-walker.

## Implication for "stack vs register vs JIT"

- REGISTER VM: fixes Reason A (self-inflicted extra dispatch) → ~dispatch-parity, ~10–15%,
  lands ~2.1–2.2×. Big rewrite; STILL loses to TW because of Reason B.
- JIT: removes all dispatch but not Reason B → 1.5–2.2× (#137); only an OPTIMIZING JIT with
  register allocation removes the stack traffic too, and even then the semantic residual
  (alloc/GC-tax/structures) remains.
- The real lever is Reason B: the dispatch-free per-operation cost — the moving-GC tax + the
  representation. That is broad constant-factor work against a very tuned target, OR
  revisiting the RSS bets (is the moving-GC per-op tax worth paying when it buys no RSS?).
  Cheap CPU levers already explored (project_profile_at_scale): countDistinct memoize shipped
  −9–15%, superinstructions neutral, the rest document-closed.

NEXT MEASUREMENT (the actionable gate): decompose the WARM dispatch-free on-CPU residual —
alloc-path/barriers vs NaN-box vs attr-ops vs force machinery — to size how much of the
1.5–2.2× is the moving-GC tax (potentially revisitable) vs irreducible work.

## One-line answer

Two reasons, both vs a tree-walking INTERPRETER (not compiled): (A) v3's STACK bytecode
dispatches MORE than the tree-walk — ~60% of opcodes shuffle values through the value stack
that the tree-walk passes in C++ return registers (a register VM would fix this, ~dispatch
parity); and (B, decisive) even dispatch-free v3 is 1.5–2.2× TW, because its per-operation
semantic work is ~2× heavier — the 8B NaN-box codec + the precise moving-GC per-op/alloc tax
(barriers/nursery/safepoints that Boehm-TW never pays) + heavier structures (separate cells,
HAMT) + cppnix's 15-year tuning. Nix's force-once graph reduction also gives the bytecode no
loop re-walk to amortize against. v3 pays the CPU cost of its RSS bets (8B Value, moving GC)
without the RSS benefit the campaign showed never materialized.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
