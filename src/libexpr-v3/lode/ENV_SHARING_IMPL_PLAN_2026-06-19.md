# Environment-sharing implementation plan (2026-06-19, user-directed)

Goal: cut the per-closure/thunk upvalue-copy allocation (TW shares one `Env` per
scope; v3 copies upvalues into each `Closure`/`Thunk` FAM). The #1 profile leaf
(`forceValue`) feeds on these fat objects. Supersedes the ES-1..4 falsifier's
DEFER (memory side 2.9%) — now pursuing the CPU side, measurable on darwin-4.

## Current representation (the thing being changed)

- `Closure` (closure.hh:60): `{desc, cu, capturedWiths, nUpvalues, _pad, upvalues[] FAM}`
  — upvalues copied inline at MAKE_CLOSURE.
- `Thunk` (closure.hh:90): `{state, hasWithsSlot, …, tail[] FAM}` — upvalues in
  tail[0..nUpvalues), copied inline at MAKE_THUNK; +optional withs slot.
- `GET_UPVALUE` (vm.cc:3916): `push(closure->upvalues[operand])` — hot, 14.5% of git ops.
- Thunk-force (vm.cc:8209): allocFakeClo + copy t->tail[]→fakeClo->upvalues[] per force.

## HONEST payoff risk (build with eyes open)

Memory: ES-1 measured 12.1 MB recoverable on firefox = 2.9% arena (below 5% bar).
CPU: the upvalue *copy* itself is ~2 ms (≈18 MB of word-copies on git) — negligible.
The real CPU hope is reduced **alloc + GC-scan** from leaner objects, but the
nursery makes alloc bump-cheap and the live-byte reduction is ~2.9%. **So env-
sharing may land sub-bar on both axes.** Stage 1 is built to MEASURE this before
the high-risk GC stages — the honest off-ramp.

## CORRECTION (2026-06-19, accurate scoping): the de-risk is PARTIAL; the hard part is UPFRONT

The v3 `Env` type is **VESTIGIAL** — `allocEnv` has ZERO callers (only the decl +
docstring), `envsAllocated`=0 at runtime, `OP_ENTER_LET`/`OP_PUSH_WITH` don't exist
in vm.cc (the closure.hh comment describes an intended design never wired). What
EXISTS: the major-GC MARK handles `CellType::Env` (mark_sweep.cc). What does NOT:
the **nursery SCAVENGER has no `GK_ENV` walker** (gc.cc has GK_CLOSURE/GK_THUNK/…
only) and there's no Env remembered-set barrier. CRUCIAL CONSEQUENCE: the nursery
is now MANDATORY (its opt-out retired), so a tenured upvalue-Env holding
nursery-payload upvalues would have its nursery pointers go stale at scavenge → UAF
(the PhD-6 class). There is **no way to run/measure even a "non-moving" Env stage
under the unavoidable nursery** without first building the scavenger Env support.
∴ env-sharing's MINIMAL RUNNABLE UNIT = representation rework + MAKE_CLOSURE/
MAKE_THUNK + GET_UPVALUE + **`GK_ENV` scavenger walker (mirror walkClosure) +
envPostConstructBarrier (mirror closurePostConstructBarrier)** — i.e. the
highest-UAF-risk moving-GC work is REQUIRED UPFRONT, not deferrable to a later
stage. This is the honest reason env-sharing is a multi-day careful effort with no
safe runnable partial: the mandatory nursery forces the PhD-6-class barrier work
into stage 1. (The earlier "stage 2 largely done" claim was wrong — only the
major-mark side existed; the moving/scavenge side does not.)

## (superseded) KEY DE-RISKER (found 2026-06-19): the Env GC infrastructure already exists

v3 already has `struct Env { Env* parent; bool isWithEnv; uint16_t nValues; Value
values[]; }` (closure.hh:46) — currently used ONLY for let/with scopes (OP_ENTER_LET
/OP_PUSH_WITH/OP_INHERIT_FROM_INIT), NOT closure upvalues (the comment states the
design chose inline-FAM upvalues for closures/thunks deliberately). Crucially the
**GC plumbing for an Env-holding-a-Value[]-FAM is already built + battle-tested**:
CellType::Env (alloc.hh:1012), allocEnv (alloc.hh:2618), and the mark-sweep + Cheney
walkers handle CellType::Env (mark_sweep.cc:334/474/1019/1329/1533/1791, gc.cc:1511).
⇒ stage 2's "make Env a moving-GC object" is LARGELY DONE — env-sharing reuses the
existing traced Env type instead of introducing a new one. The remaining work is the
REPRESENTATION rework (closures/thunks reference an upvalue-Env instead of inline FAM)
+ MAKE_CLOSURE/MAKE_THUNK construction + GET_UPVALUE access + the build/share logic.
This materially lowers the risk + effort of the original blueprint.

## Staging (each stage: byte-identical + --brute-clean + gated)

**Stage 1 (ES-IMPL-1) — NON-MOVING Env + measure (the off-ramp gate).**
- New `Env` heap type: `{nValues, Value values[] FAM}` (one per scope's captures),
  interned/shared where capture-sets coincide.
- `Closure`/`Thunk` gain an `Env*` reference (gated NIX_V3_ENV_SHARING=1); upvalues
  read via `env->values[idx]` instead of the inline FAM.
- MAKE_CLOSURE/MAKE_THUNK: build-or-share the Env instead of copying the FAM.
- GET_UPVALUE: `push(closure->env->values[operand])` under the gate (a branch on
  the hot path — measure its cost; if the branch hurts, use a separate opcode or
  a per-closure flag-free representation).
- Thunk-force: fakeClo references t's Env directly (no per-force copy).
- **Decision point:** measure darwin-4 CPU (git/firefox cache-off, on vs off) +
  arena. If sub-bar even non-moving → STOP (don't fund the moving-GC risk).
- Lower-risk variant if full scope-Env is too invasive: **tuple-interning**
  (hash-cons identical upvalue tuples — the 40%/12 MB the H-probe found).

**Stage 2 (ES-IMPL-2) — moving-GC integration (highest UAF risk).**
- `Env` becomes a first-class moving heap object: precise-root traced, Cheney-
  forwarded, Phase-D barriered (intergenerational pointers into/out of Envs via
  the remembered set / standaloneCellRoots — the PhD-6 missed-root class).
- Add `walkEnv` to gc.cc; AUDIT + --brute clean at every step; V3_DBG_GC_STRESS.

**Stage 3 (ES-IMPL-3) — grade + flip.** darwin-4 CPU+arena, byte-id, full --brute,
nixpkgs byte-equality sweep; provisional → soak → flip (gen-major discipline).

## CLOSURE vs THUNK target (found mid-increment-2, 2026-06-19)

The directive ("the #1-leaf forceValue feeds on it") points at THUNKS, not closures:
`forceValue` forces thunks, and the per-force cost is the fakeClo copy of `t->tail[]`
(vm.cc:8241) + the MAKE_THUNK construction copy (896K thunks on git). CLOSURE
env-sharing (increment-1's `Closure.upvalEnv` foundation, ~84K closures) addresses
MAKE_CLOSURE alloc — a smaller, *different* lever that does NOT touch forceValue.
So the directive-relevant work is THUNK env-sharing (harder: the thunk tail layout +
the fakeClo force path). Increment-1 (committed, validated) is the closure-side
foundation; the forceValue payoff needs the thunk-side equivalent.

REMAINING WORK (both sides multi-day):
- Closure increment-2: ~15 `closure->upvalues[]` reader sites (vm.cc 3988/4063/4072/
  4655/5753-5803/10546/10616/14240-14272) each need the `upvalEnv ? env->values[i] :
  upvalues[i]` null-check (a `closureUpvalue(c,i)` helper centralizes it); + walkEnv +
  walkClosure-walks-upvalEnv + envPostConstructBarrier + DirtyKind::Env + the two
  dirty-set switches (gc.cc 1017/1411) + mark_sweep closure→Env mark + gated
  MAKE_CLOSURE Env-build. Miss any reader → wrong value under gate-on.
- Thunk env-sharing (the forceValue target): the analogous rework on Thunk + the
  fakeClo force path — the higher-payoff, harder half.

## CLOSURE functional layer SHIPPED (increment-2b, 2026-06-19, commit 9ada0703e)

Done + validated + committed. Gated NIX_V3_ENV_SHARING (default-off). Full
moving-GC integration: GK_ENV + walkEnv (gc.cc), DirtyKind::Env +
envPostConstructBarrier + env-aware closurePostConstructBarrier (barrier.hh),
mark+evac+auditor (mark_sweep.cc), all readers routed (vm.cc). hello.drvPath
byte-id TW==off==on (envs=0.99MB → path fires); lang 143/143 + parity 20/20;
--brute 21/22 ZERO missed-root hits (sole fail = stale firefox golden, NOT us).
**HONEST: NOT a perf win yet** — bring-up builds 1 fresh Env/closure AND keeps
allocClosure(nUp) (FAM poisoned to Uninitialized so brute-scan stays clean) →
MORE memory. The real win = the FOLLOW-UP: (a) Env INTERNING (hash-cons
upvalue-tuples; multiple closures share one Env — MUST add shared-Env evac
dedup, see below) + (b) allocClosure(0) (drop the FAM, keep nUpvalues as the
logical count) → then darwin-4 CPU/arena grade.

## THUNK HALF — design constraints discovered (2026-06-19, before impl)

The thunk half is the actual forceValue lever, and it REUSES the Env infra
(GK_ENV/walkEnv/DirtyKind::Env already built): the fakeClo IS a Closure, so the
per-force win is simply `fakeClo->upvalEnv = t->upvalEnv` (share, no per-force
tail[]-copy) instead of vm.cc:8241/13799's `for i: fakeClo->upvalues[i]=t->tail[i]`.
BUT two hard constraints make it harder than the closure half:

1. **The 24B Thunk header is SACRED.** FP-2 (8cd42314d) shrank it 40→24B for
   −285 MB on M5 — the campaign's biggest realized memory win. A new
   `Thunk::upvalEnv` field (+8 B/thunk, even gate-off) would partially UNDO that.
   ∴ the Env reference must live in the `tail[]` FAM (like FP-2b relocated
   capturedWiths to a tail slot via thunkScanSize()) or be otherwise
   gate-conditional — NOT a new always-present header field. This needs careful
   tail-layout design (tail = upvalues[0..nUp) + optional withs slot + now
   optionally an Env ref), updating thunkScanSize()/fwdThunk/walkThunk/mark.

2. **fakeClo + thunk SHARE the Env** → the shared-Env evac double-visit problem
   (deferred for closures because bring-up is 1-Env-1-closure) becomes
   MANDATORY. When `fakeClo->upvalEnv = t->upvalEnv`, BOTH the thunk and the
   fakeClo reference one Env; the major-GC evac inline-visit (mark_sweep.cc
   Closure case: `for i visitValue(e->values[i])`) would double-forward. FIX:
   make the major evac walk a shared Env via mark-bit dedup (enqueue the Env on
   a worklist keyed by its mark bit, OR mark-guard the inline visit), not the
   current unconditional inline visit. The minor scavenge already dedups (walked
   set in walkClosure). This same dedup is the prerequisite for closure-side
   INTERNING, so do it once for both.

Staging suggestion for the thunk half (each byte-id + --brute, gated):
  (T-a) shared-Env evac dedup (mark_sweep) — enables sharing for BOTH closures
        (interning) and thunks; validate closures still byte-id/--brute clean.
  (T-b) Thunk tail-layout: add a gate-conditional Env ref in tail[] +
        thunkScanSize()/fwdThunk/walkThunk/mark-evac updates (header stays 24B).
  (T-c) MAKE_THUNK builds the Env (gated); thunk upvalue readers routed.
  (T-d) fakeClo force path shares it: `fakeClo->upvalEnv = t->upvalEnv` (the
        forceValue win — no per-force copy); darwin-4 CPU/arena grade.

## JIT (goal item 3) — months-long subsystem, design+spike not single-turn

Per reference_v3_vs_tw_structural_2026-06-19: JIT of hot lib.* bodies is the
only raw-CPU lever (the 4.5× gap is the interpreter machinery), but it's a
multi-month subsystem: native codegen + register allocation + calling
convention + GC safepoints + a byte-identity bail path. The right first step is
a feasibility/design doc + a tiny spike (JIT one trivial hot body, measure), NOT
a single-session implementation. See PERF_STRATEGY_2026-05-17 §Stage-12.

## Byte-identity strategy

A captured value is the same whether read from an inline FAM or a shared Env at
the same index — the lowering already assigns upvalue indices, so the Env just
relocates the storage. The gate (NIX_V3_ENV_SHARING) makes every stage A/B-able
against the inline-FAM path.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0*
