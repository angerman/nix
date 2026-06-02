# R1 deliverable — page-release spike result (the cheap falsifier)

**Date:** 2026-06-02
**Status:** R1 COMPLETE — **PASS via `munmap`**, with a spiral-explaining
finding: the *current* whole-block-free path (`std::free`) returns **0%**
of freed bytes to RSS on macOS aarch64.
**Spike:** `bench/page-release-spike.cc` (standalone; argv-selected
mechanism, fresh process each, mach `resident_size` / `/proc/self/statm`).
Pre-committed thresholds (POST_F4_GC_GOAL §4): ≥80% returned = PASS,
<30% = deepest blocker.

Companion: [`POST_F4_MEMORY_PROFILE_2026-06-02.md`](POST_F4_MEMORY_PROFILE_2026-06-02.md)
(R0), [`IMMIX_NOFL_DESIGN_2026-06-02.md`](IMMIX_NOFL_DESIGN_2026-06-02.md) (R2 design).

---

## 1. Result (macOS aarch64, 16 MB blocks = Arena::kBlockSize)

| Mechanism | freed | RSS before→after | returned | verdict |
|---|---|---|---|---|
| `free` (calloc + `std::free`) | 512 MB | 1026→1026 | **0%** | **BLOCKER** |
| `munmap` (mmap + munmap) | 512 MB | 1025→513 | **100%** | **PASS** |
| `madv_dontneed` | 512 MB | 1025→1025 | **0%** | BLOCKER |
| `madv_free` (immediate) | 512 MB | 1025→1025 | **0%** | BLOCKER (lazy) |
| `subblock` madvise(DONTNEED, mid-50%) | 512 MB | 1026→1026 | **0%** | BLOCKER |

Deterministic across reruns; scales (N=128 / 2 GB: munmap 1024/1024 = 100%,
free 0/1024). RSS measured via the same primitive as `limits.cc`.

## 2. What this explains and prescribes

**This explains the 11-failure GC spiral.** `alloc.hh::freeWholeBlock`
(the only block-release path today) calls `std::free` on `std::calloc`'d
16 MB blocks. On macOS, libmalloc **caches freed large allocations in its
magazines instead of `munmap`-ing them** → RSS never drops. Every prior GC
that "reclaimed" a block via the free-list / freeWholeBlock path was
logically correct but **RSS-invisible**. The pin was never the GC algorithm
— it was the allocator's release primitive.

**Prescription for R2:**
1. **Arena blocks must be `mmap`'d and released via `munmap`** (after
   `GC_remove_roots`), NOT calloc+free. This is the single change that
   converts "logical reclaim" into "RSS reclaim". `munmap` is portable
   (Linux returns RSS for it too; on Linux `free` of >128 KB and
   `MADV_DONTNEED` also work, so munmap is the common denominator).
2. **`madvise` does NOT return RSS on macOS** (DONTNEED is a no-op for
   anon RSS; FREE is lazy/under-pressure-only). Therefore **sub-block /
   line-level reclaim cannot lower RSS on macOS** — it only aids
   allocation *reuse*. **RSS reduction requires freeing WHOLE blocks.**
3. **⇒ Evacuation is load-bearing (not optional).** For the scattered-dead
   profile R0 measured (M5 0% fully-dead blocks despite 6.22 GB freeable),
   only Immix's opportunistic **evacuation** — concentrate live → empty
   whole blocks → `munmap` — can return that RSS. This triply confirms
   Gate C's "evacuation load-bearing" verdict (R0 §2 + R1).

## 3. Honest limits
- macOS aarch64 only (the strictest platform here). Linux x86_64 untested
  on this host, but `munmap` works there too and Linux's `free`/`MADV_DONTNEED`
  are *more* forgiving — designing for whole-block `munmap` satisfies both.
- `MADV_FREE` may reclaim under real memory pressure (the immediate 0% is
  the no-pressure case). Not relied upon — `munmap` is deterministic and
  the watchdog cannot wait for pressure.
- The spike tests the platform primitive in isolation; wiring `munmap` into
  the live arena (block lifecycle + Boehm root dereg + the active-block cur/
  end invalidation) is R2 work, but the mechanism is now proven.

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
