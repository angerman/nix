# What else (other than JIT)? — profile-grounded lever map — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

Profile at HEAD 7d0cc790a (post-HAMT-removal), darwin-4. Production warm gap:
firefox 2.46× CPU / 1.64× RSS; M5 1.82× CPU / 2.26× RSS.

## The two framing corrections this profile forces

1. **v3's `Value` is already 8 bytes (NaN-boxed) — *smaller* than the tree-walker's
   16 B niche-tagged Value** (`value.hh:261 static_assert sizeof(Value)==8`). "Match
   TW's value representation" is NOT a lever; v3 already won that. The RSS gap is in
   the *container* cells (Thunk 24 B, Closure 32 B, ValuePair 32 B, Bindings::Entry
   16 B = name 4 + pos 4 + value 8) and — far more — in **non-reclamation**.

2. **The CPU profile is dominated by data movement, not computation.** Executed-opcode
   histogram (exact, host-independent):

   | opcode | firefox | M5 | what it is |
   |---|---|---|---|
   | OP_GET_UPVALUE | 15.9% | 13.4% | load captured var (3–4 loads: closure→upvalEnv→env→idx) |
   | OP_SET_LOCAL | 10.7% | 10.5% | store TOS → slot |
   | OP_GET_LOCAL | 9.7% | 10.3% | load slot → stack |
   | OP_GET_LOCAL2 | 7.6% | 6.2% | fused two-slot load |
   | OP_RETURN | 7.8% | 7.5% | |
   | OP_MAKE_THUNK | 7.8% | 7.4% | allocate a thunk |
   | OP_ATTRS_SELECT | 2.5% | 7.4% | |

   **~44% of all dispatched opcodes are local/upvalue load-store shuffling** — pure
   operand movement a register machine (or a tree-walker's inline env-index) would not
   dispatch on. Churn: 65.7% (ff) / 65.4% (M5) of allocated thunks are never forced;
   nursery hit-rate 21.9% (ff) / 7.3% (M5).

## CPU levers (non-JIT), ranked by clean-ceiling

1. **Computed-goto / threaded dispatch — UNBUILT, clean, ~10–30% of dispatch
   (≈1–7% wall).** The dispatch loop is still a plain `switch(op)` (vm.cc:4454); the
   code comments call computed-goto "the bigger lever, comes later once the opcode set
   is stable." This is the single cleanest unbuilt CPU win. Only blocker: the opcode
   set is still churning (register-VM phases keep adding ops) — freeze it first.

2. **OP_GET_UPVALUE inline cache / restructure — UNBUILT, ~3–8%.** It's the #1 opcode
   (15.9%) and costs 3–4 dependent loads (closure → upvalEnv-check → env ptr → array
   index); env-sharing (C3) *added* the 2nd load. No inline cache today. Medium effort,
   orthogonal to dispatch.

3. **Register-VM completion — PARTIALLY SHIPPED, STUCK at +1.4% wall.** A register/
   operand-folding VM (phases 1–5b: R_PRIMOP2, R_CALL, R_RETURN, …) already exists and
   targets exactly the 44% move-ops, but real nixpkgs uses *deferred evaluation* so
   operands live on the operand-stack, not in slots → slot-folding rarely engages
   (−47% dispatch on fib, +1.4% on real code). Unlocking it = reworking the
   deferred-operand model. Highest CPU ceiling, multi-week, uncertain ROI.

4. **Cross-branch strictness — reduces OP_MAKE_THUNK (7.8%) + ALLOC.** The strictness
   pass (opt_func_strictness + opt_strict_call_unthunk, default-on) stops at branches
   (If/With/Assert) — no lattice join. But the 62–67% unforced churn is *measured to be
   99.3% genuine deferred work* (M4 RCA): only 0.7% is eliminable alias/const, already
   removed at compile time. So the headroom is a hard cross-branch demand lattice (or
   speculative-eval+deopt) for a modest, uncertain slice. Low ceiling for high effort.

## RSS levers (non-JIT), ranked

1. **Mid-eval *compacting* GC — the real RSS lever, blocked on safepoints.** v3's RSS
   gap is not cell size (8 B value); it is that the arena **never reclaims tenured
   cells** (TW's Boehm is stop-the-world and reclaims). Every reclamation lever was
   killed this campaign (#134 import-cache, #136 page-release, mid-eval reuse) for ONE
   root cause: the moving collector is gated to `exitDepth==0` because it can't relocate
   cells pinned by nested dispatch-loop C-stack frames, and the non-moving sweep can't
   free whole blocks (25–75%-live fragmentation). **To reclaim mid-eval you need precise
   stack maps / GC safepoints so the collector can run (and move) during deep eval.**

2. **Fewer cells** — strictness (thunks; low ceiling, see CPU #4), and ValuePair 32→16 B
   (FP-3, ~25 MB M5, high semantic risk — App-memo drop) / kAlign 16→8 (thunk 24 B
   stops rounding up to 32 B, ~130 MB M5 but doubles GC bitmap metadata = ~break-even).
   All small or risky — consistent with the closed campaign RSS sweep.

## The convergence (the load-bearing conclusion)

**The highest-ceiling lever on EACH axis converges on the same missing primitive —
precise stack maps / GC safepoints across the nested dispatch loop:**
- RSS: safepoints let a compacting collector run mid-eval → actually reclaim (the only
  path past the arena-never-frees wall).
- CPU: safepoints are exactly the J3 prerequisite native codegen needs to spill live v3
  pointers at allocation points.

So "beat TW" is gated on one foundational capability that unlocks both the RSS reclaim
AND the CPU codegen. That is the same conclusion the campaign reached ("fundamental
change, not a single lever"), now pinned to a concrete primitive.

**Short of that primitive, the only clean, low-risk, non-JIT wins are CPU #1
(computed-goto, ~1–7% wall) and CPU #2 (upvalue inline cache, ~2–4%).** Stacking them
narrows firefox ~2.46× → ~2.2–2.3×; it does not beat TW. Everything with a real
ceiling (register-VM completion, compacting GC, native codegen) needs the safepoint
foundation.

## Recommendation

If the goal is "ship a measurable warm CPU win cheaply": **computed-goto dispatch**
(freeze the opcode set first) + **GET_UPVALUE inline cache**. Both are well-understood,
gate-able, byte-id-preserving, and independently shippable.

If the goal is "beat TW": the prerequisite investment is **precise stack maps /
safepoints across the dispatch loop** — the J3 work — because it is the shared unlock
for both a compacting mid-eval GC (RSS) and native codegen (CPU). It is the one piece
of "fundamental change" that pays on both axes. (This is why JIT keeps surfacing: not
the codegen itself, but the safepoint substrate it forces you to build.)
