# JIT of hot lib bodies — design + staging (JIT-0, 2026-06-19)

Goal item 3 of "implement functional layer + thunk half + JIT". A JIT is the only
lever that can beat interpretation per-op (the uniform 4.5× v3-vs-TW gap is the
interpreter machinery itself — `reference_v3_vs_tw_structural_2026-06-19`). It is
a multi-week compiler backend; this doc scopes it and records the feasibility
spike that is its real first step.

## FEASIBILITY: PROVEN on this host (the #1 platform risk)

`research/jit_feasibility_spike.cc` (run: `make -C src/libexpr-v3/research jit-spike`)
emits a trivial aarch64 function as raw machine code, maps it executable, and
calls it. **Result on macOS aarch64 (Apple Silicon): WORKS** — `MAP_JIT` +
`pthread_jit_write_protect_np()` W^X toggle + `sys_icache_invalidate` + call all
succeed (returns 42). So Apple Silicon's W^X enforcement does NOT block JIT here
(no special entitlement needed for this dev build). This was the gating unknown —
if it had failed, the whole track would need out-of-process or AOT-only codegen.
The Linux path is `__builtin___clear_cache` + `PROT_EXEC` mmap (already coded in
the spike, untested on this aarch64-darwin host).

## Why JIT, scoped honestly

Forcing each thunk runs its body's bytecode ONCE (one-shot lazy eval = the
bytecode-VM worst case). A method/body JIT only pays off when a body runs MANY
times — in Nix that's the hot `lib.*` helpers applied millions of times across a
nixpkgs eval. So JIT targets **frequently-CALLED lambda bodies** (callCount hot,
already tracked per `LambdaDescriptor`), not one-shot thunks. The env-sharing work
just shipped (closures + thunks reference a shared `Env`) is a JIT enabler: a JIT'd
body reads upvalues from `closure->upvalEnv->values[i]` — a stable base+offset, far
friendlier to native codegen than chasing per-closure inline FAMs.

## Staging (each stage gated NIX_V3_JIT, byte-identical, --brute-clean)

**J1 — encoder + executable-memory manager (~1-2 wk).** A minimal aarch64 (+later
x86-64) instruction encoder (the subset the body-JIT emits: loads/stores, integer
ALU, compares, branches, call/ret) + a `JitArena` that mmaps MAP_JIT pages, batches
writes under one W^X toggle, flushes icache. Unit-tested against known encodings
(extend the spike into an encoder test). NO VM integration yet.

**J2 — template/copy-patch JIT for ONE hot arithmetic body (~1-2 wk).** Pick the
simplest hot body shape (pure integer arithmetic on upvalues/locals, returns an
int — e.g. a `lib` comparator). Use copy-and-patch (pre-compiled native templates
per opcode, concatenated — NO register allocator; keep the interpreter's value-stack
layout as the calling convention) so the VM state ABI is trivial. **Byte-identity
bail:** the JIT'd path must produce bit-identical results; any opcode/shape outside
the supported subset → fall back to the interpreter (the body stays interpretable
always; JIT is a fast path, never the only path). Gate `NIX_V3_JIT=1`, A/B vs
interpret.

**J3 — GC safepoints (the highest-risk part; ~1-2 wk).** The moving nursery +
gen-major mean JIT'd code holding v3 pointers in registers across an allocation
must spill them to a GC-visible location (the value stack / a safepoint frame) so
the scavenger can find + forward them. Strategy: only allocate at well-defined
safepoints where all live v3 pointers are already on the value stack (mirrors the
interpreter's per-op GC-root discipline); the JIT'd body keeps no v3 pointer live
in a register across a safepoint. AUDIT + BRUTE + V3_DBG_GC_STRESS at every step
(a missed root here = UAF, the PhD-6 class).

**J4 — broaden + grade (~ongoing).** Add supported opcode shapes (attr select,
list ops, calls into other JIT'd/interpreted bodies); compile-trigger on callCount
threshold; measure on **darwin-4** (the quiet host — laptop can't resolve <10% CPU)
cache-off git/firefox: JIT-on vs JIT-off CPU + the byte-identity sweep. Ship only
if it clears the bar without gaming (no benchmark-only fast paths).

## Key risks (beyond GC safepoints)

- **Byte-identity under all error/throw paths** — a JIT'd body that throws must
  produce the same error + trace as the interpreter; safest is to bail to interpret
  on any path that can throw until parity is proven.
- **Cross-body calls** — a JIT'd body calling an interpreted body (and vice-versa)
  needs a uniform entry ABI; the value-stack calling convention (J2) makes this a
  single trampoline.
- **Compile cost vs benefit** — JIT compile time must be amortized; only compile
  bodies above a callCount threshold, and cache compiled code keyed by descriptor
  (the disk-cache/linking design may persist it later).

## Status

**J1 DONE + VALIDATED (2026-06-22, commit e010aa09e).** `include/v3/jit.hh`:
JitArena (executable-memory manager) + a minimal correct aarch64 Aarch64Emitter
(movz/movk/movImm64, mov, ldr/str, add/sub/mul, addImm/subImm, cmp, b/b.cond+patch,
blr, ret).  Header-only/inline — no production-build impact until J2 includes it.
Validated by `research/jit_encoder_test.cc` (EMITS + EXECUTES generated code on
aarch64-darwin): **7/7 ALL PASS** (const materialise, load/store, ALU, compare,
conditional + unconditional branch with patching).  **Foundation only: NO VM
integration, NO GC safepoints → NO CPU win yet; the measurable win is J2+J3.**
Build lesson: a JIT'd body that cross-calls MUST save/restore X30 (LR) — the J2
trampoline ABI (a test omitting it hung).

**J2 CODEGEN PROOF DONE + VALIDATED (2026-06-22, commit 43c8d13cf).** The hard
part of J2 — emitting a Value-level op that is BYTE-IDENTICAL to the interpreter's
NaN-boxed bits, with the bail contract — is proven.  Extended the encoder
(andReg/orrReg/sbfx) and JIT'd an integer ADD over v3 Values: mask+compare to
INT_HEADER (bail if not inline-int), sbfx-unbox, add, 48-bit-overflow check (bail),
orr-rebox.  `research/jit_intop_test.cc` EXECUTES it: 7/7 int-add cases byte-id vs
the v8nan reference (incl edge-of-48-bit) + 4/4 bail cases correct (overflow ±,
non-int ±).  **STRATEGIC FINDING — a shippable JIT REQUIRES J3.** Pure-int-arith
bodies (the only ones safe without J3 GC-safepoints) have ~0 coverage on nixpkgs
(the real hot `lib.*` helpers ALLOCATE — attrset/list/string ops); a pure-arith
JIT would speed only synthetic microbenches (fib) = benchmark-gaming, which the
no-gaming ship rule forbids.  So the measurable, shippable win needs J3 (spill
live v3 pointers at allocation safepoints — the UAF-risk crux).  **CONCLUSION: J0
+ J1 + J2-codegen are proven foundations; the shippable JIT (value-stack ABI +
full body emitter + VM integration + J3) is a dedicated multi-week project, not a
campaign-tail slice.**

**J3 CONTRACT PROOF DONE + VALIDATED (2026-06-22, commit pending).** The central
J3 design question — can JIT'd allocating code keep its live v3 pointers correct
across a MOVING collection? — is answered standalone (no VM integration), the same
spike discipline as J1/J2.  `research/jit_safepoint_test.cc` JITs two bodies over a
toy moving collector whose "safepoint" mirrors the real scavenger: it copies the
value-stack root's object to to-space, REWRITES the root in place (mirrors
gc.cc:571 `visitValue`: `v.mkClosure(fwdClosure(...))`), and poisons the vacated
from-space cell.  Result: the **spilled-reload body reads the FORWARDED object**
(spills the pointer to the value-stack slot before the BLR, reloads after — the
slot now holds the forwarded address); the **register-kept control reads the
POISON** (a textbook UAF), proving the contract is load-bearing.  So the J3
calling-convention contract is confirmed correct: *spill every live v3-pointer
Value to vm.valueStack before any allocation safepoint; keep no v3 pointer in a
register across it; reload from the slot afterward.*  This de-risks the central
hazard BEFORE the multi-week VM integration — but does NOT replace it: the real
J3 risk is integrating with the ACTUAL nursery/gen-major scavenger (walking real
JIT frames as roots, the cell-write barrier interaction, AUDIT+BRUTE+GC_STRESS at
every step), which the toy collector cannot stand in for.  **Remaining shippable
work (J3-2..J4) = a dedicated multi-week project:** value-stack ABI + safepoint
emission in the body emitter, VM compile-trigger on hot callCount, dispatch + bail,
real-scavenger integration, then darwin-4 grade JIT-on vs off + byte-id sweep.

J0 (feasibility) DONE — the platform mechanism is proven runnable. J1-J4 are the
multi-week build; this is the point to decide scope/scheduling with the user, since
J1 alone (a real instruction encoder) is a meaningful sub-project. The env-sharing
foundation (shared `Env`) is in place to make J2's upvalue access codegen-clean.

## J3-2 INTEGRATION MAP (code-grounded hook points, 2026-06-22)

The dedicated multi-week VM-integration project starts from these concrete hooks
(read from the live tree, not invented).  **The central enabler: the v3 VM
ALREADY uses the value stack as its GC-root working set.**  A `CallFrame`'s locals
live at `valueStack[stackBaseOffset .. stackBaseOffset+nLocals)` (vm.cc:7,
include/v3/vm.hh:75); the scavenger walks exactly `vm.valueStack` (gc.cc:846) and
forwards each pointer Value in place (gc.cc:571).  So a JIT'd body that mirrors the
interpreter's value-stack discipline — keep the working set on the value stack,
spill/reload around any allocation — inherits the J3-proven GC-safety with NO new
root registration (the frame region is already walked).  Hooks:

1. **Compile trigger.** Add `mutable void * jittedBody = nullptr;` to
   `LambdaDescriptor` (closure.hh:324) + a cheap hot counter.  NOTE: the existing
   `callCount` (closure.hh:404, bumped vm.cc:6850) is gated behind `g_dbgAllocDump`
   (diagnostic) → J3-2 needs either to un-gate a bare increment or add a dedicated
   always-on counter.  Compile when it crosses a threshold (and the body is the
   supported opcode subset).
2. **Dispatch entry.** At the OP_CALL closure-invoke (vm.cc ~6842, after `desc` is
   resolved + the frame's `stackBaseOffset` is set), `if (desc->jittedBody && shapeSupported)`
   call the native body with the value-stack ABI instead of entering the dispatch
   loop.
3. **Value-stack ABI (the trampoline).** Pass `base = &valueStack[stackBaseOffset]`
   (locals, base+offset loads — the codegen-clean shape), the `Closure *` (upvalues
   via `closure->upvalEnv` per env-sharing), and the `withStack` floor
   (`withStackBase`).  LR saved per the J2 trampoline ABI.  Return value pushed onto
   `valueStack` exactly as OP_RETURN does (CallFrame.h: "return value is pushed onto
   valueStack and consumed by the caller").
4. **Safepoints.** Copy-patch alloc templates spill live Values to the value-stack
   slots and reload after the alloc — the exact J3-proven discipline; no roots
   beyond the already-walked frame region.
5. **Bail.** Any unsupported opcode/shape/throw-path → bail sentinel; the caller
   re-enters the interpreter for that body (the body stays interpretable always —
   the JIT is a fast path, never the only path).
6. **Validation gate.** AUDIT + full `--brute` + `V3_DBG_GC_STRESS` at every step
   (a missed root = UAF, the PhD-6 class) + byte-identity sweep + darwin-4 CPU grade
   JIT-on vs off.  Ship only if it clears the bar without gaming (no benchmark-only
   fast paths; recall the J2 finding — pure-arith bodies have ~0 nixpkgs coverage,
   so the supported subset MUST include allocating shapes, which is why J3 exists).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0*
