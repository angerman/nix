# WS-6 M2.0 — Linux reclaim characterization (measure-first, on real + synthetic workloads)

**Date:** 2026-07-14
**Depends on:** D3/M1 (`LINUX_RECLAIM_M1_2026-07-14.md`) — the OS-level mechanism (free/munmap/MADV return RSS on Linux).
**Scope:** M2.0 measurement only (no gen-major/allocator surgery committed). Reframes M2.1/M2.2.
**Host:** `linux-1` (x86_64-linux, GCC 14.3 / glibc 2.40), v3-eval from the D3 port.
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## The question M2.0 answers

M1 proved the *mechanism* (freeing a dead 16 MiB block returns its RSS on Linux). M2.0 asks the *opportunity + timing* question on actual v3 evals: does the already-active gen-major huge-block reclaim (`mark_sweep.cc:2494`, ungated) actually reduce **peak** RSS on Linux, and does enabling regular whole-block-free (M2.1) help?

## Measurement 1 — firefox.drvPath (real, scattered-dead workload)

Full eval (`NIX_V3_NO_DISK_CACHE=1`), peak via `/proc/self/status:VmHWM`:

- drv correct (`…-firefox-146.0.1.drv`); **peak RSS 1430 MB**.
- The v3 **gen-major FIRES mid-eval on Linux** (like macOS): `major-mark-sweep … markedCells=2565838 hugeMarked=292485 markMs=2235.91 sweepMs=47.47`.
- **`blocksFreed=0 bytesFreed=0`** — identical to macOS. The regular-block sweep is a no-op (`cellStarts` disabled → the `mark_sweep.cc:2438` break) AND every huge block is *marked live* (`hugeMarked=292485`, the firefox library graph).

**⇒ The DENSITY limit is host-independent and confirmed on Linux.** firefox's dead is scattered within live blocks; no block (regular or huge) goes fully dead at the sweep point. Whole-block-free (M2.1) reclaims **0** on firefox on Linux, exactly as on macOS. The macOS "scattered-dead structural floor" **transfers** to Linux.

## Measurement 2 — transient-huge-block synthetic (proves the arena mechanism)

`builtins.foldl' (a: i: a + builtins.length (builtins.genList (j: j) 3000000)) 0 (genList … 200)` — 200 transient 3M-element lists (24 MiB huge block each; each dies after `length`):

- result `600000000` (correct); **peak RSS 4662 MB** (of 4800 MB allocated).
- **`v3 sweep: … blocksFreed=198 bytesFreed=4752.0 MB`** — v3's OWN `freeHugeBlock` freed 198 dead huge blocks = 4752 MB on Linux. (On macOS this same code frees the blocks but libmalloc returns ~0 to RSS — M1.) **The arena's reclaim path works at the workload level on Linux, not just raw libc.**
- BUT peak was **not** bounded (4662 MB) — the free fired **once, late**: the `foldl'` callback runs at nested dispatch depth, and the gen-major only fires at `exitDepth==0 && !nestedDistinct` (`vm.cc:4726`), so dead blocks accumulated to 4.6 GB before a single terminal sweep freed them.

## The reframe — the M2 lever is gen-major TRIGGER FREQUENCY, not the reclaim path

Putting the two together:

1. **The reclaim mechanism already works on Linux** — both the OS syscalls (M1) and v3's own `freeHugeBlock` (measurement 2, 4752 MB). No mechanism change is needed.
2. **Whole-block-free (planned M2.1) is ~0 on scattered-dead workloads (firefox) regardless of OS** — the density limit is host-independent. M2.1 is **de-prioritised**: it only helps where blocks go *fully* dead, which real broad evals (firefox/nixpkgs) don't produce.
3. **The peak-RSS win exists only where two things hold together: (a) dead HUGE blocks (single large cells that die), and (b) `exitDepth==0` points during the eval so the gen-major fires mid-eval to reclaim them before they accumulate.** firefox has (b) but not (a) (huge blocks all live). The synthetic has (a) but not (b) (one deep foldl'). **The workload that has BOTH is M5-class (cardano/haskell.nix): huge 170k-entry Bindings that die, inside a broad eval with many top-level points.** On M5, the mid-eval gen-major on Linux would free dead huge blocks and *return their RSS* — the win macOS masked (libmalloc), and that M1 predicted.

## Recommended M2 program (revised by these measurements)

- **M2-a (the real lever): opportunistic / more-frequent gen-major trigger.** The win is not new reclaim code — it is firing the (already-working-on-Linux) huge-block reclaim *often enough* to bound peak. Two sub-levers: (i) lower `NIX_V3_MAJOR_GC_THRESHOLD_MB` on Linux; (ii) relax the `exitDepth==0`/`nestedDistinct` defer for the **non-moving mark-sweep + huge-block-free only** (the mark-sweep does NOT move cells — `vm.cc:4699` — so the defer is conservative; the pre-scavenge is the only mover and can be skipped for an opportunistic huge-only sweep). This is the memory's bookmarked "opportunistic MS trigger (~200 MB ROI)", now *worthwhile on Linux* where reclaim returns RSS. **Risk: moderate (GC-trigger timing under the always-on nursery) — build carefully, gate on `--brute` 1MB-nursery + byte-id + Linux M5 peak-RSS.**
- **M2.1 (whole-block-free for regular blocks): DE-PRIORITISE** — density-floored, ~0 on firefox on Linux (confirmed). Keep only as a cheap add-on once M2-a lands.
- **M2.2 (MADV_DONTNEED partial-page): still open, but likely page-granularity-density-limited** for scattered dead (a 4 KiB page with any live cell can't be dropped). Measure line/page occupancy on Linux (the F1 Immix probe, run on Linux) before building.
- **The decisive remaining measurement: M5 (or HNE) on Linux** — the one workload with dead huge blocks + mid-eval GC points. It quantifies the actual peak-RSS win of M2-a. Heavy setup (cardano/haskell.nix x86_64-linux IFDs); not yet run.

## Bottom line

M1's mechanism win is **real but conditional**: it converts to a peak-RSS reduction only for workloads with dead *huge* blocks, and only if the gen-major fires mid-eval to reclaim them. For scattered-dead workloads (firefox/most of nixpkgs) the RSS floor is host-independent live representation — the macOS structural-floor conclusion transfers. The fundable M2 lever is the **opportunistic gen-major trigger on Linux**, validated against **M5-on-Linux**. This is a materially better-targeted plan than "wire whole-block-free" (which the data shows is ~0 on the broad workloads).
