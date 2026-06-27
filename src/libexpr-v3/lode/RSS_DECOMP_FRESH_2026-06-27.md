# Fresh RSS decomposition vs TW — where v3's memory actually goes (2026-06-27)

Measured at HEAD 35d9a0276. maxRSS from darwin-4 (quiet host, N=5); arena + per-type +
Boehm + churn counters are DETERMINISTIC + host-independent (measured on laptop, confirmed
arena 1560MB laptop ≈ darwin-4). Answers "is the RSS gap leaner-cells, or something else?"

## M5 (cardano-node) — the RSS-critical workload

```
cold maxRSS (darwin-4, clean):  2973 MB        TW: 982 MB  → 3.03×
  arena         1560 MB (52%)   v3 cell heap
  elsewhere    ~1153 MB (39%)   C++ malloc: bytecode/CU/LambdaDescriptors + ImportCache
                                 + SQLite + ~360MB malloc fragmentation + binary ~310MB
  boehm         ~403 MB (13%)   FFI marshalling — RESERVED but ~all free (boehm_free=402.8)

arena 1560 MB allocated, by type (run-phase per-tag):
  thunks   659 MB (42%)  ← DOMINANT     closures 127 MB
  bindings 347 MB (22%)                 pairs    125 MB
  chars    135 MB                       lists     63 MB    values/envs ~0.5 MB
  → live ≈592 MB (B0.3); ~970 MB is DEAD/unreclaimed in the arena

churn: thunks 17.2M allocated, 6.3M forced → 63% NEVER forced
       nursery hit-rate 7% (93% of allocs overflow the 32MB nursery → straight to tenured)
```

## firefox — smaller, same shape

```
cold maxRSS: 678 MB    TW: 358 MB  → 1.89×
  arena 352 MB | boehm ~403 reserved (mostly free; resident ≪) | elsewhere small
  arena by type: thunks 117 MB, bindings 143 MB, pairs 30, chars 26, closures 15, lists 12
  thunks 2.88M allocated, 0.99M forced → 66% never forced; nursery hit 21.9%
```

## What this OVERTURNS and CONFIRMS

- **"Leaner cells" is NOT the lever.** v3's atomic `Value` is **8 B** — *smaller* than TW's
  16 B niche-tagged Value. Thunk is at its 24 B floor (FP-2). The cells aren't fat.
- **The arena is THUNK-dominated** (M5 thunks 659 MB / 42%). But the thunks are real
  laziness, not trivially-avoidable (#135 killed maybeThunk: only 0.7% are trivial
  var/const). 63% never forced = correct laziness TW also pays — v3 just stores it heavier
  (24B Thunk + 8B Value box + alignment + nursery copy vs TW's inline-in-16B-Value).
- **39% of M5 RSS is "elsewhere"** — the non-arena C++ heap (bytecode + CU +
  LambdaDescriptors + caches + ~360MB malloc fragmentation + binary). **This chunk alone
  (~1153 MB) exceeds TW's ENTIRE RSS (982 MB).** It is the structural cost of being a
  bytecode VM (a tree-walker compiles nothing) + cache/fragmentation — and it was
  under-weighted in the "live representation" framing.
- **~970 MB of the M5 arena is dead + unreclaimed** — the reclamation target the whole
  GC/BiBOP campaign proved unrecoverable (arena pins pages; moving churns peak).

## Verdict — re-confirmed with fresh, decomposed numbers

The RSS gap is genuinely multi-component, no single lever:
1. dead arena ~970 MB — reclamation DEAD (BiBOP + all GC levers killed)
2. thunk volume 659 MB — real laziness, near per-cell floor, count not trivially reducible
3. elsewhere ~1153 MB — bytecode-VM + cache + fragmentation tax (no tree-walker equivalent)
4. boehm ~403 MB — FFI floor (mostly reserved-free; resident ≪ nominal)

Beating TW on RSS needs progress on SEVERAL of these at once — the foundational program the
campaign named. The single most concentrated v3-specific cost is the thunk representation
(count × per-thunk bytes), which is also the CPU bottleneck (dispatch on force) — so a
strict-eval / unboxed-thunk effort is the one direction that would move BOTH axes, but it is
L3-hard (byte-id risk) and was the deferred conclusion, not a cheap lever.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
