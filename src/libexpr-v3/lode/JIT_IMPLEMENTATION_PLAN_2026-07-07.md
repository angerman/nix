# Copy-patch JIT — phased implementation plan (2026-07-07)

Phase-0 deliverable: the concrete, HEAD-grounded (`b500141b7`) build plan for the
copy-patch body JIT, plus a Rule-0 falsifier spike that de-risks the ONE remaining
uncertainty the prior J0–J3 spikes left open (perf-honesty of the stencil chain
including the safepoint spill/reload tax).

**This supersedes nothing** — it is the executable refinement of
`C4_JIT_BUILD_PLAN_2026-06-23.md` (#152–154) + `JIT_DESIGN_2026-06-19.md` (J0–J4)
+ `WARM_EVAL_JIT_SCOPING_2026-06-29.md`, with exact `vm.cc` hook lines and a
pre-committed per-phase SHIP/KILL gate.

## 0. Bottom line up front (the honest go/no-go)

- **Mechanism risk: RETIRED.** J0 (MAP_JIT/W^X), J1 (encoder), J2 (NaN-box byte-id
  + bail), J3 (spill/reload-across-move contract) were already proven standalone.
  This Phase-0's new spike (`research/jit_copypatch_bench.cc`) closes the last open
  one: a STITCHED copy-patch stencil chain that calls back into the runtime AND
  pays the J3 spill/reload discipline at an allocation safepoint is (a) byte-
  identical to the interpreter, (b) survives a real in-place-forwarding move, and
  (c) is directionally faster than switch dispatch. **All three PASS.**
- **Ceiling: UNCHANGED and honest.** A copy-patch JIT removes the DISPATCH slice
  (measured 7.1% HNE / 15.5% ff / 22.9% M5 on-CPU, `PROFILE_AT_SCALE_2026-06-21`).
  Amdahl on the warm gap: firefox 2.00×→**1.69×**, M5 1.84×→**1.42×**, HNE
  4.92×→**4.57×** TW — every one still **> 1×**. **A JIT NARROWS; it does NOT beat
  TW alone.** It is one of ~4 changes single-eval parity needs (allocate-less +
  leaner-cells + this + reg-alloc), per the campaign conclusion.
- **Recommendation: GO only as a paired, later-sequenced investment — NOT now as a
  standalone "beat TW" play.** See §8. The repeated-eval applied-import cache
  remains the shipped moat; the JIT is a CPU-quality lever with a proven mechanism
  and a ceiling that is bounded below "win" until the allocate-less lever lands.

## 1. What the JIT attacks, grounded at HEAD

The dispatch loop is `dispatchLoop(VMState & vm, size_t exitDepth, bool reuseScope)`
at `vm.cc:4316`; the hot core is `while (running) { … switch(decodeOp(instr)) … }`
(`vm.cc:4582`, decode at `vm.cc:5015`, switch at `vm.cc:5155`). Per-op it: reads
`cu->code[ip++]`, decodes op+operand, switch-dispatches, mutates
`vm.valueStack` (a `std::vector<Value, traceable_allocator>` at `vm.hh:129`), and
advances `ip`. The JIT removes the decode + switch + ip-advance for a compiled body.

The op mix it targets (`PROFILE_AT_SCALE` histogram, consistent across workloads):
`OP_GET_UPVALUE` 13–17% ≫ `OP_SET_LOCAL` 10–12% > `OP_GET_LOCAL` 9–10% >
`OP_RETURN` 7–8% ≈ `OP_MAKE_THUNK` 7–8% > `OP_GET_LOCAL2` 6–7% > `OP_ATTRS_SELECT`
2.6/7.4% > `OP_BRANCH_FALSE` 3–4%. **~52% of dynamic ops are trivial stack traffic**
— each pays a dispatch the JIT removes. But the *time* is dominated by ALLOC (~20%)
+ BINDINGS/countDistinct (~16%) + real primop work — which the JIT does NOT remove.

Frame mechanics the ABI must mirror (fork-verified line cites):
- **Call entry** (`vm.cc:6947–6969`): save caller `ip` into `frames.back().ip`;
  `newBase = valueStack.size()`; `valueStack.resize(newBase + d->nLocals)`; move
  args into `valueStack[newBase + i]`; `frames.push_back(CallFrame{…, .ip =
  d->codeOffset, .stackBaseOffset = newBase, …})`; set dispatch-locals `ip/cu/
  closure/stackBase`.
- **Return teardown** (`vm.cc:8598`, core at `8757–8872`): read frame fields;
  `valueStack.resize(fStackBase)`; `withStack.resize(fWithBase)`; `frames.pop_back()`;
  if `CFF_THUNK_RETURN` write `thunk->evaluated`+`Evaluated`; if `CFF_FORCE_RETRY`
  re-force; `push(vm, retVal)`.
- **CallFrame** (`vm.hh:66`): `{cu, closure, thunk, ip, stackBaseOffset,
  withStackBase, flags, forceWriteTarget, deepForceCursor, defEnv, memoKeyIdx}`.

## 2. The stencil / copy-patch approach

**Copy-and-patch (Xu & Kjolstad PLDI'21), NO register allocator.** One pre-defined
native *stencil* per supported opcode; the compiler stitches the stencils for a
body's instruction stream into one executable buffer and patches immediates (slot
indices, primop-table pointers, branch targets). The **value stack IS the calling
convention** (the J2/J3 decision): a stencil reads/writes `valueStack[stackBase+slot]`
via a base register (`X19` = `&valueStack[stackBaseOffset]`, callee-saved so it
survives BLRs). This makes cross-body calls a single trampoline and makes J3 spill
targets trivial (the live set is already the value-stack frame the scavenger walks).

**Two viable stencil sources — pick hand-written for J1–J3, revisit clang-generated
at J4:**
- **(A) Hand-written stencils** via the existing `Aarch64Emitter` (`include/v3/jit.hh`).
  Proven: `jit_encoder_test` 7/7, `jit_intop_test` 7/7 byte-id + 4/4 bail. Small,
  auditable, zero build-system surgery. **Chosen for J1–J3** (the ~10 opcodes below).
- **(B) clang-generated stencils**: write each opcode body in C, compile to a
  relocatable object with a fixed ABI, extract the `.text` + relocations at build
  time, patch holes at runtime. Scales to many opcodes with less hand-encoding but
  needs a build step + relocation parser + per-platform object handling. **Deferred
  to J4** when opcode coverage growth makes hand-encoding the bottleneck.

**Bail = the interpreter.** Any unsupported opcode/shape/throw-path emits a bail
sentinel; the caller re-enters `dispatchLoop` for that body. The body is ALWAYS
interpretable; the JIT is a fast path, never the only path (`JIT_DESIGN` risk #1).

## 3. Phases (each gated `NIX_V3_JIT`, byte-identical, `--brute`-clean)

### J0 — infra (DONE)
MAP_JIT + `pthread_jit_write_protect_np` W^X + `sys_icache_invalidate` proven
runnable (`jit_feasibility_spike`). `JitArena::finalize` mmaps MAP_JIT pages, one
W^X toggle, icache flush (`include/v3/jit.hh:164`). **Retirement: DONE (e010aa09e).**

### J1 — encoder + stencil table + code buffer (DONE + extend)
`Aarch64Emitter` (movz/movk/movImm64, mov, ldr/str, add/sub/mul, addImm/subImm,
cmp, and/orr, sbfx, b/b.cond+patch, blr, ret) validated (`jit_encoder_test` 7/7).
**Remaining J1 work (~2–3 d):** a `StencilTable` keyed by `Op` returning the
emit-lambda for the supported subset; a per-CU/per-descriptor code-buffer sub-
allocator inside one `JitArena` (J1 currently one page per body — fine for a spike,
wasteful at scale). x86-64: add the Linux encoder path (`PROT_EXEC` +
`__builtin___clear_cache`, already coded for the buffer; the *encoder* is aarch64-
only today — see §7).
- **Gate:** encoder unit tests green on both arches; no VM integration yet ⇒ no
  perf claim. **KILL only if** a supported opcode cannot be encoded byte-id
  (it can — J2 proved the hard one).

### J2 — arithmetic + stack-op body JIT, no calls (~1–2 wk)
Supported subset (the ~52% trivial dynamic ops + int arith), each a stencil:
`OP_GET_LOCAL` (`vm.cc:5189`), `OP_SET_LOCAL` (`5248`), `OP_GET_LOCAL2` (`5c`),
`OP_GET_UPVALUE` (`5278`, read `closure->upvalEnv->values[i]` — env-sharing gives a
stable base+offset), `OP_ADD`/`OP_SUB`/`OP_MUL` (int fast path `5467` + **bail on
non-int / overflow**, exactly the J2-proven shape), `OP_LIT_INT`, `OP_BRANCH_FALSE`
(`45`)/`OP_JUMP` (`44`) with intra-body patching, `OP_RETURN` (`8598`) via the
trampoline. `OP_FORCE` fast path (`9299` peek) inlineable; slow path bails.
- **Byte-id bail contract:** any operand not matching the fast-path tag → bail.
- **Gate (SHIP→J3):** a real hot body of ONLY these ops compiles, runs byte-id vs
  interpreter on a fixture battery, `--brute` clean (no allocation ⇒ no GC risk
  yet). **KILL if** byte-id fails on any shape and can't be bailed cleanly.
- **Known limit (do NOT ship J2 alone):** pure-arith bodies have ~0 nixpkgs
  coverage (`JIT_DESIGN` J2 finding) → J2 is a stepping stone, not shippable. The
  shippable subset MUST include allocating bodies ⇒ J3 is mandatory.

### J3 — safepoints + the value-stack spill (the crux; ~1–2 wk, HIGH risk)
The de-risked story (`WARM_EVAL_JIT_SCOPING`): JIT'd code holds live `Value`s in
registers; a body that ALLOCATES can trigger a nursery scavenge that MOVES targets
→ stale register = UAF (PhD-6 class). **Contract (J3-proven + this-Phase-0-re-
proven under a stitched chain):** before any allocation call, STR every live
pointer-`Value` to its `valueStack[stackBase+slot]` slot; keep NONE in a register
across the call; after, LDR from the slot — the scavenger rewrote it in place.
- **No stack maps.** Liveness = "what's on the value stack," identical to the
  interpreter's per-op GC-root discipline. `walkAllV3Roots` (`precise_root.cc:43`
  walks `vm.valueStack`) + the scavenger (`gc.cc:866` walks `valueStack`, `gc.cc:582`
  `visitValue` forwards pointer payloads **in place**) already enumerate exactly the
  frame region a JIT'd body uses. **No new root registration** — the JIT inherits
  J3-safety by mirroring the interpreter's op-boundary invariant.
- **Allocating stencils:** `OP_MAKE_THUNK` (`vm.cc:6284`), `OP_ATTRS_INIT`/`_DYN`,
  `OP_LIST_INIT`, `OP_STR_CONCAT`, `OP_CALL_PRIMOP` (`13101`) — each emits
  spill-before / reload-after around its runtime call. Start with the SIMPLEST
  allocating shape (a body doing loads + `OP_MAKE_THUNK` + return) and grow.
- **Barrier interaction (Constraint 0):** the nursery + Phase-D write barriers +
  gen-major are DEFAULT-ON. A JIT'd store of a nursery pointer into a tenured
  container must fire the same Phase-D barrier the interpreter does (`barrier.cc`);
  the safe start is that JIT'd bodies build only fresh/nursery objects and any
  tenured-write path bails to the interpreter until the barrier stencil is proven.
- **Gate (SHIP→J4):** an ALLOCATING JIT'd body, full `--brute` **22/22** under the
  1 MB-nursery moving-GC stress (`V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1`) +
  `V3_DBG_GC_STRESS`, byte-id sweep on hello/git/firefox drvPath. **KILL if** any
  missed-root/UAF surfaces that can't be fixed by tightening the spill set (i.e. if
  the spill discipline proves insufficient — this Phase-0 spike says it is
  sufficient for the modeled shape, but the REAL scavenger + barrier is the true
  test).

### J3.5 — value-stack ABI trampoline + tiering trigger + deopt (~1–2 wk, MED risk)
- **Trampoline:** marshal `dispatchLoop`'s value-stack ↔ the JIT'd body's ABI
  (`base = &valueStack[stackBaseOffset]`, `Closure*` for upvalues via
  `closure->upvalEnv`, `withStackBase`; LR saved per the J2 ABI). Return value
  pushed onto `valueStack` exactly as `OP_RETURN` does (`vm.cc:8872`). Makes a JIT'd
  `LambdaDescriptor` OP_CALL/OP_CALL_N-compatible.
- **Tiering trigger:** add `mutable void * jittedBody = nullptr;` to
  `LambdaDescriptor` (`closure.hh:330`). The existing `callCount` (`closure.hh:410`,
  bumped at `vm.cc:7805`) is **gated behind `g_dbgAllocDump`** → J3.5 needs a
  dedicated always-on hot counter (a bare `uint32_t hotCount` bumped unconditionally
  at the OP_CALL closure-invoke, `vm.cc:~7805` / `6947`) or an un-gated bare
  increment. Compile when `hotCount` crosses a threshold AND the body is
  fully-supported-shape; cache the native ptr on the descriptor.
- **Dispatch entry:** at the OP_CALL closure-invoke (after `desc` resolved +
  `stackBaseOffset` set, `vm.cc:~6947`), `if (desc->jittedBody && shapeSupported)`
  call native with the value-stack ABI instead of entering the switch.
- **Deopt/bail:** any bailed op → sentinel → caller re-enters `dispatchLoop` for
  that body byte-for-byte.
- **Gate:** OP_CALL of a JIT'd body byte-id vs interpret; `--brute` 22/22.

### J4 — broaden coverage + measure + ship gated (~ongoing)
Add `OP_ATTRS_SELECT` (+IC), `OP_CALL`/`OP_CALL_N` cross-body, more primops.
Consider switching stencil source to clang-generated (§2 option B) here.
- **SHIP gate (darwin-4, the perf gate — laptop cannot resolve <10% CPU):** cache-
  off git/firefox/M5 warm CPU JIT-on vs JIT-off, byte-id sweep, `--brute` 22/22.
  **Ship only if** it clears a pre-committed **≥15% warm-CPU narrowing on a real
  workload** (knowing the end-state is ~1.5–2.2×, NOT <1×) **without gaming** (no
  benchmark-only fast paths; the supported subset MUST cover allocating shapes).
  git-note every measurement to the tested commit (`bench/baselines/darwin4-rows.tsv`
  + `git notes`).

## 4. Effort estimate

| phase | scope | effort | risk |
|---|---|---|---|
| J0 | infra | DONE | — |
| J1 | encoder + stencil table + buffer sub-alloc + x86-64 encoder | ~2–3 d (+~1 wk x86-64) | LOW |
| J2 | arith/stack body JIT, no calls, bail contract | ~1–2 wk | LOW–MED |
| J3 | safepoints + spill + real scavenger + barrier | ~1–2 wk | **HIGH** (UAF) |
| J3.5 | ABI trampoline + tiering + deopt | ~1–2 wk | MED |
| J4 | broaden + darwin-4 grade + ship gated | ongoing | MED |

**Total minimal viable copy-patch (J1–J3.5, aarch64 only): ~4–6 weeks**, matching
the prior estimate. x86-64 codegen adds ~1–2 wk (encoder + Linux W^X path).

## 5. aarch64-darwin + x86-64-linux split

- **aarch64-darwin (primary):** J0 platform proven; `Aarch64Emitter` complete;
  MAP_JIT + W^X toggle + icache flush. All J1–J3 spikes run here.
- **x86-64-linux:** the `JitArena` buffer path is coded (`PROT_EXEC` +
  `__builtin___clear_cache`, `include/v3/jit.hh:180`) but the **instruction encoder
  is aarch64-only** — a parallel `X64Emitter` (or the clang-generated-stencil route,
  §2-B, which sidesteps hand-encoding two ISAs) is required. Recommended: land
  aarch64 end-to-end first (J1–J4), then either hand-write the x64 encoder or adopt
  clang-generated stencils for BOTH arches (option B amortizes across ISAs). The
  bytecode, ABI, safepoint contract, and tiering are arch-independent; only the
  stencil bodies differ.

## 6. Risk register (unchanged shape, re-confirmed)

1. **Missed-spill UAF (J3 crux)** — mitigated by mirroring the proven
   `walkAllV3Roots`/scavenger value-stack discipline + the `--brute`-with-
   allocating-JIT'd-bodies gate. This Phase-0 spike re-proved the discipline is
   load-bearing on a stitched chain, but the REAL scavenger + Phase-D barrier is
   the true test (J3 gate).
2. **Phase-D write-barrier interaction** — a JIT'd tenured-write must fire the
   barrier; start by bailing tenured-writes to the interpreter (§J3).
3. **Deopt semantic exactness** — every bailed op byte-id; start tiny, grow behind
   byte-id.
4. **Gaming** — the JIT MUST cover allocating bodies, not just arith (J2 finding).
5. **Ceiling** — narrows, doesn't beat TW alone (§7); only worth it paired + later.

## 7. The ceiling, reproduced from first principles (this Phase-0's spike)

`research/jit_copypatch_bench.cc` prints an Amdahl bridge anchored to the MEASURED
`PROFILE_AT_SCALE` dispatch fractions:

```
firefox   warm_gap 2.00×  − dispatch(16%) → floor 1.69× TW  (> 1×)
M5        warm_gap 1.84×  − dispatch(23%) → floor 1.42× TW  (> 1×)
HNE(cold) warm_gap 4.92×  − dispatch(7%)  → floor 4.57× TW  (> 1×)
```

Removing **all** dispatch (an unrealistic upper bound; copy-patch removes most but
not all, and adds a spill tax) still leaves every workload > 1× TW. The residue is
ALLOC (~20%) + BINDINGS/countDistinct (~16%) + parse + primop real work — untouched
by dispatch removal. **This is why the JIT is necessary-but-not-sufficient.**

## 8. Go / no-go

- **The 4–6 wk copy-patch build is technically sound and de-risked** — every hard
  mechanism (platform, encoder, byte-id codegen, safepoint spill under a move,
  stitched-chain correctness) is now proven. There is no discovery risk left; it is
  an engineering build with a HIGH-risk J3 integration gated by `--brute`.
- **But its ceiling (~1.5–2.2× warm, floor 1.42–1.69× even removing ALL dispatch)
  does NOT beat TW alone.** As a standalone "beat TW" play it FAILS the bar.
- **RECOMMENDATION:** **GO only as a paired, later-sequenced investment.** Sequence
  it AFTER (or alongside) the allocate-less lever (L3 thunk-churn / deeper
  strictness) that removes the ALLOC residue the JIT can't touch — because the JIT's
  narrowing only becomes a *win* once the non-dispatch residue is also cut. On its
  own, now, it is a CPU-quality improvement (~15–30% warm on JIT-coverable paths at
  the J4 gate) but not a parity change. **Do NOT start it as the beat-TW play; the
  repeated-eval applied-import cache remains the shipped moat.** If the user funds a
  multi-front single-eval-parity program, the JIT is 1 of ~4 required changes and
  this plan is its executable spec.

## 9. Artifacts (this Phase-0)

- `research/jit_copypatch_bench.cc` + Makefile target `jit-copypatch-bench` — the
  stitched-stencil throughput + correctness + Amdahl-bridge spike. Correctness
  (byte-id + survive-move) PASS; raw ratio is high-variance laptop noise (do NOT
  cite the magnitude — cite the Amdahl bridge for the portable ceiling).
- This doc.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
