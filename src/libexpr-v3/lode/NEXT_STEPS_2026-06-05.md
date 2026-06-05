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
and it will keep going wrong. There are three divergent pipelines, and only
one is real:

| Path | `optimise()` | `applyStrictnessPasses` | `NIX_VM_STATS` | `V3_TIMING` |
|---|---|---|---|---|
| `v3-eval --expr` | ✗ (skipped by design) | ✗ | ✓ dumps | ✓ but `run=` mis-brackets |
| `v3-eval --emit-bytecode` | ✓ | ✓ (just fixed) | n/a (no run) | n/a |
| **`nix eval` + `NIX_V3_DIRECT_EVAL` (PRODUCTION)** | ✓ | ✓ | ✗ not dumped | ✗ |

So today you **cannot get a production-faithful alloc count or timing from
one command**: `--expr` over-counts (no opt), `nix eval` under-instruments.

**Actions:**
1. Add a `v3-eval` mode that runs the *exact* `run.cc` sequence
   (`optimise → applyStrictnessPasses → computeFreeVars → compile → run`)
   AND dumps `NIX_VM_STATS` + `V3_TIMING`. (Implemented alongside this doc as
   `v3-eval --optimize`; validate fib's thunk count drops 635621 → ~0.)
2. Fix **`V3_TIMING`'s `max(run=)` mis-bracket** — it showed `run=0.6ms` for
   a 0.82s fold; the re-entrant primop path escapes the timer, so the
   eval-hot column lies post-eval/apply.
3. Wire **`NIX_VM_STATS` into the `nix eval` / `runRootExpr` path** (today it
   only dumps from the `v3-eval` binary).

**Standing rule until this exists:** validate every bytecode/alloc claim
through `--emit-bytecode` (now strictness-complete) or the production runner
— **never `--expr`** (it skips `optimise()`).

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
