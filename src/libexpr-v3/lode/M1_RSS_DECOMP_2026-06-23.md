# M1 — v3 RSS decomposition (M5, darwin-4, cache-off) — 2026-06-23

Proper profile (RSS time-series + vmmap near peak), not guesses.  Task #133.

## Peak is at the END (monotonic), not a transient

RSS time-series (0.25 s samples) over M5 v3 eval: monotone 1→154→322→431→676→857→
1139→1714→2239→2300→2541→2791→**peak 2911 MB @ t=12.5 s (end)**.  So the peak ≈ the
final state — arena + Boehm + C++ structures all in it.  ⇒ arena reclaim CAN help
the peak (it is part of it); the peak is NOT a fetch/parse transient.

## vmmap region breakdown near peak (resident)

| region | resident | what |
|---|---|---|
| MALLOC_LARGE | **1543 MB (53%)** | the v3 ARENA (98×16 MB calloc'd blocks) |
| MALLOC_SMALL (live+empty) | **~690 MB (24%)** | C++ malloc: per-CU bytecode/IR/ICs/PosTables (thousands of .nix files) + ~360 MB FRAGMENTATION ("empty" freed-but-resident) |
| Boehm heap | ~402 MB (14%) | TW Values via the FFI |
| __TEXT/__LINKEDIT/__OBJC | ~310 MB (11%) | nix binary code (FIXED, shared, not reducible) |

Fixed overhead (trivial `1+1` eval): v3 31 MB / TW 28 MB — so the M5 footprint is
almost entirely workload-driven, not baseline.

## Key conclusions (re-prioritize the plan)

1. **The ARENA (1543 MB) is the #1 RSS lever — it alone exceeds TW's ENTIRE 982 MB
   RSS.**  Arena is thunk-dominated (637 MB thunks at peak-live, prior data).
   ⇒ **thunk-avoidance (#135) is the top RSS lever AND the top CPU lever** (ALLOC
   ~20% on-CPU) — do it first.  ImportCache eviction (2851 pinned results) ALSO
   attacks the arena (it pins Value subgraphs IN the arena, not "elsewhere") —
   un-pinning lets the arena reclaim them.
2. **MALLOC_SMALL (~690 MB) is the #2 lever** — NOT a single cache; it's the C++
   per-CU structures (bytecode + IR + attrSelect/recSlot ICs + PosTables for
   thousands of cached files) PLUS ~360 MB malloc fragmentation.  Levers: free IR
   after lowering (keep only bytecode); compact/evict cold CUs; malloc fragmentation
   (try `MALLOC_*`/madvise or jemalloc).  Needs a sub-RCA (heap profiler) to split
   live-structures vs fragmentation before attacking.
3. **Boehm (402 MB)** = TW Values via FFI; reduce by cutting FFI/TW-Value creation
   (V3-NATIVE-ward) — separate track.
4. Binary code (310 MB) is fixed — not a lever.

## Corrected plan deltas

- #135 (thunk-avoidance) PROMOTED to #1 (hits the arena = #1 RSS AND CPU = #1 gap).
- #134 (was "ImportCache eviction = elsewhere") RE-SCOPED: ImportCache eviction
  attacks the ARENA (un-pin); ADD a MALLOC_SMALL sub-RCA + CU-structure / malloc-
  fragmentation lever.
- #136 (mid-eval) confirmed: the peak IS arena-inclusive, so mid-eval's −13.6% M5
  RSS is real (median) — but +52% CPU keeps it opt-in; revisit only after #135.

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0*
