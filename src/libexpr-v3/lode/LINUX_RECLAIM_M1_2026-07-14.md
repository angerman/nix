# D3 / WS-6 M1 — Linux reclaim falsifier + v3 Linux port

**Date:** 2026-07-14
**Deliverable:** D3 of the CI-throughput goal (WS-6 M1, measurement only — no M2 build).
**Question → DECISION INPUT #2:** does the macOS "reclaim levers are dead" verdict transfer to Linux?
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## 0. Why this needed a port first

Every "page-release is dead" falsification in the project (R1 note `alloc.hh:2758`; `blocksFreed=0`; the Boehm-unmap and mid-eval-reuse KILLs) was measured on **macOS aarch64**, where `std::free` of a calloc'd ≥16 MiB block returns ~0 % resident (libmalloc caches large frees in its magazines) and `madvise(MADV_DONTNEED/FREE)` is a no-op. CI runs on **Linux**, whose glibc allocator behaves differently. The verdict had never been transfer-tested. Doing so required a working v3 build on Linux — which did not exist (the port was unfinished).

## 1. The Linux port (x86_64-linux, GCC 14.3 / glibc 2.40 / libstdc++)

`v3-eval` (and the WS-3 `--worker` mode) now **builds and runs on x86_64-linux**. The 279 initial GCC errors collapsed to **4 root causes**, all genuine cross-platform bugs (libc++/clang was silently lenient; libstdc++/GCC is correct) — every fix is a no-op on macOS (confirmed: macOS v3-eval rebuilds clean + `--brute` green):

1. **Missing standard includes** (~250 cascade errors): `closure.hh` had *no* includes yet declares `std::string`/`std::vector` members; `ir.hh` lacked `<memory>` for `std::shared_ptr`. libc++ pulls these in transitively; libstdc++ correctly does not. Added `<cstddef>/<cstdint>/<string>/<vector>` to closure.hh, `<memory>` to ir.hh. (The cascade: the missing `std::string` made `LambdaDescriptor::name`/`contextualName` fail to declare → 243 "no member 'name'" errors in vm.cc.)
2. **Clang-only diagnostic pragmas** (44 errors): `vm.cc`/`value_serialize.cc`/`disasm.cc` suppress `-Wswitch-enum` with `#pragma clang diagnostic ignored`, which **GCC ignores** — so GCC re-enforced `-Werror=switch-enum`. Converted to `#pragma GCC diagnostic` (honored by *both* compilers; the one genuinely clang-only warning, `-Wunneeded-internal-declaration` in `lexer.l`, was left untouched).
3. **Ref-qualifier overload** (`ir_dump.cc:63`): `str() &&` cannot legally coexist with an unqualified `str() const` — clang accepted it, GCC (correctly) rejected. Ref-qualified the const overload (`const &`).

Functional sanity on Linux: `1+2`→`3`, `genList … 1000`→`1000`, `--worker` streams `3` / `"ab"`.

## 2. M1 measurement — the reclaim MECHANISM on Linux

`scratchpad/reclaim-probe.cc` allocates 40 × 16 MiB (640 MiB), faults every page resident, then releases via the three paths the v3 arena uses, reading `/proc/self/statm` RSS before/after:

| path (arena analogue)                              | peak   | after release | **returned** |
|----------------------------------------------------|--------|---------------|--------------|
| (a) `calloc` + `std::free`  (`freeHugeBlock`)      | 642 MB | 2 MB          | **640 MB**   |
| (b) `mmap` + `munmap`  (`mapArenaBlock` release)   | 642 MB | 2 MB          | **640 MB**   |
| (c) `mmap` + `MADV_DONTNEED`  (in-place page drop) | 642 MB | 2 MB          | **640 MB**   |

**All three return ~100 %** on Linux x86_64 / glibc 2.40 — the exact opposite of the macOS R1 finding (~0 % for (a), no-op for (c)).

## 3. DECISION INPUT #2

**The macOS "reclaim is dead" verdict does NOT transfer to Linux.** At the mechanism level, freeing a dead 16 MiB arena block returns its full resident footprint to the OS on Linux — via `std::free` (the *already-default, ungated* huge-block sweep at `mark_sweep.cc:2493`), via `munmap`, and via `MADV_DONTNEED`. Against the PRD M1 threshold (≥150 MB reclaimable → M2 fundable; <50 MB → verdict transfers), the mechanism returns 640 MB ⇒ **M2 is FUNDABLE on Linux.**

**Caveat — the remaining risk is block-death DENSITY, not the mechanism.** The macOS sweep observed `blocksFreed=0` for regular blocks because live cells cluster 25–75 % per block (blocks rarely go *fully* dead), which is a host-INDEPENDENT property of the arena's live/dead distribution. So on Linux the *mechanism* works, but the *actual* per-workload peak-RSS win still depends on how many blocks become fully dead. Two things make Linux strictly better than the macOS result even so:
- The **huge-block** path (single >4 MiB allocations → dedicated blocks, `kHugeCutoff`) frees on a per-object basis, and its `std::free` was returning 0 on macOS purely because of libmalloc — on Linux it returns 100 %. That reclaim was being lost to the allocator, not to density.
- The **mmap + MADV_DONTNEED** in-place path (currently dead-gated) can drop resident pages of *partially* dead blocks without needing whole-block death — and it works on Linux (row c). This sidesteps the density limit entirely for the "recycle for reuse" case and is the more promising M2 lever than whole-block-free.

**Recommended M2 (if funded):** (i) confirm the default huge-block sweep returns RSS on a real Linux M5 eval (fast, now that v3 builds there); (ii) prototype the mmap-backed arena blocks + periodic `MADV_DONTNEED` of sparse blocks on Linux — the row-(c) result says this reclaims where macOS could not. Pre-committed gate unchanged: ≥150 MB M5 peak-RSS reduction on Linux, `--brute` green, CPU ≤ +3 %.

**What is NOT claimed:** a full M5-on-Linux peak-RSS delta (that is M2/measurement, needs the cardano flake + store on the Linux host). M1's job was to falsify "the mechanism is dead everywhere" — done: it is a macOS-specific allocator artifact, not a portable truth.
