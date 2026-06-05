# v3 wall-time optimization plan — quantified levers + concrete steps

**Audience:** the v3 team. **Status:** ACTIONABLE PLAN, 2026-06-05.
Self-contained — consolidates the 2026-06-05 bytecode survey, the
execution-weighted quantification ([[QUANTIFICATION_2026-06-05]]), and the
register-pressure sizing into one hand-off. Supersedes the scattered notes
in [[NEXT_STEPS_2026-06-05]] §2 for *what to build next on wall*.

---

## 0. Executive summary

Pure-recursion wall is at the interpreter ceiling (`fib33` ≈ 2.0× TW);
`fold-add-1M` ≈ 4.3×; peak RSS ≈ 4.4–5.3×. Execution-weighted measurement on
real evals (hello + firefox, production path) gives **one dominant wall lever
and one contained second**, and **kills three tempting dead ends**:

| Rank | Lever | Dynamic weight | Effort | What it is |
|---|---|---|---|---|
| **1** | Stack-motion | **51% of all dispatch** | M→L, phased | Reduce `GET/SET_LOCAL`/`GET_UPVALUE` traffic — stack-oriented lowering, then operand-folding superinstructions, then (gated) a register VM |
| **2** | Rec-binding machinery | **9.2%** | M (~1 wk) | Demote acyclic (`DAG`) `let`/formals from a synthetic rec-attrset to plain ordered locals |
| — | Memory | 4.4–5.3× RSS | separate track | After 1+2 plateau, pivot here (higher slope) |

**Do NOT build** (measured <1.6% dynamic — §3): the constant-spill peephole
(0.0%), a `WITH_LOOKUP` inline cache (<1.55%), `APPLY_OVERRIDES` elision
(<1.55%), or an `AttrSelect` PIC (2.8%, below the standing <10% criterion).

The opcode mix and register pressure are **workload-invariant** (hello ≈
firefox to the decimal — §2), so this ranking holds across complexity and
almost certainly cardano-node / M5.

---

## 1. State & strategic frame

- `fib33` 2.0× = the interpreter ceiling (~1.5–2×, per the architecture
  review). Pure compute wall is essentially done.
- The only remaining *wall* lever above the floor is **stack-motion (51%)**,
  which is structural — no peephole touches it materially (constant-spill, the
  obvious candidate, is 0.0% on real code).
- **Memory (4.4–5.3× RSS) has more slope per engineering-day** ([[memory-first-class]]).
  Recommended arc: harvest Levers 1+2 (wall), then pivot to memory.

---

## 2. The data (decision-grade, reproducible)

**Dynamic opcode mix** (`NIX_VM_OPCOUNTS=1`, production path):

| op / family | hello (11.1M disp) | firefox (21.4M disp) |
|---|---|---|
| **stack-motion** | **51.36%** | **51.20%** |
|  GET_LOCAL / GET_UPVALUE / SET_LOCAL | 21.0 / 14.4 / 11.3 | 21.1 / 14.3 / 11.3 |
|  SET_LOCAL_KEEP / GET_LOCAL_FORCE | 3.1 / 1.6 | 2.9 / 1.5 |
| MAKE_THUNK | 5.85% | 5.98% |
| REC_BINDING_SLOT_REF + ATTRS_REC_SET | 5.1 + 4.1 = **9.2%** | 5.2 + 4.2 = **9.4%** |
| AttrSelect family | 2.76% | 2.80% |

**Register pressure** (max simultaneously-live local slots/fn;
`analyze-operands.py` R1; ~90K functions each):

| p50 | p90 | p99 | max | mean | declared nLocals (mean) |
|---|---|---|---|---|---|
| 1 | 3 | 7 | 10565¹ | **1.78** | 3.5 |

- 8-register file → **99.4–99.5%** of functions never spill; 16 → 99.8%.
- ¹ the max is a single huge-attrset literal (pressure ≈ nLocals), the <0.6%
  p99→max tail — handled by spill.

**Workload-invariance:** hello (simple) and firefox (2× dispatch, far more
deps/overlays) are near-identical in both tables. The mix is a property of the
LOWERING, not the program.

**Reproduce:**
```
# dynamic profile (production)
NIX_V3_DIRECT_EVAL=1 NIX_VM_OPCOUNTS=1 NIX_VM_STATS=1 \
  nix eval --impure --expr '(import <nixpkgs> {}).<pkg>.drvPath'
# static corpus + register pressure
NIX_V3_EMIT_BYTECODE=1 NIX_V3_EMIT_BYTECODE_OUT=/tmp/x.bc NIX_V3_NO_DISK_CACHE=1 \
  NIX_V3_DIRECT_EVAL=1 nix eval --impure --expr '…'
python3 bench/analyze-operands.py /tmp/x.bc      # R1 pressure, C1 spill, D/S/B
```
**Measurement discipline (hard rule):** `v3-eval --expr` skips `optimise()`
AND strictness — its counts are NOT production. Always measure through
`nix eval`+`NIX_V3_DIRECT_EVAL`, `v3-eval --optimize`, or `--emit-bytecode`
(all run the full pipeline). This rule exists because a careful review once
concluded the opposite of the truth by reading `--expr`.

---

## 3. What NOT to build (falsified — keep them dead)

Each looked attractive on a non-dynamic basis; execution-weighting kills it.
Re-opening any requires new dynamic data + a pre-committed threshold.

| Candidate | Why it looked good | Measured | Verdict |
|---|---|---|---|
| Constant-spill peephole | ~6 ops/call on fib | **0.0%** (250 ops / 996K) | fib-loop artifact; not general |
| `WITH_LOOKUP` inline cache | 26,682 static sites | **<1.55%** dynamic | not a wall lever |
| `APPLY_OVERRIDES` elision | emitted per rec-attrset | **<1.55%** | tiny |
| `AttrSelect` PIC (Stage 5) | "attrsets everywhere" | 2.76% | below the standing <10% criterion |

(A `with`/overrides change may still be justified for *correctness/clarity* —
just not for wall.)

---

## 4. LEVER 1 — Stack-motion (51%). The wall ceiling.

51% of every dispatch is moving values between the operand stack and local
slots, because the lowering is strict A-normal-form: **every** subexpression
becomes a `let`-binding → a `SET_LOCAL` to spill it and a `GET_LOCAL` to
reload it, even single-use intermediates and constants. Attack it in three
phases, **measuring between each** — stop when wall returns drop below the
memory track's slope.

### Phase 1A — Stack-oriented lowering for single-use intermediates *(do first)*

> **MEASURED 2026-06-05 — Phase 1A is ALREADY SUBSTANTIALLY SHIPPED (the #542
> defer mechanism), and the residual is Phase-1C territory.** The emit-time
> stack scheduler this phase describes already exists in `emit.cc` (#542
> `pendingDefer`): it keeps single-use (OnceLinear) values on the operand
> stack and the SET_LOCAL_KEEP peephole fuses adjacent `SET;GET`. Verified on
> hello.drvPath (production): stack-motion (`SET_LOCAL`+`GET_LOCAL`+`KEEP`)
> defer-ON **3.80M** vs `NIX_V3_NO_DEFER=1` **4.55M** → defer already removes
> **−16.5%**; the adjacent `SET_LOCAL n; GET_LOCAL n` count (`analyze-operands.py`
> D2) is **0** — fully elided. The C2 detector finds **82.2% of static SET/GET
> are write-once+read-once single-use**, but **154,828 are NON-adjacent**: the
> consumer isn't the next op, so the value is *buried* on the stack and must
> spill. Keeping those on the stack requires **reordering** (blocked by Nix's
> strict force order) or **register allocation** — i.e. **Phase 1C**, not an
> incremental scheduler. Per this phase's own kill criterion, the
> incremental-1A-beyond-defer headroom is below threshold → **go to 1B-lite**
> (and 1C is the structural endgame for the non-adjacent residual). `C2`
> detector added to `analyze-operands.py`.

**Idea.** A subexpression whose value is used exactly once, by an instruction
that consumes it from the top of the operand stack with nothing clobbering the
stack in between, does not need a slot — leave it on the stack. This removes
matched `SET_LOCAL n … GET_LOCAL n` pairs (the spill 11% + part of the reload
21%).

**Where.** Emitter (`emit.cc`) and/or the IR shape from `cli/lower_v3.hh`.
Two viable implementations:
  1. *Emit-time stack scheduler:* during `emit.cc`, track which IR bindings
     are single-use and consumed by the next emitted op; emit the producer's
     value directly (no `SET_LOCAL`) and skip the matching `GET_LOCAL`.
  2. *IR-level:* stop forcing a binding for every subexpression in
     `lower_v3.hh` (tree-shaped sub-expressions stay inline) so `emit.cc`
     naturally leaves them on the stack.

**Concrete steps.**
1. In `analyze-operands.py`, add a detector for `SET_LOCAL n; …(no stack
   clobber)…; GET_LOCAL n` where slot `n` is write-once **and read-once**
   (generalises C1 beyond literals). Run on hello+firefox corpora →
   static candidate count + the `NIX_VM_OPCOUNTS` weight of the
   `SET_LOCAL`/`GET_LOCAL` it would remove. **Pre-commit a SHIP threshold**
   (e.g. ≥10% reduction in `SET_LOCAL`+`GET_LOCAL` dynamic count).
2. Implement the emit-time scheduler (option 1 is lower-risk — it doesn't
   change the IR contract).
3. **Correctness invariants:** preserve Nix's strict left-to-right *force*
   order (the operand stack must hold values in the same order the spilled
   slots would have been forced); the operand stack is GC-scanned, so keeping
   values there is root-safe; do not stack-schedule across a `FORCE`/effectful
   op that the original order forced earlier.

**Validation.** `all-v3-tests.sh --core` byte-identical; `run-cutover-parity-tests.sh`
on the nixpkgs sample byte-identical to TW; re-measure the dynamic
`SET_LOCAL`/`GET_LOCAL` share — must clear the pre-committed threshold.
**Kill criterion:** if the dynamic stack-motion drop is < threshold, revert
(it's a peephole carcass otherwise) and go to 1B.

**Effort:** ~1–2 weeks. **Expected:** removes a meaningful slice of the 11%
spill + a chunk of the 21% reload; doesn't touch `GET_UPVALUE` (14%).

### Phase 1B-lite — operand-folding superinstructions *(incremental register-ization)*
**Idea.** Fold the operand loads of the hottest binary ops into the op
itself, without a full register-VM rewrite. e.g. `GET_LOCAL a; GET_LOCAL b;
CALL_PRIMOP __X` → a single `OP_PRIMOP2_LL <X> a b` that reads slots a,b
directly. The `analyze-operands.py` D1 result (all adjacent `GET_LOCAL;
GET_LOCAL` are *different-slot*) confirms the target is a 2-slot-operand op,
not a DUP.

**Concrete steps.**
1. From the production `NIX_VM_BIGRAMS`/`NIX_VM_TRIGRAMS` dynamic n-grams
   (already shipped), pick the top operand-load→op trigrams (expect
   `GET_LOCAL GET_LOCAL CALL_PRIMOP`, `GET_LOCAL LIT_INT CALL_PRIMOP`,
   `GET_UPVALUE … `).
2. Add register-form opcodes in `bytecode.hh` for the top 3–5; encode the
   operand slots in the 24-bit operand (≤16 regs ⇒ 4 bits each ⇒ 3 slots
   fit). Emit them from `emit.cc` (pattern-match the trigram at emit time).
   Handle them in `vm.cc`'s dispatch.
3. Extend the disassembler (`disasm.cc`) — add `opName`, `opExtraWords`, and a
   resolved annotation for each new op (the table-driven design means
   name+width are the only required changes; see disasm.cc header).

**Validation.** Same suites; re-measure dynamic dispatch total (should drop by
the fused-trigram weight). **Kill:** < pre-committed dispatch reduction.
**Effort:** ~1 week per batch of ops. This is the highest ROI-per-effort
attack and a stepping-stone to 1C.

### Phase 1C — full register VM *(GATED — only if 1A+1B-lite plateau AND wall still > memory)*
**Idea.** Replace the stack calling convention with a register file: operands
name registers; ops are 2/3-address (`ADD rd, ra, rb`). This structurally
removes the operand-load dispatches that 1A/1B chip at.

**Sizing (from §2):** a **fixed 8–16-register file + memory spill** covers
99.4–99.8% of functions; mean pressure 1.78. Register allocation is
linear-scan over the live intervals `analyze-operands.py` R1 already computes
— start from that liveness.

**Concrete steps.** (1) New register-form opcode set in `bytecode.hh`;
(2) a register allocator pass (linear-scan, 8–16 phys regs, spill the rare
high-pressure function) consuming R1-style liveness; (3) re-target `emit.cc`;
(4) rewrite the `vm.cc` dispatch loop for register operands; (5) disassembler
support; (6) extensive parity validation.

**Gating contract.** Do NOT start 1C until: (a) 1A and 1B-lite are landed and
re-measured, and (b) the wall-vs-memory decision (`§"Memory"` in
[[ROADMAP_TO_VISION]] + [[memory-first-class]]) still ranks wall above memory.
A register VM is multi-KLoC against a hard ~1.5–2× ceiling; do not spend it if
1A+1B already land near the floor or if memory is the bigger gap.

---

## 5. LEVER 2 — Rec-binding machinery (9.2%). DAG `let`/formals demotion.

**The waste.** A `let` whose bindings reference each other (`let a=1; b=a+1;
…`) — and **every function's formals** (`{ a, b ? d }: …`) — lowers to a
*synthetic rec attrset*: `ATTRS_LET_REC_INIT` + a `MAKE_THUNK` per binding +
`APPLY_OVERRIDES` + a `REC_BINDING_SLOT_REF` per use. But Nix `let` is
recursive-by-default, and **most such groups are acyclic DAGs** (`b` uses `a`;
no cycle). A DAG needs no knot-tying: ordered plain locals suffice. Only true
mutual recursion (`a = f b; b = g a`) needs the attrset.

Dynamic footprint: `REC_BINDING_SLOT_REF` 5.1% + `ATTRS_REC_SET` 4.1% = 9.2%,
plus the demotable share of the 5.85% `MAKE_THUNK` and the per-group
`ATTRS_LET_REC_INIT`/`APPLY_OVERRIDES`. Demotion turns `SLOT_REF` indirection
into direct `GET_LOCAL`, drops the attrset + `APPLY_OVERRIDES`, and (with a
literal-field check, survey finding A) removes literal-binding thunks.

**This extends the existing non-recursive-`let` demotion** (which today only
handles the *zero-reference* case) to the acyclic-multi-binding case.

**Concrete steps.**
1. In the lowering (`cli/lower_v3.hh`, where `let`/formals become a `LetRec`
   IR node; the freevar/iteration logic is in `ir.cc`), build the
   binding-dependency graph for each binding group.
2. Compute SCCs / detect cycles. If the group is acyclic → topologically
   order the bindings and lower them as ordered plain bindings (each a thunk
   in a local slot; uses become direct slot reads). Keep the `LetRec`
   synthetic-attrset path **only** for groups containing a true cycle.
3. Formals: a formal default that references another formal (`c ? a + 1`)
   makes that group "referencing" — same DAG test applies; the common
   `{ a, b ? <literal/closed> }` is fully independent → plain locals.
4. Carry survey finding **A** along: a binding whose value is a literal stores
   the value directly (no `MAKE_THUNK`) even inside a still-rec group.
5. Elide `APPLY_OVERRIDES` for any rec attrset with no static `__overrides`
   key (the keys are in the `REC_INIT` symbol list — knowable at emit).

**Validation.** `all-v3-tests.sh --core` + the lang suite byte-identical
(watch the `let`-rec and `inherit`/`inherit (x)` cases — see
`test/run-let-rec-publish-split-tests.sh`); `run-cutover-parity-tests.sh`
byte-identical to TW; **re-measure** `REC_BINDING_SLOT_REF`+`ATTRS_REC_SET`+
`ATTRS_LET_REC_INIT`+`APPLY_OVERRIDES`+`MAKE_THUNK` dynamic share — expect a
multi-point drop. **Pre-commit** a SHIP threshold (e.g. ≥4% dynamic-dispatch
reduction) before implementing.

**Effort:** ~1 week. **Risk:** correctness of the cycle detection +
force-order under reordering — lean on byte-identity + the existing rec/inherit
repros. **Bonus:** also reduces thunks (memory) — feeds Lever 3.

---

## 6. LEVER 3 — pivot to memory (after 1+2)

With wall near the floor, peak RSS (4.4–5.3×) is the larger gap and has more
slope. The GC track is paused (`GC_PAUSE_2026-05-29`, Immix projected below
SHIP); re-evaluate per `EXIT_GC_SPIRAL_PLAN` once Levers 1+2 land. Levers 1A
(fewer slots) and 2 (fewer thunks) already shave memory — measure peak-RSS
deltas there first; they may move the GC decision.

---

## 7. Recommended sequence + decision gates

1. **Lever 2 first** (DAG `let`/formals demotion) — ~1 wk, contained, clear
   9.2% target, and it de-risks/feeds Lever 1 (fewer slots & thunks). Gate:
   ≥4% dynamic dispatch drop + byte-identical.
2. **Lever 1A** (stack-oriented lowering) — ~1–2 wk. Gate: ≥10% `SET/GET_LOCAL`
   dynamic drop.
3. **Lever 1B-lite** (operand-folding superinstructions) — batches of ~1 wk.
   Gate: per-batch dispatch drop ≥ threshold.
4. **Decision point:** re-measure wall + peak RSS. If wall still > memory in
   priority AND 1A+1B plateaued above the floor → **Lever 1C (register VM)**.
   Else → **Lever 3 (memory)**.

Every step: measure on the production path, pre-commit a threshold, revert on
miss (no opt-in carcasses). This is the [[measure-twice-cut-once]] +
[[falsification-rule]] discipline that already killed three candidate levers.

---

## 8. Tooling reference

- `bench/analyze-operands.py` — R1 (register pressure), C1 (const-spill),
  D/S/B detectors, `--cu N|last`. Runs on any `--emit-bytecode` /
  `NIX_V3_EMIT_BYTECODE` dump.
- `bench/analyze-bytecode.py` — opcode histogram, n-grams, stack-motion %.
- `v3-eval --emit-bytecode` — production-faithful disassembly (runs
  `applyStrictnessPasses`); `--optimize` — production-faithful eval for
  `NIX_VM_STATS`/`V3_TIMING`.
- Dynamic counters: `NIX_VM_OPCOUNTS`, `NIX_VM_BIGRAMS`, `NIX_VM_TRIGRAMS`,
  `NIX_VM_STATS` (alloc), `NIX_VM_OPCYCLES` (per-op ns).
- Disassembler internals: `disasm.cc` (table-driven — new opcodes need
  `opName` + `opExtraWords` + an optional resolved annotation).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
