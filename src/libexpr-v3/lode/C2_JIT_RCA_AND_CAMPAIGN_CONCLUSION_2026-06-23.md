# C2 (#137) JIT RCA + BEAT_TW campaign conclusion — 2026-06-23

Task #137: "RCA-first — confirm hot-body coverage + the JIT ceiling BEFORE the
multi-week build" (plan C2).  Plus the honest end-state of the whole campaign.

## JIT RCA — what the interpreter actually does (firefox.drvPath, exact counters)

`bench/profile-at-scale.sh` deterministic counters (exact, host-independent — the
macOS `sample` CPU-category pass couldn't attach over SSH, so this relies on the
exact opcode histogram + the prior-session on-CPU categories):

- **36.86 M opcodes executed**, of which ~52% are TRIVIAL stack ops:
  OP_GET_UPVALUE 15.9% + OP_GET_LOCAL 9.7% + OP_GET_LOCAL2 7.6% + OP_SET_LOCAL
  10.7% + OP_RETURN 7.8%.  Each pays a per-op dispatch — exactly what native
  codegen removes.
- OP_MAKE_THUNK 7.8% (2.88 M thunks), **65.7% of thunks UNFORCED** (the churn).
- nursery hit-rate 21.9%.
- On-CPU categories (prior profile, PROFILE_AT_SCALE): DISPATCH 7–23%, ALLOC ~20%,
  countDistinct ~16–17%, GC ≤7%, TLS ~6% (+ PARSE+LOWER 20–29% which is
  amortized warm).

## JIT ceiling — it NARROWS the gap but does NOT beat TW alone

The warm (production) gap is **2.49× firefox / 1.83× M5** (#132).  JIT attacks
DISPATCH (7–23% of on-CPU) and can inline some ALLOC fast-paths, but:
- it does NOT remove the allocations themselves (ALLOC ~20% — the cells still get
  built), the countDistinct chain-walk (algorithmic, ~16%), or the real primop/
  force work;
- removing dispatch optimistically takes warm 1.83–2.49× → **~1.5–2.2×** — a real
  narrowing, but still **> 1× (does not beat TW)**.

So JIT is a legitimate CPU-narrowing lever, not a "beat TW" lever on its own.

## JIT cost — shippable JIT REQUIRES the multi-week J3 build

The hot bodies allocate (MAKE_THUNK 7.8%, and the trivial loads feed allocating
expressions), so a shippable JIT needs J3 GC-safepoints (spill live v3 pointers at
allocation safepoints — the real-scavenger integration).  A pure-arith JIT (no
allocation) would game the benchmark (the fusion-kill / real-world-gains rule).
J0 (platform) / J1 (encoder) / J2 (byte-id codegen) / J3 (safepoint contract) are
all proven standalone (JIT_DESIGN_2026-06-19), but J3 real-scavenger integration +
value-stack ABI trampoline + compile-trigger + bail + darwin-4 grade is a
dedicated multi-week project.  RCA verdict: **deferred — the ceiling (narrows to
~1.5–2.2×, doesn't beat TW) does not justify starting the multi-week build as a
"beat TW" play; it's a future CPU-quality investment.**

## BEAT_TW campaign — honest end-state (8/9 todos resolved)

| # | lever | verdict |
|---|---|---|
| 131 | measurement harness | DONE (beat-tw-compare.sh) |
| 133 | M1 RSS decomp | DONE (arena 53% / MALLOC_SMALL 24% / Boehm 14%) |
| 135 | thunk-avoidance (maybeThunk) | **KILL** — 0.7% avoidable |
| 134 | ImportCache eviction | **KILL** — arena never releases pages |
| 136 | arena page-release (M3) | **KILL** — no whole-dead/sparse blocks |
| 138 | GC mark cost | moot (gated on M3) |
| 139 | MALLOC_SMALL CU-shrink | modest/foundational (descriptor repack + jemalloc) |
| 132 | WARM head-to-head | DONE — production gap CPU 1.83–2.49×, RSS 1.64–2.26× |
| 137 | JIT | RCA done; multi-week build deferred (ceiling-bounded) |

### The answer to "conceptually v3 should beat TW on both — we're doing something wrong"

We are NOT doing a single fixable thing wrong.  Measured conclusions:
- **RSS**: every reclaim lever is provably dead (eviction, mid-eval reuse, whole-
  block-free, evacuation) because the arena never returns pages AND the live set is
  too evenly distributed to free blocks.  The gap is the LIVE REPRESENTATION —
  v3's cells + per-CU structures + Boehm FFI are structurally heavier than TW's
  16 B niche-tagged Value (which inlines ints/bools/small-lists, captures whole
  Envs, and avoids 1.65 M thunks via maybeThunk).  M5's live arena ALONE (~1023 MB)
  exceeds TW's entire RSS (982 MB).
- **CPU**: the production gap (1.83–2.49×) is structural per-op interpreter cost +
  allocation volume.  JIT narrows dispatch but can't beat TW alone; allocation
  churn (65.7% unforced thunks) is the L3-hard lever (strictness/eager-eval,
  byte-id risk).

**Beating TW on both fronts requires FUNDAMENTAL architectural change** — a leaner
cell representation (TW-style niche-tagging / inlining), native codegen (JIT), and
lower allocation volume — NOT any of the remaining single levers.  This is a
long-horizon program, honestly scoped.  The cheap-lever search is complete: it
found 4 KILLs and 1 modest/foundational, all measure-first, all Rule-0 clean.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
