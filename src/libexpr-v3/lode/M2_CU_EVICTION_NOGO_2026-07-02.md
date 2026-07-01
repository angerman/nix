# M2 (CU bytecode eviction) — GO/NO-GO = NO-GO; + bounded-memory interim conclusion

**Date:** 2026-07-02  **Branch:** angerman/2.35-eval-profiling-v2
**Gate:** M2.1 cold-CU instrument (mid-eval mark's referenced-CU set `MarkVisitor::walkedCUs_`
vs `importCache().cus`); reports evictable ("cold" = unreferenced) CU bytes per mid-eval sweep.

## Result — NO-GO: ~0MB evictable at peak

firefox (COMPLETES correctly under mid-eval GC; near-peak measurement), all 5 sweeps:
```
1/297 CUs cold, 0/35MB   1/314 0/36   1/316 0/36   1/593 0/41   1/596 0/41MB  (0% of CU-bytecode)
```
M5 (early sweeps; mid-eval GC too slow on M5 to complete — see note): 7/347 0MB, 8/827 0MB.

At every mid-eval safepoint (near peak) essentially EVERY cached CU is referenced by a live
thunk/closure — so CU eviction recovers **~0MB at peak**. The CU-bytecode (35–158MB) is fully
LIVE during eval (haskell.nix's package-set functions stay pinned in the in-progress graph);
CUs only go cold at teardown, far too late to lower peak. ⇒ M2.2/M2.3/M2.4 FALSIFIED (nothing
to evict). Validated: firefox byte-id ✓, --brute 22/22 (instrument gated NIX_VM_STATS).

## Bounded-memory INTERIM CONCLUSION — every non-arena lever is dead

The full M0+M1+M2 measurement sweep:
| lever | verdict |
|---|---|
| M1.A Boehm dereg | MOOT — arena-noroot already default; 403MB Boehm-native, unmap falsified (macOS) |
| M1.B SQLite cap | MOOT — disk cache REDUCES peak (615 vs 710); DB mmap'd/OS-bounded; in-proc 2MB |
| M1.C descriptor diag | DEFERRED — 87-site refactor for ~10MB, M2-subsumed |
| M1.D malloc-frag reclaim | FALSIFIED — pressure_relief=0MB (system libmalloc; nothing frees mid-eval) |
| M2 CU eviction | FALSIFIED — ~0MB evictable mid-eval (all CUs live at peak) |

So the "rest" bucket is genuinely LIVE-or-RESERVED (Boehm-reserved + live CUs + posPool/symtab
+ binary), none reclaimable mid-eval. **The ONLY remaining bounded-memory lever is M3 — the
arena** (M5: 1544MB of 2326), which is the never-munmaps wall the entire GC/BiBOP campaign
already found unrecoverable on peak. Bounded memory below current REQUIRES cracking the arena:
M3.1 (hard-cap free-list — but it fights the same wall; may cap only dead slack, not the live
peak) or the radical M3.2 (file-backed mmap'd arena — OS evicts cold live cells at page-fault
cost, the only design that bounds below the live set). There is no cheap non-arena win.

## Note (issue, not a correctness bug)

`NIX_V3_MIDEVAL_GC=1` is pathologically SLOW on M5 (>75s vs ~10s) — many expensive full marks
on M5's huge graph; the earlier "empty result" was the wall-time cap aborting, NOT a byte-id
divergence (firefox mideval completes correctly + byte-id). Opt-in only; a perf pathology of
mid-eval GC on huge workloads, not a production correctness bug. (Left as-is; mideval is opt-in.)

M2.1 instrument (importCacheColdBytes + MarkVisitor::walkedCUs()) retained gated as the
measurement tool. Plan: lode/BOUNDED_MEMORY_PLAN_2026-06-29.md.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
