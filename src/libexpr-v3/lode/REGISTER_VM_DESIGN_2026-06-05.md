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

## Phase 5 — AUTHORIZED + UNDERWAY (2026-06-06)

**Approach (no mode flag):** a function runs stack-free when its emit produces
only register-form ops; add the ops one at a time, each `--core` byte-identical,
growing the stack-free-eligible function set. **Foundation infra LANDED:**
`getLocalPositions_` makes the multi-word-op emit scans robust (`02db677b1`).

**Landed so far:**
- `R_PRIMOP2` (`1010108ee`/`0f99dce3a`) — binary primops, slot→slot.
- `R_RETURN` (`03f304e90`) — return a slot. With R_PRIMOP2-on-tail, a
  **straight-line arithmetic function is now FULLY stack-free**:
  `map (x: x-1)`'s lambda = `OP_R_PRIMOP2 __sub r1=r0,#1; OP_R_RETURN r1` —
  zero operand-stack ops. Proven.

**Remaining ops to make BRANCHY+RECURSIVE functions (fib) stack-free** (each
its own `--core`-validated commit; the `getLocalPositions_` fix makes the
follow-up-word ops safe):
1. **`R_BRANCH_FALSE` target, cond_slot** — branch on `regs[cond_slot]`
   (force-writeback-to-slot if non-WHNF). `operand=target` (24-bit, so the
   existing compactFuseSetGet jump rebase works) + 1 follow-up `cond_slot`.
   Needs: `isBranchOp`+`opExtraWords(+1)` (disasm), serialize skip-1 (×4) +
   verifier, dispatch. Drops the If's `GET_LOCAL cond`.
2. **Register-mode `If` emit (branch-result-to-slot)** — allocate a result
   slot R for the If; each branch's tail writes R (R_PRIMOP2 dst=R, or a
   `R_MOVE src→R`); the merge/return reads R via R_RETURN. This is what lets a
   branchy function avoid leaving the value on the stack — the key coordinated
   emit change.
3. **`R_STR_CONCAT2` d, a, b** — fib's `+`. Synchronous like R_PRIMOP2 (force
   both slots, 2-int fast path else general concat, write d). Messy (string
   coercion/context, byte-identity-critical).
4. **`R_CALL` dst, callee_slot, arg_slot** — the crux (fib's recursion, async).
   Read callee+arg from slots; set up the callee frame; on return write the
   result to caller slot `dst` by REUSING the A8 force-writeback (the caller
   frame already supports "write the returned value to a slot offset" —
   `setForceWriteback`), with a non-re-entrant variant (don't re-scan after).
   This is the calling-convention change; the writeback machinery already
   exists, which is what makes it tractable rather than a full rewrite.
5. **`R_GET_UPVALUE_REC_BINDING` → slot** (fib resolves `fib`) and **`R_FORCE`
   slot** (force in place) — the remaining fib-body ops.

When 1–5 land, fib's `func "n"` emits entirely register-form ops → executes
with the operand stack dropped → the v3-beats-TW compute target. Order:
3 + 1 (tractable, synchronous/branch) → 2 (the coordinated If emit) → 4/5
(the calling-convention crux).

### LANDED (2026-06-06): `R_BRANCH_FALSE` (item 1) + `R_CALL` (item 4, the crux)

- **`R_BRANCH_FALSE`** (`ce96347c2`) — branch on `regs[cond_slot]`. fib's
  `n < 2` is now `R_PRIMOP2 __lessThan r1=r0,#2 ; R_BRANCH_FALSE if !r1 -> T`.

- **`R_CALL dst, callee_slot, arg_slot`** — the async crux, and it turned out
  TRACTABLE because the A8 writeback machinery already does "deliver a callee's
  return value into a caller slot." The shipped design:
  - **Phase 1 (callee force):** if `regs[callee_slot]` is a genuinely non-WHNF
    indirection (`Tag::Thunk`/`Tag::Slot`), force-writeback IT to its slot +
    re-execute (the R_PRIMOP2 arg-force pattern). This resolves a thunk-callee to
    its Closure on the iterative frame-push path rather than C-recursing.
  - **Phase 2 (the call):** plain single-arg user closures (`arity==1`,
    `!selectorSym`, `!identityLambda` — the deep-recursion case) take the
    iterative `op_call_dispatch` path with `setForceWriteback(caller, dst)` set
    **but NOT `CFF_FORCE_RETRY`** — so the callee's `OP_RETURN` →
    `applyForceWriteback` drops the result into `regs[dst]` and execution
    continues PAST the op (no re-exec). Every other callee shape (primop,
    selector/identity lambda, `__functor`, multi-arity PAP — all SHALLOW) runs
    synchronously via `callClosure` + a direct slot store, which sidesteps
    threading the writeback through op_call_dispatch's ~9 inline fast-path exits.
  - **Emit** (`tryEmitRCall`, the call analogue of `tryEmitRPrimop2`): fuses
    `vC = App(fun,arg)` + an OnceLinear `vF = Force(vC)` into one R_CALL writing
    vF's slot — dropping the arg GET, the result round-trip, and the FORCE.
    Operands must be slot-resident or the deferred stack TOP (peek-then-commit so
    a bail emits nothing). fib's recursive body went from
    `GET fib; SET_KEEP; R_PRIMOP2 r4; GET r4; CALL; FORCE; SET 6` to
    `GET fib; SET 3; R_PRIMOP2 r4; R_CALL r6=r3,r4`.

  **BUG FOUND + FIXED during validation (the lang suite earned its keep):**
  Phase 1 originally also forced `Tag::App`/`App3`. But a `Tag::App` is a PARTIAL
  APPLICATION (PAP) — WHNF for call purposes (OP_CALL's own handler applies it at
  `op_call_have_fun`, it does NOT iter_force it). Forcing a PAP re-enters
  op_force_slow on an already-WHNF value and, with the re-execute, SPINS FOREVER.
  Caught by `eval-okay-listtoattrs.nix` (lib `concat = fold f ""` — a PAP — used
  as a `+` operand): `--core` hung on it (16 closures, tight loop). Fix: Phase 1
  forces only `Thunk`/`Slot`; PAP callees fall through to Phase 2's `callClosure`.
  **Lesson:** `Tag::App` is dual-use (PAP vs deferred-call); never blanket-force
  it — mirror OP_CALL's PAP-vs-iter_force discriminator.

  Validation: lang 142/143 (the 1 fail = pre-existing `eval-okay-types`, identical
  ON/OFF), fib=55 with 2 R_CALL ops, smoke ALL PASS, IR-checks 29/29. Gate
  `NIX_V3_NO_R_CALL`.

Remaining for full fib stack-freedom: items 2 (register-mode If, branch-result-
to-slot), 3 (R_STR_CONCAT2 for the `+`), 5 (R_GET_UPVALUE_REC_BINDING→slot,
R_FORCE). The callee is still resolved via `GET_UPVALUE_REC_BINDING; SET` (item 5)
and the `+` still round-trips via `GET_LOCAL2; STR_CONCAT` (item 3).

### MILESTONE MEASUREMENT (2026-06-06) — register VM core is a real compute win

With R_PRIMOP2 + R_BRANCH_FALSE + R_CALL all landed (the three fundamental op
classes — arithmetic, branching, calls — now register-addressed), measured on
the PRODUCTION path (`nix eval --impure --expr` + `NIX_V3_DIRECT_EVAL=1`, the
gates `NIX_V3_NO_REG_PRIMOP2 / NO_R_BRANCH / NO_R_CALL` flip it OFF):

- **fib27 dispatch: 11,441,212 → 6,356,250 = −44.4%** (NIX_VM_OPCOUNTS, tail -1).
- **fib30 wall: 1.47× ± 0.19 faster** (hyperfine -N -w2 -r12: 1.514 s ± 0.046 ON
  vs 2.229 s ± 0.275 OFF; user-CPU 1.222 s vs 1.661 s = 1.36×).

This vindicates the register-VM thesis on compute-bound code: nearly HALF the
dispatch eliminated, ~1.47× wall. (Real overhead-dominated corpora — hello /
firefox drvPath — are defer/thunk/attrset-bound and see little of this, exactly
as the Phase-1 real-code finding predicted; the win is on compute kernels.)

Items 2/3/5 would shave the last ~7 stack ops/node (2× GET_UPVALUE+SET, the
GET_LOCAL2, STR_CONCAT, the branch return) for an estimated further ~20-30%
dispatch on fib — incremental polish on top of this landed milestone.

### COMPLETE (2026-06-06): items 2 + 3 landed → register VM architecturally whole

All major op classes are now register-addressable.  Landed on top of the above:
- **`R_STR_CONCAT2`** (`f052a88d2`, item 3) — register `+`/concat; reuses the
  whole OP_STR_CONCAT body via a case-head label + synthesised operand + the
  CFF_FORCE_WB writeback at the shared exit.
- **register-mode `If` + `R_MOVE`** (`6a20243ec`, item 2) — branch-result-to-
  slot: each branch's tail op writes the If's merge slot directly (via the new
  `dstOverride` on R_PRIMOP2/R_CALL/R_STR_CONCAT2, or `R_MOVE` for a var tail),
  so the merge holds the value in a register and a tail If fuses to `R_RETURN`.

fib's `func "n"` now runs **register-mode end to end**:
```
R_PRIMOP2 r1 = n<2 ; R_BRANCH_FALSE if !r1 -> else
  R_MOVE r2 = r0                                  (then)
else:
  GET_UPVALUE_REC_BINDING fib ; SET 3 ; R_PRIMOP2 r4 ; R_CALL r6 = r3,r4
  GET_UPVALUE_REC_BINDING fib ; SET 7 ; R_PRIMOP2 r8 ; R_CALL r10 = r7,r8
  R_STR_CONCAT2 r2 = r6 ++ r10
R_RETURN r2
```

**Final measurement (full register VM, production `nix eval` path):**
- **fib27 dispatch: 11,441,212 → 6,038,440 = −47.2%** (vs −44.4% R_CALL-only).
- **fib30 wall: 1.54× ± 0.10 faster** (1.372 s vs 2.112 s; user-CPU 1.34×).

**Op classes register-addressable:** compute (`R_PRIMOP2`), branch
(`R_BRANCH_FALSE`), branch-value-merge (register-mode If), call (`R_CALL`),
return (`R_RETURN`), string-concat/`+` (`R_STR_CONCAT2`), slot copy (`R_MOVE`),
dual-load (`GET_LOCAL2`).

### ITEM 5a LANDED (2026-06-06) → fib runs with the operand stack FULLY dropped

`OP_GET_UPVALUE_REC_BINDING_SLOT` (`<this commit>`) — the register-result form of
GET_UPVALUE_REC_BINDING: resolve the captured rec-attrset upvalue's `name` slot
and write the Tag::Slot straight into a LOCAL slot (no push + SET).
operand = SymbolId (remapped like GET_UPVALUE_REC_BINDING); 3 follow-ups
[dst, upvalIdx, icIdx].  Emitted (tryEmitRecBindToSlot) for a RecBindingSlotRef
whose var is used as a call callee (the `appFunVars` set) and whose source is a
captured upvalue — the fib self-resolution shape — dropping the materialising
SET that tryEmitRCall would emit.  Done as a DIRECT emit (no instruction-removing
peephole, no position shift); the serializer symbol-remap is mirrored from
GET_UPVALUE_REC_BINDING with skip-3 at all 4 walk sites + the verifier, and
r1-trigger-verify (270/270 byte-match) confirms it.

fib's `func "n"` now has **ZERO operand-stack ops** — every instruction is
register-addressed:
```
R_PRIMOP2 r1 = n<2 ; R_BRANCH_FALSE if !r1 -> else
  R_MOVE r2 = r0
else:
  GET_UPVALUE_REC_BINDING_SLOT r3 = fib ; R_PRIMOP2 r4 ; R_CALL r6 = r3,r4
  GET_UPVALUE_REC_BINDING_SLOT r7 = fib ; R_PRIMOP2 r8 ; R_CALL r10 = r7,r8
  R_STR_CONCAT2 r2 = r6 ++ r10
R_RETURN r2
```

**Final register VM measurement (production `nix eval`):**
- **fib27 dispatch: 11,441,212 → 5,402,820 = −52.8%** (more than HALF eliminated;
  item 5a added −10.5% over the −47.2% from the rest by dropping the 2 SETs/node).
- fib30 wall 1.36–1.54× faster (the stack baseline is noisy on this host, per the
  user; dispatch is the deterministic metric).

The register VM is now COMPLETE end to end: every opcode class in a compute
kernel is register-addressable and the canonical recursive function executes with
the operand stack entirely dropped.  Gate `NIX_V3_NO_RBSR_SLOT`.

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
