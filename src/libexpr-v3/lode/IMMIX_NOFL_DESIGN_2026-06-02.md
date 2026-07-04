# GC design decision: a modern Immix (Nofl-style) for v3

**Date:** 2026-06-02
**Status:** DESIGN DECISION + RESEARCH — the chosen GC direction for v3's non-moving branch is a **modern, precise Immix in the Nofl style** (Yang & Wingo 2025 / Whippet `mmc`). **R0-gated** per [`POST_F4_GC_GOAL_2026-06-02.md`](POST_F4_GC_GOAL_2026-06-02.md): committed as *the* design for the high-L / non-moving Gate-C outcome; R0 re-measurement still confirms the regime (and could surface low-L-trough copying or a cache-problem instead — see §7).
**Author:** session synthesis

Companion docs:
- [`POST_F4_GC_GOAL_2026-06-02.md`](POST_F4_GC_GOAL_2026-06-02.md) — the R0→family→implement goal; this doc is the concrete answer to its Gate-C "non-moving" branch
- [`GC_DESIGN_POST_CHENEY_2026-05-28.md`](GC_DESIGN_POST_CHENEY_2026-05-28.md) — the design-space survey (Immix = option A2)
- [`GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md`](GC_PRECISE_ROOT_FOUNDATION_2026-05-27.md) — Stages 1/3/5, the precise-root substrate Nofl needs
- [`TW_VALUE_ERADICATION_GOAL_2026-06-02.md`](TW_VALUE_ERADICATION_GOAL_2026-06-02.md) — F4 (bridge deletion) = the precondition; cleaner roots post-F4

---

## 1. The decision

**Implement a modern Immix — specifically the Nofl-style precise mark-region collector — as v3's tenured GC.** Not Cheney (falsified, 2× peak), not flat mark-sweep (worst on the page-release pin), not GHC's concurrent non-moving (its concurrency targets latency v3 doesn't have — §5.4). Nofl is the option that simultaneously satisfies v3's three constraints: high-L peak avoidance, variable-size Bindings, and the arena page-release pin.

This is **R0-gated** (per the goal): it is the chosen design *for the non-moving regime*. If R0's L(t) reveals deep low-L troughs (copying-at-trough reopens) or the excess lives in caches (not a GC problem), the decision revisits. But the committed direction for "v3 needs a non-moving GC" is **Nofl-Immix**.

---

## 2. Immix recap (the base)

Immix (Blackburn & McKinley, PLDI 2008) is a **mark-region** collector: bump-pointer allocation speed without copying's 2× space overhead, by marking/reclaiming at *region* granularity.

- Heap = **blocks** (32 KB) ÷ **lines** (128 B) → 256 lines/block.
- **Allocation** = bump-pointer into *holes* (runs of free lines) — fast like copying, but into recycled space, no copy.
- **Marking** marks live objects, derives line/block marks → blocks classified free / recyclable / full.
- **Reclamation** = sweep lines (a bitmap), not copy objects.
- **Opportunistic evacuation** = the one moving feature: when fragmented, copy live out of sparse blocks into dense ones during the mark pass, then free the emptied blocks wholesale.

Classic Immix marks lines *conservatively* (an object straddling a line boundary marks an overflow line) — an approximation Nofl removes.

---

## 3. What Nofl adds (the "modern" part)

**Nofl** ("No Free List") is Andy Wingo's precise mark-region collector — the space implementation under Whippet's `mmc` (mostly-marking collector). Reference: **Yang & Wingo, "Nofl: A Precise Immix" (arXiv 2503.16971, 2025)** + Wingo's Whippet blog series.

Refinements over Blackburn-McKinley Immix that matter for v3:

1. **Precise, via a metadata byte per granule.** Instead of conservative line-mark bits, Nofl keeps one **metadata byte per 16-byte granule** in a side table, encoding mark state + object-start + pinned/forwarded. This removes Immix's conservative line-overflow approximation — **exact**, given precise roots. (v3 has precise roots: Stages 1/3/5.)
2. **16-byte granule** — matches v3's `Value` size (16 B) and allocation alignment exactly. The metadata-byte-per-granule maps cleanly onto v3's existing allocation grain.
3. **In-place sticky-mark-bit generational mode** — generational collection *without a separate copying nursery*: the metadata byte's mark state is "sticky" across minor cycles, so young objects are the unmarked ones. This is the weak-generational answer (v3's young mortality measured 0.43-0.58) and could *subsume the opt-in Phase E nursery* rather than coexist with it.
4. **Optional evacuation/compaction via the metadata byte** — forwarding state lives in the same per-granule byte; defrag is opportunistic, precise.
5. **Embeddable by design** — Whippet/Nofl is a library a runtime links against, with a narrow embedder API (the embedder supplies tracing + roots). This is *exactly* v3's "GC as a component over my own arena/Value/roots" situation.

---

## 4. Why Nofl fits v3 (the three constraints, all satisfied)

| v3 constraint | How Nofl answers it |
|---|---|
| **High-L peak** (Cheney's 2× killer at L=0.74) | mark-region is **in-place** — peak ≈ N + ~1.5% metadata (1 byte / 16 B granule), no copy peak |
| **Variable-size Bindings (84% of arena)** | objects span granules/lines naturally; no size classes (flat mark-sweep's weakness); metadata-byte object-start bit handles boundaries precisely |
| **Page-release pin** (arena has no madvise/munmap → freed bytes don't reach OS) | mark-region **frees whole blocks** (→ `munmap`); **opportunistic evacuation concentrates live → frees MORE blocks** even when dead is scattered (the thing flat mark-sweep can't do); madvise dead lines in recyclable blocks |
| **Precise roots exist** (Stages 1/3/5, cleaner post-F4) | Nofl's *precise* mode is the natural fit — no conservative scanning; mark sets per-granule metadata from `walkAllV3Roots`/`RootVisitor` |
| **16-byte Value** | granule = 16 B = Value size; metadata-byte grain aligns with allocation |
| **Single-threaded batch eval** | **stop-the-world Nofl** — drop all of Whippet's concurrent machinery (§5.4) |

---

## 5. The v3 adaptation (how it differs from stock Whippet/GHC)

### 5.1 Structure
- **Block** (size TBD — 32 KB Immix-classic, or align to v3's existing arena block; R2 decides) ÷ **line** (128 B = 8 granules) ÷ **granule** (16 B = `Value`).
- **Per-granule metadata byte** side table: mark / object-start / pinned / forwarded.

### 5.2 Mark
- Reuse **`walkAllV3Roots` + `RootVisitor`** (Stage 3) — precise. Mark sets the granule metadata bytes; derive line/block occupancy from them.

### 5.3 Reclaim + the page-release pin (built IN, not bolted on)

**CORRECTED 2026-06-02 by the R1 page-release spike ([`R1_PAGE_RELEASE_RESULT_2026-06-02.md`](R1_PAGE_RELEASE_RESULT_2026-06-02.md)).** On macOS aarch64, measured: `munmap` returns 100% of freed bytes to RSS; **`std::free` returns 0%** (libmalloc caches large frees in magazines — *this was the pin behind all 11 prior falsifications*); and critically **`madvise(MADV_DONTNEED)` ALSO returns 0%** (and `MADV_FREE` is lazy). So:

- **Whole free block → `munmap`** — the ONLY mechanism that returns RSS on macOS. (R2.0 `55745a112` already replaced the arena's `std::free` block path with mmap/munmap.)
- ~~Recyclable block → madvise dead page-aligned line runs~~ — **DOES NOT WORK on macOS** (`madvise(DONTNEED)` = 0% RSS returned). Span-level madvise is NOT a release mechanism here. Recyclable blocks are still *bump-allocated into* (their free lines reused), but their dead pages are NOT returned until the whole block frees.
- **Therefore evacuation is MANDATORY, not opportunistic** (R0 Gate-C caveat): the only way to return RSS from a partially-dead block is to evacuate its live cells into another block so the whole source block can `munmap`. This is the answer to "dead is scattered so non-moving can't release pages" — and on macOS it is the *sole* answer, because the madvise-the-holes alternative is a no-op. (On Linux, `madvise(DONTNEED)` *does* decommit, so span-level release is available there as a cheaper-than-evacuation path; the design must not assume it — macOS forces whole-block-munmap-via-evacuation as the portable mechanism.)

This sharpens §1: Immix's opportunistic evacuation is a *load-bearing requirement* for v3 on macOS, not a defrag nicety. R2.4a (`703cff5ed`) already validated it's safe (precise walk reaches ~100% of movable objects).

### 5.4 Stop-the-world, NOT concurrent (the key divergence from GHC)
GHC's non-moving collector is *concurrent* (SATB barrier, concurrent marking) — but that machinery exists **entirely for latency** (bounded pauses on large server heaps). **v3 is a single-threaded batch CLI evaluator; pause time is irrelevant.** So v3 takes the *non-moving mark-region shape* but **drops all concurrency** — no SATB barrier, no concurrent marking, no sync. This is dramatically simpler than GHC's collector. v3 wants non-moving for the **peak-RSS** reason, not GHC's latency reason.

### 5.5 Generational (Phase 2, optional)
- Sticky mark bits in the metadata byte → in-place generational. Could **replace** the opt-in Phase E nursery (one mechanism instead of two). Gated on whether R0's L(t) shows generational structure worth it.

---

## 6. Port vs reimplement

Two routes:
- **(a) Link Whippet** (it's embeddable C). Fastest to a working collector; but grafting it onto v3's arena/`Value`/`Tag`/Boehm-integration + the existing `precise_root`/`mark_sweep` substrate is awkward, and it pulls an external dependency into the V3-NATIVE core.
- **(b) Reimplement Nofl-style over v3's substrate**, using Whippet/Nofl + the PLDI'08 paper as the **design reference**. Reuses `precise_root.{cc,hh}`, the arena, the `mark_sweep.cc` scaffolding, the `bytecode.hh` per-block bitmap infra. Aligns with V3-NATIVE (own the GC). More code, but no external dependency and clean integration.

**Lean (b)**, with Whippet/Nofl as the reference implementation to study during R2. The metadata-byte design is small (~2-4 KLoC); the existing substrate covers mark (precise_root) + the per-block bitmap (bytecode.hh) + the gate (`NIX_V3_MAJOR_GC`). Final route is an R2 decision after reading the Nofl source.

---

## 7. R0 gate — Nofl is the *non-moving-branch* answer

Per [`POST_F4_GC_GOAL`](POST_F4_GC_GOAL_2026-06-02.md), the family is data-derived. Nofl-Immix is committed as the design **iff** R0 confirms:
- **Gate A**: M5 still ≥ 4096 post-F4 (GC actually needed).
- **Gate B**: excess in v3_arena, not caches (GC problem, not eviction).
- **Gate C**: L_median ≥ 0.5 AND L_min ≥ 0.3 → **non-moving → Nofl-Immix.**
- **Gate D**: freeable ≥ excess (GC can reach the target).

If Gate C instead shows **L_min < 0.3** (deep low-L troughs) → generational-copying-at-trough may beat Nofl; revisit. If Gate A says M5 already under watchdog → no GC; Nofl shelved. So: **Nofl is the chosen design, R0 confirms it's the right regime.**

The **R1 page-release spike** (madvise/munmap proves ≥80% of freed bytes return) still runs *first* — Nofl's whole-block-free is the page-release mechanism, so R1 validates the foundation Nofl depends on.

---

## 8. Phased plan (slots into the goal's R-phases)

| Phase | Work | Gate |
|---|---|---|
| **R0** | re-measure post-F4 profile (peak/bucket/L/freeable) | confirms non-moving regime → Nofl |
| **R1** | madvise/munmap page-release spike | ≥80% freed-bytes-return (Nofl's whole-block-free depends on it) |
| **R2.1** | metadata-byte side table + granule/line/block structure over the arena | structure compiles; mark sets bytes; cross-check vs `live_trace` live-set |
| **R2.2** | mark (reuse `walkAllV3Roots`) + reclaim (free/recyclable/full classification) + bump-into-holes allocator | `--quick`/`--core` green under `NIX_V3_MAJOR_GC=1`; drvPath byte-equal |
| **R2.3** | page-release: whole-block munmap + madvise dead lines | M5 peak drops; the pin is closed |
| **R2.4** | opportunistic evacuation (defrag → free more blocks) | M5 peak < 4096 (THE goal) |
| **R3 (opt)** | sticky-bit generational; subsume Phase E nursery | only if R0 L(t) shows generational structure worth it |

**SHIP gate** (R2.4): M5 peak < 4096 MB; HNE/hello below post-F4 baseline; wall ≤10%/15%; drvPath byte-equal; 6/6 + 15/15 + brute.

---

## 9. Honest limits

- **R0-gated.** This commits the *design* for the non-moving regime; R0 confirms the regime. Don't start R2 before R0 + R1.
- **Page-release is still the hard part.** Nofl frees whole blocks, but if v3's post-F4 dead arena is sub-page-scattered, even mark-region needs *evacuation* (R2.4) to free contiguous blocks — and evacuation is the moving/complex part. R1 + Gate D confirm there's enough page-aligned dead to release before R2.4 is worth it.
- **Reimplement vs port (§6) is an R2 decision** — reading the Nofl source may reveal it's cleaner to link than to reimplement; keep both open.
- **Evacuation reintroduces moving** — Nofl is "mostly non-moving," but its defrag moves objects (with forwarding). That needs the precise roots to be *complete* (any missed root → corruption on evacuation). The Stage-1/3/5 substrate + the brute audit must cover this; evacuation is the riskiest piece.
- **Block size + metadata layout** (granule 16 B is clean; block 32 KB vs v3's arena block size) are R2.1 details, not settled here.
- **Generational (R3) is optional** — only if L(t) shows it pays; v3's young mortality (0.43-0.58) is *weak*, so the sticky-bit generational benefit is modest.
- **Whippet/Nofl is 2025-era research** — mature enough to study, but v3's reimplementation is its own engineering; don't assume the paper's numbers transfer.

---

## 10. Research bibliography

- **Blackburn & McKinley, "Immix: a Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance," PLDI 2008** — the base design (blocks/lines, mark-region, opportunistic evacuation).
- **Yang & Wingo, "Nofl: A Precise Immix," arXiv 2503.16971 (2025)** — the modern precise refinement (metadata-byte-per-granule, the design v3 targets).
- **Andy Wingo, Whippet blog series (wingolog.org, 2022-2025)** — `mmc` (mostly-marking collector), embeddable GC API, the defaults-for-dynamic-languages rationale.
- **Whippet source** (`github.com/wingo/whippet`, `nofl-space.h`, `mmc.c`) — the reference implementation for R2.
- **GHC nonmoving collector** (Gamari & Dijkstra, "Alligator Collector," ISMM 2020) — the *contrast*: concurrent non-moving for latency; v3 takes the non-moving shape, drops the concurrency (§5.4).
- **Jones, Hosking, Moss, *The Garbage Collection Handbook* (2011)** §10.2 (Immix), §9 (generational) — the textbook reference.
- **Blackburn, Cheng, McKinley, "Myths and Realities," SIGMETRICS 2004** — why copying inverts above L≈0.4-0.5 (the high-L → non-moving rationale).

---

## 11. Cross-references

- [[post-f4-gc-goal-2026-06-02]] — R0→family→implement goal; Nofl is the Gate-C non-moving answer
- [[gc-design-post-cheney-2026-05-28]] — Immix = option A2 in the survey
- [[gc-precise-root-foundation-2026-05-27]] — Stages 1/3/5, the substrate Nofl reuses
- [[tw-value-eradication-goal-2026-06-02]] — F4 deletion = precondition; cleaner roots post-F4
- [[memory-first-class]] — RSS-primary framing
- [[falsification-rule]] / [[measure-twice-cut-once]] — R0/R1 gates
- Code: `precise_root.{cc,hh}` (mark substrate), `mark_sweep.cc` (scaffolding), `bytecode.hh` (per-block bitmap), `alloc.hh` (arena, the page-release pin), `nursery.hh` (Phase E, possibly subsumed by R3)

---

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.*
