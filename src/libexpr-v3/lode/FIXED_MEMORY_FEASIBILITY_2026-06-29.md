# Fixed/bounded-memory eval — feasibility (2026-06-29)

Reframe: not "beat TW's peak" (the failed campaign) but "make v3's RSS = live-set + BOUNDED
overhead, capped + workload-independent, accepting slightly higher CPU." Two Explore agents
mapped the bytecode/CU lifetime and the non-arena/cache/boehm footprint.

## The recurring WALL: the v3 arena never munmaps

Every prior RSS lever died here: ImportCache-results eviction (+52–215MB WORSE, measured),
mid-eval in-place reuse (no peak drop), BiBOP (churns peak). Reason: the arena is a page
reservation that never returns pages to the OS, so dropping a *reference* can't lower RSS —
and re-allocating fresh grows PAST the prior peak. So "reclaim to lower peak" is dead for the
arena. For FIXED memory the move is CAP-not-reclaim (prevent growth), not release.

## Do we GC the bytecode cache? NO — and that's the fresh, UN-WALLED lever

Finding (agent A): the compiled code — one `CompilationUnit` per imported .nix file (bytecode
+ constants + LambdaDescriptors + inline caches) — is **malloc-deserialized and held in a
process-static `importCache().cus` deque for the WHOLE eval** (~700MB on HNE; large on M5's
IFD-generated package set). It is NEVER evicted. The only LRU (`NIX_V3_IMPORT_CACHE_MAX_ENTRIES`)
evicts the *results* map (arena Value graphs — falsified, hits the wall), NOT the CUs. The
experimental AOT mmap cache (`NIX_V3_AOT_CACHE_FILE`) still deserializes into malloc vectors —
it saves disk I/O, not resident memory.

KEY: the CU bytecode is MALLOC, not arena → evicting it actually FREES memory. **It is NOT
subject to the arena-never-munmaps wall** that killed every other RSS lever. So the user's
instinct ("do we need to hold the bytecode whole-eval / mmap as needed") points at a
genuinely un-explored, un-walled lever:
  - **Evict cold CUs + re-load from the disk cache on demand** — trades CPU (re-deserialize)
    for bounded resident bytecode. Achievable; the disk cache already holds them.
  - BLOCKER: CU-lifetime. Live thunks/closures hold `desc->cu` backpointers (+ the deque is
    used for stable addresses). A CU is evictable only when nothing live references it →
    needs ref-tracking (refcount per CU, or evict only fully-forced cold imports).
  - Zero-copy mmap (true "mmap as needed") is harder: blocked by deserialize-into-vectors +
    runtime-MUTABLE inline caches (attrSelectCache LRU) + per-load symbol/PosIdx remap. That's
    a position-independent on-disk format redesign. Evict+reload is the pragmatic version.

## Non-arena bounding levers (the user's target — mostly un-walled)

| bucket | M5/HNE | boundable? | how | wall? |
|---|---|---|---|---|
| CU bytecode (malloc) | ~700MB HNE | YES (fresh) | evict cold CUs + reload; CPU trade | NO (malloc, not arena) |
| SQLite page cache | ~270MB HNE | YES | `cache_size` pragma (not yet exposed) | NO |
| Boehm reservation | ~403MB (~all free) | YES | arena DEREGISTRATION (remove GC_add_roots → Boehm stops conservatively pinning free pages); ~1–2 days, Stage-6 prereq, not landed | NO |
| posSnapshotPool / symbolTable / stringContext | ~70–80MB | NO | correctness-needed, write-once | — |
| malloc fragmentation | ~360MB M5 | platform | jemalloc decay / madvise | — |

## The arena (the central, walled problem)

M5 arena 1560MB = live ~592 (irreducible floor; anonymous, needed; only leaner cells shrink
it) + dead ~970 (the wall). To bound the dead slack WITHOUT munmap: a HARD-CAP free-list —
refuse to map a new block while a reusable dead cell exists, force-reuse instead. Untested as
a hard cap (the existing reuse fires at mid-eval sweeps, AFTER the high-water; aggressive
triggering measured WORSE under BiBOP due to moving+frag). The non-moving hard-cap policy is
the one un-tried arena idea; cost = frequent sweeps (CPU) — exactly the CPU-for-memory trade
the goal accepts. Uncertain it beats the "pages stay mapped" wall.

RADICAL option for true fixed memory: a FILE-BACKED (mmap'd) arena → the OS evicts cold live
cells to the backing file → resident bounded to the working set, at page-fault I/O cost. The
only way to bound BELOW the live set. Big redesign; the "mmap as needed" idea applied to the
arena itself.

## Verdict

Partially achievable, and the user's framing found the un-walled lever the campaign never
tried. Realistic M5: bound the non-arena (CU evict + SQLite pragma + Boehm dereg ≈ 1.3GB →
~300MB) ⇒ **3.1GB → ~1.8GB, capped + workload-independent** — a real "fixed memory" win even
above TW. Getting BELOW TW (982MB) additionally needs the arena dead-slack cap (hard, the
wall) + eventually leaner cells (foundational). The bytecode/cache bounding is the
high-value, buildable, un-walled first step.

Measure-first plan (pre-committed bound target before building):
  1. quantify CU bytecode resident (importCacheBytecodeBytes) + SQLite + Boehm per workload;
  2. prototype CU evict+reload with CU-refcount (the un-walled lever) → does it cap the
     bytecode bucket byte-id, at what CPU cost?
  3. expose SQLite cache_size; land arena deregistration (Boehm);
  4. (separate, hard) arena hard-cap free-list OR file-backed arena.
Gate: a pre-committed RSS CEILING the eval stays under across firefox/M5/HNE, at ≤X% CPU.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
