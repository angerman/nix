# v3 optimization-lever quantification — execution-weighted, real workloads (2026-06-05)

**Status:** MEASUREMENT RESULT. Quantifies the candidate levers from the
2026-06-05 bytecode survey ([[NEXT_STEPS_2026-06-05]] + the fib/attrset/
with/formals/fix review) against REAL nixpkgs evals through the PRODUCTION
path. Several candidate levers are **falsified** here; read §3 before acting
on any of them.

**Method.** Dynamic opcode profile via `NIX_VM_OPCOUNTS=1` on the production
path (`nix eval --impure` + `NIX_V3_DIRECT_EVAL=1`). Static corpus +
register pressure via `NIX_V3_EMIT_BYTECODE` (full compiled corpus,
`NIX_V3_NO_DISK_CACHE=1`) fed to `bench/analyze-operands.py` (R-series) +
`analyze-bytecode.py`. Two workloads: `hello.drvPath` (light) and
`firefox.drvPath` (heavy, override/overlay-rich). cardano-node / M5 need the
cardano flake (not in this env's registry) — firefox is the heavy proxy;
§4 shows why the result is expected to hold for them.

---

## 1. Dynamic opcode mix — and it is WORKLOAD-INVARIANT

| opcode / family | hello (11.1M disp) | firefox (21.4M disp) |
|---|---|---|
| **stack-motion family** | **51.36%** | **51.20%** |
|  · GET_LOCAL | 21.02% | 21.13% |
|  · GET_UPVALUE | 14.36% | 14.33% |
|  · SET_LOCAL | 11.32% | 11.32% |
|  · SET_LOCAL_KEEP | 3.11% | 2.90% |
|  · GET_LOCAL_FORCE | 1.55% | 1.52% |
| MAKE_THUNK | 5.85% | 5.98% |
| REC_BINDING_SLOT_REF | 5.12% | 5.16% |
| ATTRS_REC_SET | 4.11% | 4.24% |
| RETURN | 5.45% | 5.43% |
| BRANCH_FALSE | 3.27% | 3.34% |
| CALL_PRIMOP | 3.10% | 3.05% |
| CALL | 1.74% | 1.68% |
| AttrSelect family | 2.76% | 2.80% |

**hello (simple) and firefox (2× dispatch, vastly more deps/overlays) are
near-identical to the decimal.** The opcode mix is a property of the
LOWERING, not the program — every Nix expr is attrsets/lambdas/let/with,
lowered the same way; "more convoluted" means MORE of the same shapes, not a
different mix. So the levers below are **workload-universal** — no need to
re-measure the mix per workload (cardano/M5 included; verify-don't-assume,
but the prediction is strong).

---

## 2. Register pressure — the register-VM sizing answer (also workload-invariant)

Register pressure = max simultaneously-live local slots per function (linear
live-interval estimate; `analyze-operands.py` R1). This is the "ideal
register count", vs the declared `nLocals` (which the A-normal-form
allocator inflates by rarely reusing slots).

| percentile | reg-pressure (hello / firefox) | declared nLocals |
|---|---|---|
| p50 | 1 / 1 | 3 |
| p90 | 3 / 3 | 5 |
| p99 | 7 / 7 | 15 |
| max | 10565 / 10565¹ | 10574 |

| register file | % functions that never spill (hello / firefox) |
|---|---|
| 4 regs | 96.2% / 96.2% |
| **8 regs** | **99.5% / 99.4%** |
| 16 regs | 99.9% / 99.8% |
| 32 regs | 100.0% / 100.0% |

¹ The max is one pathological function whose pressure ≈ nLocals (every slot
live at once) — a huge flat attrset/list literal shared by both evals (it
imports the same nixpkgs). It is the <0.6% p99→max tail.

**Ideal register file: 8 (covers 99.4–99.5% of functions), 16 for 99.8%+,
with memory spill for the rare huge-attrset outlier.** Mean pressure is
**1.78** vs mean declared nLocals **3.5** — the allocator over-reserves
~1.8 slots/function, and most functions need only **1–3** registers. Again
hello ≈ firefox: the distribution is lowering-intrinsic.

---

## 3. Per-finding verdict (what survives, what measurement KILLED)

| finding | static | **dynamic** | verdict |
|---|---|---|---|
| **(E) stack-motion** | 49.4% | **51.4%** | **#1 lever — but STRUCTURAL** |
| ↳ constant-spill peephole | 250 ops (**0.0%**) | negligible | **FALSIFIED as general** |
| **(H) rec-binding machinery** | — | **9.2%** (SLOT_REF 5.1 + REC_SET 4.1) | **#2 lever (contained)** |
| MAKE_THUNK | 87K static | 5.85% (650K) | real; (H)+(A) reduce it |
| (B) WITH_LOOKUP inline cache | 26,682 static | **<1.55%** | **FALSIFIED as wall lever** |
| (D) APPLY_OVERRIDES elision | 1,134 static | **<1.55%** | **FALSIFIED — tiny** |
| (A) literal `rec` field thunks | subset | subset of 5.85% | narrow |
| AttrSelect PIC (Stage 5) | 23,183 | 2.76% | below existing <10% kill-criterion |

Three candidate levers from the survey are **killed by execution-weighted
measurement** — each looked attractive on a different non-dynamic basis:

- **Constant-spill (NEXT_STEPS §2a) — 0.0% of the real corpus** (125
  candidates / 250 saveable ops in 996K instructions). It looked dominant on
  *fib* only because fib is a tight arithmetic loop; real nixpkgs has almost
  none and they aren't hot. **NEXT_STEPS §2(a) is corrected by this** — the
  51% stack-motion is genuine local/upvalue traffic, NOT spill, so the lever
  is structural (register VM), not a peephole.
- **WITH_LOOKUP inline cache (B)** — 26,682 *static* occurrences made it look
  broad, but **<1.55% dynamic**; the with-lookups aren't in hot paths. The IC
  would touch <1.55% of dispatch. Do not prioritize for wall.
- **APPLY_OVERRIDES elision (D)** — <1.55% dynamic. Tiny.

This is the measure-first discipline working: of six survey candidates, the
data leaves a clean two-item ranking.

---

## 4. Implications / recommended ordering

1. **Stack-motion (51%) is THE wall lever and it is structural.** It is the
   register-VM / wider-operand decision ([[NEXT_STEPS_2026-06-05]] §4), not a
   peephole — confirmed by constant-spill being 0.0%. §2 here is the sizing
   input: a **fixed 8–16-register file + spill** covers 99.4–99.8% of
   functions; mean pressure 1.78 means the common case is tiny. The
   pressure-vs-nLocals gap (1.8 slots/fn) also bounds a cheaper interim win:
   a slot-reuse / liveness-based allocator that shrinks frames without going
   full register-VM.
2. **Rec-binding machinery (9.2%) is the contained #2 lever.** The DAG-
   orderable `let`/formals demotion (survey finding H) turns
   `REC_BINDING_SLOT_REF` indirection into direct `GET_LOCAL`, drops the
   synthetic attrset + per-binding thunk, and feeds both #1 (less stack
   motion) and memory (fewer of the 650K thunks). Extends the existing
   non-rec-`let` demotion to the acyclic-multi-binding case.
3. **Do not build:** the constant-spill peephole, the WITH_LOOKUP IC, or
   APPLY_OVERRIDES elision as *wall* levers — all measured <1.6% (constant-
   spill 0.0%). (A WITH/overrides change may still be justified for
   correctness/clarity, but not for wall.)
4. **Workload-invariance (§1, §2)** means this ranking is stable across
   hello → firefox and, by the lowering-intrinsic argument, cardano-node /
   M5. Re-confirm on cardano/M5 when the flake is available, but expect the
   same mix.

---

## Tooling

- `bench/analyze-operands.py` R1 (register pressure) + C1 (constant-spill)
  added this session; runs on any `--emit-bytecode` / `NIX_V3_EMIT_BYTECODE`
  corpus dump.
- Reproduce: `NIX_V3_DIRECT_EVAL=1 NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 nix eval
  --impure --expr '(import <nixpkgs> {}).<pkg>.drvPath'` for the dynamic
  profile; add `NIX_V3_EMIT_BYTECODE=1 NIX_V3_EMIT_BYTECODE_OUT=… NIX_V3_NO_DISK_CACHE=1`
  then `analyze-operands.py` for the static corpus + register pressure.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
