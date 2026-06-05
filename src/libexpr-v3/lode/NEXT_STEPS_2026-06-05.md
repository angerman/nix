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

With thunks off the table, fib's `func 2` (~7M× in fib33) still wastes work,
in priority order:

**(a) Constant spill-and-reload — biggest, and general.** Every literal is
`LIT_INT k; SET_LOCAL s; … GET_LOCAL s` instead of pushed where needed. fib
does this 3× per call (`2` for `n<2`, `1` for `n-1`, `2` for `n-2`) →
~6 wasted ops/call. Not fib-specific: it's how the A-normal-form lowering
emits *every* literal operand. Peephole: a `SET_LOCAL` whose only use is a
`GET_LOCAL` of the same slot with no stack-clobbering op between → drop both,
keep the value on the stack. **Measure-first:** dump 3–4 workloads via
`--emit-bytecode`, count `LIT*;SET_LOCAL;…;GET_LOCAL` statically (extend
`analyze-operands.py` D2), confirm via `NIX_VM_OPCOUNTS`, pre-commit a
threshold before writing the pass.

**(b) Redundant rec-binding resolution.** fib resolves `fib` twice per call
(`GET_UPVALUE; REC_BINDING_SLOT_REF`). CSE across the intervening `CALL` —
the rec-binding lookup is idempotent (the IC softens but doesn't remove the
instructions).

**(c) Orphaned dead functions.** The de-thunk inlined the `(n-1)`/`(n-2)`
thunk bodies but left the now-unreferenced thunk functions in the module —
`deadFunctionElim` isn't sweeping post-de-thunk residue. One-time, easy.

**(d) General stack motion (the ceiling).** `nLocals=15`, pervasive
`SET_LOCAL`/`GET_LOCAL` round-trips. This is the ~48% stack-motion only a
register VM / wider-operand superinstructions structurally remove — see §4.

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
