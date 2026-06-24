# What would a proper safepoint foundation yield? — estimate — 2026-06-24

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.

A "safepoint foundation" = precise stack maps + GC safepoints across the nested
dispatch loop, so live v3 pointers held in re-entrant C++ primop frames are *precise
and movable* (not conservatively pinned). It is the single shared prerequisite for the
two highest-ceiling levers: (a) a **compacting mid-eval GC** (RSS) and (b) **native
codegen** (CPU). This estimates the yield of each, grounded in measured data.

Baseline (production warm, darwin-4): firefox 2.46× CPU / 1.64× RSS; M5 1.82× / 2.26×.
TW absolutes: firefox 0.74s/358MB; M5 3.60s/982MB. v3: 1.82s/586MB; 6.56s/2218MB.

## (a) RSS yield — compacting mid-eval GC

Basis: the live-fraction data (`LIVE_FRACTION_SPIKE`, `L_TIME_SERIES_DATA`): L = 0.41–
0.69 at peak (firefox arena 352MB cold, ~157MB live → ~45% live; HNE L_min 0.41). A
full sliding mark-**compact** (not opportunistic Immix evacuation — that was killed by
"blocks 25–75% live, no sparse blocks", but full compaction ignores per-block density)
reclaims the dead fraction. It only addresses the **arena** (~53% of M5 RSS); the
non-arena buckets (MALLOC_SMALL CU/bytecode metadata + ~360MB jemalloc fragmentation;
Boehm FFI ~402MB; binary ~310MB) are untouched.

| | now | compacted | residual driver |
|---|---|---|---|
| firefox RSS | 1.64× | **~1.34×** | non-arena + heavier live rep |
| M5 RSS | 2.26× | **~1.5–1.7×** | "live arena alone ≈ TW total" + CU metadata + Boehm |

Hard ceiling (from BEAT_TW): even *perfect* arena reclaim leaves firefox 1.34× and M5
≈ TW-on-arena-alone (so >1× once non-arena is added). **Compaction narrows RSS by
~0.3–0.6× of the ratio but does NOT beat TW** — because v3's *live* representation
(24B thunks, App-memo pairs, the CU/bytecode metadata that a tree-walker simply does
not have) is heavier than TW's whole heap.

## (b) CPU yield — native codegen

Basis: the JIT RCA (`C2_JIT_RCA`) + the opcode profile (~44% of executed ops are
GET/SET_LOCAL/UPVALUE shuffle; DISPATCH 7–23%). Native codegen with register
allocation removes the shuffle + dispatch + decode, but the **real-work floor remains**:
ALLOC (~20–27%), thunk force, primop/FFI bodies, GC. v3 still allocates *more* and
*heavier* than TW, and that floor is heavier than TW's.

| | now | native codegen | residual driver |
|---|---|---|---|
| firefox CPU | 2.46× | realistic **~1.7–2.0×**, aggressive ~1.3× | ALLOC volume + churn + GC |
| M5 CPU | 1.82× | realistic **~1.4–1.6×**, aggressive ~1.2× | same |

The JIT RCA's pre-committed ceiling was 1.5–2.2× ("narrows, does not beat <1× alone;
ALLOC + countDistinct + real work remain; pure-arith speedups = gaming the benchmark"
since nixpkgs is alloc/dispatch-bound, not arithmetic-bound).

## The tension (the non-obvious part)

**The two yields trade against each other.** A compacting GC that reclaims aggressively
must mark + relocate + pointer-fixup all live cells, repeatedly, during the eval — that
is CPU cost that *eats into* the native-codegen CPU win. Conversely, collecting rarely
(to protect CPU) lets the arena high-water mark rise (worse RSS). TW (Boehm, stop-the-
world on heap pressure) sits at one point on this Pareto frontier. So you cannot simply
stack the (a) and (b) ceilings: **"beat TW on BOTH axes simultaneously" is strictly
harder than either per-axis ceiling**, because the GC frequency that minimizes RSS is
not the one that minimizes CPU.

## Net

A fully-exploited safepoint foundation moves v3 from
**firefox 2.46×/1.64× → ~1.7–2.0× CPU / ~1.34× RSS** and
**M5 1.82×/2.26× → ~1.4–1.6× CPU / ~1.5–1.7× RSS.**

That is a large narrowing — v3 becomes *competitive* (within ~1.3–2×) on both axes —
but on the available evidence it does **not beat TW on either axis alone**, and the
RSS/CPU tension means the simultaneous frontier is worse than the per-axis numbers.

To actually cross <1× would require the foundation **plus** attacking the residual:
the heavier live representation (container shrinks — SoA Bindings, the 24B-cluster /
kAlign work) AND the allocation volume (the 62–67% thunk churn, which the M4 RCA showed
is ~99.3% irreducible lazy data — the hardest residual).

## Cost & strategic framing

Effort: multi-month. Precise movable roots through re-entrant primop frames (spill live
v3 ptrs at alloc points = task #152/C4.1), safepoint polls on back-edges/alloc, a
sliding mark-compact integrated with the existing precise walker, and (for the CPU
half) a codegen backend + deopt + the value-stack ABI trampoline (#153).

The honest framing: the safepoint foundation is **necessary but not sufficient**. It is
the *only* investment that moves both axes materially and is the prerequisite for every
lever with a real ceiling (reclamation, native codegen) AND for the long-horizon vision
items (incremental/salsa eval, parallel eval — both need precise, movable roots too).
But as a "beat TW" play in isolation it yields *competitive, not winning*. The decision
is therefore strategic: it is worth it if the goal is "a modern movable-GC + native-
codegen substrate that is competitive with TW and unlocks incremental/parallel eval";
it is NOT a guaranteed path to <1× on RSS, where v3's heavier live representation +
bytecode metadata is a structural disadvantage a tree-walker doesn't carry.
