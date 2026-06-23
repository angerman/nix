# Change 4 (#152–154) — JIT build plan — 2026-06-23

The SECOND real lever (after HAMT).  #137 RCA'd it: dispatch is 7–23% of on-CPU
(36.86 M opcodes, ~52% trivial stack ops), and a JIT narrows the warm gap
(2.49× ff / 1.83× M5) to ~1.5–2.2× — a real narrowing but NOT a win alone
(ALLOC + countDistinct + real work remain), so it is sequenced AFTER HAMT.  This is
the executable build plan, de-risked the same way as the HAMT GC plan: the hard
part maps onto a PROVEN mechanism.

## Foundations already proven (standalone)

- **J0 platform** — exec-codegen works on macOS aarch64 (research/jit_feasibility_spike.cc).
- **J1 encoder** — aarch64 instruction encoder (research/jit_encoder_test.cc).
- **J2 byte-id codegen** — NaN-box int-ADD compiled native, byte-identical to the
  interpreter + bail on non-int (research/jit_intop_test.cc; 7/7 + 4/4).
- **J3 safepoint CONTRACT** — the GC-safepoint protocol proven in isolation
  (research/jit_safepoint_test.cc).
- Design: lode/JIT_DESIGN_2026-06-19.md, lode/JIT_CONFIDENCE_2026-05-23.md.

What's missing is INTEGRATION into the live VM under the moving GC.

## The crux (J3) — and why it's de-risked

JIT'd native code holds live v3 `Value`s in registers/native-stack slots.  When a
JIT'd body ALLOCATES (which real nixpkgs bodies do — pure-arith-only JIT = gaming,
forbidden), the nursery scavenge can fire and MOVE those Values' targets → the
native code's copies go stale = UAF.  The body must therefore SPILL its live v3
pointers at every allocation safepoint into a structure the scavenger scans.

**De-risking insight:** that structure ALREADY exists.  `GcRoot` RAII +
`walkAllV3Roots` (precise_root.cc:196: "Values held in C++ helper frames +
registered via the GcRoot") is exactly how C++ helper frames keep live Values
findable across a scavenge.  The JIT extends the SAME mechanism: a JIT'd frame
maintains a shadow list of its live v3 Values (spilled at safepoints) that
`walkAllV3Roots` enumerates — identical in spirit to GcRoot, just emitted by codegen
instead of C++ RAII.  No new GC-root theory; mirror the proven GcRoot path.

## Build steps (#152 → #154)

1. **#152 — J3 real-scavenger integration.**  Emit, per JIT'd body: a shadow frame
   of live-Value slots; at each allocation call (the safepoints), spill live v3
   pointers into it; register the shadow frame with `walkAllV3Roots` (mirror the
   GcRoot side-stack); after the alloc, reload (the scavenger may have updated the
   slots).  The contract is J3-proven; this wires it to the REAL scavenger.
2. **#153 — value-stack ABI trampoline + trigger + bail.**
   - Trampoline: marshal the interpreter's value-stack ↔ the JIT'd body's ABI
     (args in, result out) so a JIT'd `LambdaDescriptor` is call-compatible with
     OP_CALL/OP_CALL_N.
   - Compile trigger: when a `LambdaDescriptor::callCount` crosses a threshold,
     compile its body (background or inline); cache the native pointer on the desc.
   - Deopt/bail: unsupported ops (most of them, initially) fall back to the
     interpreter byte-for-byte; start with a small opcode subset (the trivial
     stack ops + int arith — ~52% of dynamic ops) and grow coverage.
3. **#154 — validate + measure + ship gated.**  byte-id + full `--brute` 22/22
   under 1 MB-nursery moving-GC stress with JIT'd bodies that ALLOCATE (exercises
   the safepoint spill — the UAF crux; a missed spill shows as a brute-audit
   missed root or a divergence).  Measure warm CPU on darwin-4 on the POST-Change-1
   stream.  Ship behind `NIX_V3_JIT` (default per cost/benefit).

## Risk register

- **Missed-spill UAF (the J3 crux)** — mitigated by mirroring the proven GcRoot/
  walkAllV3Roots path + the `--brute`-with-allocating-JIT'd-bodies gate.
- **Deopt semantic exactness** — every bailed op must match the interpreter
  byte-for-byte; start tiny, grow coverage behind byte-id.
- **Gaming** — the JIT MUST cover allocating bodies, not just pure arithmetic
  (a pure-arith JIT games the benchmark; #137 / the fusion-kill rule).
- **Ceiling (#137)** — narrows, does not beat TW alone; only worth it AFTER HAMT,
  and only if the warm-CPU delta clears the darwin-4 noise floor on real bodies.

## Decision

Both real levers are now blueprinted to executable form: HAMT (#1, algorithm proven
+ GC integration recipe in C2_HAMT_GC_INTEGRATION_PLAN) and JIT (#2, J0–J3 proven +
this integration recipe).  Both are multi-week builds gated on `--brute` under
moving-GC stress + (HAMT) a full nixpkgs byte-id soak + darwin-4 measurement.  JIT
is sequenced AFTER HAMT (lower ceiling; compiles the HAMT-reduced/strictness-
unchanged stream).  Neither can be completed at the quality bar in a single session;
both are now de-risked to "mirror this proven pattern, in these files, behind this
gate."

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
