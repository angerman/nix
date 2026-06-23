# M4/C1 — thunk-avoidance (maybeThunk-equivalent) RCA — 2026-06-23

Task #135.  Measure-first (Rule 0): does v3 over-create *trivial* thunks that TW's
`maybeThunk` avoids (bare variable refs → return the slot; constants → return
`&this->v`)?  If so, avoiding them cuts both arena RSS and ALLOC-CPU.

## Instrument

`NIX_V3_THUNK_BODY_STATS=1` (gated, vm.cc `OP_MAKE_THUNK`) categorizes EVERY thunk
v3 creates at RUNTIME by its body's first two ops:

- **alias(var)** — `OP_GET_LOCAL|OP_GET_UPVALUE ; OP_RETURN` — a pure variable
  alias (exactly TW's `maybeThunk` ExprVar case: it returns the slot, no thunk).
- **const** — `OP_LIT_* ; OP_RETURN` — a constant (TW's `maybeThunk` constant
  case: returns `&this->v`).
- **force-alias** — `OP_GET_*_FORCE ; OP_RETURN` — forces an existing var then
  returns (NOT maybeThunk-avoidable; forcing is real demand work).
- **real** — anything else (genuine deferred subcomputation; TW thunks these too).

`alias + const` = the AVOIDABLE fraction (what a v3 maybeThunk-equivalent removes).

## Result — the maybeThunk hypothesis is FALSIFIED

| workload | total thunks | alias(var) | const | force-alias | **real** | **AVOIDABLE** |
|---|---|---|---|---|---|---|
| firefox.drvPath | 2,880,002 | 15,259 (0.5%) | 3,828 (0.1%) | 0 | **2,860,915 (99.3%)** | **19,087 (0.7%)** |
| M5 cardano-node.name | 2,085 | 10 (0.5%) | 4 (0.2%) | 0 | **2,071 (99.3%)** | **14 (0.7%)** |

(firefox is the clean representative case: 2.88M thunks from pure-v3 lazy nixpkgs
eval, no IFD.  M5 `.name` under `NO_NATIVE_CALL_FLAKE` routes most work through
TW/IFD so its v3 thunk count is tiny — but the RATIO is IDENTICAL: 0.7% avoidable.)

**Only 0.7% of v3's thunks are the trivial forms TW's `maybeThunk` avoids.**  v3's
IR optimizer (opt_dce / opt_beta_reduce / opt_inline / opt_const_fold — all
default-on) ALREADY eliminates trivial alias/const thunks at COMPILE time; what
survives to runtime is 99.3% real deferred work.  **There is no maybeThunk gap to
close.**

## Why the "2.88M v3 vs 2.15M TW" framing was misleading

The prior count (v3 2.88M vs TW 2.15M; TW "avoids 1.65M via maybeThunk") suggested
v3 over-thunks.  But:

1. TW's 1.65M `maybeThunk`-avoided positions correspond to v3 thunks the **opt
   passes already removed** — they never reach `OP_MAKE_THUNK`.  The 0.7% residual
   is the only overlap left.
2. The v3-vs-TW thunk-COUNT excess (~730K) is **real deferred subcomputations**,
   not trivial aliases.  At 24 B/thunk that excess is **≤ 17 MB of arena** —
   negligible vs the 1543 MB M5 arena (M1).  Thunk-COUNT avoidance is **not** a
   meaningful RSS lever.
3. The 637 MB thunk arena (M5 peak-live, prior data) is REAL laziness — necessary
   for correctness; TW carries the same thunks (at 16 B in its Boehm heap).  The
   only thunk-SIZE lever (24 B → smaller) is already at the **24 B floor**
   (FP-2 complete, project memory) — TW's 16 B is `{Env*,Expr*}`; v3's 24 B header
   is the validated minimum.

## VERDICT — #135 maybeThunk-style avoidance: KILL

Measure-first falsified the lever.  Re-prioritization:

- **The real arena RSS lever is the LIVE SET, not the thunk count.**  What pins
  arena Value subgraphs is the ImportCache (2851 pinned import results, M1) →
  **#134 ImportCache eviction is the top arena RSS lever**, not thunk-avoidance.
- **The real thunk lever is CHURN, not count.**  Profile data: 62–67% of allocated
  thunks are NEVER forced (allocated-vs-forced, PROFILE_AT_SCALE).  Those wasted
  allocations cost ALLOC-CPU — but they are "real" by body shape (genuine deferred
  exprs that simply go undemanded), so removing them needs strictness analysis /
  eager-eval of provably-cheap-and-needed exprs, with byte-id + semantics risk.
  This is the L3-hard churn lever already flagged + deferred (project memory).  It
  is a CPU lever, NOT a maybeThunk lever.
- **The structural CPU lever stays JIT (#137)** — the per-op interpreter overhead,
  not thunk count.

Instrument retired in the same commit (Rule 0: falsify → delete code + gate).

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
