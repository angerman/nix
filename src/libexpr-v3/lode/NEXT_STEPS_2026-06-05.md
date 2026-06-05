# v3 next steps — corrected fib analysis + where the real wall levers are (2026-06-05)

**Status:** TEAM GUIDANCE. Written after a bytecode review of `fib33` that
reached a wrong conclusion via non-production tooling, then self-corrected.
The correction and the meta-fix it implies are the load-bearing content;
the codegen levers and strategic read follow.

**Anchors (current HEAD, same-binary same-host wall vs cppnix TW):**
`fib33` ≈ 2.0× (3.30s / 1.62s) — the interpreter ceiling. `fold-add-1M` ≈
4.3× (0.60s / 0.14s). Peak RSS ≈ 4.4–5.3× — the larger remaining gap.

---

## 0. The correction (read first, so nobody chases a non-lever)

**fib's strict-argument thunks are NOT an open lever — production already
eliminates them.** `run.cc → ir::applyStrictnessPasses` de-thunks
`(n-1)`/`(n-2)` into inline `CALL_PRIMOP __sub`; the production bytecode for
`fib` has **zero** per-call `MAKE_THUNK` (one total, for the top-level `fib`
binding). fib's 2.0× is *already* the de-thunked figure.

A careful review concluded the opposite ("~635K surviving arg thunks, the
dominant cost") because it measured two **non-production** paths:
- `v3-eval --emit-bytecode` was missing `applyStrictnessPasses` (now fixed),
  so the disassembly showed a *pre-strictness* form with the `MAKE_THUNK`s.
- the `thunks=635621` count came from `v3-eval --expr`, which runs **no
  `optimise()` at all**.

Both are strictly worse than what executes. This is a textbook
[[measure-twice-cut-once]] failure: the load-bearing number was never taken
through the production pipeline.

---

## 1. META-FIX (do this FIRST): one measurement path that equals production

This is the highest-leverage item — it is *why* a careful review went wrong
and it will keep going wrong. The divergent pipelines, and which are real:

| Path | `optimise()` | `applyStrictnessPasses` | `NIX_VM_STATS` | `V3_TIMING` |
|---|---|---|---|---|
| `v3-eval --expr` | ✗ (skipped by design) | ✗ | ✓ dumps (but pre-opt counts) | ✓ but `run=` mis-brackets¹ |
| `v3-eval --emit-bytecode` | ✓ | ✓ (fixed `55e108a87`) | n/a (no run) | n/a |
| **`v3-eval --optimize`** (added with this doc) | ✓ | ✓ | ✓ dumps | ✓ accurate |
| **`nix eval` + `NIX_V3_DIRECT_EVAL` (PRODUCTION)** | ✓ | ✓ | ✓ dumps | ✓ accurate |

> ¹ The `run=` mis-bracket is real **only on the `--expr` path** (no
> eval/apply de-thunk → the re-entrant primop timing it referred to). On the
> production / `--optimize` paths it does **not** reproduce — see the
> correction below.

**Done (the meta-fix landed):** the `v3-eval --optimize` mode (`v3-eval.cc:440`)
runs the *exact* production sequence (`optimise → applyStrictnessPasses →
computeFreeVars → compile → run`) and dumps `NIX_VM_STATS` + `V3_TIMING`.
Validated: fib27 thunk count drops `635621 → 1` (vs `--expr`'s 635621).
There is now **one production-faithful measurement command**, and the
standing rule below makes it the only sanctioned one.

### Correction (same day) — Actions 2 & 3 were themselves mis-premised

This is the part most worth reading, because **the original Actions 2 and 3
repeated the exact failure this doc exists to prevent**: they were written
from the reviewer's table, not re-measured on the production path. When
re-measured, both evaporate:

- **`NIX_VM_STATS` is already dumped on `nix eval`.** The table cell "✗ not
  dumped" was wrong. Measured: `nix eval` (production, `NIX_V3_DIRECT_EVAL=1`)
  on fib27 reports `thunks=0` — the production de-thunked count, not 635621.
  So the original Action 3 ("wire it into `runRootExpr`") is **already
  satisfied**; do not build it. Residue: `nix eval` reports `thunks=0` where
  `--optimize` reports `thunks=1` (the top-level `fib` binding). A **1-thunk
  reconcile** so the two faithful paths agree exactly is the only real work
  here — low priority.
- **`V3_TIMING`'s `run=` is accurate on the production path.** Measured:
  `nix eval` fib27 → `run=219.978ms` inside `wall=0.35s` (the ~130 ms balance
  is startup + print). That is *not* a mis-bracket — `run=` is the eval time.
  The mis-bracket the reviewer saw was the `--expr` path (footnote ¹). So the
  original Action 2 ("fix the `max(run=)` mis-bracket") is **a no-op on
  production**; verify-then-skip. If anyone reproduces a `run=` lie, first
  confirm it is not the unoptimised `--expr` path (now superseded by
  `--optimize`).

**Net:** the meta-fix goal — *one command that equals production, so nobody
re-measures a non-production path* — is **met** by `--optimize` plus the
already-faithful `nix eval`. Phase 1 is done bar the 1-thunk reconcile. The
broader lesson: an Action list derived from a review's table, not from the
production runner, is the [[measure-twice-cut-once]] failure one level up —
re-measure before you build.

**Standing rule:** validate every bytecode/alloc/timing claim through
`v3-eval --optimize`, `--emit-bytecode` (strictness-complete), or the
production runner (`nix eval` + `NIX_V3_DIRECT_EVAL`) — **never `--expr`**
(it skips `optimise()` and over-counts by construction).

---

## 2. The genuinely-open codegen holes (production-accurate, from fib's body)

> **QUANTIFIED 2026-06-05 — see [[QUANTIFICATION_2026-06-05]].** The
> measure-first step below was done on real workloads (hello + firefox,
> production path) and **reordered this list**. Headline corrections:
> (a) constant-spill is **FALSIFIED as a general lever** (0.0% of the real
> corpus — a fib-only artifact); (d) stack-motion is the real #1 at **51%
> dynamic** and is structural (register VM, ideal file 8–16 regs); and the
> separately-surveyed WITH_LOOKUP-IC and APPLY_OVERRIDES levers are both
> **<1.55% dynamic** (falsified for wall). The genuinely-open #2 is the
> rec-binding machinery (9.2% dynamic) via DAG-`let`/formals demotion.

> **SHIPPED 2026-06-05 (disposition after the QUANTIFICATION).** All of §2
> was implemented and validated (`--core` 19/19 byte-identical, IR-checks
> 28/28, smoke green). The real-corpus measurement landed *during* the work,
> so the framing is corrected here but the commits are KEPT (correct +
> real-neutral — no regression):
> - **(c) `46f57e490`** — post-strictness dead-function sweep. Real CU-size /
>   compile-time hygiene (fib 81→71 B; 22% of funcs cleared on a synthetic).
>   Uncontested win.
> - **(a) `68bd198de`** — constant rematerialization. fib −16.3% ops / ~7%
>   wall, but **real-corpus 0.0% (neutral on hello, σ±0.02)** — a fib/tight-
>   arithmetic micro-win, NOT a general lever. Kept (correct, dormant on real
>   workloads); `NIX_V3_NO_CONST_REMAT=1` opts out.
> - **(b) `9eaf666e0`** — `OP_GET_UPVALUE_REC_BINDING` superinstruction.
>   fib −4.88% ops / ~1% wall; touches a **real 5.12% of dispatch** (RBSR is
>   real on real corpora) but wall-marginal (cheap ops; hello is overhead-
>   dominated so unmeasurable there). Kept; `NIX_V3_NO_FUSE_RECBIND=1` opts
>   out. NOTE: this is the *dispatch-fusion* take on the rec-binding hole; the
>   bigger #2 lever below (DAG-`let`/formals demotion) is complementary.
>
> **Lesson:** §2(a)/(b) were measured on **fib** (eval-dominated but
> unrepresentative — tight arithmetic recursion); the QUANTIFICATION's
> real-corpus opcode-mix is the discipline-correct basis and supersedes the
> fib wall numbers. fib over-stated both. The next lever (#2) is chosen from
> the real-corpus ranking, not fib.

With thunks off the table, fib's `func 2` (~7M× in fib33) still wastes work:

**(a) Constant spill-and-reload — ~~biggest, and general~~ FALSIFIED.** On
fib it looked big (`LIT_INT k; SET_LOCAL s; … GET_LOCAL s`, ~6 ops/call).
**Measured on real corpora it is 0.0%** (125 candidates / 250 ops in 996K
instructions) — a tight-arithmetic-loop artifact, not general. Do NOT build
the peephole as a wall lever. (See QUANTIFICATION §3.)

**(b) Redundant rec-binding resolution.** fib resolves `fib` twice per call
(`GET_UPVALUE; REC_BINDING_SLOT_REF`). CSE across the intervening `CALL` —
the rec-binding lookup is idempotent (the IC softens but doesn't remove the
instructions).

**(c) Orphaned dead functions.** The de-thunk inlined the `(n-1)`/`(n-2)`
thunk bodies but left the now-unreferenced thunk functions in the module —
`deadFunctionElim` isn't sweeping post-de-thunk residue. One-time, easy.

**(d) General stack motion (the ceiling) — the real #1, MEASURED 51%.**
Pervasive `SET_LOCAL`/`GET_LOCAL`/`GET_UPVALUE` round-trips from A-normal-
form lowering. **51.4% of dynamic dispatch on hello, 51.2% on firefox**
(49.4% static both) — workload-invariant. Only a register VM / wider-operand
superinstructions structurally remove it (§4). Register-pressure sizing
(QUANTIFICATION §2): ideal file is **8–16 registers** (99.4–99.8% of
functions never spill; mean pressure 1.78 vs declared nLocals 3.5). A
cheaper interim: a slot-reuse/liveness allocator that reclaims the ~1.8
over-reserved slots/fn without the full register VM.

**Verified non-holes (do NOT chase):** `+` → `OP_STR_CONCAT` already has a
2-int fast path; fib's arg thunks (already de-thunked).

---

## 3. Strategic read: pure-recursion wall is essentially done

`fib33` at **2.0×** is the interpreter ceiling (~1.5–2×, per the architecture
review). The §2(a–c) peepholes might take it to ~1.5× and help `fold`
(4.3×) similarly — **bounded, worth doing, diminishing.** Past that, wall
returns shrink fast.

The bigger gap is **memory: 4.4–5.3× peak RSS** — and per
[[memory-first-class]], memory has higher slope per engineering-day. The GC
track is paused (`GC_PAUSE_2026-05-29`, after Immix projected below SHIP),
but with wall approaching the floor the wall-vs-memory tradeoff should be
re-evaluated now. **Recommendation:** harvest §2(a–c) (≈1–2 wk of contained
codegen wins), then **pivot back to memory**, where the remaining slope is.

> **UPDATE 2026-06-05.** §2(a–c) are harvested (SHIPPED block in §2; real-
> corpus-neutral for (a)/(b), so the wall floor barely moved on real
> workloads — as the QUANTIFICATION predicted). The remaining *codegen*
> lever per the real-corpus ranking is the **#2 rec-binding machinery (9.2%)
> via DAG-`let`/formals demotion** — `let`/formals whose bindings form an
> acyclic dependency DAG don't need the synthetic rec-attrset +
> `REC_BINDING_SLOT_REF` indirection + per-binding thunks; demote to direct
> `GET_LOCAL`s (extends the shipped non-rec-`let` demotion to the acyclic-
> multi-binding case). This is the one §2 lever the real corpus says is worth
> building; it feeds #1 (less stack motion) AND memory (fewer of the 650K
> thunks). After it, the §1 wall floor is reached on real workloads and the
> **memory pivot** is the slope.
>
> **UPDATE 2026-06-05 (later) — #2 SHIPPED (`2d752453c`); CODEGEN TRACK
> EXHAUSTED.** The DAG demotion landed (~70% of recursive lets demote;
> `--core` 19/19). Real-corpus (hello): **attrset allocations −8.6%**,
> `ATTRS_REC_SET` −13.3% — but the RBSR indirection is *traded* for direct
> upvalue access (≈net-neutral dispatch), so **wall + peak-RSS are neutral**.
> So all three contained levers (§2a, §2b, #2) are **wall-neutral on real
> workloads** — exactly the QUANTIFICATION's prediction that the *only* wall
> lever is the structural #1 register VM (§4, gated). The DAG demotion's
> value is **allocation churn** (−8.6% attrsets, less GC pressure), a memory-
> adjacent win. **Conclusion: the contained codegen track is harvested and
> the real-workload wall floor is reached.** The remaining slope is **memory**
> (per [[memory-first-class]]) — the pivot is now the clear next direction;
> the register VM (§4) stays gated unless the §3 tradeoff flips back to wall.

---

## 4. The register-VM decision — gate it, don't start it

The stack-motion ceiling (§2d) is the only thing blocking sub-1.5× wall, and
a register VM is the only structural fix — but it's a multi-KLoC investment
against a hard ~1.5–2× interpreter ceiling. `analyze-operands.py` D1 already
informs it: all adjacent `GET_LOCAL;GET_LOCAL` are *different-slot* (→ an
operand-parameterized 2-slot push, i.e. the register direction, not a DUP
peephole). **Gate:** don't begin the register VM until (a) §2 peepholes are
harvested and re-measured, and (b) the §3 tradeoff still says wall > memory.
If memory wins (likely), the register VM waits.

---

## Lesson codified (the throughline)

The v3-eval **`--expr`** path skips `optimise()` and strictness; only the
`run.cc` / `nix eval --impure` (NIX_V3_DIRECT_EVAL) path is production. Any
"how many thunks / how fast / what's in the bytecode" claim taken through
`--expr` is wrong by construction. The disassembler is only as honest as the
pass sequence behind it — which is exactly why `--emit-bytecode` was made to
call `applyStrictnessPasses` (it was the divergence that misled this review).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
