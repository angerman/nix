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

## Phase 1 RESULT (2026-06-05) + the real-code ceiling

`OP_R_PRIMOP2` (binary primop, fixed-arity-2 — the dominant case) is LANDED
(`1010108ee` + relaxed `0f99dce3a`), `--core` 19/19 byte-identical (incl. r1
cache). fib's three binary primops are register-addressed
(`__lessThan r1=r0,#2`, `__sub r4=r0,#1`, `__sub r8=r0,#2`); **fib27 total
dispatch −22.2%** (11.44M→8.90M, CALL_PRIMOP 1.27M→0). Wall is noise-bound on
this host but user-CPU is directionally lower.

**Real-code finding (load-bearing for the remaining phases).** R_PRIMOP2
fires **~0 on hello** (real nixpkgs): binary-primop operands there are almost
always **deferred** — the #542 mechanism keeps OnceLinear values on the
operand stack and consumes them via fast paths, so they are NOT in slots, and
register-addressing (slot reads) does not apply. The register-form synchronous
ops (R_PRIMOP2, and R_STR_CONCAT etc. to follow) therefore help **slotted**
operands (params / Many-use) — fib/compute's pattern — but the real-nixpkgs
operand mix is defer-resident, so per-op register forms are largely a
fib/compute win. **The real-code prize is structural: Phase 4 (a register
allocator that slots everything and supersedes the stack/defer model) + Phase
5 (drop the operand stack)** — not the op-by-op hybrid. Until then the
register layer compounds on compute-heavy / fib-like evals (the v3-beats-TW
target there), and is dormant-but-correct on real nixpkgs.

`OP_CALL` is the next big fib lever (14.3% post-Phase-1) but is **async** (the
callee runs in a new frame; the result returns via OP_RETURN), so a register
form must thread the dst slot through the return — a calling-convention change
(part of Phase 3/4), unlike the synchronous R_PRIMOP2.

## Continuation ROI (assessed 2026-06-05) — why Phase 1 is the validated stop

After Phase 1, every incremental step was assessed; all are multi-week (the
real prize) or low-ROI in-session:
- **More synchronous register ops** (R_STR_CONCAT for fib's `+`, R_PRIMOP1,
  n-ary R_CALL_PRIMOP): fib-specific (defer-resident on real code per the
  finding), wall unmeasurable on this host, and STR_CONCAT in particular is
  messy (int-fast-path + string-coercion + context + result-to-slot, all
  byte-identity-critical) for ~3.6% of fib dispatch. Low ROI.
- **Phase 4 (slot-reuse allocator):** shrinks `nLocals`, but frames live on
  the `valueStack` (depth × nLocals × 16 B) — **tiny vs the 520 MB arena**, so
  the memory benefit is negligible; and it is correctness-critical (reuse a
  live slot ⇒ silent corruption) + must interact with defer/R_PRIMOP2/upvalue.
  Negligible ROI.
- **Phase 3 async R_CALL / Phase 5 drop-stack:** the genuine real-code prize,
  but a **multi-week structural rewrite** (calling-convention change /
  full-register emit+dispatch + the allocator). Cannot be landed
  incrementally `--core`-green in-session.

**Verdict:** Phase 1 (R_PRIMOP2) is the validated foundational milestone — the
register VM technique implemented, working, measured (−22.2% fib dispatch),
byte-identical. The full register VM (real-code wall payoff) is a deliberate
multi-week Phase-5 investment, and the fib-specificity finding above is the
measure-twice input for whether to make it. Resume here (Phase 5, per-function
register-mode hybrid for incremental validation) when that investment is
authorized.

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
