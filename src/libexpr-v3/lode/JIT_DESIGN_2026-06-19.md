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

J0 (feasibility) DONE — the platform mechanism is proven runnable. J1-J4 are the
multi-week build; this is the point to decide scope/scheduling with the user, since
J1 alone (a real instruction encoder) is a meaningful sub-project. The env-sharing
foundation (shared `Env`) is in place to make J2's upvalue access codegen-clean.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0*
