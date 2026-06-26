# BiBOP B0.3 — GO/NO-GO projection result

**Date:** 2026-06-26  **Branch:** angerman/2.35-eval-profiling-v2
**Instrument:** `v3 BiBOP-projection` line (mark_sweep.cc, gated NIX_VM_STATS). Per-type
LIVE bytes → project the per-type-segregated+compacted arena footprint =
sum_lane ceil(liveBytes_lane / 16MB) blocks, vs the current mapped arena. Reclaim =
arena − footprint. Host-independent (cell/block counts). Pre-committed ship threshold:
**≥ 80 MB net firefox peak-RSS.**

## firefox.drvPath (full, representative), per mid-eval sweep

```
arena=101MB live=65MB | mixed-perfect 67MB(+34) | LANES 134MB(-34) | per-type-own 168MB(-67)
arena=151MB live=88MB | mixed-perfect 101MB(+50)| LANES 134MB(+17) | per-type-own 168MB(-17)
arena=235MB live=129MB| mixed-perfect 134MB(+101)| LANES 185MB(+50)| per-type-own 218MB(+17)
arena=352MB live=180MB| mixed-perfect 185MB(+168)| LANES 268MB(+84)| per-type-own 302MB(+50)  ← PEAK (peak-live)
arena=352MB live=148MB| mixed-perfect 151MB(+201)| LANES 185MB(+168)| per-type-own 218MB(+134) ← PEAK (low-live trough)
```

(LANES = the BiBOP plan: Closure/Thunk/Bindings/List/Pair own lanes + a shared cold lane
for Value/Env/Chars. Footprint already includes per-lane rounding.)

**Verdict: firefox PASSES the ≥80MB threshold** — LANES reclaim is **84 MB at peak-live**
(conservative; compaction fires when the live set is largest) and **168 MB at the
low-live trough** (opportunistic trigger). Both clear 80 MB.

## Two big caveats (honest)

1. **Perfect-recycling assumption.** The projection assumes each lane compacts to
   ceil(live/16MB) — i.e. the Immix recycling packs the live remainder with zero
   fragmentation. Real recycling is imperfect → realizable < 84 MB. The peak-live 84 MB is
   already the conservative live point but the OPTIMISTIC packing point. Net: firefox is
   **marginal-passing**, not comfortable.

2. **16 MB blocks are coarse for segregation at small-arena scale.** `seg-overhead (lanes
   vs mixed) = +84 MB` at peak: segregating into ~5-8 lanes, each needing ≥1 whole 16 MB
   block, wastes ~84 MB in partial tail blocks — so BiBOP captures only ~HALF of the
   168 MB theoretical (unachievable) mixed-perfect compaction. At firefox's 352 MB arena
   this fixed per-lane overhead is large; at M5's ~3 GB arena it is negligible. **DESIGN
   REFINEMENT flagged: smaller lane blocks (1-2 MB) would slash the rounding waste and make
   BiBOP strongly positive at small/medium scale too** — but that's a larger allocator
   change (the 16 MB block size + side-table sizing is baked in).

## M5 — NOT measured (IFD wall locally)

M5 (cardano-node.name) aborts on a missing IFD path locally (cardano-node-plan-to-nix-pkgs)
— it needs darwin-4 where the IFDs are built. The partial pre-abort projection (arena
≤151 MB) is unrepresentative. **M5's full ~3 GB arena would make the 16 MB-block rounding
negligible → almost certainly a strong GO**, but this MUST be confirmed on darwin-4 to
complete B0.3.

## Decision

firefox clears the pre-committed bar (84-168 MB ≥ 80 MB) under perfect-recycling, and M5
is expected stronger — so the projection is a **GO, but a MARGINAL/caveated one**: the
realizable win shrinks under imperfect recycling, and the 16 MB-block rounding caps the
small-workload benefit at ~half theoretical. Recommended before the multi-week B1+ build:
(a) confirm M5 on darwin-4 (expected strong), and (b) decide the lane block size (keep
16 MB = simpler but small-workload-marginal; or 1-2 MB lane blocks = bigger change but
strongly positive everywhere). This is a genuine cost/scope call for the user.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
