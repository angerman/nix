# PLAN BEAT-TW v2 — execution log (2026-06-13)

Companion to `PLAN_BEAT_TW_V2_2026-06-13.md`. Records what each Wave-1 item
actually did when implemented + measured (darwin laptop; CPU = min user over 3,
NIX_V3_DIRECT_EVAL=1; arena = NIX_VM_STATS `v3_arena`, the deterministic RSS
proxy — laptop maxRSS is noise per QG-2; every v3 run byte-identical to TW).
Branch `angerman/2.35-eval-profiling-v2`.

## Wave 1 outcomes

| item | verdict | result |
|---|---|---|
| §1.2 intersectAttrs | **SHIPPED** 6df8d0ed6 | iterate smaller side; hello.drvPath 1.11→0.93s (−16%), firefox 4.65→4.13s (−11%, matches the §0.2 11% share). Byte-identical by construction. |
| §1.1a 1GB threshold | **FALSIFIED** | HNE peak RSS 1397→2021MB (+44.7%) — HNE's GC is useful (frees ~624MB whole blocks). Not shipped; threshold stays 256MB. |
| §1.1b GC backoff | **SHIPPED** 0ddc9ad6a | freed<5% → next threshold = heapBytes×8. M5 26.40→19.53s (−26%), arena IDENTICAL (2147.5MB), HNE-safe (yield key skips useful GC; HNE fires=1, RSS 1393MB unchanged). |
| §1.3 materialize k-way | **FALSIFIED** | byte-identical + O(U·k), but hello −1.1% / firefox −0.7% = noise. The introsort is ~1%, not the §0.2 "9%" (that 9% is collect/alloc → workstream A removes the calls). Reverted. |
| §1.4 has-context filter | **FALSIFIED** | byte-identical + drv-hash-safe, but string-churn microbench +10.5% SLOWER — context is rare → side-table is tiny → unordered_map::find is already ~free → the filter's fmix64 hash is net overhead. Reverted. |
| §1.5 cross-Force deferral | **DEFERRED** | delicate emit-ordering (#668 branch-flush class), small foldl target, needs IR-CHECK fixtures. Re-evaluate at DP-1. |
| §1.6 OP_RETURN diet | **DEFERRED (sub-bar)** | ~3 magic-static guards/return × 5.7M ≈ 0.3% of fib33 — an order of magnitude below the −3% bar; OP_RETURN edit risk not justified. |
| §1.7 OVERRIDES chain guard | **SHIPPED** 40a64c2dd | privatize chain dst before in-place override writes (C-1 class). No-op for Sorted dst; lang 143/143; run-1.7 tests 8/8. |
| §1.8 mark speedup | **FALSIFIED** | The existing `v3 mark-split` (NIX_VM_STATS) settles it: firefox mark is **100% precise-walk** (preciseWalk=883ms, drainConservative=0, cStackConservative=0; markedCells=3.9M → 225ns/cell; M5 identical at 220ns/cell). The per-edge `regionOf` binary search over ~29 blocks is ~5-10 cycles/edge (<10% of 225ns/cell) — the cost is **cache misses** on the random graph walk over the 503MB live set + scattered mark-bitmap writes. Block-aligned-mmap targets the <10% lookup → cannot deliver the −60% bar. Not implemented (risky allocator rewrite avoided). The firefox-mark lever is marking FEWER cells (generational — workstream E), not faster block lookup. |
| hygiene | **SHIPPED** 7114d136c | lint-no-inline-getenv cold-site marker → core 20/20 (clean QG-1 baseline). |
| E-stage-0 | **VIABLE** | firefox mid-eval L_resident peak 0.438 (<0.80) + dead-lines 42.7% (>20%) → workstream E NOT killed. hello has no exitDepth==0 mid-eval samples (import-dominated; its RSS path is A+F+G). Confirms the exitDepth==0 safepoint is starved (firefox: 2 samples) → E stage-1 needs gcPending in ALL dispatch loops. |

## Key cross-cutting finding (for DP-1)

The §0.2 hello "cheap-win" profile shares (materialize-introsort 9%, context-probe
6%) DO NOT reproduce as wall-CPU wins. A `/usr/bin/sample` of hello.drvPath shows
it dominated by forceValue / callClosure / dispatchLoop / **primImport +
disk_cache::lookup** — hello.drvPath wall is import/disk-cache-dominated, a poor
proxy for eval-compute micro-shares. **DP-1 must re-profile with a WARM disk cache
and eval-compute isolated** before funding any further §0.3 micro-items. The real
hello/git levers are import/cache (workstream G) + core dispatch (workstream C),
not the cheap micro-ops; §1.2 (a genuine algorithmic fix) is the exception that
did land.

**The firefox CPU question is structurally hard (both levers falsified).** The
single biggest firefox cost is the one major GC (~0.9s mark / 3.9M cells). Two
ways to attack it, both now killed: (a) *skip* it — needs a higher GC threshold,
which regresses HNE +44.7% (§1.1a); (b) *make the mark cheap* — block-aligned mmap
(§1.8) targets the <10% block-lookup, not the ~90% cache-miss graph walk. The only
real lever is marking FEWER cells (generational / incremental GC = workstream E
stage-1+, a Wave-3 multi-week effort) — or workstream C (dispatch) for the
non-GC residual. firefox stays ~2.38× until then; this is the honest ceiling for
the cheap-items wave.

## Re-pinned 7-row table (QG-4)

See `bench/baselines/seven-rows.tsv` (regenerate: `make -C bench pin-seven-rows`;
guard: `make -C bench ratchet-check`, fails on >3% CPU / >5% arena regression).
Post-Wave-1-ships vs the §5 "now (darwin-4)" column, several rows improved on
laptop largely via §1.2:

| row | §5 "now" | laptop post-W1 |
|---|---|---|
| attrNames | 0.70× | 0.46× (v3 WINS) |
| fib | 1.36× | 1.37× |
| hello | 1.65× | 1.28× |
| git | 1.88× | 1.54× |
| firefox | 2.78× | 2.38× (arena 503MB) |
| M5 | — | 2.31× (19.23s w/ §1.1b backoff vs 26.40s no-backoff; arena 2147MB) |

(Laptop ratios; the formal darwin-4 re-pin per QG-2 is the canonical bar. The TSV
is the laptop ratchet floor for this branch.)

*Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
Group. SPDX-License-Identifier: Apache-2.0.*
