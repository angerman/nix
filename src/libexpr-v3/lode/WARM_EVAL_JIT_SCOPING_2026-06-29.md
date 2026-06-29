# Warm-eval-targeted JIT — scoping, with J3 GC-safepoints as the crux (2026-06-29)

Scopes what a JIT that targets the WARM run-phase (hot lambda/thunk bodies, IFD + caches
already hot) would actually require, given the warm-vs-cold finding (IFD_AND_FIRST_EVAL),
the register-VM measurement (REGISTER_VM_MEASUREMENT), and the JIT RCA ceiling
(C2_JIT_RCA_AND_CAMPAIGN_CONCLUSION). Design only — no production code.

## TL;DR

- **J0/J1/J2/J3 are ALL proven standalone** (platform, encoder, byte-id codegen, safepoint
  contract). What's missing is INTEGRATION into the live VM under the real moving GC
  (#152–154) — a dedicated ~4–6 week build.
- **J3 does NOT need pc-indexed stack maps.** The proven contract is "spill every live
  v3-pointer Value to `vm.valueStack` before any allocation safepoint; keep none in a
  register across it; reload after" — and `vm.valueStack` is already enumerated by
  `walkAllV3Roots`, so the moving GC finds + forwards them with NO new root mechanism. This
  is the central de-risk and it is contract-proven.
- **The warm framing simplifies the SURFACE but cannot skip J3** — hot nixpkgs bodies
  ALLOCATE (a non-allocating JIT has ~0 real coverage = benchmark gaming, forbidden). So the
  minimal shippable warm JIT still needs the safepoint spill — but that's the de-risked,
  stack-map-free contract above.
- **Ceiling, honest: a JIT NARROWS, it does not BEAT TW.** Copy-patch dispatch-removal →
  warm 2.0/1.84× → ~1.5–2.2× (RCA). An *optimizing* JIT (reg-alloc) additionally removes the
  cheap stack traffic (reg-VM measurement: ~5–10% CPU) but NOT the ALLOC (~20%), countDistinct
  (~16%), primop/attr real work — and it ADDS a safepoint spill/reload tax. Beating TW (<1×)
  needs the JIT PLUS the orthogonal allocate-less (strictness) + leaner-cell changes; the JIT
  alone is necessary-not-sufficient.

## What is already proven (standalone spikes; cite)

- **J0 platform** — `research/jit_feasibility_spike.cc`: MAP_JIT + `pthread_jit_write_protect_np`
  W^X + `sys_icache_invalidate` work on macOS aarch64 (the gating platform unknown). Linux path
  (`PROT_EXEC` + `__builtin___clear_cache`) coded, untested here.
- **J1 encoder** — `include/v3/jit.hh` (197 lines, header-only, zero production impact until
  included): `Aarch64Emitter` (movImm64/ldr/str/add/sub/mul/cmp/b.cond+patch/blr/ret) +
  `JitArena::finalize` (mmaps MAP_JIT, W^X toggle, icache flush). `research/jit_encoder_test.cc`
  7/7 emits+executes.
- **J2 byte-id codegen** — `research/jit_intop_test.cc`: NaN-box int-ADD compiled native,
  mask/compare to INT_HEADER → sbfx-unbox → add → 48-bit-overflow check → orr-rebox, with the
  BAIL contract; 7/7 byte-identical to the v8nan reference + 4/4 bail cases. Proves Value-level
  bit-exact codegen + the always-interpretable fallback.
- **J3 safepoint CONTRACT** — `research/jit_safepoint_test.cc`: JITs two bodies over a toy
  moving collector whose safepoint mirrors the real scavenger (copies root to to-space,
  rewrites the value-stack slot in place — mirrors `gc.cc visitValue` `v.mkClosure(fwdClosure)`
  — poisons the from-space cell). The **spilled-reload body reads the FORWARDED object**; the
  **register-kept body reads POISON** (textbook UAF). So the contract is load-bearing and
  correct: spill→safepoint→reload.

The root machinery the JIT mirrors already exists: `gc_root.cc:16` `gcRootStack()` +
`:49 registerRoot` is the C++ side-stack of live `Value*` that `walkAllV3Roots`
(`precise_root.cc`) enumerates across a scavenge. The JIT's "shadow frame" is the same idea,
emitted by codegen: spill live Values into value-stack slots `walkAllV3Roots` already scans.

## J3 — what it actually requires (the crux), and why warm-framing doesn't dodge it

Hot nixpkgs bodies allocate (OP_MAKE_THUNK 7.8% of ops; the trivial loads feed allocating
attrset/list/string ops). When a JIT'd body allocates, the nursery scavenge can fire and MOVE
the targets of v3 pointers the native code holds in registers → stale = UAF. Required:

1. **Allocation safepoints with value-stack spill** (the proven contract). Before every alloc
   call the codegen emits stores of all live v3-pointer Values to their value-stack slots;
   after, reloads. No pc-indexed stack maps — liveness is "what's on the value stack," the
   interpreter's existing per-op GC-root discipline.
2. **Real-scavenger integration** (#152): wire the spill to the LIVE `walkAllV3Roots` + real
   nursery scavenge / mid-eval compactor (the spike used a toy collector). The value stack is
   already a scavenger root source, so this is "ensure the JIT keeps the same invariant the
   interpreter does at each op boundary," validated by AUDIT + BRUTE + `V3_DBG_GC_STRESS`.
3. **Bail/throw safety**: any path that can throw bails to the interpreter until error+trace
   parity is proven (JIT_DESIGN risk #1).

Warm-framing helps the SURFACE: only hot RUN-phase bodies are compiled; everything else (parse,
lower, cold, unsupported opcodes, throw paths) stays interpreted byte-for-byte. It does NOT
remove the J3 requirement (the hot bodies allocate). Net: J3 is mandatory but de-risked.

## Minimal viable warm-eval JIT (the #152–154 build)

- **#152 J3 real integration** (~1–2 wk, HIGH risk = missed-root UAF, but contract-proven +
  mirrors GcRoot): per JIT'd body emit value-stack spills at alloc safepoints; reload after;
  validate under 1 MB-nursery moving-GC stress.
- **#153 ABI + trigger + bail** (~1–2 wk, MED risk): value-stack calling-convention trampoline
  so a JIT'd `LambdaDescriptor` is OP_CALL/OP_CALL_N-compatible; compile-trigger on
  `LambdaDescriptor::callCount` threshold (cache the native ptr on the desc); deopt/bail for
  every unsupported opcode (start with the ~52% trivial stack ops + int arith, grow). Body
  stays always-interpretable.
- **#154 validate + measure + ship gated** (~1 wk): byte-id + full `--brute` 22/22 under
  moving-GC stress with ALLOCATING JIT'd bodies (exercises J3); darwin-4 warm CPU JIT-on vs
  JIT-off, no gaming.

Total ~4–6 weeks for the copy-patch (no-reg-alloc) version. This keeps the value-stack as the
ABI (J2), so cross-body calls are one trampoline and J3 spill targets are trivial.

## The ceiling — does an optimizing JIT change it?

Reconciling the two measurements:
- **JIT RCA**: dispatch is 7–23% on-CPU; removing it → warm 1.5–2.2× TW (still > 1×). ALLOC
  ~20%, countDistinct ~16%, primop/attr real work REMAIN.
- **Register-VM measurement**: the collapsible stack ops are ~49% of op COUNT but the CHEAPEST
  by time; the CPU is dominated by CALL_PRIMOP/ATTRS_SELECT/STR_CONCAT (real work). Reg-alloc
  collapse buys only ~5–10% CPU.

So:
- **Copy-patch JIT** (no reg-alloc, the #152–154 plan): removes dispatch → ~1.5–2.2×. ADDS a
  safepoint spill/reload tax on allocating bodies (partly offsetting). Narrows, doesn't beat.
- **Optimizing JIT** (reg-alloc): additionally removes the ~5–10% stack traffic. Still leaves
  ALLOC/countDistinct/primop/attr. ~1.3–1.8× plausibly. Still doesn't beat TW.
- **Beating TW (<1×)** would need the JIT to attack the RESIDUE: inline the bump-alloc fast
  path AND escape-analyze non-escaping thunks to stack-allocate (cuts ALLOC + churn — but
  that's the strictness/escape lever), inline caches for ATTRS_SELECT, inline hot primops. That
  is a multi-MONTH optimizing compiler, and it competes with TW's 15-year tuning. Even then,
  the campaign's structural finding stands: v3 must also allocate LESS (strictness surviving
  dynamic dispatch) and use LEANER cells — the JIT is 1 of ~4 changes (ARCH_BEAT_TW_PROGRAM).

**Verdict: an optimizing JIT does NOT, on its own, change the "narrows-but-doesn't-beat"
ceiling.** It is necessary-not-sufficient. The warm framing makes a ~4–6 wk copy-patch JIT
feasible and de-risked (J3 contract-proven, no stack maps), delivering ~1.5–2.2× — a genuine
CPU-quality investment, not a beat-TW play.

## Go/no-go gate

After #152+#153 (a working allocating JIT'd body under the REAL moving GC, byte-id +
`--brute` clean), measure on darwin-4 the warm CPU of a real JIT-coverable hot path
(JIT-on vs JIT-off, no gaming). Pre-commit: continue to #154/J4 coverage only if it clears a
NARROWING bar (e.g. ≥15% warm CPU on a real workload) — knowing the end-state is ~1.5–2.2×,
not <1×. If beating TW is the actual goal, the JIT must be sequenced WITH the allocate-less +
leaner-cell changes, not pursued as a standalone winner.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
