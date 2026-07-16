# WS-5.0 — COW / zygote parallel-eval-density baseline (measured on Linux)

**Date:** 2026-07-16
**Scope:** measure-first (Rule 0) for WS-5, before any CU/descriptor layout surgery.
**Host:** `linux-1` (x86_64-linux); v3-eval `--cow-fork` harness (this commit).
**Copyright:** (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group. SPDX-License-Identifier: Apache-2.0.

---

## The question

M2-b showed peak RSS is *live* memory dominated by caches (CU-bytecode 212 MB, ImportCache ~700 MB) — not reclaimable garbage. The CI goal is "N evals per box", so the lever is **sharing** those live pages across concurrent evals, not freeing them. WS-5.0 measures how much a forked child *already* shares with a warmed parent (copy-on-write), and thus whether the planned CU/descriptor side-arraying (WS5-D1) is a prerequisite or an increment.

## The harness — `v3-eval --cow-fork [--child-expr E]`

Warms the process caches by evaluating `--expr`, then `fork()`s at a quiescent post-eval point (single-threaded — the only thread, the heap-trace sampler, is off by default, so the fork is safe), and re-evaluates (`--child-expr`, or `--expr` again) in the child. Reports, from `/proc/self/smaps_rollup`, the child's `Private_Dirty` (its private per-eval cost) vs `Shared_Clean`. The parent's warm graph is inherited as `Shared_Dirty` (COW) — the child only pays for what it privately writes.

## Results (nixpkgs pinned; firefox.drvPath warm parent)

| case | parent warm Rss / Private_Dirty | child per-eval PRIVATE cost | fresh-process baseline | sharing |
|---|---|---|---|---|
| **A. same-expr** (child re-evals firefox) | 947 / 866 MB | **27 MB** | ~1430 MB (M2.0 firefox) | **~97 %** (~35× density) |
| **B. diff-package** (child evals git) | 944 / 863 MB | **190 MB** | **283 MB** (fresh git.drvPath) | **33 % saved** |

## Interpretation

- **Case A is the dominant CI scenario** — N runners re-evaluating the *same* flake. A forked child adds only **27 MB** on top of the shared warm image, because the applied-import cache (inherited COW) serves the top-level result, so the child re-does almost no allocation. This is the RSS-side complement of D4/KPI-4 (which showed the same reuse costs 0.1 ms of CPU). **KPI-5 (incremental RSS ≤ 70 % of fresh) passes by ~35×.** N concurrent identical evals cost `~866 MB shared once + N×27 MB`, versus `N×947 MB` without a zygote.
- **Case B (distinct jobs)** — a child evaluating a *different* package still shares the nixpkgs CU bytecode + descriptors it has in common with the parent: 190 MB private vs 283 MB standalone = **33 % saved / ~93 MB shared**. Some of the 190 MB is git's own (intrinsic, unavoidable) value graph; some is the child re-dirtying *shared* CU pages via inline-cache writes + LambdaDescriptor counter bumps + the `cu` back-pointer re-stamp — that latter slice is what WS5-D1 (side-arraying) would recover.

## Verdict — the parallel-eval-density win is achievable NOW via a zygote; D1 is incremental

The headline WS-5 objective ("run N evals per box") is **already delivered by COW + a warm-parent fork**, with no layout change: ~35× density for the repeated-CI case, 33 % saving for distinct jobs. Fork-safety with the moving nursery + Boehm held in practice (the child completed a full re-eval and reported clean smaps). This reframes the remaining WS-5 program:

- **The shippable win = a production zygote/fork-server** (WS5-D3): warm one parent (AOT loaded + nixpkgs prelude evaluated), `fork()` per CI job. The measurement harness here is the prototype; productionising it (a fork-server front-end, per-child request routing, Boehm `GC_atfork` hardening for the threaded-daemon case) is the deliverable.
- **WS5-D2 (consume the AOT mmap in place)** is the complementary lever for CI that uses *separate processes* rather than forks (each runner a fresh `nix`): today the AOT file (D4/W3) is copied out + deserialized into private vectors, so cross-*process* CU sharing is ~0 (Shared_Clean was only 12–20 MB). Making the CU readable in place from the mmap turns those 212 MB into `Shared_Clean` across independent processes — the same win as the zygote, without fork. **This is the higher-value next build for a process-per-job CI.**
- **WS5-D1 (side-array CU/descriptor mutables)** is now an **increment**, not a prerequisite: it recovers the "child re-dirties shared CU pages" slice of case B (and lowers the parent's own dirty footprint + enables D2's in-place sharing to stay clean). Worthwhile, but its ceiling is bounded by the measured numbers above — build it after D2/D3, gated on a re-measure showing case B's private cost actually drops.

**Bottom line:** WS-5 is a GO and the win is real — but the fast path to it is the **zygote (D3) + in-place AOT (D2)**, which capture the sharing that COW already demonstrates, rather than the invasive layout surgery (D1) the PRD listed first. Same convergence as DECISION INPUT #1 and M2-b: the CI wins are **sharing + process/parent reuse**, not per-eval work.
