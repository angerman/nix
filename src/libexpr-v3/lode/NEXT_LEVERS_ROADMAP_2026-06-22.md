# v3 next-levers roadmap (2026-06-22)

Grounded in the PROFILE_AT_SCALE_2026-06-21 cost map + L1 (countDistinct, shipped)
+ the cache-on reframe below.  Successor to MEMORY_FORWARD_PLAN_2026-06-14 (whose
RSS levers — thunk-header, nursery/gen-major — are shipped; pair-tax + L2 closed).

## THE REFRAME — measure WARM, not just cold (2026-06-22, darwin-4, pinned)
We had only ever measured cache-OFF.  Cache-off includes the parse→IR→optimise→
bytecode pipeline, which the disk cache amortises in any repeated/warm eval.

| workload | cache-off (×TW) | **warm/prod (×TW)** | lowering tax |
|----------|----------------:|--------------------:|-------------:|
| git      | 1.41  (4.15×)   | **0.71  (2.09×)**   | 50%          |
| firefox  | 2.64  (3.62×)   | **1.78  (2.44×)**   | 33%          |
| M5       | 10.84 (3.04×)   | **6.45  (1.79×)**   | 40%          |

**The production (warm) gap is ~1.8–2.4× TW — a third to half the cold gap.**
M5 (the flagship) is only **1.79×** warm.  Two regimes, two lever sets:
- **WARM eval-loop** (dev loops, repeated evals): gap 1.8–2.4×; lowering is cached
  away.  Target = ALLOC (churn) + DISPATCH (upvalue/local) + TLS.
- **COLD lowering** (CI, first eval): the 33–50% tax is real.  Target = parse/lex/
  intern/optimise speed.
PROCESS: add a WARM row to `bench/baselines/darwin4-rows.tsv`; track both. Cache-off
stays the byte-id/correctness oracle; warm is the production-CPU metric.

## Post-L1 cost map (cache-off on-CPU; warm drops the lowering rows)
ALLOC ~20–29% · DISPATCH 7–24% (M5 highest) · PARSE+LOWER+HASH ~20–29% (= the
amortisable tax) · TLS ~6% M5 · GC 4–7% · STRING ~3% · FORCE+CALL ~2–3% (dead) ·
OTHER ~9–11%.  Opcodes: OP_GET_UPVALUE 13–17% ≫ SET/GET_LOCAL > OP_RETURN ≈
OP_MAKE_THUNK 7–8%.  Thunk churn: **62–67% allocated-but-never-forced** (drives
ALLOC + the 637 MB M5 thunk RSS).

## LEVER ROADMAP (prioritised; cheap-falsifier-first, measure-before-build)

### TIER 1 — bounded, do-next (warm eval-loop)
**T1a · TLS hoisting** — attacks TLS (~6% M5 warm).
- macOS `_tlv_get_addr` resolves `threadArena()`/`activeVMStack()` per use.
- Lever: hoist them into dispatchLoop locals (resolve once per loop entry/re-entry).
- Falsifier (cheap): hoist `threadArena` in the hot path, measure M5 warm.
- Risk: MEDIUM (TLS identity stable within a dispatch; nested-VM/ re-entry care).
  macOS-specific (Linux TLS ≈ a register). Effort: small. **Best bounded next.**

**T1b · Dispatch superinstructions** — attacks DISPATCH (7–24%).
- Computed-goto was NEUTRAL → the cost is per-opcode WORK, not the switch. Fuse hot
  bigrams (esp. around OP_GET_UPVALUE 13–17%) into fused fast-paths.
- Falsifier (needs a small instrument): opcode BIGRAM histogram → top fusable pair
  → fuse one → measure. (Prior single tries GET_LOCAL/GET_UPVALUE+ATTRS_SELECT were
  neutral — do NOT repropose those; let the bigram data pick.)
- Risk: MEDIUM (byte-id). Effort: small-medium. Sizing UNCERTAIN → falsifier-gated.

### TIER 2 — structural prizes (the step-change; sizing-gated)
**T2a · L3 unforced-thunk churn** — attacks ALLOC (~20%) + the 637 MB M5 thunk RSS.
- 62–67% of thunks never forced. v3's per-thunk churn is costlier than TW's
  (24 B + nursery/barrier machinery vs TW 16 B + Boehm), so fewer/cheaper thunks
  helps the gap on BOTH CPU and RSS, every workload.
- Two sub-paths: (a) FEWER thunks via demand/strictness analysis (don't thunk
  PROVABLY-forced bindings/args — extends the shipped `opt_strict_call_unthunk`);
  (b) cheaper churn via the allocator (entangled with L2 — bigger nursery catches
  churn but copies heavy survivors → M5 regression, see L2 closure).
- **Falsifier FIRST (FP-3-style sizing gate):** static fraction of OP_MAKE_THUNK
  sites that are PROVABLY forced. If large → build the strictness pass. If most are
  genuinely conditional → the lever is small, document-close.
- Risk: HIGH (eager eval of a thunk that would have errored but was never forced →
  introduces a TW-absent error → byte-id BREAK). Must be provably-forced, not
  speculative. The biggest RSS+CPU prize; the hardest. Effort: large.

**T2b · JIT (the big swing)** — attacks DISPATCH + the systemic per-op cost.
- JIT-0 (done) proved exec-codegen on macOS aarch64 + designed J1–J4. Native code
  for hot lambda bodies structurally removes dispatch + per-op overhead.
- Risk: HIGH (multi-week subsystem; codegen correctness; byte-id). Biggest CPU
  potential — the only lever that changes the *systemic* per-op cost in kind.
- Sequence AFTER T1 (harvest bounded gains) + the T2a falsifier; resume from JIT-0.

### TIER 3 — cold/CI regime (lowering; secondary for warm)
**T3 · Lowering-pipeline speedup** — attacks PARSE+LOWER+HASH (33–50% of COLD).
- Amortised in warm/dev loops → only matters for CI/cold/first-eval.
- Sub-levers: faster lexer (`yylex`), symbol interning (HASH — high on HNE),
  fewer/cheaper optimiser passes, `addAttrLeaf`.
- Falsifier: profile the cold parse+lower phase in isolation; pick the hottest.
- Risk: LOW-MEDIUM (no eval semantics). Effort: medium. Priority: after Tier 1/2
  unless CI/cold latency becomes a stated goal.

### TIER 4 — deferred / low
- **L2-adaptive nursery** (closed-deferred): small/medium arena win (−20–38%),
  M5-safe only via multi-cycle hysteresis = real GC surgery. Revisit if small-eval
  latency becomes a priority. Meanwhile `NIX_V3_NURSERY_SIZE=256` is the manual win.
- **Allocator micro-opts**: zeroing (calloc/memset), Bindings-merge memmove. Diffuse.
- **OP_GET_UPVALUE fast-path**: the #1 opcode; env-sharing added an Env deref —
  check it's not slower than needed (could fold into T1b or stand alone).

## DEAD — do NOT revisit (falsified/closed, with reason)
- forceValue / callClosure micro-opt — ≤3% on-CPU at scale (the old narrative was wrong).
- AttrSelect shapes/PIC — <10% hit (SP-1..4).
- Pair-tax / ValuePair shrink — net-marginal at high risk (closed 2026-06-20).
- Computed-goto dispatch — neutral (CG-1..4).
- L2 fixed-bigger nursery — regresses M5 +14–41% (closed 2026-06-22).
- toString(int) cache / genList fusion — real-world-neutral / falsified.

## STRATEGIC READ
L1 (countDistinct) was the one concentrated hot-spot; harvesting it took the warm
gap toward ~1.8–2.4×. What remains is SYSTEMIC (allocation + dispatch + per-op), so
bounded levers (TLS ~6%, superinstr ~?) give incremental gains until a STRUCTURAL
change — T2a (fewer thunks via strictness) or T2b (native bodies via JIT) — delivers
the step-change. RSS is largely addressed (nursery/gen-major/thunk-header shipped);
the remaining RSS prize is T2a (thunk churn). Recommended order:
**T1a (TLS) → T1b falsifier (bigram) → T2a falsifier (provably-forced) → then the
justified structural build (T2a strictness or T2b JIT).**

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
