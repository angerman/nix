# v3 register VM — implementation design (2026-06-05)

**Status:** ACTIVE IMPLEMENTATION. Lever 1C of [[WALL_OPTIMIZATION_PLAN_2026-06-05]].
The contained codegen levers (§2/Lever-2, 1A-via-defer, 1B-lite) are
wall-neutral on real workloads; the register VM is the **structural** lever
that removes the 51%-of-dispatch stack-motion no peephole touches. Goal vs TW:
**below 1×** (a bytecode VM should beat a tree-walker).

## The model

Today every op communicates through the **operand stack**: `GET_LOCAL a;
GET_LOCAL b; CALL_PRIMOP p; SET_LOCAL d` is 5 dispatches + 2 pushes + 2 pops +
1 push + 1 pop to compute `d = p(a,b)`. The A-normal-form lowering names every
subexpression, so this repeats for **every** node — 51% of all dispatch.

The register VM observes that **the per-frame local-slot region
`valueStack[stackBase .. stackBase+nLocals)` already IS a register file**.
A 3-address op reads its operands directly from slots and writes its result
to a destination slot, with **no operand-stack round-trip**:
`R_<op> d, a, b  ⟹  regs[d] = op(regs[a], regs[b])`. That collapses the
5-dispatch sequence to **one**.

Crucially, A8's `setForceWriteback(frame, off)` already forces a value and
writes the result back to **slot `off`** (relative to `stackBase`) — the exact
primitive a register-addressed strict op needs. So strict-arg forcing reuses
the existing iterative-force machinery: push the unforced slot value, set the
writeback to the arg's slot offset, rewind to the op, `goto op_force_slow`;
on re-entry the slot holds the forced value and the op re-scans. The HOT path
(args already WHNF — the common case after strictness) touches no stack.

## Phases (each lands `--core` 19/19 byte-identical + IR-checks + r1 cache)

- **Phase 1 — `OP_R_CALL_PRIMOP` (register-addressed primop call).** The
  clearest "compute" op and the cleanest A8 reuse. Reads N args from slots or
  inline immediates, invokes the primop, writes the result to a dst slot.
  Encoding: `operand = poIdx`; `word1 = (nArgs<<24)|dst`; then `nArgs` arg
  descriptors — `bit31` set ⇒ inline signed immediate (small ints, covers
  fib's `n-1`/`n<2`), else a 24-bit slot index. Emitted when a binding
  `d = PrimOpCall(po, args)` has all args slot-resident or literal and `d`
  gets a slot. Falls back to stack `CALL_PRIMOP` otherwise.
- **Phase 2 — register-form typed binary/unary ops** (`R_ADD`/`R_SUB`/…/
  `R_STR_CONCAT`/`R_NOT`) on the same encoding. (Real code routes arithmetic
  through `CALL_PRIMOP`, so Phase 1 covers most; Phase 2 is the typed-op tail.)
- **Phase 3 — register-form `CALL`** (callee + args from slots, result to a
  slot) — removes the call-site `GET;GET;CALL;SET`.
- **Phase 4 — slot/register allocator.** Linear-scan over `analyze-operands.py`
  R1 liveness: reuse dead slots (mean pressure 1.78 vs declared nLocals 3.5),
  spill the rare high-pressure function (8–16 regs cover 99.4–99.8%). Shrinks
  frames (memory) and bounds the register-form operand width.
- **Phase 5 — drop the operand stack for fully-register functions** (the
  structural endgame; only after 1–4 measure positive).

## Encoding & width (table-driven disasm + serializer)

Every new opcode needs: `bytecode.hh` def; `vm.cc` dispatch; `disasm.cc`
`opName` + `opExtraWords` (variable `1+nArgs` for `R_CALL_PRIMOP`) + a resolved
annotation; and — because the operands are slots/poIdx/immediates, NOT
SymbolIds — the `serialize.cc` walks must **skip** the follow-up words but must
NOT remap them, and the `primops.cc` CU-verifier must advance identically.
`r1-trigger-verify` (the cold-vs-warm cache round-trip) is the guard that
catches any serializer-width gap (it caught the §2(b) gap on the first run).

## Gates (per phase, pre-committed)

`--core` 19/19 byte-identical to TW; IR-checks; smoke; differential ON/OFF
identical; and a **dynamic dispatch drop** measured on hello + fib (production
path). Each register-form op family is gated `NIX_V3_NO_REG_<X>=1` (default-ON
A/B bisect). Wall is measured but is NOT the per-phase gate (real-workload wall
is overhead-dominated; the structural win compounds across phases and shows on
compute-heavy / fib-like evals — the v3-beats-TW target).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
