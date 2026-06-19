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

## Byte-identity strategy

A captured value is the same whether read from an inline FAM or a shared Env at
the same index — the lowering already assigns upvalue indices, so the Env just
relocates the storage. The gate (NIX_V3_ENV_SHARING) makes every stage A/B-able
against the inline-FAM path.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0*
